/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         cache_guard.h
 * Description:  关键词层和向量层共用的准入规则与命中护栏。
 *               「这个问题值不值得缓存」和「这个候选答案敢不敢用」都放在这里，
 *               两层用同一把尺子，避免行为不一致。
 * ==============================================================================
 */

#pragma once

#include <string>

// 问题是否值得进入关键词/向量层：实词足够、无指代词、无否定词
bool cache_question_eligible(const std::string& normalized_question);

// 候选答案的护栏：否定极性必须一致，英文/数字实体必须一致
bool cache_guard_pass(const std::string& query, const std::string& candidate);
