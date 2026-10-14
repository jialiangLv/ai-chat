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

// 流式内容的类型：推理模型会先输出一段思考过程，再输出正式回答
enum class DeltaKind {
    Reasoning,  // 思考过程，对应上游的 reasoning_content 字段
    Content     // 正式回答，对应上游的 content 字段
};

// 内容生成的回调类型：收到一个"字"返回 true 表示继续，返回 false 表示客户端已断开
using DeltaSink = std::function<bool(DeltaKind kind, const std::string& text)>;

// 一次对话的 token 用量，取自上游响应最后一帧的 usage 字段
// 这是服务端统计出的真实值，比本地估算准确
struct Usage {
    bool valid = false;         // 上游是否返回了用量信息
    int  prompt_tokens = 0;     // 输入消耗
    int  completion_tokens = 0; // 输出消耗
    int  total_tokens = 0;      // 合计
    int  reasoning_tokens = 0;  // 输出中属于思考过程的部分
};

// 调用大模型接口，把流式返回的内容逐字回调出去，用量信息写入 usage
bool generate_deltas(const std::string& prompt, const DeltaSink& on_delta, Usage& usage);

// 在 HTTP 服务上注册聊天相关路由
void register_chat_routes(httplib::Server& server);
