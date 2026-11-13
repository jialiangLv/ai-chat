/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         keyword.h
 * Description:  关键词缓存的对外接口。
 *               把问题切成「两字一组」的积木块，用 IDF 加权后比重叠比例；
 *               只处理字面接近的重复问题，是向量层之前的廉价预筛。
 * ==============================================================================
 */

#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cache.h"
#include "db.h"

class KeywordIndex {
public:
    void load(Db& db);

    void add(const ExactCacheKey& key, long long answer_id,
             const std::string& question, long long expires_at);

    bool search(const ExactCacheKey& key, const std::string& normalized_question,
                double threshold, long long& answer_id, std::string& question,
                double& score) const;

private:
    struct Entry {
        long long                 answer_id = 0;
        std::string               question;
        std::string               normalized;
        long long                 expires_at = 0;
        std::unordered_set<std::string> grams;
    };

    static std::string scope_of(const ExactCacheKey& key);
    static std::unordered_set<std::string> make_grams(const std::string& text);
    static double weight_of(const std::string& gram,
                            const std::unordered_map<std::string, int>& df,
                            size_t total_docs);

    void add_locked(const ExactCacheKey& key, long long answer_id,
                    const std::string& question, long long expires_at);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<Entry>> scopes_;
    std::unordered_map<std::string, int> df_;
    size_t total_docs_ = 0;
};

// 检索；命中时 answer 为完整答案
bool keyword_lookup(Db& db, const KeywordIndex& index, const ExactCacheKey& key,
                    const std::string& normalized_question, std::string& answer,
                    std::string& error, double& score);
