/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         cache.cpp
 * Description:  精确缓存的实现。
 *               负责问题规范化、SHA-256 指纹、answers 表读写；
 *               答案本体只存一份，后续关键词层和向量层都通过 id 指向它。
 * ==============================================================================
 */

#include "cache.h"

#include <ctime>
#include <stdexcept>
#include <string>

#include <openssl/evp.h>

namespace {

// 把一个 Unicode 码点按 UTF-8 追加到输出串
void append_utf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// 从 i 位置读一个 UTF-8 码点并把 i 推到下一个字符。
// 遇到非法字节就当成单字节处理，保证不会越界或死循环
bool decode_utf8(const std::string& text, size_t& i, unsigned int& cp) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
        cp = lead;
        ++i;
        return true;
    }

    int extra = 0;
    unsigned int value = 0;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        value = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        value = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        value = lead & 0x07;
    } else {
        cp = lead;
        ++i;
        return false;
    }

    if (i + extra >= text.size()) {
        cp = lead;
        ++i;
        return false;
    }
    for (int k = 1; k <= extra; ++k) {
        const unsigned char next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xC0) != 0x80) {
            cp = lead;
            ++i;
            return false;
        }
        value = (value << 6) | (next & 0x3F);
    }
    cp = value;
    i += extra + 1;
    return true;
}

// 首尾可以去掉的标点。只列常见中英文标点，避免把 "C++" 这种尾巴误删
bool is_edge_punctuation(unsigned int cp) {
    switch (cp) {
        case '.': case ',': case '!': case '?': case ';': case ':':
        case '"': case '\'': case ')': case ']': case '}':
            return true;
        case 0x3002:  // 。
        case 0xFF0C:  // ，
        case 0xFF01:  // ！
        case 0xFF1F:  // ？
        case 0xFF1B:  // ；
        case 0xFF1A:  // ：
        case 0x3001:  // 、
        case 0xFF09:  // ）
        case 0x3011:  // 】
        case 0x300B:  // 》
        case 0x3009:  // 〉
        case 0x201D:  // ”
        case 0x2019:  // ’
        case 0x2026:  // …
        case 0x2014:  // —
        case 0x00B7:  // ·
            return true;
        default:
            return false;
    }
}

// 去掉字符串首尾的标点。每次都完整解码一个码点，保证中文不会被切坏
void trim_edge_punctuation(std::string& text) {
    while (!text.empty()) {
        size_t i = 0;
        unsigned int cp = 0;
        decode_utf8(text, i, cp);
        if (!is_edge_punctuation(cp)) {
            break;
        }
        text.erase(0, i);
    }

    while (!text.empty()) {
        size_t start = text.size() - 1;
        while (start > 0 &&
               (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
            --start;
        }
        size_t i = start;
        unsigned int cp = 0;
        decode_utf8(text, i, cp);
        if (!is_edge_punctuation(cp)) {
            break;
        }
        text.erase(start);
    }
}

// 把键的各个字段拼成一个不会互相混淆的字符串，再做 SHA-256
std::string exact_key_hash(const ExactCacheKey& key) {
    std::string composite;
    composite += std::to_string(key.user_id);
    composite.push_back('\x1f');
    composite += key.model;
    composite.push_back('\x1f');
    composite += key.prompt_hash;
    composite.push_back('\x1f');
    composite += key.question;
    return sha256_hex(composite);
}

}  // namespace

std::string normalize_question(const std::string& text) {
    std::string out;
    size_t i = 0;
    bool pending_space = false;

    while (i < text.size()) {
        unsigned int cp = 0;
        decode_utf8(text, i, cp);

        // 空白统一成一个空格，且不放在开头
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000) {
            pending_space = !out.empty();
            continue;
        }

        // 全角 ASCII（！？ＡＢＣ 等）转半角
        if (cp >= 0xFF01 && cp <= 0xFF5E) {
            cp -= 0xFEE0;
        }
        // 只对 ASCII 字母做小写转换
        if (cp >= 'A' && cp <= 'Z') {
            cp = cp - 'A' + 'a';
        }

        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        append_utf8(out, cp);
    }

    trim_edge_punctuation(out);
    return out;
}

std::string sha256_hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        return "";
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }
    EVP_MD_CTX_free(ctx);

    static const char* kHex = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        hex.push_back(kHex[digest[i] >> 4]);
        hex.push_back(kHex[digest[i] & 0x0F]);
    }
    return hex;
}

bool init_cache_schema(Db& db, std::string& error) {
    const std::string table_sql =
        "CREATE TABLE IF NOT EXISTS answers ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  user_id     INTEGER NOT NULL,"
        "  model       TEXT    NOT NULL,"
        "  prompt_hash TEXT    NOT NULL,"
        "  question    TEXT    NOT NULL,"
        "  answer      TEXT    NOT NULL,"
        "  created_at  INTEGER NOT NULL,"
        "  expires_at  INTEGER NOT NULL,"
        "  exact_hash  TEXT UNIQUE"
        ")";
    const std::string index_sql =
        "CREATE INDEX IF NOT EXISTS idx_answers_expires ON answers(expires_at)";

    if (db.exec(table_sql, {}, error) < 0) {
        return false;
    }
    if (db.exec(index_sql, {}, error) < 0) {
        return false;
    }

    // 启动时清一次过期缓存，避免表无限增长
    db.exec("DELETE FROM answers WHERE expires_at <= ?",
            {std::to_string(static_cast<long long>(std::time(nullptr)))}, error);
    return true;
}

bool cache_lookup(Db& db, const ExactCacheKey& key, std::string& answer,
                  std::string& error) {
    if (key.question.empty()) {
        return false;
    }

    std::vector<std::string> row;
    const bool found = db.query_row(
        "SELECT answer FROM answers WHERE exact_hash = ? AND expires_at > ?",
        {exact_key_hash(key), std::to_string(static_cast<long long>(std::time(nullptr)))},
        row, error);
    if (!found) {
        return false;
    }
    answer = row.empty() ? "" : row[0];
    return !answer.empty();
}

bool cache_store(Db& db, const ExactCacheKey& key, const std::string& raw_question,
                 const std::string& answer, long long ttl_seconds, std::string& error) {
    if (key.question.empty() || answer.empty()) {
        return false;
    }

    const long long now = static_cast<long long>(std::time(nullptr));
    const long long expires_at = now + (ttl_seconds > 0 ? ttl_seconds : 0);

    // 同一个问题被并发问两次时，后写入的覆盖先写入的，不会报唯一键冲突
    const int changes = db.exec(
        "INSERT INTO answers (user_id, model, prompt_hash, question, answer,"
        " created_at, expires_at, exact_hash) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(exact_hash) DO UPDATE SET"
        "   answer = excluded.answer,"
        "   question = excluded.question,"
        "   created_at = excluded.created_at,"
        "   expires_at = excluded.expires_at,"
        "   user_id = excluded.user_id,"
        "   model = excluded.model,"
        "   prompt_hash = excluded.prompt_hash",
        {std::to_string(key.user_id), key.model, key.prompt_hash, raw_question, answer,
         std::to_string(now), std::to_string(expires_at), exact_key_hash(key)},
        error);
    return changes >= 0;
}

bool cache_find_id(Db& db, const ExactCacheKey& key, long long& answer_id,
                   std::string& error) {
    std::vector<std::string> row;
    if (!db.query_row("SELECT id FROM answers WHERE exact_hash = ?",
                      {exact_key_hash(key)}, row, error) ||
        row.empty()) {
        return false;
    }
    try {
        answer_id = std::stoll(row[0]);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}
