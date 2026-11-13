/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      AiChat
 * File:         main.cpp
 * Description:  程序入口。
 *               负责进程骨架：读取端口与静态目录配置、注册聊天路由、
 *               托管前端页面并启动 HTTP 服务。
 * ==============================================================================
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "auth.h"
#include "cache.h"
#include "chat.h"
#include "db.h"
#include "httplib.h"
#include "usage.h"

// 读取环境变量，若未设置则返回预设的默认值
std::string get_env_var(const char* key, const std::string& default_val) {
    const char* val = std::getenv(key);
    return val ? std::string(val) : default_val;
}

// 去掉字符串两端的空白字符（含 Windows 换行残留的 \r）
std::string trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// 极简 .env 解析：逐行读 KEY=VALUE，跳过空行与 # 注释行，写入进程环境
// 已存在的环境变量不覆盖，这样可以用命令行临时覆盖 .env 里的值；没有该文件也不算错误
void load_dotenv(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(trimmed.substr(0, eq));
        std::string value = trim(trimmed.substr(eq + 1));
        if (!key.empty() && std::getenv(key.c_str()) == nullptr) {
            setenv(key.c_str(), value.c_str(), 1);
        }
    }
}

int main() {
    // 先把 .env 里的配置读进环境，后续统一用 get_env_var 取值
    load_dotenv(".env");

    int port = std::stoi(get_env_var("PORT", "8080"));
    std::string web_root = get_env_var("WEB_ROOT", "./web");

    httplib::Server server;

    // 请求体大小上限。挡住"发个几十 MB 请求"这种最粗暴的滥用——
    // 这和 token 估算无关，纯粹是 HTTP 层的防护，超限时返回 413
    server.set_payload_max_length(1024 * 1024);  // 1 MB

    // 打开用户数据库（不存在就创建），并建表。数据库放本机文件里，
    // 不需要额外的数据库服务
    const std::string db_path = get_env_var("DB_PATH", "./data/ai_chat.db");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(db_path).parent_path(), ec);

    Db db;
    std::string db_error;
    if (!db.open(db_path, db_error)) {
        std::cerr << "[ai-chat] 打开数据库失败：" << db_error << "（" << db_path << "）"
                  << std::endl;
        return 1;
    }
    if (!init_schema(db, db_error)) {
        std::cerr << "[ai-chat] 建表失败：" << db_error << std::endl;
        return 1;
    }
    if (!init_usage_schema(db, db_error)) {
        std::cerr << "[ai-chat] 建用量表失败：" << db_error << std::endl;
        return 1;
    }
    if (!init_cache_schema(db, db_error)) {
        std::cerr << "[ai-chat] 建缓存表失败：" << db_error << std::endl;
        return 1;
    }
    std::cout << "[ai-chat] 用户数据库：" << db_path << std::endl;

    // 注册所有路由（聊天 / 注册 / 登录 / 我的信息）
    register_chat_routes(server, db);

    // 把前端页面目录托管为静态站点，访问 / 时自动返回 index.html
    if (!server.set_mount_point("/", web_root)) {
        std::cerr << "[ai-chat] 警告：静态目录不可用：" << web_root
                  << "，请在项目根目录启动，或用 WEB_ROOT 指定路径" << std::endl;
    }

    // 流式响应属于长连接，放宽读写超时，避免内容还没打完连接就被掐断
    server.set_read_timeout(30, 0);
    server.set_write_timeout(30, 0);

    std::cout << "[ai-chat] 服务启动，请在浏览器打开 http://127.0.0.1:" << port << "/"
              << std::endl;

    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "[ai-chat] 启动失败，端口可能已被占用：" << port << std::endl;
        return 1;
    }
    return 0;
}
