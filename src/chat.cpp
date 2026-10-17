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

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "ac_automaton.h"
#include "json.hpp"
#include "tokenizer.h"

namespace {

// 读取环境变量，未设置或为空时返回默认值
std::string get_env(const char* key, const std::string& default_val = "") {
    const char* val = std::getenv(key);
    return (val && *val) ? std::string(val) : default_val;
}

// 恒定时间比较：不因首个不同的字节就提前返回，避免通过响应耗时逐字节猜出令牌
// 长度不同直接返回 false——长度本身不算秘密
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

// 从 Authorization: Bearer <token> 里取出令牌并与期望值比对
bool token_matches(const httplib::Request& req, const std::string& expected) {
    if (!req.has_header("Authorization")) {
        return false;
    }
    const std::string value = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    if (value.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return secure_equals(value.substr(prefix.size()), expected);
}

// 敏感词拦截时回给前端的标记响应头，前端据此决定这一轮要不要计入上下文
constexpr const char* kBlockedHeader = "X-Ai-Chat-Blocked";

// 最多携带的消息条数（一问一答算两条），超出就丢掉最早的部分
constexpr size_t kMaxHistoryMessages = 20;

// 历史过长时只保留最近若干条，并保证数组以 user 消息开头——否则开头的 assistant 会成为孤儿
void trim_messages(std::vector<Message>& messages) {
    if (messages.size() <= kMaxHistoryMessages) {
        return;
    }
    messages.erase(messages.begin(), messages.end() - kMaxHistoryMessages);

    // 截断点可能落在一条问答的中间，把开头的 assistant 一并推掉
    size_t drop = 0;
    while (drop < messages.size() && messages[drop].role != "user") {
        ++drop;
    }
    if (drop > 0 && drop < messages.size()) {
        messages.erase(messages.begin(), messages.begin() + drop);
    }
}

// 统一设置跨域响应头，便于将来前端与后端分开部署时直接跨域调用
void set_cors_headers(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    // 自定义响应头默认不暴露给跨域的前端，要显式列出来，否则前端读不到它
    res.set_header("Access-Control-Expose-Headers", kBlockedHeader);
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

// 构造一个流式分片：思考过程写进 reasoning_content，正式回答写进 content
// 字段名与上游接口保持一致，前端据此把两类内容分别渲染
std::string make_delta_line(DeltaKind kind, const std::string& text) {
    nlohmann::json choice;
    choice["index"] = 0;
    choice["delta"][kind == DeltaKind::Reasoning ? "reasoning_content" : "content"] = text;
    choice["finish_reason"] = nullptr;
    return make_sse_line(choice);
}

// 构造只带用量信息的一条分片，choices 为空数组（对齐 OpenAI stream_options 的约定）
std::string make_usage_line(const Usage& usage) {
    nlohmann::json details;
    details["reasoning_tokens"] = usage.reasoning_tokens;

    nlohmann::json usage_json;
    usage_json["prompt_tokens"] = usage.prompt_tokens;
    usage_json["completion_tokens"] = usage.completion_tokens;
    usage_json["total_tokens"] = usage.total_tokens;
    usage_json["completion_tokens_details"] = details;

    nlohmann::json chunk;
    chunk["id"] = "chatcmpl-local";
    chunk["object"] = "chat.completion.chunk";
    chunk["created"] = 0;
    chunk["model"] = "deepseek-flash";
    chunk["choices"] = nlohmann::json::array();
    chunk["usage"] = usage_json;
    return "data: " + chunk.dump() + "\n\n";
}

// 用 SSE 格式返回一段固定的文本，供不需要调用模型的分支复用（如敏感词拦截）
// 带上标记响应头：这一轮模型并没有参与，前端不该把它计入发给模型的上下文
void stream_fixed_text(httplib::Response& res, const std::string& text) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_header(kBlockedHeader, "1");

    res.set_chunked_content_provider(
        "text/event-stream",
        [text](size_t offset, httplib::DataSink& sink) -> bool {
            if (offset > 0) {
                return false;
            }
            std::string opening = make_role_line();
            if (!sink.write(opening.data(), opening.size())) {
                return false;
            }
            std::string chunk = make_delta_line(DeltaKind::Content, text);
            if (!sink.write(chunk.data(), chunk.size())) {
                return false;
            }
            std::string done = "data: [DONE]\n\n";
            sink.write(done.data(), done.size());
            sink.done();
            return true;
        });
}

// 处理一次聊天请求：解析请求体 → 敏感词检查 → 计算输入 token → 以 SSE 逐字流式返回
void handle_chat(const httplib::Request& req, httplib::Response& res,
                 const AcAutomaton& sensitive) {
    set_cors_headers(res);

    ChatRequest chat_req = parse_chat_request(req.body);
    if (!chat_req.parse_ok) {
        res.status = 400;
        nlohmann::json err;
        err["error"] = chat_req.parse_error;
        res.set_content(err.dump(), "application/json");
        return;
    }

    // 历史过长就截断，避免请求体随对话无限增长
    trim_messages(chat_req.messages);

    // 第一步：敏感词检查。历史是客户端给的、不可信，所以要全量复查；
    // 但两类消息的处置不同——不能因为陈年旧事把新对话锁死
    const std::string kFiltered = "[该内容已被过滤]";
    for (size_t i = 0; i < chat_req.messages.size(); ++i) {
        Message& msg = chat_req.messages[i];
        if (msg.role != "user") {
            continue;
        }

        std::vector<std::string> hits = sensitive.find_all(msg.content);
        if (hits.empty()) {
            continue;
        }

        // 命中的词只记在服务端日志里，不回给客户端——否则等于告诉对方该绕开哪个词
        std::cout << "[ai-chat] 命中敏感词：";
        for (size_t k = 0; k < hits.size(); ++k) {
            std::cout << (k ? "、" : "") << hits[k];
        }

        // 本轮输入命中（最后一条必定是本轮的 user 消息）→ 拒绝整个请求，改一句重发即可
        if (i + 1 == chat_req.messages.size()) {
            std::cout << "（本轮输入 " << hits.size() << " 处）→ 已拒绝" << std::endl;
            stream_fixed_text(res, "触发到了知识盲区，请换个问题再问");
            return;
        }

        // 历史消息命中 → 就地替换成占位符，本轮照常进行
        std::cout << "（历史第 " << i << " 条）→ 已清洗" << std::endl;
        msg.content = kFiltered;
    }

    // 第二步：token 计算。统计本轮输入，当前是本地近似值，只打印到日志
    TokenResult tokens = count_tokens(chat_req.messages.back().content);
    std::cout << "[ai-chat] 收到请求 model=" << chat_req.model
              << " 本轮输入 token 估算=" << tokens.num_tokens
              << "，共 " << chat_req.messages.size() << " 条消息" << std::endl;

    // 关闭各级缓冲，否则客户端要等全部内容生成完才能看到数据
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");

    // 注意：此处不能再调用 res.set_content()，那会写入 Content-Length 使响应退化为非流式
    res.set_chunked_content_provider(
        "text/event-stream",
        [chat_req, estimated_input = tokens.num_tokens](size_t offset,
                                                        httplib::DataSink& sink) -> bool {
            // offset 是本连接上已写出的字节数；非 0 说明是重复回调，本 provider 只跑一轮
            if (offset > 0) {
                return false;
            }

            std::string opening = make_role_line();
            if (!sink.write(opening.data(), opening.size())) {
                return false;
            }

            // 不再需要自己控制节奏：上游模型边生成边吐，天然就是流式的打字机效果
            Usage usage;
            bool sent_all = generate_deltas(
                chat_req.messages,
                [&sink](DeltaKind kind, const std::string& text) -> bool {
                    std::string chunk = make_delta_line(kind, text);
                    if (!sink.write(chunk.data(), chunk.size())) {
                        return false;
                    }
                    if (!sink.is_writable()) {
                        return false;  // 客户端已关闭连接，不必再空转
                    }
                    return true;
                },
                usage);

            if (!sent_all) {
                return false;  // 客户端中途断开，本轮响应到此为止
            }

            // 上游统计的真实用量，作为额外一帧转发给前端
            if (usage.valid) {
                std::string usage_line = make_usage_line(usage);
                sink.write(usage_line.data(), usage_line.size());
                std::cout << "[ai-chat] 上游真实用量 输入=" << usage.prompt_tokens
                          << " 输出=" << usage.completion_tokens
                          << "（其中思考 " << usage.reasoning_tokens << "）"
                          << " 合计=" << usage.total_tokens
                          << " ｜本地估算输入=" << estimated_input << std::endl;
            }

            std::string done = "data: [DONE]\n\n";
            sink.write(done.data(), done.size());
            sink.done();  // 必须调用：写出 chunked 结束块，否则客户端会一直等待
            return true;
        });
}

}  // namespace

// 从 HTTP 请求体解析出 ChatRequest，messages 数组即完整对话历史，最后一条是用户本轮输入
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
            for (const auto& item : body_json["messages"]) {
                // 结构不完整的消息直接跳过，不让它影响整条请求
                if (!item.is_object() || !item.contains("role") || !item["role"].is_string() ||
                    !item.contains("content") || !item["content"].is_string()) {
                    continue;
                }
                Message message;
                message.role = item["role"].get<std::string>();
                message.content = item["content"].get<std::string>();
                req.messages.push_back(std::move(message));
            }
        }

        if (req.messages.empty()) {
            req.parse_error = "messages 为空或格式不正确";
            return req;
        }
        // 最后一条必须是用户消息，它代表本轮输入
        if (req.messages.back().role != "user") {
            req.parse_error = "messages 的最后一条必须是 role=user 的消息";
            return req;
        }
        req.parse_ok = true;
    } catch (const std::exception& e) {
        req.parse_error = std::string("请求体不是合法 JSON：") + e.what();
    }
    return req;
}

// 调用大模型接口，把整段对话发过去并把流式返回的内容逐字回调出去，用量信息写入 usage
bool generate_deltas(const std::vector<Message>& messages, const DeltaSink& on_delta,
                     Usage& usage) {
    std::string api_key = get_env("DEEPSEEK_API_KEY");
    if (api_key.empty()) {
        on_delta(DeltaKind::Content,
                 "[配置缺失] 没有读到 DEEPSEEK_API_KEY，请检查项目根目录下的 .env");
        return true;
    }

    std::string model = get_env("DEEPSEEK_MODEL", "deepseek-flash");
    std::string base_url = get_env("DEEPSEEK_BASE_URL", "https://api.deepseek.com/v1");

    // 把 base_url 拆成「scheme://主机」和「路径前缀」两段，
    // 因为 httplib::Client 的构造函数只接受主机部分：
    //   https://api.deepseek.com/v1  →  https://api.deepseek.com  +  /v1
    std::string host = base_url;
    std::string path_prefix;
    size_t scheme_end = base_url.find("://");
    size_t path_begin = (scheme_end == std::string::npos)
                            ? std::string::npos
                            : base_url.find('/', scheme_end + 3);
    if (path_begin != std::string::npos) {
        host = base_url.substr(0, path_begin);
        path_prefix = base_url.substr(path_begin);
    }

    httplib::Client client(host);
    client.set_connection_timeout(10, 0);  // 连不上就尽快失败，别让用户干等
    client.set_read_timeout(60, 0);        // 默认 300 秒过于宽松，收紧一些

    // 把整段对话原样组装成请求体的 messages 数组
    nlohmann::json message_array = nlohmann::json::array();
    for (const Message& message : messages) {
        nlohmann::json item;
        item["role"] = message.role;
        item["content"] = message.content;
        message_array.push_back(std::move(item));
    }

    nlohmann::json body;
    body["model"] = model;
    body["stream"] = true;
    body["messages"] = std::move(message_array);

    // 上游一个数据块不等于完整的一行，所以要跨回调累积，只处理已经切完整的行
    std::string buffer;
    // 留一份最前面的原始字节，用于上游返回非 200 时给出可读的错误信息
    std::string raw_head;

    auto res = client.Post(
        path_prefix + "/chat/completions",
        {{"Authorization", "Bearer " + api_key}},
        body.dump(), "application/json",
        [&](const char* data, size_t length) -> bool {
            if (raw_head.size() < 512) {
                raw_head.append(data, std::min(length, 512 - raw_head.size()));
            }
            buffer.append(data, length);

            size_t newline;
            while ((newline = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                // SSE 规范允许 \r\n 行尾，去掉可能残留的 \r
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.compare(0, 6, "data: ") != 0) {
                    continue;  // 帧之间的空行等非数据行，直接跳过
                }

                std::string payload = line.substr(6);
                if (payload == "[DONE]") {
                    return true;
                }

                try {
                    auto frame = nlohmann::json::parse(payload);

                    // 用量信息出现在整条流的最后一帧，逐字段取值并记下来
                    if (frame.contains("usage") && frame["usage"].is_object()) {
                        const auto& u = frame["usage"];
                        usage.valid = true;
                        usage.prompt_tokens = u.value("prompt_tokens", 0);
                        usage.completion_tokens = u.value("completion_tokens", 0);
                        usage.total_tokens = u.value("total_tokens", 0);
                        if (u.contains("completion_tokens_details") &&
                            u["completion_tokens_details"].is_object()) {
                            usage.reasoning_tokens =
                                u["completion_tokens_details"].value("reasoning_tokens", 0);
                        }
                    }

                    // 内容帧：先确认 choices 非空再取，避免下标越界
                    if (!frame.contains("choices") || !frame["choices"].is_array() ||
                        frame["choices"].empty()) {
                        continue;
                    }
                    auto delta = frame["choices"][0]["delta"];

                    // reasoning_content 与 content 两个字段每帧都存在，但可能为 null；
                    // 必须先判断类型再取值，否则 get<std::string>() 会抛异常
                    for (const std::string& field : {"reasoning_content", "content"}) {
                        if (!delta.contains(field) || !delta[field].is_string()) {
                            continue;
                        }
                        std::string text = delta[field].get<std::string>();
                        if (text.empty()) {
                            continue;
                        }
                        DeltaKind kind = (field == "reasoning_content")
                                             ? DeltaKind::Reasoning
                                             : DeltaKind::Content;
                        if (!on_delta(kind, text)) {
                            return false;  // 客户端已断开，顺带中断上游请求
                        }
                    }
                } catch (const std::exception&) {
                    // 解析不了的帧跳过即可，不影响后续内容
                }
            }
            return true;
        });

    if (!res) {
        on_delta(DeltaKind::Content,
                 "[连接失败] " + httplib::to_string(res.error()));
        return true;
    }
    if (res->status != 200) {
        on_delta(DeltaKind::Content,
                 "[上游错误] HTTP " + std::to_string(res->status) + "：" + raw_head);
    }
    return true;
}

// 在 HTTP 服务上注册聊天相关路由
void register_chat_routes(httplib::Server& server) {
    // 访问令牌启动时读一次；main() 已保证它非空（fail-closed）
    const std::string token = get_env("AI_CHAT_TOKEN");

    // 鉴权中间件：在路由分发之前执行，一个地方管住所有接口。
    // 只守 /v1/ 前缀——原因见下面两行注释
    server.set_pre_routing_handler(
        [token](const httplib::Request& req, httplib::Response& res) {
            const bool is_api = req.path.rfind("/v1/", 0) == 0;

            // 静态文件必须放行：页面本身不是秘密，若连页面都要令牌，
            // 用户连输入令牌的界面都看不到
            // OPTIONS 预检必须放行：浏览器按规范不会在预检里带 Authorization
            if (!is_api || req.method == "OPTIONS") {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            if (token_matches(req, token)) {
                return httplib::Server::HandlerResponse::Unhandled;
            }

            set_cors_headers(res);
            res.status = 401;
            res.set_header("WWW-Authenticate", "Bearer");
            nlohmann::json err;
            err["error"] = "未授权：缺少或错误的访问令牌";
            res.set_content(err.dump(), "application/json");
            return httplib::Server::HandlerResponse::Handled;  // 已应答，跳过路由
        });

    // 敏感词表在启动时加载并构建一次，之后所有请求共用同一份自动机
    auto sensitive = std::make_shared<AcAutomaton>();
    std::string dict_path = get_env("SENSITIVE_DICT", "dict/sensitive.txt");
    size_t word_count = sensitive->load_file(dict_path);
    if (word_count == 0) {
        std::cerr << "[ai-chat] 警告：敏感词表为空或读取失败：" << dict_path
                  << "，敏感词检查将形同虚设" << std::endl;
    } else {
        std::cout << "[ai-chat] 敏感词表已加载 " << word_count << " 条：" << dict_path
                  << std::endl;
    }

    // 跨域预检：前端与后端分开部署时浏览器会先发 OPTIONS，同源托管时用不到
    server.Options("/v1/chat/completions",
                   [](const httplib::Request&, httplib::Response& res) {
                       set_cors_headers(res);
                       res.status = 204;
                   });

    server.Post("/v1/chat/completions",
                [sensitive](const httplib::Request& req, httplib::Response& res) {
                    handle_chat(req, res, *sensitive);
                });
}
