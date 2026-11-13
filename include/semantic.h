/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         semantic.h
 * Description:  语义缓存的向量层对外接口。
 *               负责向量表、进程内向量索引、语义检索与几条命中护栏
 *               （否定词、数字/英文实体、指代词、短问题）。
 * ==============================================================================
 */

#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache.h"
#include "db.h"

// 进程内的向量索引：按「用户 + 模型 + 提示词指纹」隔离，每个范围一个数组
class SemanticIndex {
public:
    void load(Db& db);

    // 是否适合走向量层：实词足够、无指代词、无否定词
    bool search(const ExactCacheKey& key, const std::string& normalized_question,
                const std::vector<float>& query_vector, double threshold,
                long long& answer_id, std::string& question, float& score) const;

    void add(const ExactCacheKey& key, long long answer_id, const std::string& question,
             const std::vector<float>& vector, long long expires_at);

private:
    struct Entry {
        long long               answer_id = 0;
        std::string             question;
        std::vector<float>      vector;
        long long               expires_at = 0;
    };

    static std::string scope_of(const ExactCacheKey& key);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<Entry>> scopes_;
};

// 建向量表（幂等）
bool init_semantic_schema(Db& db, std::string& error);

// 语义检索；命中时 answer 为完整答案
bool semantic_lookup(Db& db, const SemanticIndex& index, const ExactCacheKey& key,
                     const std::string& normalized_question,
                     const std::vector<float>& query_vector, std::string& answer,
                     std::string& error, float& score);

// 写入一条答案的向量，并同步更新内存索引
bool semantic_store(Db& db, SemanticIndex& index, const ExactCacheKey& key,
                    long long answer_id, const std::string& question,
                    const std::vector<float>& vector, long long expires_at,
                    std::string& error);
