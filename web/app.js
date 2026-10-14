/* ==============================================================================
   页面逻辑：把输入发到 POST /v1/chat/completions，并把流式返回的内容逐字渲染。
   流式解析的关键在于：网络一次 read 拿到的分块可能切断一个 UTF-8 字符，
   也可能只含半个 SSE 帧，所以必须先解码再缓存、按空行切出完整帧才解析。
   模型会先输出一段思考过程（reasoning_content）再输出正式回答（content），
   两者分别渲染：思考用灰色小字，正式回答开始后自动折叠。
   ============================================================================== */

const chatEl = document.getElementById('chat');
const welcomeEl = document.getElementById('welcome');
const statusEl = document.getElementById('status');
const inputEl = document.getElementById('input');
const sendEl = document.getElementById('send');
const stopEl = document.getElementById('stop');
const payloadEl = document.getElementById('payload');
const payloadToggleEl = document.getElementById('payload-toggle');
const sentBlockEl = document.getElementById('sent-block');
const sentTitleEl = document.getElementById('sent-title');
const sentTextEl = document.getElementById('sent-text');
const historyTitleEl = document.getElementById('history-title');
const historyTextEl = document.getElementById('history-text');

// 当前流式请求的中断句柄，非空表示正在生成，用于「停止生成」
let controller = null;

// 已发生的对话轮次。每次请求都作为 messages 一起发给服务端——多轮上下文的全部机制
// 就是这个数组在变长，接口里没有单独的 history 字段。
// 两条纪律：只放「模型参与过」的轮次；assistant 只放 content，绝不放 reasoning_content
let history = [];

// 前端也保留最近若干条，与服务端的截断上限保持一致
const MAX_HISTORY_MESSAGES = 20;

// 把一轮问答计入历史
function commitTurn(userText, assistantText) {
  if (!assistantText) {
    return;
  }
  history.push({ role: 'user', content: userText });
  history.push({ role: 'assistant', content: assistantText });
  if (history.length > MAX_HISTORY_MESSAGES) {
    history = history.slice(-MAX_HISTORY_MESSAGES);
  }
}

// 判断消息区是否已经贴在底部，用于决定要不要跟着新内容自动滚动
function isNearBottom() {
  return chatEl.scrollHeight - chatEl.scrollTop - chatEl.clientHeight < 80;
}

// 滚到消息区底部
function scrollToBottom() {
  chatEl.scrollTop = chatEl.scrollHeight;
}

// 往消息区追加一条消息，返回可继续写入的元素句柄
function appendMessage(role) {
  welcomeEl.hidden = true;

  const row = document.createElement('div');
  row.className = 'msg msg--' + role;

  // 只有助手消息带头像，用户消息是右侧的独立气泡
  if (role === 'assistant') {
    const avatar = document.createElement('div');
    avatar.className = 'msg__avatar';
    avatar.textContent = 'AI';
    row.appendChild(avatar);
  }

  const body = document.createElement('div');
  body.className = 'msg__body';
  const handle = { body };

  if (role === 'assistant') {
    // 思考区域：灰色小字，点击标题可折叠/展开
    const box = document.createElement('div');
    box.className = 'reasoning';
    box.hidden = true;

    const toggle = document.createElement('button');
    toggle.type = 'button';
    toggle.className = 'reasoning__toggle';
    toggle.textContent = '思考过程';
    toggle.addEventListener('click', () => box.classList.toggle('is-collapsed'));

    const text = document.createElement('div');
    text.className = 'reasoning__text';

    box.append(toggle, text);
    body.appendChild(box);

    handle.reasoningBox = box;
    handle.reasoningText = text;
  }

  const content = document.createElement('div');
  content.className = 'msg__content';
  body.appendChild(content);
  handle.content = content;

  if (role === 'assistant') {
    // 真实 token 用量脚注，等到上游返回 usage 才显示
    const usage = document.createElement('div');
    usage.className = 'msg__usage';
    usage.hidden = true;
    body.appendChild(usage);
    handle.usage = usage;
  }

  row.appendChild(body);
  chatEl.appendChild(row);
  scrollToBottom();
  return handle;
}

// 解析一帧 SSE 事件，把内容按类型累加到 out 上（out = {reasoning, content}）
function parseEvent(event, out) {
  for (const line of event.split('\n')) {
    if (!line.startsWith('data:')) {
      continue;
    }
    const payload = line.slice(5).trim();
    if (payload === '[DONE]') {
      return;
    }
    try {
      const frame = JSON.parse(payload);
      // 用量帧的 choices 是空数组，所以要在取 delta 之前先收下来
      if (frame.usage) {
        out.usage = frame.usage;
      }
      const delta = frame.choices?.[0]?.delta;
      if (!delta) {
        continue;
      }
      // 两个字段每帧都存在但可能为 null，所以要判类型
      if (typeof delta.reasoning_content === 'string') {
        out.reasoning += delta.reasoning_content;
      }
      if (typeof delta.content === 'string') {
        out.content += delta.content;
      }
    } catch (e) {
      // JSON 还没收全，忽略，等下一块
    }
  }
}

// 渲染真实 token 用量脚注
function renderUsage(el, usage) {
  let text = '输入 ' + usage.prompt_tokens + ' · 输出 ' + usage.completion_tokens;
  const reasoning = usage.completion_tokens_details?.reasoning_tokens;
  if (reasoning) {
    text += '（其中思考 ' + reasoning + '）';
  }
  el.textContent = text + ' · 合计 ' + usage.total_tokens + ' tokens';
  el.hidden = false;
}

// 最近一次实际发出的请求体
let lastRequest = null;

// 渲染面板。分两块，因为这两个东西很容易被当成一回事：
//   「本次发出的请求」——实际 POST 出去的内容，其中必然包含本轮的提问，
//                        哪怕这一轮随后会被敏感词拦截（不发给服务端就无从检查）
//   「累积的上下文」  —— 之后每一轮都会带上这些；被拦截的轮次不会出现在这里
function renderPayload() {
  if (lastRequest) {
    sentTitleEl.textContent =
      '本次发出的请求（' + lastRequest.messages.length + ' 条）';
    sentTextEl.textContent = JSON.stringify(lastRequest, null, 2);
    sentBlockEl.hidden = false;
  }

  historyTitleEl.textContent =
    '累积的上下文（' + history.length + ' 条）—— 下一轮会带上这些';
  historyTextEl.textContent = JSON.stringify({ messages: history }, null, 2);

  payloadToggleEl.textContent = '请求体 (' + history.length + ')';
}

// 记录本次实际发出的请求体并刷新面板
function showRequestPayload(body) {
  lastRequest = body;
  renderPayload();
}

// 切换「发送 / 停止」按钮的显隐，生成期间保持输入框可用，方便继续打字
function setGenerating(generating) {
  sendEl.hidden = generating;
  stopEl.hidden = !generating;
}

// 输入框随内容自动增高，上限由 CSS 的 max-height 控制
function autoResize() {
  inputEl.style.height = 'auto';
  inputEl.style.height = inputEl.scrollHeight + 'px';
}

// 发送一次对话请求并流式渲染回复
async function send() {
  const prompt = inputEl.value.trim();
  if (!prompt || controller) {
    return;
  }

  inputEl.value = '';
  autoResize();

  appendMessage('user').content.textContent = prompt;
  const reply = appendMessage('assistant');
  reply.content.classList.add('msg__content--typing');

  const startedAt = Date.now();
  let firstChunkAt = 0;
  let firstContentAt = 0;
  let contentStarted = false;
  let blocked = false;  // 声明在外层：catch 分支也要用它
  const out = { reasoning: '', content: '', usage: null };

  setGenerating(true);
  statusEl.textContent = '思考中…';

  controller = new AbortController();
  try {
    // 先把请求体构造出来：一份显示给用户看，一份直接发出去，两者保证是同一个对象
    // messages = 已有历史 + 本轮输入，这就是「多轮上下文」的全部
    const requestBody = {
      model: 'deepseek-flash',
      stream: true,
      messages: [...history, { role: 'user', content: prompt }]
    };
    showRequestPayload(requestBody);

    const resp = await fetch('/v1/chat/completions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(requestBody),
      signal: controller.signal
    });

    if (!resp.ok) {
      reply.content.textContent = '请求失败：HTTP ' + resp.status;
      statusEl.textContent = '';
      return;
    }

    // 服务端用这个头标记「本轮被拦截，模型没有参与」——这一轮不该进上下文
    blocked = resp.headers.get('X-Ai-Chat-Blocked') === '1';

    const reader = resp.body.getReader();
    // stream 模式下，跨分块被切断的半个 UTF-8 字符会被解码器自己留住拼回
    const decoder = new TextDecoder('utf-8');
    let buffer = '';

    while (true) {
      const { value, done } = await reader.read();
      if (done) {
        break;
      }

      // 先判断用户有没有手动往上翻：翻了就别强行拽回底部
      const stickToBottom = isNearBottom();
      buffer += decoder.decode(value, { stream: true });

      // SSE 帧以空行分隔，先缓存再切，不能假设一次 read 就是完整一帧
      let sep;
      while ((sep = buffer.indexOf('\n\n')) >= 0) {
        const event = buffer.slice(0, sep);
        buffer = buffer.slice(sep + 2);
        parseEvent(event, out);
      }

      if (out.reasoning) {
        if (!firstChunkAt) {
          firstChunkAt = Date.now();
        }
        reply.reasoningBox.hidden = false;
        reply.reasoningText.textContent = out.reasoning;
      }

      if (out.content) {
        if (!contentStarted) {
          contentStarted = true;
          firstContentAt = Date.now();
          reply.reasoningBox.classList.add('is-collapsed');  // 开始作答，收起思考
        }
        // 每收到一块就用最新的完整文本重新渲染整段。流式过程中 Markdown 是不完整的
        // （半截的 **、没闭合的代码块），但重新解析会自动"自愈"，不需要特殊处理
        reply.content.innerHTML = renderMarkdown(out.content);
      }

      if (out.usage) {
        renderUsage(reply.usage, out.usage);
      }
      if (out.content) {
        statusEl.textContent = '作答中…';
      }
      if (stickToBottom) {
        scrollToBottom();
      }
    }

    // 本轮结束。只放 out.content（正文），思考内容 out.reasoning 绝不入历史——
    // 它是模型的内部过程，回灌会污染后续推理且白烧 token。
    // 被拦截的轮次模型根本没被调用，也一并跳过，否则等于把服务端的话术冒充成模型说过的话
    if (!blocked) {
      commitTurn(prompt, out.content);
    }

    const total = Date.now() - startedAt;
    let summary = '完成，共 ' + total + ' ms';
    if (firstContentAt) {
      summary = '思考 ' + (firstContentAt - startedAt) + ' ms · 共 ' + total + ' ms';
    }
    statusEl.textContent = summary;
  } catch (err) {
    if (err.name === 'AbortError') {
      // 被中断时把已经收到的部分计入历史：模型确实产出了这些内容，如实记录
      if (!blocked) {
        commitTurn(prompt, out.content);
      }
      statusEl.textContent = '已停止生成（思考 ' + out.reasoning.length + ' 字，正文 ' +
        out.content.length + ' 字）';
    } else {
      reply.content.innerHTML = renderMarkdown(out.content) +
        '<p class="msg__error">[出错] ' + escapeHtml(String(err)) + '</p>';
      statusEl.textContent = '';
    }
  } finally {
    reply.content.classList.remove('msg__content--typing');
    controller = null;
    setGenerating(false);
    renderPayload();  // 本轮结束后刷新「累积的上下文」
    inputEl.focus();
  }
}

sendEl.addEventListener('click', send);

payloadToggleEl.addEventListener('click', () => {
  payloadEl.hidden = !payloadEl.hidden;
});

// 代码块的「复制」按钮是渲染出来的，且流式期间会被反复重建，
// 所以用事件委托统一处理，而不是每次渲染都去绑定
chatEl.addEventListener('click', (e) => {
  const button = e.target.closest('.md-copy');
  if (!button) {
    return;
  }
  const code = button.parentElement.querySelector('code');
  if (!code) {
    return;
  }
  navigator.clipboard.writeText(code.textContent).then(
    () => {
      button.textContent = '已复制';
      setTimeout(() => { button.textContent = '复制'; }, 1200);
    },
    () => {
      // 非安全上下文（例如用局域网 IP 访问）下剪贴板 API 不可用
      button.textContent = '复制失败';
      setTimeout(() => { button.textContent = '复制'; }, 1200);
    }
  );
});

stopEl.addEventListener('click', () => {
  // 中断 fetch，服务端会因此写失败，进而中断对上游模型的请求
  if (controller) {
    controller.abort();
  }
});

inputEl.addEventListener('input', autoResize);

inputEl.addEventListener('keydown', (e) => {
  // isComposing 用于跳过中文输入法选词时的回车，避免拼音还没上屏就把消息发出去
  if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
    e.preventDefault();
    send();
  }
});

inputEl.focus();
