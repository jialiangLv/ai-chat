# ==============================================================================
# Copyright (c) 2026 Nexus_Yao. All rights reserved.
#
# Project:      AiChat
# File:         CMakeLists.txt
# Description:  ai-chat 最小链路的构建脚本。
#               配置 C++17，仅依赖系统线程库与随仓库携带的 header-only 库，
#               不引入 OpenSSL / MySQL 等第一步用不到的依赖。
# ==============================================================================

# 设置最低 CMake 版本要求
cmake_minimum_required(VERSION 3.10)

# 定义项目名称及版本号
project(AiChat VERSION 1.0)

# 指定 C++ 编译标准为 C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 随仓库携带的第三方头文件（httplib.h / json.hpp）放在 include/ 下
include_directories(${PROJECT_SOURCE_DIR}/include)

# 扫描 src 目录下的所有 .cpp 源码文件并存入 SOURCES 变量
# 注：新增源文件后需重新运行 CMake 以刷新 GLOB 缓存，否则新文件不参与编译
file(GLOB SOURCES "src/*.cpp")

# 声明编译目标
add_executable(ai_chat ${SOURCES})

# cpp-httplib 依赖系统线程库
find_package(Threads REQUIRED)
target_link_libraries(ai_chat Threads::Threads)

# 规范化输出路径：可执行文件统一输出到项目根目录的 bin/ 下
set_target_properties(ai_chat PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
