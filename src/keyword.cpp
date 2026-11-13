/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         keyword.cpp
 * Description:  关键词缓存的实现。
 *               问题按 UTF-8 码点切成 bigram，用 IDF 给常见块降权，
 *               再按「共同权重 / 较短一侧权重」算 0~1 的相似度。
 * ==============================================================================
 */

#include "keyword.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "cache_guard.h"

namespace {

// 读取环境变量
std::string get_env(const char* key, const std::string& default_val = "") {
    const char* val = std::getenv(key);
    return (val && *val) ? std::string(val) : default_val;
}

// UTF-8 解码：非法字节按单字节处理
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

void append_utf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool is_content_codepoint(unsigned int cp) {
    if ((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
        (cp >= 'A' && cp <= 'Z')) {
        return true;
    }
    return cp >= 0x3400;
}

double threshold_from_env() {
    const std::string text = get_env("KEYWORD_CACHE_THRESHOLD", "0.85");
    try {
        const double value = std::stod(text);
        return (value > 0.0 && value <= 1.0) ? value : 0.85;
    } catch (const std::exception&) {
        return 0.85;
    }
}

}  // namespace

std::string KeywordIndex::scope_of(const ExactCacheKey& key) {
    std::string scope = std::to_string(key.user_id);
    scope.push_back('\x1f');
    scope += key.model;
    scope.push_back('\x1f');
    scope += key.prompt_hash;
    return scope;
}

std::unordered_set<std::string> KeywordIndex::make_grams(const std::string& text) {
    std::vector<unsigned int> codepoints;
    size_t i = 0;
    while (i < text.size()) {
        unsigned int cp = 0;
        decode_utf8(text, i, cp);
        if (is_content_codepoint(cp)) {
            codepoints.push_back(cp);
        }
    }

    std::unordered_set<std::string> grams;
    for (size_t k = 0; k + 1 < codepoints.size(); ++k) {
        std::string gram;
        append_utf8(gram, codepoints[k]);
        append_utf8(gram, codepoints[k + 1]);
        grams.insert(std::move(gram));
    }
    return grams;
}

double KeywordIndex::weight_of(const std::string& gram,
                               const std::unordered_map<std::string, int>& df,
                               size_t total_docs) {
    const auto it = df.find(gram);
    const int frequency = (it == df.end()) ? 0 : it->second;
    return std::log(1.0 + (static_cast<double>(total_docs) + 1.0) /
                              (static_cast<double>(frequency) + 1.0));
}

void KeywordIndex::add_locked(const ExactCacheKey& key, long long answer_id,
                              const std::string& question, long long expires_at) {
    Entry entry;
    entry.answer_id = answer_id;
    entry.question = question;
    entry.normalized = normalize_question(question);
    entry.expires_at = expires_at;
    entry.grams = make_grams(entry.normalized);
    if (entry.grams.empty()) {
        return;
    }

    auto& entries = scopes_[scope_of(key)];

    // 同一条答案重复写入时原地替换，避免索引里出现重复项
    for (Entry& existing : entries) {
        if (existing.answer_id == answer_id) {
            for (const std::string& gram : existing.grams) {
                --df_[gram];
            }
            for (const std::string& gram : entry.grams) {
                ++df_[gram];
            }
            existing = std::move(entry);
            return;
        }
    }

    for (const std::string& gram : entry.grams) {
        ++df_[gram];
    }
    ++total_docs_;
    entries.push_back(std::move(entry));
}

void KeywordIndex::add(const ExactCacheKey& key, long long answer_id,
                       const std::string& question, long long expires_at) {
    std::lock_guard<std::mutex> lock(mutex_);
    add_locked(key, answer_id, question, expires_at);
}

void KeywordIndex::load(Db& db) {
    std::vector<std::vector<std::string>> rows;
    std::string error;
    const bool ok = db.query_all(
        "SELECT id, user_id, model, prompt_hash, question, expires_at "
        "FROM answers WHERE expires_at > ?",
        {std::to_string(static_cast<long long>(std::time(nullptr)))}, rows, error);
    if (!ok) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    scopes_.clear();
    df_.clear();
    total_docs_ = 0;

    for (const auto& row : rows) {
        if (row.size() != 6) {
            continue;
        }
        ExactCacheKey key{0, row[2], row[3], ""};
        long long answer_id = 0;
        long long expires_at = 0;
        try {
            answer_id = std::stoll(row[0]);
            key.user_id = std::stol(row[1]);
            expires_at = std::stoll(row[5]);
        } catch (const std::exception&) {
            continue;
        }
        add_locked(key, answer_id, row[4], expires_at);
    }
}

bool KeywordIndex::search(const ExactCacheKey& key,
                          const std::string& normalized_question, double threshold,
                          long long& answer_id, std::string& question,
                          double& score) const {
    const auto query_grams = make_grams(normalized_question);
    if (query_grams.empty()) {
        return false;
    }

    const long long now = static_cast<long long>(std::time(nullptr));

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = scopes_.find(scope_of(key));
    if (it == scopes_.end()) {
        return false;
    }

    double query_weight = 0.0;
    for (const std::string& gram : query_grams) {
        query_weight += weight_of(gram, df_, total_docs_);
    }
    if (query_weight <= 0.0) {
        return false;
    }

    struct Candidate {
        double score = 0.0;
        size_t index = 0;
    };
    std::vector<Candidate> candidates;

    const auto& entries = it->second;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        if (entry.expires_at <= now) {
            continue;
        }

        // 长度差太多的不可能是同一个问题。比如「怎么重置密码」和
        // 「怎么重置密码后无法登录」是包含关系，但后半句在问另一件事；
        // 允许 1.6 倍以内的差异，刚好放行「请问」这类前缀
        const size_t shorter = std::min(query_grams.size(), entry.grams.size());
        const size_t longer = std::max(query_grams.size(), entry.grams.size());
        if (shorter == 0 ||
            static_cast<double>(longer) > static_cast<double>(shorter) * 1.6) {
            continue;
        }

        double shared_weight = 0.0;
        for (const std::string& gram : query_grams) {
            if (entry.grams.count(gram) != 0) {
                shared_weight += weight_of(gram, df_, total_docs_);
            }
        }
        if (shared_weight <= 0.0) {
            continue;
        }

        double entry_weight = 0.0;
        for (const std::string& gram : entry.grams) {
            entry_weight += weight_of(gram, df_, total_docs_);
        }

        // 分母取较短的一侧：允许对方多几个字（如「请问」前缀），但不允许少掉核心词
        const double denominator = std::min(query_weight, entry_weight);
        if (denominator <= 0.0) {
            continue;
        }
        const double candidate_score = shared_weight / denominator;
        if (candidate_score >= threshold) {
            candidates.push_back({candidate_score, i});
        }
    }

    if (candidates.empty()) {
        return false;
    }

    const size_t keep = std::min<size_t>(5, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + keep,
                      candidates.end(), [](const Candidate& a, const Candidate& b) {
                          return a.score > b.score;
                      });
    candidates.resize(keep);

    for (const Candidate& candidate : candidates) {
        const Entry& entry = entries[candidate.index];
        if (!cache_guard_pass(normalized_question, entry.question)) {
            continue;
        }
        answer_id = entry.answer_id;
        question = entry.question;
        score = candidate.score;
        return true;
    }
    return false;
}

bool keyword_lookup(Db& db, const KeywordIndex& index, const ExactCacheKey& key,
                    const std::string& normalized_question, std::string& answer,
                    std::string& error, double& score) {
    long long answer_id = 0;
    std::string matched_question;
    if (!index.search(key, normalized_question, threshold_from_env(), answer_id,
                      matched_question, score)) {
        return false;
    }
    std::cout << "[ai-chat] 关键词候选：相似度=" << score
              << " 缓存问题=" << matched_question << std::endl;

    std::vector<std::string> row;
    if (!db.query_row("SELECT answer FROM answers WHERE id = ? AND expires_at > ?",
                      {std::to_string(answer_id),
                       std::to_string(static_cast<long long>(std::time(nullptr)))},
                      row, error) ||
        row.empty() || row[0].empty()) {
        return false;
    }
    answer = row[0];
    return true;
}
