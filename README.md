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
  markdown.js         极简 Markdown 渲染器（把模型输出转成安全的 HTML）
  markdown-test.html  渲染器的浏览器测试页，直接用浏览器打开即可
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

### 历史与本轮的处置不同

既然 `messages` 是客户端给的，历史就是**不可信输入**，必须全量复查。但两类消息的处置
不一样，因为「本轮」和「历史」的含义不同：

| | 含义 | 命中时 |
|---|---|---|
| **本轮**（最后一条 user） | 用户现在想说的 | **拒绝整个请求**——改一句重发即可 |
| **历史**中的 user 消息 | 已经发生过的事 | **就地替换成 `[该内容已被过滤]`**，本轮照常进行 |

历史不能用「拒绝」来处理——否则一次命中会让**之后每一轮都带上那段历史**，请求次次被拒，
对话就**永久锁死**了。替换而不是删除，是为了不破坏 `user`/`assistant` 的配对结构。

前端也遵守同一条纪律：**只有「模型参与过」的轮次才进 `history`**。被拦截的一轮模型根本
没被调用，界面照常显示提示，但**不该混进发给模型的上下文里**——否则等于把服务端的话术
冒充成"模型说过的话"。

前端靠响应头 `X-Ai-Chat-Blocked: 1` 判断本轮是否被拦截（该头由 `Access-Control-Expose-Headers`
显式暴露给前端）。不用「响应里有没有 usage」之类的间接信号来判断，是因为那会把"被拦截"
和"调用出错"两种情况混为一谈。

## 前端 Markdown 渲染

助手的正文会用 `web/markdown.js` 渲染成 HTML（用户的提问和思考过程保持纯文本）。
支持代码块、行内代码、粗体、斜体、标题、列表、引用、链接、分隔线；不认识的语法
**原样显示成纯文本**——宁可不渲染，也不渲染错。

### 一条不能违反的规则：先转义，再变换

模型可以输出 `<img src=x onerror=...>`，而且用户能用提示词诱导它这么做。所以渲染器
的流程必须是：

```
1. 先把 & < > " ' 转义成 HTML 实体
2. 再在转义后的文本上做 Markdown 替换，且只产出白名单标签
```

**顺序绝不能反。** 先变换再转义会把渲染器自己生成的 `<strong>` 也转义掉，渲染直接失效；
先转义再变换，则模型给的任何 HTML 都已被中和成纯文本——**安全性是构造出来的，而不是
靠事后过滤**。这也是为什么没有直接引入 marked.js：它允许原始 HTML 直通，必须再配一层
DOMPurify，两层都得对才行。

改动渲染器后，用浏览器打开 `/markdown-test.html` 跑一遍用例（含 XSS 载荷）。该页用
`textContent` 显示结果，所以**即使转义有漏洞，页面也只会报 FAIL，不会真的执行载荷**。

### 流式过程中的不完整语法

流式返回时 Markdown 是**不完整**的（半截的 `**`、没闭合的代码块）。渲染器的处理是
**每收到一块就用最新的完整文本重新渲染整段**——不完整时退化成原样显示，标记一闭合就
自动变正确，不需要任何特殊处理。代价是 O(n²)（实际量级可忽略），以及流式期间选中文字
会被 DOM 重建打断，这是所有流式富文本的固有代价。

## 接口

`POST /v1/chat/completions`，请求体是 OpenAI 的子集：

```json
{
  "model": "deepseek-flash",
  "stream": true,
  "messages": [
    { "role": "user",      "content": "我叫小明" },
    { "role": "assistant", "content": "好的小明" },
    { "role": "user",      "content": "我叫什么？" }
  ]
}
```

**接口是无状态的**，和 OpenAI 一样：多轮上下文的全部机制就是客户端每次把**完整对话**
放进 `messages` 发过来——**没有单独的 `history` 字段**，也不需要发明一个。最后一条
必须是 `role=user`，它代表本轮输入。

服务端有两个相关的保护：

- **截断**：最多携带最近 20 条消息，超出就丢掉最早的部分。截断后会往前推到第一条
  `user` 消息，避免开头的 `assistant` 成为孤儿（所以实际条数可能略少于 20）
- **敏感词分治**：见下一节

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

1. 鉴权（当前**任何能访问端口的人都能使用**，且花的是 `.env` 里那把 key）
2. 会话持久化（目前历史只在浏览器内存里，刷新即失）
3. 发送前的长度检查（`count_tokens()` 目前只打印日志；注意它的估算偏差很大，
   因为完全没算约 30 token 的固定模板开销，只适合做很宽松的粗筛，不能用于计费）
4. 限流
5. 思考过程的 Markdown 渲染（目前只有正文渲染，思考区是纯文本）
