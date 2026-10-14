/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         chat.h
 * Description:  聊天模块的对外接口。
 *               定义一次对话请求的结构体，以及解析、内容生成、路由注册
 *               三类入口；其中内容生成是从"写死假文字"切换到"真实模型
 *               流式调用"的替换点。
 * ==============================================================================
 */

#pragma once

#include <functional>
#include <string>

#include "httplib.h"

// 一次对话请求的最小字段，字段名对齐 OpenAI /v1/chat/completions 的子集
struct ChatRequest {
    std::string model = "deepseek-flash";  // 请求指定的模型名
    std::string user_message;              // messages 中最后一条 role=user 的 content
    bool        stream = true;             // 是否流式返回
    bool        parse_ok = false;          // 解析是否成功
    std::string parse_error;               // 解析失败时的原因，直接回给客户端
};

// 从 HTTP 请求体（JSON 字符串）解析出 ChatRequest
ChatRequest parse_chat_request(const std::string& body);

// 内容生成的回调类型：收到一个"字"返回 true 表示继续，返回 false 表示客户端已断开
using DeltaSink = std::function<bool(const std::string&)>;

// 逐字产出回复内容：当前是服务端写死的假文字，后续替换为真实模型的流式调用
bool generate_deltas(const std::string& prompt, const DeltaSink& on_delta);

// 在 HTTP 服务上注册聊天相关路由
void register_chat_routes(httplib::Server& server);
