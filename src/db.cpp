/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         db.cpp
 * Description:  SQLite 封装的实现。
 *               负责把参数绑定到预编译语句、执行、取回结果；并发由一把互斥锁保证。
 * ==============================================================================
 */

#include "db.h"

#include <sqlite3.h>

namespace {

// 把参数依次绑定到预编译语句的第 1..n 个占位符。
// SQLITE_TRANSIENT 让 SQLite 自己拷贝一份，调用方的字符串可以立刻释放
void bind_params(sqlite3_stmt* stmt, const std::vector<std::string>& params) {
    for (size_t i = 0; i < params.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1,
                          SQLITE_TRANSIENT);
    }
}

}  // namespace

Db::~Db() {
    if (db_) {
        sqlite3_close(db_);
    }
}

// 打开数据库文件
bool Db::open(const std::string& path, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        error = db_ ? sqlite3_errmsg(db_) : "无法打开数据库";
        return false;
    }
    // 并发写入时先等一会儿，而不是立刻返回 SQLITE_BUSY
    sqlite3_busy_timeout(db_, 3000);
    return true;
}

// 执行语句并返回受影响行数（用于判断 UPDATE 是否真的改到了行）
int Db::exec(const std::string& sql, const std::vector<std::string>& params,
             std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db_);
        return -1;
    }

    bind_params(stmt, params);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return -1;
    }

    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes;
}

// 查询单行；列值统一按文本取回
bool Db::query_row(const std::string& sql, const std::vector<std::string>& params,
                   std::vector<std::string>& row, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db_);
        return false;
    }

    bind_params(stmt, params);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        row.clear();
        int columns = sqlite3_column_count(stmt);
        for (int i = 0; i < columns; ++i) {
            const unsigned char* text = sqlite3_column_text(stmt, i);
            row.push_back(text ? reinterpret_cast<const char*>(text) : "");
        }
        sqlite3_finalize(stmt);
        return true;
    }

    if (rc != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
    }
    sqlite3_finalize(stmt);
    return false;
}
