/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         tokenizer.cpp
 * Description:  Token 计数模块的实现。
 *               把文本拆成几类特征按系数加权，近似真实的 BPE 分词结果。
 *               系数来自对 DeepSeek 的实测拟合，不依赖任何外部服务或词表。
 * ==============================================================================
 */

#include "tokenizer.h"

#include <cctype>
#include <cmath>

namespace {

// 以下系数来自对 DeepSeek 实测数据的拟合：19 个训练样本、6 个留出样本。
// 训练集平均绝对误差 1.01 token，留出样本 1.46 token，最大约 4 token。
// 注意它们只对当前模型成立——换模型后必须重新测一轮再更新。
constexpr double kIntercept       = 30.504;  // 固定开销：系统提示 + 单条消息模板
constexpr double kPerMessage      = 2.0;     // 每多一条消息的角色标记开销
constexpr double kHan             = 0.526;   // 每个非 ASCII 码点（汉字、中文标点）
constexpr double kWord            = 0.663;   // 每个英文单词
constexpr double kExtraLetter     = 0.250;   // 单词中超出首字符的每个字母
constexpr double kDigit           = 0.331;   // 每个数字
constexpr double kSymbol          = 0.618;   // 每个 ASCII 符号

}  // namespace

// 统计文本里的各类特征并按拟合系数加权
double content_tokens(const std::string& text) {
    size_t han = 0;            // 非 ASCII 码点数
    size_t words = 0;          // 英文单词数
    size_t extra_letters = 0;  // 单词里超出首字符的字母数（长词会被 BPE 拆开）
    size_t digits = 0;
    size_t symbols = 0;
    size_t word_len = 0;

    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);

        if (c >= 0x80) {
            // 非 ASCII：只数首字节。UTF-8 的续字节形如 10xxxxxx，不重复计数
            if ((c & 0xC0) != 0x80) {
                ++han;
            }
            if (word_len > 0) {
                ++words;
                extra_letters += word_len - 1;
                word_len = 0;
            }
            continue;
        }

        if (std::isalpha(c)) {
            ++word_len;
            continue;
        }

        if (word_len > 0) {
            ++words;
            extra_letters += word_len - 1;
            word_len = 0;
        }
        if (std::isdigit(c)) {
            ++digits;
        } else if (!std::isspace(c)) {
            ++symbols;
        }
    }

    if (word_len > 0) {
        ++words;
        extra_letters += word_len - 1;
    }

    return kHan * static_cast<double>(han)
         + kWord * static_cast<double>(words)
         + kExtraLetter * static_cast<double>(extra_letters)
         + kDigit * static_cast<double>(digits)
         + kSymbol * static_cast<double>(symbols);
}

// 单条消息的估算：固定开销 + 内容
TokenResult count_tokens(const std::string& text) {
    TokenResult result;
    result.code = 200;
    result.num_tokens = static_cast<int>(std::lround(kIntercept + content_tokens(text)));
    return result;
}

// 整段对话的估算：固定开销 + 各条内容 + 每多一条消息的角色标记
TokenResult estimate_conversation(const std::vector<std::string>& contents) {
    TokenResult result;
    result.code = 200;
    if (contents.empty()) {
        return result;
    }

    double total = kIntercept;
    for (size_t i = 0; i < contents.size(); ++i) {
        if (i > 0) {
            total += kPerMessage;
        }
        total += content_tokens(contents[i]);
    }

    result.num_tokens = static_cast<int>(std::lround(total));
    return result;
}
