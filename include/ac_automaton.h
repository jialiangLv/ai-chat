/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         ac_automaton.h
 * Description:  AC 自动机（Aho-Corasick）的对外接口。
 *               一种多模式串匹配算法：先用一批词建表，之后每次扫描文本
 *               都能一次找出命中的全部词，代价与词表规模无关。
 * ==============================================================================
 */

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class AcAutomaton {
public:
    // 往词典里加一个词（可以重复调用，全部加完后必须调用 build）
    void add(const std::string& word);

    // 构建失配指针，必须在所有 add 之后、任何匹配之前调用一次
    void build();

    // 返回文本中命中的全部词，按结束位置从前往后排列，可能有重复
    std::vector<std::string> find_all(const std::string& text) const;

    // 从文件加载词典，每行一个词，加载完自动构建失配指针
    // 返回成功加载的词数，文件不存在时返回 0
    size_t load_file(const std::string& path);

    // 词典中的词数
    size_t word_count() const { return word_count_; }

private:
    // 自动机的一个状态，对应词典中某个前缀
    struct Node {
        std::unordered_map<unsigned char, int> children;  // 字母表是字节，用哈希表存子节点
        int fail = 0;                                     // 失配指针，指向另一个节点下标
        std::vector<std::string> outputs;                 // 走到本节点时命中的词（已合并 fail 链）
    };

    std::vector<Node> nodes_ = std::vector<Node>(1);  // 下标 0 恒为根节点
    size_t word_count_ = 0;
};
