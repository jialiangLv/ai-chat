/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         tokenizer.h
 * Description:  Token 计数模块的对外接口。
 *               当前是本地近似实现，返回值形状对齐远程 tokenizer 服务的
 *               响应体（{code, num_tokens, msg}），因此后续整体替换为
 *               远程调用时，调用方代码无需改动。
 * ==============================================================================
 */

#pragma once

#include <string>
#include <vector>

// 一次 token 统计的结果，字段对齐远程接口 /tokenizer/<model> 的响应体
struct TokenResult {
    int         code       = 200;  // 200 表示成功，非 200 表示失败（远程调用时会用到）
    int         num_tokens = 0;    // 统计出的 token 数
    std::string msg;               // 失败时的原因说明
};

// 把一段 UTF-8 文本切成"计量单元"：
// ASCII 中连续的非空白字符算一个词，空白字符各自单独成一个单元，
// 非 ASCII 按 UTF-8 首字节确定长度、每个码点算一个单元。
// 切分过程不丢弃任何字节，因此同一份结果既能用于计数，也能按单元逐字回放原文；
// 聊天模块正是复用它来"一个字一个字地吐"，避免 UTF-8 处理逻辑出现两份
std::vector<std::string> split_utf8_units(const std::string& text);

// 近似估算一段文本的 token 数，当前实现直接以计量单元个数作为结果
TokenResult count_tokens(const std::string& text);
