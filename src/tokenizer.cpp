/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         tokenizer.cpp
 * Description:  Token 计数模块的本地近似实现。
 *               按 UTF-8 码点切分文本并计数，不依赖任何外部服务与词表，
 *               作为将来接入真实 tokenizer 服务前的占位实现。
 * ==============================================================================
 */

#include "tokenizer.h"

#include <cctype>

// 把一段 UTF-8 文本切成"计量单元"：ASCII 连续非空白算一个词，空白各自单独成单元，
// 非 ASCII 每个码点算一个单元；切分不丢弃任何字节，可原样回放
std::vector<std::string> split_utf8_units(const std::string& text) {
    std::vector<std::string> units;
    std::string ascii_word;
    size_t i = 0;

    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);

        if (c < 0x80) {
            // ASCII 字节：空白既是词的边界，也单独作为一个单元，保证回放时不丢空格
            if (std::isspace(c)) {
                if (!ascii_word.empty()) {
                    units.push_back(ascii_word);
                    ascii_word.clear();
                }
                units.push_back(std::string(1, static_cast<char>(c)));
            } else {
                ascii_word.push_back(static_cast<char>(c));
            }
            i += 1;
        } else {
            // 非 ASCII 字节：先结束当前 ASCII 词，再按首字节判断该码点占几个字节
            if (!ascii_word.empty()) {
                units.push_back(ascii_word);
                ascii_word.clear();
            }
            size_t len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
            // 容错：文本末尾若被截断成半个码点，按剩余长度切，避免越界
            if (i + len > text.size()) {
                len = text.size() - i;
            }
            units.push_back(text.substr(i, len));
            i += len;
        }
    }

    if (!ascii_word.empty()) {
        units.push_back(ascii_word);
    }
    return units;
}

// 近似估算一段文本的 token 数：以计量单元个数作为结果，纯空白单元不计入
TokenResult count_tokens(const std::string& text) {
    TokenResult result;
    result.code = 200;

    for (const std::string& unit : split_utf8_units(text)) {
        // 空白单元只为回放保真而存在，不代表一个 token，计数时跳过
        if (unit.size() == 1 && std::isspace(static_cast<unsigned char>(unit[0]))) {
            continue;
        }
        ++result.num_tokens;
    }
    return result;
}
