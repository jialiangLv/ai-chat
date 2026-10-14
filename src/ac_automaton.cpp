/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         ac_automaton.cpp
 * Description:  AC 自动机的实现。
 *               结构是「字典树 + 失配指针」：先把所有词装进一棵共享前缀的树，
 *               再按层序为每个节点算出一条 fail 边，使得匹配失败时能退到
 *               「已匹配部分的最长后缀」而不是退回根部重来。
 * ==============================================================================
 */

#include "ac_automaton.h"

#include <fstream>
#include <queue>

// 去掉字符串首尾的空白字符（含 Windows 换行残留的 \r）
static std::string trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// 往词典里加一个词：沿着字符逐层下行，缺节点就新建
void AcAutomaton::add(const std::string& word) {
    if (word.empty()) {
        return;
    }

    int cur = 0;  // 从根节点出发
    for (unsigned char c : word) {
        auto it = nodes_[cur].children.find(c);
        if (it == nodes_[cur].children.end()) {
            nodes_.push_back(Node{});
            int next = static_cast<int>(nodes_.size()) - 1;
            nodes_[cur].children[c] = next;
            cur = next;
        } else {
            cur = it->second;
        }
    }

    nodes_[cur].outputs.push_back(word);  // 词的最后一个字符所在节点记下这个词
    ++word_count_;
}

// 按 BFS 层序构建失配指针，并顺便把 fail 链上的命中结果合并到本节点
void AcAutomaton::build() {
    std::queue<int> pending;

    // 根的直接子节点：失配只能回到根
    for (const auto& entry : nodes_[0].children) {
        nodes_[entry.second].fail = 0;
        pending.push(entry.second);
    }

    while (!pending.empty()) {
        int u = pending.front();
        pending.pop();

        // 合并：走到 u 就意味着也匹配了 fail(u) 的全部命中（那些是 u 所代表串的后缀）
        for (const std::string& word : nodes_[nodes_[u].fail].outputs) {
            nodes_[u].outputs.push_back(word);
        }

        for (const auto& entry : nodes_[u].children) {
            unsigned char c = entry.first;
            int v = entry.second;

            // 从 fail(u) 出发沿 fail 链上溯，找第一个也有 c 子节点的节点
            int f = nodes_[u].fail;
            while (f != 0 && nodes_[f].children.count(c) == 0) {
                f = nodes_[f].fail;
            }
            auto it = nodes_[f].children.find(c);
            nodes_[v].fail = (it != nodes_[f].children.end()) ? it->second : 0;

            pending.push(v);
        }
    }
}

// 扫描文本，一次走完并收集所有命中的词
std::vector<std::string> AcAutomaton::find_all(const std::string& text) const {
    std::vector<std::string> hits;
    int cur = 0;

    for (unsigned char c : text) {
        // 走不动就沿 fail 链回退，直到能继续或回到根
        while (cur != 0 && nodes_[cur].children.count(c) == 0) {
            cur = nodes_[cur].fail;
        }
        auto it = nodes_[cur].children.find(c);
        cur = (it != nodes_[cur].children.end()) ? it->second : 0;

        // outputs 在 build 时已经合并过 fail 链，这里直接取即可
        for (const std::string& word : nodes_[cur].outputs) {
            hits.push_back(word);
        }
    }
    return hits;
}

// 从文件加载词典，每行一个词，加载完自动构建失配指针
size_t AcAutomaton::load_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return 0;
    }

    size_t loaded = 0;
    std::string line;
    while (std::getline(file, line)) {
        std::string word = trim(line);
        if (word.empty() || word[0] == '#') {
            continue;  // 跳过空行与注释行
        }
        add(word);
        ++loaded;
    }

    build();
    return loaded;
}
