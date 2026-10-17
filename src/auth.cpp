/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         auth.cpp
 * Description:  用户与会话模块的实现。
 *               密码哈希用 OpenSSL 的 PBKDF2-HMAC-SHA256，每个用户独立随机盐；
 *               会话令牌是 32 字节随机数，服务端只存令牌本身，可随时删除撤销。
 * ==============================================================================
 */

#include "auth.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstdlib>
#include <ctime>
#include <vector>

namespace {

constexpr int kIterations = 120000;  // PBKDF2 迭代次数
constexpr int kSaltBytes = 16;
constexpr int kHashBytes = 32;
constexpr int kTokenBytes = 32;

// 二进制转十六进制
std::string to_hex(const std::vector<unsigned char>& data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char byte : data) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

// 十六进制转二进制；格式不对返回 false
bool from_hex(const std::string& hex, std::vector<unsigned char>& out) {
    if (hex.size() % 2 != 0) {
        return false;
    }
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = value(hex[i]);
        int lo = value(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return true;
}

// 恒定时间比较，避免靠响应耗时逐字节猜出
bool secure_equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

// 用 PBKDF2-HMAC-SHA256 派生密钥
bool pbkdf2(const std::string& password, const std::vector<unsigned char>& salt,
            int iterations, std::vector<unsigned char>& out) {
    out.resize(kHashBytes);
    return PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                             salt.data(), static_cast<int>(salt.size()), iterations,
                             EVP_sha256(), kHashBytes, out.data()) == 1;
}

// 生成随机字节
bool random_bytes(size_t count, std::vector<unsigned char>& out) {
    out.resize(count);
    return RAND_bytes(out.data(), static_cast<int>(count)) == 1;
}

// 密码存储格式：pbkdf2$迭代次数$盐(hex)$哈希(hex)
std::string hash_password(const std::string& password) {
    std::vector<unsigned char> salt;
    std::vector<unsigned char> hash;
    if (!random_bytes(kSaltBytes, salt) || !pbkdf2(password, salt, kIterations, hash)) {
        return "";
    }
    return "pbkdf2$" + std::to_string(kIterations) + "$" + to_hex(salt) + "$" + to_hex(hash);
}

// 校验密码是否与存储的哈希匹配
bool verify_password(const std::string& password, const std::string& stored) {
    const size_t p1 = stored.find('$');
    const size_t p2 = (p1 == std::string::npos) ? std::string::npos : stored.find('$', p1 + 1);
    const size_t p3 = (p2 == std::string::npos) ? std::string::npos : stored.find('$', p2 + 1);
    if (p3 == std::string::npos || stored.substr(0, p1) != "pbkdf2") {
        return false;
    }

    const int iterations = std::atoi(stored.substr(p1 + 1, p2 - p1 - 1).c_str());
    if (iterations <= 0) {
        return false;
    }

    std::vector<unsigned char> salt;
    if (!from_hex(stored.substr(p2 + 1, p3 - p2 - 1), salt)) {
        return false;
    }

    std::vector<unsigned char> derived;
    if (!pbkdf2(password, salt, iterations, derived)) {
        return false;
    }
    return secure_equals(to_hex(derived), stored.substr(p3 + 1));
}

// 生成随机会话令牌（不全等 unlikely）
std::string make_session_token() {
    std::vector<unsigned char> bytes;
    if (!random_bytes(kTokenBytes, bytes)) {
        return "";
    }
    return to_hex(bytes);
}

}  // namespace

// 恒定时间比较，避免靠响应耗时逐字节猜出秘密
bool secure_compare(const std::string& a, const std::string& b) {
    return secure_equals(a, b);
}

// 建表。注意 SQLite 的 prepare 只执行第一条语句，所以两张表要分开执行
bool init_schema(Db& db, std::string& error) {
    const std::string users_sql =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT NOT NULL UNIQUE,"
        "  password_hash TEXT NOT NULL,"
        "  quota INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ")";
    const std::string sessions_sql =
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  token TEXT PRIMARY KEY,"
        "  user_id INTEGER NOT NULL REFERENCES users(id),"
        "  expires_at INTEGER NOT NULL"
        ")";
    // 给「删除过期会话」准备的索引
    const std::string sessions_index_sql =
        "CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at)";

    return db.exec(users_sql, {}, error) >= 0 &&
           db.exec(sessions_sql, {}, error) >= 0 &&
           db.exec(sessions_index_sql, {}, error) >= 0;
}

// 创建用户
bool create_user(Db& db, const std::string& username, const std::string& password,
                 long long initial_quota, long& user_id, std::string& error) {
    const std::string hash = hash_password(password);
    if (hash.empty()) {
        error = "密码哈希失败";
        return false;
    }

    const int changes = db.exec(
        "INSERT INTO users (username, password_hash, quota, created_at) VALUES (?, ?, ?, ?)",
        {username, hash, std::to_string(initial_quota),
         std::to_string(static_cast<long long>(std::time(nullptr)))},
        error);
    if (changes < 0) {
        if (error.find("UNIQUE") != std::string::npos) {
            error = "用户名已存在";
        }
        return false;
    }

    std::vector<std::string> row;
    if (!db.query_row("SELECT id FROM users WHERE username = ?", {username}, row, error) ||
        row.empty()) {
        return false;
    }
    user_id = std::stol(row[0]);
    return true;
}

// 校验用户名与密码
bool check_credentials(Db& db, const std::string& username, const std::string& password,
                       long& user_id, std::string& error) {
    std::vector<std::string> row;
    if (!db.query_row("SELECT id, password_hash FROM users WHERE username = ?", {username},
                      row, error) ||
        row.size() != 2) {
        return false;  // 用户不存在（调用方对外统一回"用户名或密码错误"）
    }
    if (!verify_password(password, row[1])) {
        return false;
    }
    user_id = std::stol(row[0]);
    return true;
}

// 创建会话
bool create_session(Db& db, long user_id, long ttl_seconds, std::string& token,
                    std::string& error) {
    // 顺手清掉已过期的会话。放在登录时做，不需要后台线程：
    // 过期行只在「有新登录」时才可能堆积，表因此自我平衡。
    // 清理失败不影响登录，所以错误单独用一个变量接住
    std::string ignored;
    db.exec("DELETE FROM sessions WHERE expires_at < ?",
            {std::to_string(static_cast<long long>(std::time(nullptr)))}, ignored);

    token = make_session_token();
    if (token.empty()) {
        error = "生成令牌失败";
        return false;
    }
    const long long expires_at =
        static_cast<long long>(std::time(nullptr)) + ttl_seconds;
    return db.exec("INSERT INTO sessions (token, user_id, expires_at) VALUES (?, ?, ?)",
                   {token, std::to_string(user_id), std::to_string(expires_at)},
                   error) >= 0;
}

// 按令牌查身份
AuthUser authenticate(Db& db, const std::string& token) {
    AuthUser user;
    if (token.empty()) {
        return user;
    }

    std::vector<std::string> row;
    std::string error;
    const long long now = static_cast<long long>(std::time(nullptr));
    const bool found = db.query_row(
        "SELECT u.id, u.username FROM sessions s JOIN users u ON u.id = s.user_id "
        "WHERE s.token = ? AND s.expires_at > ?",
        {token, std::to_string(now)}, row, error);

    if (found && row.size() == 2) {
        user.ok = true;
        user.id = std::stol(row[0]);
        user.username = row[1];
    }
    return user;
}

// 查询余额
long long get_quota(Db& db, long user_id, std::string& error) {
    std::vector<std::string> row;
    if (!db.query_row("SELECT quota FROM users WHERE id = ?", {std::to_string(user_id)},
                      row, error) ||
        row.empty()) {
        return -1;
    }
    return std::stoll(row[0]);
}

// 原子扣减：只有余额足够时 WHERE 才成立，并发下不会出现双花
bool deduct_quota(Db& db, long user_id, long long amount, std::string& error) {
    if (amount <= 0) {
        return true;
    }
    const int changes = db.exec(
        "UPDATE users SET quota = quota - ? WHERE id = ? AND quota >= ?",
        {std::to_string(amount), std::to_string(user_id), std::to_string(amount)}, error);
    return changes > 0;
}
