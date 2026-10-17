/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         tokenizer.h
 * Description:  Token 计数模块的对外接口。
 *               基于实测数据拟合的估算模型：把文本拆成几类特征（汉字、英文词、
 *               词内额外字母、数字、符号）按各自系数加权，再加上固定模板开销。
 *               实测平均误差约 1 token，远优于按"一个汉字/一个单词算一个"的做法。
 * ==============================================================================
 */

#pragma once

#include <string>
#include <vector>

// 一次 token 统计的结果，字段对齐远程接口 /tokenizer/<model> 的响应体
struct TokenResult {
    int         code       = 200;  // 200 表示成功
    int         num_tokens = 0;    // 估算出的 token 数
    std::string msg;               // 失败时的原因说明
};

// 估算文本内容本身的 token 数，**不含**固定模板开销
double content_tokens(const std::string& text);

// 估算一条消息的 token 数，含固定模板开销（系统提示 + 该条消息的模板）
TokenResult count_tokens(const std::string& text);

// 估算整段对话的 prompt token 数：固定开销 + 各条内容 + 每多一条消息的角色标记
TokenResult estimate_conversation(const std::vector<std::string>& contents);
