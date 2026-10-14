/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         chat.cpp
 * Description:  聊天模块的实现。
 *               完成一次对话请求的全部处理：解析请求体、计算输入 token、
 *               再以 OpenAI 兼容的 SSE 格式逐字流式返回一段内容。
 * ==============================================================================
 */

#include "chat.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <thread>

#include "json.hpp"
#include "tokenizer.h"

namespace {

// 统一设置跨域响应头，便于将来前端与后端分开部署时直接跨域调用
void set_cors_headers(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

// 把单个 choices[0] 分片包装成完整的 SSE 数据行（含结尾空行）
// dump() 默认把非 ASCII 转义为 \uXXXX，保证 payload 是纯 ASCII 单行，SSE 分帧不会被内容中的换行打断
std::string make_sse_line(const nlohmann::json& choice) {
    nlohmann::json chunk;
    chunk["id"] = "chatcmpl-local";
    chunk["object"] = "chat.completion.chunk";
    chunk["created"] = 0;
    chunk["model"] = "deepseek-flash";
    chunk["choices"] = nlohmann::json::array({choice});
    return "data: " + chunk.dump() + "\n\n";
}

// 构造开场分片：只声明角色、内容为空，用于对齐 OpenAI 流式响应的首条 chunk
std::string make_role_line() {
    nlohmann::json choice;
    choice["index"] = 0;
    choice["delta"]["role"] = "assistant";
    choice["finish_reason"] = nullptr;
    return make_sse_line(choice);
}

// 构造一个内容分片：delta.content 就是本次要"吐"出来的那个字
std::string make_content_line(const std::string& content) {
    nlohmann::json choice;
    choice["index"] = 0;
    choice["delta"]["content"] = content;
    choice["finish_reason"] = nullptr;
    return make_sse_line(choice);
}

// 处理一次聊天请求：解析请求体 → 计算输入 token → 以 SSE 逐字流式返回
void handle_chat(const httplib::Request& req, httplib::Response& res) {
    set_cors_headers(res);

    ChatRequest chat_req = parse_chat_request(req.body);
    if (!chat_req.parse_ok) {
        res.status = 400;
        nlohmann::json err;
        err["error"] = chat_req.parse_error;
        res.set_content(err.dump(), "application/json");
        return;
    }

    // 第二步：token 计算。当前是本地近似值，只打印到日志，暂不参与业务
    TokenResult tokens = count_tokens(chat_req.user_message);
    std::cout << "[ai-chat] 收到请求 model=" << chat_req.model
              << " 输入 token 估算=" << tokens.num_tokens
              << " 内容=\"" << chat_req.user_message << "\"" << std::endl;

    // 关闭各级缓冲，否则客户端要等全部内容生成完才能看到数据
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");

    // 注意：此处不能再调用 res.set_content()，那会写入 Content-Length 使响应退化为非流式
    res.set_chunked_content_provider(
        "text/event-stream",
        [chat_req](size_t offset, httplib::DataSink& sink) -> bool {
            // offset 是本连接上已写出的字节数；非 0 说明是重复回调，本 provider 只跑一轮
            if (offset > 0) {
                return false;
            }

            std::string opening = make_role_line();
            if (!sink.write(opening.data(), opening.size())) {
                return false;
            }

            bool sent_all = generate_deltas(
                chat_req.user_message,
                [&sink](const std::string& unit) -> bool {
                    std::string chunk = make_content_line(unit);
                    if (!sink.write(chunk.data(), chunk.size())) {
                        return false;
                    }
                    if (!sink.is_writable()) {
                        return false;  // 客户端已关闭连接，不必再空转
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 打字机节奏
                    return true;
                });

            if (!sent_all) {
                return false;  // 客户端中途断开，本轮响应到此为止
            }

            std::string done = "data: [DONE]\n\n";
            sink.write(done.data(), done.size());
            sink.done();  // 必须调用：写出 chunked 结束块，否则客户端会一直等待
            return true;
        });
}

}  // namespace

// 从 HTTP 请求体解析出 ChatRequest，取 messages 中最后一条 role=user 的 content 作为本轮输入
ChatRequest parse_chat_request(const std::string& body) {
    ChatRequest req;
    try {
        nlohmann::json body_json = nlohmann::json::parse(body);

        if (body_json.contains("model") && body_json["model"].is_string()) {
            req.model = body_json["model"].get<std::string>();
        }
        if (body_json.contains("stream") && body_json["stream"].is_boolean()) {
            req.stream = body_json["stream"].get<bool>();
        }

        if (body_json.contains("messages") && body_json["messages"].is_array()) {
            for (const auto& message : body_json["messages"]) {
                if (message.contains("role") && message["role"] == "user" &&
                    message.contains("content") && message["content"].is_string()) {
                    req.user_message = message["content"].get<std::string>();
                }
            }
        }

        if (req.user_message.empty()) {
            req.parse_error = "messages 中缺少 role=user 的 content";
            return req;
        }
        req.parse_ok = true;
    } catch (const std::exception& e) {
        req.parse_error = std::string("请求体不是合法 JSON：") + e.what();
    }
    return req;
}

// 逐字产出回复内容：当前返回服务端写死的假文字，后续替换为真实模型的流式调用
bool generate_deltas(const std::string& prompt, const DeltaSink& on_delta) {
    std::string reply =
        "你好，我是 ai-chat 的最小演示版。"
        "你刚才说的是：「" + prompt + "」。"
        "目前我还不联网、不调用任何模型，这段文字是服务端写死的，"
        "只用来验证「接收请求 → 解析请求 → 计算 token → 流式返回」这条链路是通的。";

    // 按 UTF-8 码点逐字吐出，避免把一个汉字拆成三个字节导致乱码
    for (const std::string& unit : split_utf8_units(reply)) {
        if (!on_delta(unit)) {
            return false;
        }
    }
    return true;
}

// 在 HTTP 服务上注册聊天相关路由
void register_chat_routes(httplib::Server& server) {
    // 跨域预检：前端与后端分开部署时浏览器会先发 OPTIONS，同源托管时用不到
    server.Options("/v1/chat/completions",
                   [](const httplib::Request&, httplib::Response& res) {
                       set_cors_headers(res);
                       res.status = 204;
                   });

    server.Post("/v1/chat/completions", handle_chat);
}
