/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         db.h
 * Description:  SQLite 的极简封装。
 *               一个连接 + 一把互斥锁。这个规模不需要连接池；一把锁既保证并发
 *               安全，开销也完全够用。所有 SQL 都走参数绑定，不拼字符串。
 * ==============================================================================
 */

#pragma once

#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

class Db {
public:
    ~Db();

    // 打开数据库文件，不存在则创建
    bool open(const std::string& path, std::string& error);

    // 执行不带结果集的语句，返回受影响行数；失败返回 -1
    int exec(const std::string& sql, const std::vector<std::string>& params,
             std::string& error);

    // 查询单行；没有匹配行时返回 false 且 error 为空
    bool query_row(const std::string& sql, const std::vector<std::string>& params,
                   std::vector<std::string>& row, std::string& error);

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};
