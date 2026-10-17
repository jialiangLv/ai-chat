/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         usage.cpp
 * Description:  用量明细的实现。
 *               一行是一次请求的 token 消耗；统计直接交给 SQL 的 SUM 聚合，
 *               不需要把数据读进内存再算。
 * ==============================================================================
 */

#include "usage.h"

#include <ctime>
#include <iostream>

// 建表与索引。索引是给「按用户 + 时间段查」准备的
bool init_usage_schema(Db& db, std::string& error) {
    const std::string table_sql =
        "CREATE TABLE IF NOT EXISTS usage_log ("
        "  id                INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  user_id           INTEGER NOT NULL,"
        "  model             TEXT    NOT NULL,"
        "  prompt_tokens     INTEGER NOT NULL,"
        "  completion_tokens INTEGER NOT NULL,"
        "  reasoning_tokens  INTEGER NOT NULL,"
        "  total_tokens      INTEGER NOT NULL,"
        "  created_at        INTEGER NOT NULL"
        ")";
    const std::string index_sql =
        "CREATE INDEX IF NOT EXISTS idx_usage_user_time ON usage_log(user_id, created_at)";

    return db.exec(table_sql, {}, error) >= 0 && db.exec(index_sql, {}, error) >= 0;
}

// 记录一次请求的用量。注意这是「尽力而为」：失败只打日志，不影响已经扣掉的额度
void record_usage(Db& db, long user_id, const std::string& model,
                  long long prompt_tokens, long long completion_tokens,
                  long long reasoning_tokens, long long total_tokens) {
    std::string error;
    const int changes = db.exec(
        "INSERT INTO usage_log (user_id, model, prompt_tokens, completion_tokens,"
        " reasoning_tokens, total_tokens, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
        {std::to_string(user_id), model, std::to_string(prompt_tokens),
         std::to_string(completion_tokens), std::to_string(reasoning_tokens),
         std::to_string(total_tokens),
         std::to_string(static_cast<long long>(std::time(nullptr)))},
        error);

    if (changes < 0) {
        std::cout << "[ai-chat] 写用量日志失败：" << error << std::endl;
    }
}

// 汇总某个用户在 since 之后的用量。
// SUM 在没有匹配行时返回 NULL，用 COALESCE 兜成 0
bool query_usage(Db& db, long user_id, long long since, UsageStats& out,
                 std::string& error) {
    std::vector<std::string> row;
    const bool found = db.query_row(
        "SELECT COUNT(*),"
        "       COALESCE(SUM(prompt_tokens), 0),"
        "       COALESCE(SUM(completion_tokens), 0),"
        "       COALESCE(SUM(total_tokens), 0) "
        "FROM usage_log WHERE user_id = ? AND created_at >= ?",
        {std::to_string(user_id), std::to_string(since)}, row, error);

    if (!found || row.size() != 4) {
        return false;
    }

    out.requests = std::stoll(row[0]);
    out.prompt_tokens = std::stoll(row[1]);
    out.completion_tokens = std::stoll(row[2]);
    out.total_tokens = std::stoll(row[3]);
    return true;
}

// 今天零点（本地时区）
long long start_of_today() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    return static_cast<long long>(std::mktime(&local));
}

// 本月一号零点（本地时区）
long long start_of_month() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    local.tm_mday = 1;
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    return static_cast<long long>(std::mktime(&local));
}
