/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         embedding.cpp
 * Description:  Embedding 服务的实现。
 *               复用 cpp-httplib 发 HTTPS 请求，解析 OpenAI 兼容响应；
 *               返回前统一做 L2 归一化，让余弦相似度退化成一次点积。
 * ==============================================================================
 */

#include "embedding.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "httplib.h"
#include "json.hpp"

namespace {

// 读取环境变量，未设置或为空时返回默认值
std::string get_env(const char* key, const std::string& default_val = "") {
    const char* val = std::getenv(key);
    return (val && *val) ? std::string(val) : default_val;
}

// 把 base_url 拆成「scheme://主机」和「路径前缀」，httplib::Client 只接受主机部分
void split_base_url(const std::string& base_url, std::string& host,
                    std::string& path_prefix) {
    host = base_url;
    path_prefix.clear();

    const size_t scheme_end = base_url.find("://");
    const size_t path_begin =
        (scheme_end == std::string::npos) ? std::string::npos
                                          : base_url.find('/', scheme_end + 3);
    if (path_begin != std::string::npos) {
        host = base_url.substr(0, path_begin);
        path_prefix = base_url.substr(path_begin);
    }
}

}  // namespace

bool embedding_configured() {
    return !get_env("EMBEDDING_API_KEY").empty();
}

int embedding_dim() {
    const std::string text = get_env("EMBEDDING_DIM");
    if (text.empty()) {
        return 0;
    }
    try {
        return std::stoi(text);
    } catch (const std::exception&) {
        return 0;
    }
}

bool embed_question(const std::string& text, std::vector<float>& out,
                    std::string& error) {
    out.clear();

    const std::string api_key = get_env("EMBEDDING_API_KEY");
    if (api_key.empty()) {
        error = "未配置 EMBEDDING_API_KEY";
        return false;
    }

    const std::string base_url =
        get_env("EMBEDDING_BASE_URL", "https://api.siliconflow.cn/v1");
    const std::string model =
        get_env("EMBEDDING_MODEL", "Qwen/Qwen3-Embedding-0.6B");
    const int dim = embedding_dim();

    std::string host;
    std::string path_prefix;
    split_base_url(base_url, host, path_prefix);

    httplib::Client client(host);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(15, 0);

    nlohmann::json body;
    body["model"] = model;
    body["input"] = text;
    if (dim > 0) {
        body["dimensions"] = dim;  // Qwen3 系列支持按需截断维度
    }

    auto res = client.Post(path_prefix + "/embeddings",
                           {{"Authorization", "Bearer " + api_key}},
                           body.dump(), "application/json");
    if (!res) {
        error = "embedding 请求失败：" + httplib::to_string(res.error());
        return false;
    }
    if (res->status != 200) {
        error = "embedding 上游 HTTP " + std::to_string(res->status) + "：" +
                res->body.substr(0, 200);
        return false;
    }

    try {
        const auto json = nlohmann::json::parse(res->body);
        if (!json.contains("data") || !json["data"].is_array() ||
            json["data"].empty()) {
            error = "embedding 响应缺少 data 数组";
            return false;
        }
        const auto& embedding = json["data"][0]["embedding"];
        if (!embedding.is_array() || embedding.empty()) {
            error = "embedding 响应缺少向量";
            return false;
        }

        out.reserve(embedding.size());
        for (const auto& value : embedding) {
            out.push_back(value.is_number() ? value.get<float>() : 0.0f);
        }
    } catch (const std::exception& e) {
        error = std::string("embedding 响应解析失败：") + e.what();
        out.clear();
        return false;
    }

    // 接口通常已经归一化，这里再算一次，避免换供应商后行为不一致
    double norm_sq = 0.0;
    for (const float value : out) {
        norm_sq += static_cast<double>(value) * value;
    }
    if (norm_sq <= 0.0) {
        error = "embedding 返回零向量";
        out.clear();
        return false;
    }
    const float norm = static_cast<float>(std::sqrt(norm_sq));
    for (float& value : out) {
        value /= norm;
    }
    return true;
}
