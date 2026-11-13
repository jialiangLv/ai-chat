/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         semantic.cpp
 * Description:  语义缓存向量层的实现。
 *               向量以十六进制文本存进 SQLite，进程内保留一份按范围隔离的
 *               索引；命中前统一走 cache_guard 的护栏，宁可漏不可错。
 * ==============================================================================
 */

#include "semantic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

std::string encode_vector(const std::vector<float>& vector) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(vector.size() * sizeof(float) * 2);

    size_t pos = 0;
    for (const float value : vector) {
        unsigned char bytes[sizeof(float)];
        std::memcpy(bytes, &value, sizeof(float));
        for (const unsigned char byte : bytes) {
            out[pos++] = kHex[byte >> 4];
            out[pos++] = kHex[byte & 0x0F];
        }
    }
    return out;
}

std::vector<float> decode_vector(const std::string& hex) {
    std::vector<float> vector;
    if (hex.size() % (sizeof(float) * 2) != 0) {
        return vector;
    }

    auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        return -1;
    };

    vector.reserve(hex.size() / (sizeof(float) * 2));
    for (size_t i = 0; i + sizeof(float) * 2 <= hex.size();
         i += sizeof(float) * 2) {
        unsigned char bytes[sizeof(float)];
        for (size_t j = 0; j < sizeof(float); ++j) {
            const int high = hex_value(hex[i + j * 2]);
            const int low = hex_value(hex[i + j * 2 + 1]);
            if (high < 0 || low < 0) {
                return {};
            }
            bytes[j] = static_cast<unsigned char>((high << 4) | low);
        }
        float value = 0.0f;
        std::memcpy(&value, bytes, sizeof(float));
        vector.push_back(value);
    }
    return vector;
}

// 阈值用真实问题对校准过：Qwen3-Embedding 对「近似改写」约 0.88，
// 对「相关但不同」约 0.76。取 0.86 只放行接近逐字改写的，宁可漏不可错
double threshold_from_env() {
    const std::string text = get_env("SEMANTIC_CACHE_THRESHOLD", "0.86");
    try {
        const double value = std::stod(text);
        return (value > 0.0 && value <= 1.0) ? value : 0.86;
    } catch (const std::exception&) {
        return 0.86;
    }
}

}  // namespace

std::string SemanticIndex::scope_of(const ExactCacheKey& key) {
    std::string scope = std::to_string(key.user_id);
    scope.push_back('\x1f');
    scope += key.model;
    scope.push_back('\x1f');
    scope += key.prompt_hash;
    return scope;
}

void SemanticIndex::load(Db& db) {
    std::vector<std::vector<std::string>> rows;
    std::string error;
    const bool ok = db.query_all(
        "SELECT v.answer_id, v.user_id, v.model, v.prompt_hash, a.question,"
        "       a.expires_at, v.vector_hex "
        "FROM answer_vectors v JOIN answers a ON a.id = v.answer_id "
        "WHERE a.expires_at > ?",
        {std::to_string(static_cast<long long>(std::time(nullptr)))}, rows, error);
    if (!ok) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    scopes_.clear();
    for (const auto& row : rows) {
        if (row.size() != 7) {
            continue;
        }
        Entry entry;
        try {
            entry.answer_id = std::stoll(row[0]);
            entry.expires_at = std::stoll(row[5]);
        } catch (const std::exception&) {
            continue;
        }
        entry.question = row[4];
        entry.vector = decode_vector(row[6]);
        if (entry.vector.empty()) {
            continue;
        }

        ExactCacheKey key{0, row[2], row[3], ""};
        try {
            key.user_id = std::stol(row[1]);
        } catch (const std::exception&) {
            continue;
        }
        scopes_[scope_of(key)].push_back(std::move(entry));
    }
}

bool SemanticIndex::search(const ExactCacheKey& key,
                           const std::string& normalized_question,
                           const std::vector<float>& query_vector, double threshold,
                           long long& answer_id, std::string& question,
                           float& score) const {
    const long long now = static_cast<long long>(std::time(nullptr));

    std::vector<std::pair<float, size_t>> scored;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = scopes_.find(scope_of(key));
        if (it == scopes_.end()) {
            return false;
        }

        const auto& entries = it->second;
        for (size_t i = 0; i < entries.size(); ++i) {
            const Entry& entry = entries[i];
            if (entry.expires_at <= now || entry.vector.size() != query_vector.size()) {
                continue;
            }
            float dot = 0.0f;
            for (size_t d = 0; d < query_vector.size(); ++d) {
                dot += query_vector[d] * entry.vector[d];
            }
            if (dot >= static_cast<float>(threshold)) {
                scored.emplace_back(dot, i);
            }
        }

        if (scored.empty()) {
            return false;
        }
        const size_t keep = std::min<size_t>(5, scored.size());
        std::partial_sort(scored.begin(), scored.begin() + keep, scored.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first; });
        scored.resize(keep);

        // 护栏用的是问题原文，所以拷出来后再判断
        for (const auto& candidate : scored) {
            const Entry& entry = entries[candidate.second];
            if (!cache_guard_pass(normalized_question, entry.question)) {
                continue;
            }
            answer_id = entry.answer_id;
            question = entry.question;
            score = candidate.first;
            return true;
        }
        return false;
    }
}

void SemanticIndex::add(const ExactCacheKey& key, long long answer_id,
                        const std::string& question,
                        const std::vector<float>& vector, long long expires_at) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry entry;
    entry.answer_id = answer_id;
    entry.question = question;
    entry.vector = vector;
    entry.expires_at = expires_at;
    scopes_[scope_of(key)].push_back(std::move(entry));
}

bool init_semantic_schema(Db& db, std::string& error) {
    const std::string table_sql =
        "CREATE TABLE IF NOT EXISTS answer_vectors ("
        "  answer_id   INTEGER PRIMARY KEY,"
        "  user_id     INTEGER NOT NULL,"
        "  model       TEXT    NOT NULL,"
        "  prompt_hash TEXT    NOT NULL,"
        "  vector_hex  TEXT    NOT NULL,"
        "  created_at  INTEGER NOT NULL"
        ")";
    const std::string index_sql =
        "CREATE INDEX IF NOT EXISTS idx_vectors_scope "
        "ON answer_vectors(user_id, model, prompt_hash)";

    if (db.exec(table_sql, {}, error) < 0 || db.exec(index_sql, {}, error) < 0) {
        return false;
    }

    // answers 里的过期条目已经在 init_cache_schema 里删掉了，
    // 这里把指向它们的孤儿向量一并清掉，避免表无限增长
    return db.exec(
               "DELETE FROM answer_vectors "
               "WHERE answer_id NOT IN (SELECT id FROM answers)",
               {}, error) >= 0;
}

bool semantic_lookup(Db& db, const SemanticIndex& index, const ExactCacheKey& key,
                     const std::string& normalized_question,
                     const std::vector<float>& query_vector, std::string& answer,
                     std::string& error, float& score) {
    long long answer_id = 0;
    std::string matched_question;
    if (!index.search(key, normalized_question, query_vector, threshold_from_env(),
                      answer_id, matched_question, score)) {
        return false;
    }
    std::cout << "[ai-chat] 语义候选：相似度=" << score
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

bool semantic_store(Db& db, SemanticIndex& index, const ExactCacheKey& key,
                    long long answer_id, const std::string& question,
                    const std::vector<float>& vector, long long expires_at,
                    std::string& error) {
    if (answer_id <= 0 || vector.empty()) {
        return false;
    }

    const int changes = db.exec(
        "INSERT INTO answer_vectors (answer_id, user_id, model, prompt_hash,"
        " vector_hex, created_at) VALUES (?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(answer_id) DO UPDATE SET vector_hex = excluded.vector_hex,"
        " created_at = excluded.created_at",
        {std::to_string(answer_id), std::to_string(key.user_id), key.model,
         key.prompt_hash, encode_vector(vector),
         std::to_string(static_cast<long long>(std::time(nullptr)))},
        error);
    if (changes < 0) {
        return false;
    }

    index.add(key, answer_id, question, vector, expires_at);
    return true;
}
