/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         embedding.h
 * Description:  Embedding 服务的对外接口。
 *               把一句话发到 OpenAI 兼容的 /v1/embeddings，取回归一化后的
 *               浮点向量；配置缺失时调用方据此跳过整个语义层。
 * ==============================================================================
 */

#pragma once

#include <string>
#include <vector>

// 是否配置了 EMBEDDING_API_KEY
bool embedding_configured();

// 配置里声明的向量维度；未配置返回 0
int embedding_dim();

// 调用 embedding 接口，成功时 out 为 L2 归一化后的向量
bool embed_question(const std::string& text, std::vector<float>& out,
                    std::string& error);
