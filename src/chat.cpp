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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ac_automaton.h"
#include "auth.h"
#include "db.h"
#include "json.hpp"
#include "tokenizer.h"
#include "usage.h"

namespace {

// 读取环境变量，未设置或为空时返回默认值
std::string get_env(const char* key, const std::string& default_val = "") {
    const char* val = std::getenv(key);
    return (val && *val) ? std::string(val) : default_val;
}

// 读取整型环境变量，未设置或解析失败时用默认值——配置写错不该让请求崩掉
long get_env_long(const char* key, long default_val) {
    const std::string text = get_env(key);
    if (text.empty()) {
        return default_val;
    }
    try {
        return std::stol(text);
    } catch (const std::exception&) {
        return default_val;
    }
}

// 允许使用的模型名清单。未配置 ALLOWED_MODELS 时，只允许 .env 里配置的默认模型
std::vector<std::string> allowed_models() {
    std::vector<std::string> models;
    const std::string list = get_env("ALLOWED_MODELS");

    if (list.empty()) {
        models.push_back(get_env("DEEPSEEK_MODEL", "deepseek-flash"));
        return models;
    }

    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        std::string item = (comma == std::string::npos)
                               ? list.substr(start)
                               : list.substr(start, comma - start);
        // 去掉首尾空白，跳过空项
        size_t begin = item.find_first_not_of(" \t");
        size_t end = item.find_last_not_of(" \t");
        if (begin != std::string::npos) {
            models.push_back(item.substr(begin, end - begin + 1));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return models;
}

// 判断模型名是否在允许清单里
bool is_allowed_model(const std::vector<std::string>& models, const std::string& name) {
    return std::find(models.begin(), models.end(), name) != models.end();
}

// 把清单拼成 "a, b, c"，用于错误提示
std::string join_names(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        out += (i ? ", " : "");
        out += items[i];
    }
    return out;
}

// 按用户的令牌桶限流。
// 状态只放内存：单进程下它就是全局的；而且限流的目的是保护服务，
// 不该反过来给数据库加压。代价是重启后桶清空，可以接受
class RateLimiter {
public:
    // 放行返回 true；不放行时 wait_seconds 给出建议等待的秒数
    bool allow(long user_id, double rate_per_sec, double burst, double& wait_seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();

        Bucket& bucket = buckets_[user_id];
        if (!bucket.initialized) {
            bucket.tokens = burst;
            bucket.updated = now;
            bucket.initialized = true;
        }

        // 按流逝的时间补充令牌，最多补到 burst
        const double elapsed = std::chrono::duration<double>(now - bucket.updated).count();
        bucket.tokens = std::min(burst, bucket.tokens + elapsed * rate_per_sec);
        bucket.updated = now;

        if (bucket.tokens < 1.0) {
            wait_seconds = (rate_per_sec > 0) ? (1.0 - bucket.tokens) / rate_per_sec : 1.0;
            return false;
        }
        bucket.tokens -= 1.0;
        return true;
    }

private:
    struct Bucket {
        bool initialized = false;
        double tokens = 0;
        std::chrono::steady_clock::time_point updated;
    };

    std::mutex mutex_;
    std::unordered_map<long, Bucket> buckets_;
};

// 从 Authorization: Bearer <token> 里取出令牌；没有或格式不对返回空串
std::string bearer_token(const httplib::Request& req) {
    if (!req.has_header("Authorization")) {
        return "";
    }
    const std::string value = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    if (value.compare(0, prefix.size(), prefix) != 0) {
        return "";
    }
    return value.substr(prefix.size());
}

// 中间件在每次请求开始时写入当前线程的身份，handler 从这里读。
// 为什么能用 thread_local：httplib 的 pre_routing_handler 与路由 handler 在同一个
// 工作线程里顺序执行，而且中间件对每个请求都会执行——只要它在开头先清空，
// 就不可能读到上一个请求残留的值
thread_local AuthUser g_current_user;

// 返回一个 JSON 错误响应
void send_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    nlohmann::json err;
    err["error"] = message;
    res.set_content(err.dump(), "application/json");
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
std::string make_sse_line(const std::string& model, const nlohmann::json& choice) {
    nlohmann::json chunk;
    chunk["id"] = "chatcmpl-local";
    chunk["object"] = "chat.completion.chunk";
    chunk["created"] = 0;
    chunk["model"] = model;  // 必须是实际用到的模型，不能硬编码
    chunk["choices"] = nlohmann::json::array({choice});
    return "data: " + chunk.dump() + "\n\n";
}

// 构造开场分片：只声明角色、内容为空，用于对齐 OpenAI 流式响应的首条 chunk
std::string make_role_line(const std::string& model) {
    nlohmann::json choice;
    choice["index"] = 0;
    choice["delta"]["role"] = "assistant";
    choice["finish_reason"] = nullptr;
    return make_sse_line(model, choice);
}

// 构造一个流式分片：思考过程写进 reasoning_content，正式回答写进 content
// 字段名与上游接口保持一致，前端据此把两类内容分别渲染
std::string make_delta_line(const std::string& model, DeltaKind kind, const std::string& text) {
    nlohmann::json choice;
    choice["index"] = 0;
    choice["delta"][kind == DeltaKind::Reasoning ? "reasoning_content" : "content"] = text;
    choice["finish_reason"] = nullptr;
    return make_sse_line(model, choice);
}

// 构造只带用量信息的一条分片，choices 为空数组（对齐 OpenAI stream_options 的约定）
std::string make_usage_line(const std::string& model, const Usage& usage) {
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
    chunk["model"] = model;
    chunk["choices"] = nlohmann::json::array();
    chunk["usage"] = usage_json;
    return "data: " + chunk.dump() + "\n\n";
}

// 用 SSE 格式返回一段固定的文本，供不需要调用模型的分支复用（如敏感词拦截）
// 带上标记响应头：这一轮模型并没有参与，前端不该把它计入发给模型的上下文
void stream_fixed_text(httplib::Response& res, const std::string& model,
                       const std::string& text) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_header(kBlockedHeader, "1");

    res.set_chunked_content_provider(
        "text/event-stream",
        [text, model](size_t offset, httplib::DataSink& sink) -> bool {
            if (offset > 0) {
                return false;
            }
            std::string opening = make_role_line(model);
            if (!sink.write(opening.data(), opening.size())) {
                return false;
            }
            std::string chunk = make_delta_line(model, DeltaKind::Content, text);
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
                 const AcAutomaton& sensitive, Db& db) {
    set_cors_headers(res);

    // 中间件已经保证登录；这里再兜一层，纯内存判断不查库
    if (!g_current_user.ok) {
        send_error(res, 401, "未授权：请先登录");
        return;
    }

    ChatRequest chat_req = parse_chat_request(req.body);
    if (!chat_req.parse_ok) {
        res.status = 400;
        nlohmann::json err;
        err["error"] = chat_req.parse_error;
        res.set_content(err.dump(), "application/json");
        return;
    }

    // 解析并校验模型名：客户端没指定就用 .env 里的默认值；
    // 不在白名单里的立刻拒绝，不转发给上游
    const std::vector<std::string> models = allowed_models();
    const std::string model = chat_req.model.empty()
                                  ? get_env("DEEPSEEK_MODEL", "deepseek-flash")
                                  : chat_req.model;
    if (!is_allowed_model(models, model)) {
        res.status = 400;
        nlohmann::json err;
        err["error"] = "不支持的模型：" + model + "（可用：" + join_names(models) + "）";
        res.set_content(err.dump(), "application/json");
        return;
    }

    // 配额检查：没有余额就不调模型。扣减发生在回答结束之后（那时才拿到 usage），
    // 所以最后一个请求可能略微超额，这个代价可以接受
    std::string quota_error;
    const long long quota = get_quota(db, g_current_user.id, quota_error);
    if (quota <= 0) {
        send_error(res, 403, "额度不足，请联系管理员");
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
            stream_fixed_text(res, model, "触发到了知识盲区，请换个问题再问");
            return;
        }

        // 历史消息命中 → 就地替换成占位符，本轮照常进行
        std::cout << "（历史第 " << i << " 条）→ 已清洗" << std::endl;
        msg.content = kFiltered;
    }

    // 第二步：估算整段 prompt，超限就拒绝。
    // 估算放在敏感词清洗之后，因为真正发给模型的是清洗过的版本。
    // 模型平均误差约 1 token，所以阈值可以设得贴近真实上限、基本不会误伤；
    // 但它终究是估算——真正的上限只有上游知道，超长时它会返回 400
    std::vector<std::string> contents;
    contents.reserve(chat_req.messages.size());
    for (const Message& message : chat_req.messages) {
        contents.push_back(message.content);
    }
    TokenResult input_estimate = count_tokens(chat_req.messages.back().content);
    TokenResult prompt_estimate = estimate_conversation(contents);

    const long max_prompt_tokens = get_env_long("MAX_PROMPT_TOKENS", 32000);
    if (prompt_estimate.num_tokens > max_prompt_tokens) {
        std::cout << "[ai-chat] prompt 估算 " << prompt_estimate.num_tokens
                  << " 超过上限 " << max_prompt_tokens << "，已拒绝" << std::endl;
        // 注意这里是 stream_fixed_text：它会带上 X-Ai-Chat-Blocked 标记，
        // 前端因此不会把这一轮写进历史。否则这条超长消息会一直留在上下文里，
        // 之后每次请求都再次超限——和敏感词那次是同一个问题
        stream_fixed_text(res, model,
                          "对话太长了（估算 " + std::to_string(prompt_estimate.num_tokens) +
                              " token，上限 " + std::to_string(max_prompt_tokens) +
                              "）。请新建一个对话，或删掉一些历史消息。");
        return;
    }

    std::cout << "[ai-chat] 收到请求 model=" << model
              << " 本轮输入估算=" << input_estimate.num_tokens
              << "，整段 prompt 估算=" << prompt_estimate.num_tokens
              << "（" << chat_req.messages.size() << " 条消息）" << std::endl;

    // 关闭各级缓冲，否则客户端要等全部内容生成完才能看到数据
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");

    // 注意：此处不能再调用 res.set_content()，那会写入 Content-Length 使响应退化为非流式
    res.set_chunked_content_provider(
        "text/event-stream",
        [chat_req, model, user = g_current_user, &db,
         estimated_prompt = prompt_estimate.num_tokens](
            size_t offset, httplib::DataSink& sink) -> bool {
            // offset 是本连接上已写出的字节数；非 0 说明是重复回调，本 provider 只跑一轮
            if (offset > 0) {
                return false;
            }

            std::string opening = make_role_line(model);
            if (!sink.write(opening.data(), opening.size())) {
                return false;
            }

            // 不再需要自己控制节奏：上游模型边生成边吐，天然就是流式的打字机效果
            Usage usage;
            bool sent_all = generate_deltas(
                model,
                chat_req.messages,
                [&sink, model](DeltaKind kind, const std::string& text) -> bool {
                    std::string chunk = make_delta_line(model, kind, text);
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
                std::string usage_line = make_usage_line(model, usage);
                sink.write(usage_line.data(), usage_line.size());

                // 按真实 token 数扣减额度。原子 UPDATE：余额不够时不会改动任何行，
                // 并发下也不会出现双花
                std::string deduct_error;
                if (!deduct_quota(db, user.id, usage.total_tokens, deduct_error)) {
                    std::cout << "[ai-chat] 扣减额度失败：" << deduct_error << std::endl;
                }

                // 记一行明细。尽力而为：写失败只打日志，不影响已经扣掉的额度
                record_usage(db, user.id, model, usage.prompt_tokens,
                             usage.completion_tokens, usage.reasoning_tokens,
                             usage.total_tokens);

                std::cout << "[ai-chat] 上游真实用量 输入=" << usage.prompt_tokens
                          << "（估算 " << estimated_prompt << "，误差 "
                          << (estimated_prompt - usage.prompt_tokens) << "）"
                          << " 输出=" << usage.completion_tokens
                          << "（其中思考 " << usage.reasoning_tokens << "）"
                          << " 合计=" << usage.total_tokens
                          << " ｜用户=" << user.username << std::endl;
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
bool generate_deltas(const std::string& model, const std::vector<Message>& messages,
                     const DeltaSink& on_delta, Usage& usage) {
    std::string api_key = get_env("DEEPSEEK_API_KEY");
    if (api_key.empty()) {
        on_delta(DeltaKind::Content,
                 "[配置缺失] 没有读到 DEEPSEEK_API_KEY，请检查项目根目录下的 .env");
        return true;
    }

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

// 注册：必须带正确的邀请码——否则任何人注册新号就能继续白嫖额度
void handle_register(const httplib::Request& req, httplib::Response& res, Db& db) {
    set_cors_headers(res);

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
        send_error(res, 400, "请求体不是合法 JSON");
        return;
    }
    if (!body.is_object()) {
        send_error(res, 400, "请求体必须是 JSON 对象");
        return;
    }

    const std::string username = body.value("username", "");
    const std::string password = body.value("password", "");
    const std::string code = body.value("code", "");

    const std::string expected_code = get_env("REGISTER_CODE");
    if (expected_code.empty()) {
        send_error(res, 403, "服务端未开放注册");
        return;
    }
    if (!secure_compare(code, expected_code)) {
        send_error(res, 403, "邀请码不正确");
        return;
    }
    if (username.size() < 2 || username.size() > 32) {
        send_error(res, 400, "用户名长度应为 2–32 个字符");
        return;
    }
    if (password.size() < 6) {
        send_error(res, 400, "密码至少 6 位");
        return;
    }

    long user_id = 0;
    std::string error;
    const long long initial_quota = get_env_long("INITIAL_QUOTA", 100000);
    if (!create_user(db, username, password, initial_quota, user_id, error)) {
        send_error(res, 400, error);
        return;
    }

    nlohmann::json out;
    out["ok"] = true;
    out["username"] = username;
    res.set_content(out.dump(), "application/json");
    std::cout << "[ai-chat] 新用户注册：" << username << "（初始额度 " << initial_quota
              << "）" << std::endl;
}

// 登录：校验通过后发放随机会话令牌
void handle_login(const httplib::Request& req, httplib::Response& res, Db& db) {
    set_cors_headers(res);

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
        send_error(res, 400, "请求体不是合法 JSON");
        return;
    }
    if (!body.is_object()) {
        send_error(res, 400, "请求体必须是 JSON 对象");
        return;
    }

    const std::string username = body.value("username", "");
    const std::string password = body.value("password", "");

    long user_id = 0;
    std::string error;
    if (!check_credentials(db, username, password, user_id, error)) {
        // 不区分「用户不存在」和「密码错误」，避免被拿去枚举账号
        send_error(res, 401, "用户名或密码错误");
        return;
    }

    std::string token;
    const long ttl = get_env_long("SESSION_TTL_SECONDS", 7 * 24 * 3600);
    if (!create_session(db, user_id, ttl, token, error)) {
        send_error(res, 500, "创建会话失败");
        return;
    }

    const long long quota = get_quota(db, user_id, error);

    nlohmann::json out;
    out["token"] = token;
    out["username"] = username;
    out["quota"] = quota;
    res.set_content(out.dump(), "application/json");
}

// 查询当前登录用户与剩余额度
void handle_me(const httplib::Request&, httplib::Response& res, Db& db) {
    set_cors_headers(res);

    std::string error;
    const long long quota = get_quota(db, g_current_user.id, error);

    nlohmann::json out;
    out["username"] = g_current_user.username;
    out["quota"] = quota;
    res.set_content(out.dump(), "application/json");
}

// 用量统计：今日与本月
void handle_usage(const httplib::Request&, httplib::Response& res, Db& db) {
    set_cors_headers(res);

    UsageStats today;
    UsageStats month;
    std::string error;
    if (!query_usage(db, g_current_user.id, start_of_today(), today, error) ||
        !query_usage(db, g_current_user.id, start_of_month(), month, error)) {
        send_error(res, 500, "统计失败");
        return;
    }

    auto to_json = [](const UsageStats& s) {
        nlohmann::json j;
        j["requests"] = s.requests;
        j["prompt"] = s.prompt_tokens;
        j["completion"] = s.completion_tokens;
        j["total"] = s.total_tokens;
        return j;
    };

    nlohmann::json out;
    out["today"] = to_json(today);
    out["month"] = to_json(month);
    res.set_content(out.dump(), "application/json");
}

// 在 HTTP 服务上注册所有路由
void register_chat_routes(httplib::Server& server, Db& db) {
    // 限流器要活到服务结束，所以用 shared_ptr 按值捕获，避免悬垂引用
    auto limiter = std::make_shared<RateLimiter>();

    // 鉴权中间件：在路由分发之前执行，一个地方管住所有需要登录的接口
    server.set_pre_routing_handler(
        [&db, limiter](const httplib::Request& req, httplib::Response& res) {
            // 每个请求都先清空，保证不会读到上一个请求残留的身份
            g_current_user = AuthUser{};

            // 需要登录的路径：聊天接口、我的信息、用量统计。
            // 注册/登录不在此列——它们本来就是用来换取身份的
            const bool guarded = req.path.rfind("/v1/", 0) == 0 ||
                                 req.path == "/api/me" ||
                                 req.path == "/api/usage";

            // 静态文件与 OPTIONS 预检必须放行：页面本身不是秘密；
            // 浏览器按规范不会在预检里带 Authorization，拦了跨域全废
            if (!guarded || req.method == "OPTIONS") {
                return httplib::Server::HandlerResponse::Unhandled;
            }

            g_current_user = authenticate(db, bearer_token(req));
            if (!g_current_user.ok) {
                set_cors_headers(res);
                res.status = 401;
                res.set_header("WWW-Authenticate", "Bearer");
                nlohmann::json err;
                err["error"] = "未授权：请先登录";
                res.set_content(err.dump(), "application/json");
                return httplib::Server::HandlerResponse::Handled;  // 已应答，跳过路由
            }

            // 只对聊天接口按用户限流。现在有身份了，不必再按 IP 误伤共享出口
            if (req.path.rfind("/v1/", 0) == 0) {
                const double per_minute =
                    static_cast<double>(get_env_long("RATE_LIMIT_PER_MINUTE", 20));
                const double burst =
                    static_cast<double>(get_env_long("RATE_LIMIT_BURST", 10));

                double wait_seconds = 0;
                if (!limiter->allow(g_current_user.id, per_minute / 60.0, burst,
                                    wait_seconds)) {
                    const int retry_after = std::max(1, static_cast<int>(std::ceil(wait_seconds)));
                    set_cors_headers(res);
                    res.status = 429;
                    res.set_header("Retry-After", std::to_string(retry_after));
                    nlohmann::json err;
                    err["error"] = "请求太频繁，请稍后再试";
                    res.set_content(err.dump(), "application/json");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }

            return httplib::Server::HandlerResponse::Unhandled;
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
    const auto preflight = [](const httplib::Request&, httplib::Response& res) {
        set_cors_headers(res);
        res.status = 204;
    };
    server.Options("/v1/chat/completions", preflight);
    server.Options("/api/register", preflight);
    server.Options("/api/login", preflight);
    server.Options("/api/me", preflight);
    server.Options("/api/usage", preflight);

    server.Post("/v1/chat/completions",
                [sensitive, &db](const httplib::Request& req, httplib::Response& res) {
                    handle_chat(req, res, *sensitive, db);
                });

    server.Post("/api/register",
                [&db](const httplib::Request& req, httplib::Response& res) {
                    handle_register(req, res, db);
                });
    server.Post("/api/login",
                [&db](const httplib::Request& req, httplib::Response& res) {
                    handle_login(req, res, db);
                });
    server.Get("/api/me", [&db](const httplib::Request& req, httplib::Response& res) {
        handle_me(req, res, db);
    });
    server.Get("/api/usage", [&db](const httplib::Request& req, httplib::Response& res) {
        handle_usage(req, res, db);
    });
}
