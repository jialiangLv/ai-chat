/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         auth.h
 * Description:  用户与会话的对外接口。
 *               密码用 PBKDF2-HMAC-SHA256 + 随机盐存储（绝不存明文）；
 *               登录成功后发放随机不透明令牌，服务端查库校验、可单独撤销。
 * ==============================================================================
 */

#pragma once

#include <string>

#include "db.h"

// 一次请求解析出的身份
struct AuthUser {
    bool        ok = false;
    long        id = 0;
    std::string username;
};

// 建表（幂等）
bool init_schema(Db& db, std::string& error);

// 恒定时间比较，用于比对邀请码这类秘密
bool secure_compare(const std::string& a, const std::string& b);

// 创建用户；用户名重复时返回 false
bool create_user(Db& db, const std::string& username, const std::string& password,
                 long long initial_quota, long& user_id, std::string& error);

// 校验用户名与密码；任何一项不对都返回 false
bool check_credentials(Db& db, const std::string& username, const std::string& password,
                       long& user_id, std::string& error);

// 为用户创建会话令牌
bool create_session(Db& db, long user_id, long ttl_seconds, std::string& token,
                    std::string& error);

// 按令牌查身份；令牌无效或过期时 ok = false
AuthUser authenticate(Db& db, const std::string& token);

// 查询剩余额度；查询失败返回 -1
long long get_quota(Db& db, long user_id, std::string& error);

// 原子扣减额度；余额不足时返回 false 且不改动任何行
bool deduct_quota(Db& db, long user_id, long long amount, std::string& error);
