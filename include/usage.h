/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         usage.h
 * Description:  用量明细的对外接口。
 *               每次请求结束后把真实 token 用量记一行，统计用 SQL 直接聚合。
 *               记录是尽力而为的：写失败不影响已经扣掉的额度，也不影响请求本身。
 * ==============================================================================
 */

#pragma once

#include <string>

#include "db.h"

// 一段时间的用量汇总
struct UsageStats {
    long long requests = 0;
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
    long long total_tokens = 0;
};

// 建表与索引（幂等）
bool init_usage_schema(Db& db, std::string& error);

// 记录一次请求的真实用量。失败只打印日志，不向上抛
void record_usage(Db& db, long user_id, const std::string& model,
                  long long prompt_tokens, long long completion_tokens,
                  long long reasoning_tokens, long long total_tokens);

// 汇总某个用户在 since 之后的用量
bool query_usage(Db& db, long user_id, long long since, UsageStats& out,
                 std::string& error);

// 今天零点（本地时区）的时间戳
long long start_of_today();

// 本月一号零点（本地时区）的时间戳
long long start_of_month();
