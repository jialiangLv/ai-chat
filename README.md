# ai-chat

用 C++ 实现的 AI 助手，当前是**最小链路版本**：接收并解析浏览器的 HTTP 请求 → 计算输入 token → 以 OpenAI 兼容的 SSE 格式流式返回一段文字。

内容目前是服务端写死的假文字，**不联网、不调用任何模型**；token 数是本地近似估算。目的是先把链路跑通，之后按接缝逐步替换成真实实现。

## 目录结构

```
CMakeLists.txt    构建脚本（C++17，只依赖系统线程库）
include/
  httplib.h       cpp-httplib 0.38.0（第三方，header-only）
  json.hpp        nlohmann/json（第三方，header-only）
  chat.h          聊天模块接口：请求结构体、解析、内容生成、路由注册
  tokenizer.h     token 计数接口
src/
  main.cpp        进程骨架：读配置、注册路由、托管前端、启动服务
  chat.cpp        一次对话的全部处理：解析请求体、算 token、写 SSE 流
  tokenizer.cpp   按 UTF-8 码点切分并近似计数
web/
  index.html      最小前端：fetch + ReadableStream 逐字渲染
```

## 编译与运行

```bash
cd /home/lv/ai-chat
cmake -B build
cmake --build build -j
./bin/ai_chat
```

启动后打开 http://127.0.0.1:8080/ 即可使用。**注意要在项目根目录启动**，静态目录 `./web` 是相对当前工作目录解析的。

可用的环境变量：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `PORT` | `8080` | 监听端口 |
| `WEB_ROOT` | `./web` | 前端静态文件目录 |

端口被占用时：`PORT=8090 ./bin/ai_chat`

## 验证

```bash
# 看流式效果（-N 用于关闭 curl 自身的缓冲，否则会误以为没有流式）
curl -N -X POST http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"deepseek-flash","stream":true,"messages":[{"role":"user","content":"你好"}]}'

# 看响应头，应包含 Content-Type: text/event-stream 与 Transfer-Encoding: chunked
curl -s -D - -o /dev/null -N -X POST http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"hi"}]}'
```

预期：约每 50ms 吐出一条 `data: {...}`，最后是 `data: [DONE]`；同时服务端日志会打印本次请求的输入 token 估算值。

## 接口

`POST /v1/chat/completions`，请求体是 OpenAI 的子集：

```json
{
  "model": "deepseek-flash",
  "stream": true,
  "messages": [{ "role": "user", "content": "你好" }]
}
```

响应为标准 OpenAI 风格 SSE：逐条 `data: {"choices":[{"delta":{"content":"字"}}]}\n\n`，以 `data: [DONE]\n\n` 结束。

## 后续迭代方向

1. 把 `generate_deltas()` 换成真实的 DeepSeek 流式调用（需在 CMakeLists 中启用 OpenSSL 支持，并使用 `.env` 里的 key）
2. 把 `count_tokens()` 换成调用真实的 tokenizer 服务
3. 支持多轮上下文（`ChatRequest` 改为消息数组）
4. 会话持久化
5. 鉴权、限流、日志
