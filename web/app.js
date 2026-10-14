/* ==============================================================================
   页面逻辑：把输入发到 POST /v1/chat/completions，并把流式返回的内容逐字渲染。
   流式解析的关键在于：网络一次 read 拿到的分块可能切断一个 UTF-8 字符，
   也可能只含半个 SSE 帧，所以必须先解码再缓存、按空行切出完整帧才解析。
   ============================================================================== */

const chatEl = document.getElementById('chat');
const welcomeEl = document.getElementById('welcome');
const statusEl = document.getElementById('status');
const inputEl = document.getElementById('input');
const sendEl = document.getElementById('send');
const stopEl = document.getElementById('stop');

// 当前流式请求的中断句柄，非空表示正在生成，用于「停止生成」
let controller = null;

// 判断消息区是否已经贴在底部，用于决定要不要跟着新内容自动滚动
function isNearBottom() {
  return chatEl.scrollHeight - chatEl.scrollTop - chatEl.clientHeight < 80;
}

// 滚到消息区底部
function scrollToBottom() {
  chatEl.scrollTop = chatEl.scrollHeight;
}

// 往消息区追加一条消息，返回可继续写入正文的元素
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
  row.appendChild(body);

  chatEl.appendChild(row);
  scrollToBottom();
  return body;
}

// 解析一帧 SSE 事件，返回其中 delta.content 拼成的文本（没有内容时返回空串）
function parseEvent(event) {
  let text = '';
  for (const line of event.split('\n')) {
    if (!line.startsWith('data:')) {
      continue;
    }
    const payload = line.slice(5).trim();
    if (payload === '[DONE]') {
      return text;
    }
    try {
      const delta = JSON.parse(payload).choices?.[0]?.delta?.content;
      if (delta) {
        text += delta;
      }
    } catch (e) {
      // JSON 还没收全，忽略，等下一块
    }
  }
  return text;
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

  appendMessage('user').textContent = prompt;
  const replyEl = appendMessage('assistant');
  replyEl.classList.add('msg__body--typing');

  const startedAt = Date.now();
  let firstChunkAt = 0;
  let replyText = '';

  setGenerating(true);
  statusEl.textContent = '生成中…';

  controller = new AbortController();
  try {
    const resp = await fetch('/v1/chat/completions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        model: 'deepseek-flash',
        stream: true,
        messages: [{ role: 'user', content: prompt }]
      }),
      signal: controller.signal
    });

    if (!resp.ok) {
      replyEl.textContent = '请求失败：HTTP ' + resp.status;
      statusEl.textContent = '';
      return;
    }

    const reader = resp.body.getReader();
    // stream 模式下，跨分块被切断的半个 UTF-8 字符会被解码器自己留住拼回
    const decoder = new TextDecoder('utf-8');
    let buffer = '';

    while (true) {
      const { value, done } = await reader.read();
      if (done) {
        break;
      }

      if (!firstChunkAt) {
        firstChunkAt = Date.now();
      }

      // 先判断用户有没有手动往上翻：翻了就别强行拽回底部
      const stickToBottom = isNearBottom();
      buffer += decoder.decode(value, { stream: true });

      // SSE 帧以空行分隔，先缓存再切，不能假设一次 read 就是完整一帧
      let sep;
      while ((sep = buffer.indexOf('\n\n')) >= 0) {
        const event = buffer.slice(0, sep);
        buffer = buffer.slice(sep + 2);
        replyText += parseEvent(event);
      }

      replyEl.textContent = replyText;
      if (stickToBottom) {
        scrollToBottom();
      }
    }

    statusEl.textContent =
      '首字 ' + (firstChunkAt - startedAt) + ' ms · 完成，共 ' + (Date.now() - startedAt) + ' ms';
  } catch (err) {
    if (err.name === 'AbortError') {
      statusEl.textContent =
        '已停止生成（已收到 ' + replyText.length + ' 字）';
    } else {
      replyEl.textContent += '\n[出错] ' + err;
      statusEl.textContent = '';
    }
  } finally {
    replyEl.classList.remove('msg__body--typing');
    controller = null;
    setGenerating(false);
    inputEl.focus();
  }
}

sendEl.addEventListener('click', send);

stopEl.addEventListener('click', () => {
  // 中断 fetch，服务端会因此写失败而提前结束本轮响应
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
