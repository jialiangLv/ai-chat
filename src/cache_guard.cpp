/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         cache_guard.cpp
 * Description:  缓存准入规则与命中护栏的实现。
 *               规则都是字面判断，宁严勿宽：短问题、带指代、带否定的一律不进
 *               语义层；否定极性和实体不一致的候选一律拒绝。
 * ==============================================================================
 */

#include "cache_guard.h"

#include <algorithm>
#include <string>
#include <unordered_set>

namespace {

// 实词数量下限：低于它的问题不进关键词/向量层（「你好」「为什么」）
constexpr int kMinContentChars = 3;

// UTF-8 解码：非法字节按单字节处理，保证不会越界
bool decode_utf8(const std::string& text, size_t& i, unsigned int& cp) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
        cp = lead;
        ++i;
        return true;
    }

    int extra = 0;
    unsigned int value = 0;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        value = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        value = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        value = lead & 0x07;
    } else {
        cp = lead;
        ++i;
        return false;
    }

    if (i + extra >= text.size()) {
        cp = lead;
        ++i;
        return false;
    }
    for (int k = 1; k <= extra; ++k) {
        const unsigned char next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xC0) != 0x80) {
            cp = lead;
            ++i;
            return false;
        }
        value = (value << 6) | (next & 0x3F);
    }
    cp = value;
    i += extra + 1;
    return true;
}

// 只保留问题里真正有信息量的字，用来判断「短问题」
bool is_content_codepoint(unsigned int cp) {
    if ((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
        (cp >= 'A' && cp <= 'Z')) {
        return true;
    }
    return cp >= 0x3400;  // 中日韩统一表意文字
}

// 去掉疑问词/语气词后还剩几个实词
int content_char_count(const std::string& text) {
    static const char* kStopWords[] = {
        "为什么", "怎么", "怎样", "如何", "什么", "多少", "哪里", "哪个", "谁",
        "何时", "请问", "一下", "呢",   "吗",   "啊",   "呀",   "吧",   "的",
        "了",     "是",   "有",   "能",
    };

    std::string stripped = text;
    for (const char* word : kStopWords) {
        const std::string token(word);
        size_t pos = 0;
        while ((pos = stripped.find(token, pos)) != std::string::npos) {
            stripped.erase(pos, token.size());
        }
    }

    int count = 0;
    size_t i = 0;
    while (i < stripped.size()) {
        unsigned int cp = 0;
        decode_utf8(stripped, i, cp);
        if (is_content_codepoint(cp)) {
            ++count;
        }
    }
    return count;
}

// 是否包含否定词。只看字面，宁可把「不错」之类也当成否定，保守跳过语义层
bool has_negation(const std::string& text) {
    static const char* kNegations[] = {"不", "没", "无", "非", "别", "未", "勿"};
    for (const char* word : kNegations) {
        if (text.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 指代词/承接词：出现就说明这句话依赖上下文
bool has_context_marker(const std::string& text) {
    static const char* kMarkers[] = {
        "它",   "他",   "她",   "这个", "那个", "这些", "那些", "上面",
        "前面", "刚才", "之前", "继续", "然后", "还有", "再说", "详细",
        "展开", "举例", "再来", "进一步",
    };
    for (const char* word : kMarkers) {
        if (text.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 问题里有效字符的总数（不剔除疑问词），用于粗判长度差异
int total_content_chars(const std::string& text) {
    int count = 0;
    size_t i = 0;
    while (i < text.size()) {
        unsigned int cp = 0;
        decode_utf8(text, i, cp);
        if (is_content_codepoint(cp)) {
            ++count;
        }
    }
    return count;
}

// 提取问题里的数字和英文单词；两边集合不同就认为在问不同的东西
std::unordered_set<std::string> entity_tokens(const std::string& text) {
    std::unordered_set<std::string> tokens;
    std::string current;
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z');
        if (alnum) {
            current.push_back(static_cast<char>(
                (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
            continue;
        }
        if (!current.empty()) {
            tokens.insert(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.insert(current);
    }
    return tokens;
}

}  // namespace

bool cache_question_eligible(const std::string& normalized_question) {
    if (normalized_question.empty()) {
        return false;
    }
    if (has_negation(normalized_question)) {
        return false;
    }
    if (has_context_marker(normalized_question)) {
        return false;
    }
    return content_char_count(normalized_question) >= kMinContentChars;
}

bool cache_guard_pass(const std::string& query, const std::string& candidate) {
    if (has_negation(query) != has_negation(candidate)) {
        return false;
    }
    if (entity_tokens(query) != entity_tokens(candidate)) {
        return false;
    }

    // 长度差两倍以上的大概率不是同一个问题，比如「怎么重置密码」
    // 和「怎么重置密码后无法登录」——后者问的是另一件事
    const int query_len = total_content_chars(query);
    const int candidate_len = total_content_chars(candidate);
    if (query_len > 0 && candidate_len > 0) {
        const int shorter = std::min(query_len, candidate_len);
        const int longer = std::max(query_len, candidate_len);
        if (longer > shorter * 2) {
            return false;
        }
    }
    return true;
}
