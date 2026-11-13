/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         cache.h
 * Description:  语义缓存第一层——精确缓存的对外接口。
 *               把「用户 + 模型 + 系统提示词指纹 + 规范化问题」揉成一个
 *               SHA-256 键，命中时取回完整答案；答案本体统一存 answers 表，
 *               供后续关键词层、向量层按 id 复用。
 * ==============================================================================
 */

#pragma once

#include <string>

#include "db.h"

// 精确缓存的键：任一字段不同都不算同一个问题
struct ExactCacheKey {
    long        user_id = 0;
    std::string model;
    std::string prompt_hash;  // 系统提示词的 SHA-256 前 16 位
    std::string question;     // 规范化之后的问题
};

// 建表（幂等），并顺手清理已经过期的条目
bool init_cache_schema(Db& db, std::string& error);

// 规范化问题：统一空白、全角转半角、ASCII 转小写、去掉首尾标点
std::string normalize_question(const std::string& text);

// 计算 SHA-256，返回小写十六进制字符串
std::string sha256_hex(const std::string& data);

// 命中返回 true 并写回 answer；未命中返回 false 且 error 为空
bool cache_lookup(Db& db, const ExactCacheKey& key, std::string& answer,
                  std::string& error);

// 写入一条答案；同一个键重复写入时覆盖旧答案
bool cache_store(Db& db, const ExactCacheKey& key, const std::string& raw_question,
                 const std::string& answer, long long ttl_seconds, std::string& error);

// 取答案行 id，供关键词层、向量层引用；未找到返回 false
bool cache_find_id(Db& db, const ExactCacheKey& key, long long& answer_id,
                   std::string& error);
