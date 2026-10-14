# ai-chat

用 C++ 实现的 AI 助手。核心链路：

```
浏览器请求 → 解析 → 敏感词检查 → 计算 token → 调用大模型（流式）→ 以 SSE 逐字返回
```

模型是 DeepSeek，支持推理模型的思考内容展示；token 用量取自上游统计的真实值。

## 目录结构

```
CMakeLists.txt        构建脚本（C++17，依赖系统线程库与 OpenSSL）
include/
  httplib.h           cpp-httplib 0.38.0（第三方，header-only）
  json.hpp            nlohmann/json（第三方，header-only）
  chat.h              聊天模块接口：请求结构体、解析、内容生成、路由注册
  tokenizer.h         token 计数接口
  ac_automaton.h      AC 自动机接口（多模式串匹配）
src/
  main.cpp            进程骨架：读 .env、注册路由、托管前端、启动服务
  chat.cpp            一次对话的全部处理：解析请求体、敏感词检查、调模型、写 SSE 流
  tokenizer.cpp       按 UTF-8 码点切分并近似计数
  ac_automaton.cpp    AC 自动机的实现
web/
  index.html          前端结构
  app.css             前端样式（仿 ChatGPT 的浅色界面）
  app.js              前端逻辑：fetch + ReadableStream 逐字渲染
dict/
  sensitive.txt       敏感词表，每行一个词，# 开头为注释
```

## 编译与运行

```bash
cd /home/lv/ai-chat
cmake -B build
cmake --build build -j
./bin/ai_chat
```

**必须在项目根目录启动**——`.env`、`./web`、`dict/sensitive.txt` 都是相对当前工作目录解析的。

> 新增或删除 `src/` 下的 `.cpp` 后，要重新执行 `cmake -B build`。构建脚本用的是
> `file(GLOB ...)`，它不会自动发现新文件，只跑 `cmake --build` 会报符号未定义。

## 配置

密钥与模型配置放在项目根目录的 `.env`（已被 `.gitignore` 忽略）：

```
DEEPSEEK_API_KEY=sk-...
DEEPSEEK_BASE_URL=https://api.deepseek.com/v1
DEEPSEEK_MODEL=deepseek-flash
```

程序启动时会自动读取该文件；**已存在的环境变量不会被覆盖**，所以可以用命令行临时覆盖：

```bash
DEEPSEEK_MODEL=deepseek-v4-pro ./bin/ai_chat
```

其余环境变量：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `PORT` | `8080` | 监听端口 |
| `WEB_ROOT` | `./web` | 前端静态文件目录 |
| `SENSITIVE_DICT` | `dict/sensitive.txt` | 敏感词表路径 |

## 敏感词过滤

请求进大模型**之前**先扫一遍用户输入，命中就**直接返回一句固定话术、不调用模型**——
既做内容拦截，也省掉一次 API 调用（实测命中路径约 30ms，正常回答约 1.5s）。

匹配用的是 **AC 自动机**（Aho-Corasick）：启动时把词表建成「字典树 + 失配指针」，
之后每次扫描的代价是 `O(文本长度 + 命中数)`，**与词表大小无关**——所以词表从 7 条换成
几万条，单次请求的耗时也不变。

命中的词**只记在服务端日志，不回给客户端**，否则等于告诉对方该绕开哪个词。

词表支持重叠（`dict/sensitive.txt` 里刻意放了几组），例如扫描「网络赌博网站」会同时
报出 `网络赌博`、`赌博`、`赌博网站`——其中 `赌博` 是前者的后缀、后者的前缀，靠失配指针
才能报全。

## 接口

`POST /v1/chat/completions`，请求体是 OpenAI 的子集：

```json
{
  "model": "deepseek-flash",
  "stream": true,
  "messages": [{ "role": "user", "content": "你好" }]
}
```

响应为标准 OpenAI 风格 SSE：

- 思考过程：`data: {"choices":[{"delta":{"reasoning_content":"…"}}]}`
- 正式回答：`data: {"choices":[{"delta":{"content":"…"}}]}`
- 用量：`data: {"choices":[],"usage":{"prompt_tokens":…,"total_tokens":…}}`
- 结束：`data: [DONE]`

## 验证

```bash
# 看流式效果（-N 关闭 curl 自身缓冲，否则会误以为没有流式）
curl -N -X POST http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"你好"}]}'

# 敏感词拦截：应立刻返回固定话术，且服务端日志打印命中明细
curl -N -X POST http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"讲讲网络赌博网站"}]}'

# 响应头应包含 Content-Type: text/event-stream 与 Transfer-Encoding: chunked
curl -s -D - -o /dev/null -N -X POST http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"hi"}]}'
```

## 后续迭代方向

1. 多轮上下文（`ChatRequest` 改为完整的消息数组，目前只取最后一条 user 消息）
2. 鉴权（当前**任何能访问端口的人都能使用**，且花的是 `.env` 里那把 key）
3. 会话持久化
4. 把 `count_tokens()` 换成调真实 tokenizer 服务（目前只用它做发送前的粗估）
5. 限流
