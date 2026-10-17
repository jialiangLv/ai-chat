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
const sessionListEl = document.getElementById('session-list');
const newSessionEl = document.getElementById('new-session');
const authEl = document.getElementById('auth');
const authUsernameEl = document.getElementById('auth-username');
const authPasswordEl = document.getElementById('auth-password');
const authCodeEl = document.getElementById('auth-code');
const authSubmitEl = document.getElementById('auth-submit');
const authErrorEl = document.getElementById('auth-error');
const tabLoginEl = document.getElementById('tab-login');
const tabRegisterEl = document.getElementById('tab-register');
const userInfoEl = document.getElementById('user-info');
const logoutEl = document.getElementById('logout');
const sentBlockEl = document.getElementById('sent-block');
const sentTitleEl = document.getElementById('sent-title');
const sentTextEl = document.getElementById('sent-text');
const historyTitleEl = document.getElementById('history-title');
const historyTextEl = document.getElementById('history-text');

// 当前流式请求的中断句柄，非空表示正在生成，用于「停止生成」
let controller = null;

/* ---------- 会话与本地持久化 ----------
   会话只存在浏览器本地，服务端依旧无状态。storage key 带版本号：
   将来数据结构变了就换成 v2，旧数据自然失效——不用写迁移，也不会读到旧结构而崩 */
// 会话按用户名分开存：同一浏览器换账号时不会看到别人的对话。
// 带版本号：结构变了就换成 v2，旧数据自然失效，不用写迁移
const SESSIONS_KEY_PREFIX = 'ai-chat.sessions.v1.';

// 当前用户的存储键；未登录时为空串
function sessionsKey() {
  return authSession ? SESSIONS_KEY_PREFIX + authSession.username : '';
}

/* ---------- 登录状态 ----------
   登录成功后服务端发放一个随机会话令牌；这里只存令牌和展示用的用户名、额度。
   令牌是服务端唯一的凭据，可以随时被服务端删除（登出、过期） */
const AUTH_STORAGE_KEY = 'ai-chat.auth';
let authSession = null;  // { token, username, quota }

function loadAuthSession() {
  try {
    const data = JSON.parse(localStorage.getItem(AUTH_STORAGE_KEY) || 'null');
    return (data && data.token) ? data : null;
  } catch (e) {
    return null;
  }
}

function saveAuthSession(next) {
  authSession = next;
  if (next) {
    localStorage.setItem(AUTH_STORAGE_KEY, JSON.stringify(next));
  } else {
    localStorage.removeItem(AUTH_STORAGE_KEY);
  }
}

// 发给模型的上下文最多带多少条，与服务端的截断上限保持一致
const MAX_CONTEXT_MESSAGES = 20;

// 会话列表，元素形如：
// { id, title, createdAt, updatedAt, messages: [{ role, content, reasoning?, usage?, blocked? }] }
// messages 里存的是**界面上显示过的全部消息**，被拦截的轮次也在其中（打了 blocked 标记），
// 这样刷新后能完整还原；发给模型的上下文由它派生，见 contextMessages()
let sessions = [];
let activeId = null;

// 当前会话
function activeSession() {
  return sessions.find((s) => s.id === activeId) || null;
}

// 生成一个够用的唯一 id
function makeId() {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
}

// 把全部会话写回本地。容量超限之类的异常不能静默吞掉，否则用户以为存上了
function saveSessions() {
  const key = sessionsKey();
  if (!key) {
    return;  // 未登录不落盘
  }
  try {
    localStorage.setItem(key, JSON.stringify({ activeId, sessions }));
  } catch (err) {
    statusEl.textContent = '本地保存失败：' + err;
  }
}

// 从本地恢复会话，返回是否恢复成功
function loadSessions() {
  const key = sessionsKey();
  if (!key) {
    return false;
  }
  let data;
  try {
    data = JSON.parse(localStorage.getItem(key) || 'null');
  } catch (err) {
    return false;  // 数据损坏就当没有，重新开始
  }
  if (!data || !Array.isArray(data.sessions) || data.sessions.length === 0) {
    return false;
  }
  sessions = data.sessions;
  activeId = sessions.some((s) => s.id === data.activeId) ? data.activeId : sessions[0].id;
  return true;
}

// 派生出发给模型的上下文：先剔除被拦截的轮次，再截断到最近若干条。
// 截断后可能以 assistant 开头，要往前推到第一条 user，保证上下文从提问开始
function contextMessages(session) {
  if (!session) {
    return [];
  }
  const usable = session.messages.filter((m) => !m.blocked);
  const trimmed = usable.length > MAX_CONTEXT_MESSAGES
    ? usable.slice(-MAX_CONTEXT_MESSAGES)
    : usable;

  let start = 0;
  while (start < trimmed.length && trimmed[start].role !== 'user') {
    start += 1;
  }
  return trimmed.slice(start).map((m) => ({ role: m.role, content: m.content }));
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

/* ---------- 会话的增删与渲染 ---------- */

// 新建一个空会话并切过去
function newSession() {
  const session = {
    id: makeId(),
    title: '新对话',
    createdAt: Date.now(),
    updatedAt: Date.now(),
    messages: [],
  };
  sessions.unshift(session);
  activeId = session.id;
  lastRequest = null;  // 上一个会话的请求体与这里无关了
  renderSessionList();
  renderSession();
  saveSessions();
  inputEl.focus();
}

// 切换到指定会话。正在生成时先中断，避免两条流混在一起
function switchSession(id) {
  if (id === activeId) {
    return;
  }
  if (controller) {
    controller.abort();
  }
  activeId = id;
  lastRequest = null;  // 上一个会话的请求体与这里无关了
  renderSessionList();
  renderSession();
  saveSessions();
}

// 删除一个会话。删掉的若是当前会话，要顺带切走；一个都不剩就自动新建一个
function deleteSession(id) {
  const session = sessions.find((s) => s.id === id);
  if (!session || !confirm('删除对话「' + session.title + '」？')) {
    return;
  }
  if (id === activeId && controller) {
    controller.abort();
  }

  sessions = sessions.filter((s) => s.id !== id);
  if (!sessions.length) {
    newSession();
    return;
  }
  if (activeId === id) {
    activeId = sessions[0].id;
  }
  renderSessionList();
  renderSession();
  saveSessions();
}

// 重建侧边栏列表，最近更新的排在最前
function renderSessionList() {
  sessionListEl.replaceChildren();

  const ordered = [...sessions].sort((a, b) => b.updatedAt - a.updatedAt);
  for (const session of ordered) {
    const item = document.createElement('div');
    item.className = 'session-item' + (session.id === activeId ? ' is-active' : '');

    const title = document.createElement('button');
    title.type = 'button';
    title.className = 'session-item__title';
    title.textContent = session.title;
    title.title = session.title;  // 标题被截断时，悬停能看到完整的
    title.addEventListener('click', () => switchSession(session.id));

    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'session-item__delete';
    remove.textContent = '×';
    remove.title = '删除这个对话';
    remove.addEventListener('click', () => deleteSession(session.id));

    item.append(title, remove);
    sessionListEl.appendChild(item);
  }
}

// 按当前会话重建整个消息区。切换会话、刷新页面、删除会话时走这里；
// 正在流式生成时走增量追加，不调它
function renderSession() {
  chatEl.querySelectorAll('.msg').forEach((el) => el.remove());

  const session = activeSession();
  const messages = session ? session.messages : [];
  welcomeEl.hidden = messages.length > 0;

  for (const message of messages) {
    if (message.role === 'user') {
      appendMessage('user').content.textContent = message.content;
      continue;
    }

    const handle = appendMessage('assistant');
    if (message.reasoning) {
      handle.reasoningBox.hidden = false;
      handle.reasoningText.textContent = message.reasoning;
      handle.reasoningBox.classList.add('is-collapsed');  // 恢复时思考区默认收起
    }
    handle.content.innerHTML = renderMarkdown(message.content || '');
    if (message.usage) {
      renderUsage(handle.usage, message.usage);
    }
  }

  scrollToBottom();
  renderPayload();
}

// 把一轮问答写进指定会话。
// 用 sessionId 而不是「当前会话」：生成期间用户可能切走，回复必须落回发起它的那个会话。
// 被拦截的轮次也写进去（界面上确实发生过，刷新后应该还在），但两条都打标记，
// 派生上下文时会被整体剔除——只标 assistant 的话，留下那条 user 会变成孤儿
function commitTurn(sessionId, prompt, out, blocked) {
  const session = sessions.find((s) => s.id === sessionId);
  if (!session) {
    return;  // 会话已被删除
  }
  if (!out.content && !out.reasoning) {
    return;  // 什么都没产出，不留半截记录
  }

  session.messages.push({ role: 'user', content: prompt, blocked: blocked });
  session.messages.push({
    role: 'assistant',
    content: out.content,
    reasoning: out.reasoning,
    usage: out.usage,
    blocked: blocked,
  });

  // 第一轮提问兼作会话标题
  if (session.title === '新对话') {
    session.title = prompt.slice(0, 20);
  }
  session.updatedAt = Date.now();

  saveSessions();
  renderSessionList();  // 标题可能变了，排序也可能变
  if (session.id === activeId) {
    renderPayload();
  }
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
//   「累积的上下文」  —— 之后每一轮都会带上这些，由当前会话派生而来；
//                        被拦截的轮次、以及超长被截断的部分都不会出现在这里
function renderPayload() {
  if (lastRequest) {
    sentTitleEl.textContent =
      '本次发出的请求（' + lastRequest.messages.length + ' 条）';
    sentTextEl.textContent = JSON.stringify(lastRequest, null, 2);
    sentBlockEl.hidden = false;
  }

  const context = contextMessages(activeSession());
  historyTitleEl.textContent =
    '累积的上下文（' + context.length + ' 条）—— 下一轮会带上这些';
  historyTextEl.textContent = JSON.stringify({ messages: context }, null, 2);

  payloadToggleEl.textContent = '请求体 (' + context.length + ')';
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

  // 记下发起本轮时所在的会话：生成期间用户可能切走，回复必须落回这里而不是当前会话
  const sessionId = activeId;

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
    // messages = 本会话派生出的上下文 + 本轮输入，这就是「多轮上下文」的全部
    const requestBody = {
      model: 'deepseek-flash',
      stream: true,
      messages: [...contextMessages(activeSession()), { role: 'user', content: prompt }]
    };
    showRequestPayload(requestBody);

    // 会话令牌放在请求头里，不出现在请求体中——所以请求体面板里也看不到它
    const headers = { 'Content-Type': 'application/json' };
    if (authSession) {
      headers['Authorization'] = 'Bearer ' + authSession.token;
    }

    const resp = await fetch('/v1/chat/completions', {
      method: 'POST',
      headers: headers,
      body: JSON.stringify(requestBody),
      signal: controller.signal
    });

    if (!resp.ok) {
      // 401 要明确说出来并且把令牌栏翻出来，否则表现是"点了发送什么都没发生"
      if (resp.status === 401) {
        reply.content.textContent = '登录已过期，请重新登录。';
        statusEl.textContent = '401 未授权';
        logout();
        return;
      }
      if (resp.status === 403) {
        reply.content.textContent = '额度不足，请联系管理员。';
        statusEl.textContent = '403 额度不足';
        return;
      }
      if (resp.status === 413) {
        reply.content.textContent = '请求体太大，已被服务端拒绝。请把内容拆成几段发送。';
        statusEl.textContent = '413 请求体过大';
        return;
      }
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

    // 本轮结束，写进会话。写进会话的 assistant 里会带上 reasoning 供界面还原，
    // 但派生上下文时只取 content——思考内容是模型的内部过程，
    // 回灌给模型会污染后续推理，而且白烧 token
    commitTurn(sessionId, prompt, out, blocked);

    // 额度以服务端为准；这里先本地扣一下让显示即时更新，刷新页面时用 /api/me 校准
    if (out.usage && authSession) {
      authSession.quota = Math.max(0, authSession.quota - out.usage.total_tokens);
      saveAuthSession(authSession);
      renderUserInfo();
    }

    const total = Date.now() - startedAt;
    let summary = '完成，共 ' + total + ' ms';
    if (firstContentAt) {
      summary = '思考 ' + (firstContentAt - startedAt) + ' ms · 共 ' + total + ' ms';
    }
    statusEl.textContent = summary;
  } catch (err) {
    if (err.name === 'AbortError') {
      // 被中断时把已经收到的部分写进会话：模型确实产出了这些内容，如实记录
      commitTurn(sessionId, prompt, out, blocked);
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

/* ---------- 登录 / 注册 ---------- */

let authMode = 'login';  // 'login' | 'register'

// 显示登录遮罩，并按当前模式渲染
function showAuth() {
  authEl.hidden = false;
  authErrorEl.textContent = '';
  authCodeEl.hidden = authMode === 'login';
  authSubmitEl.textContent = authMode === 'login' ? '登录' : '注册';
  tabLoginEl.classList.toggle('is-active', authMode === 'login');
  tabRegisterEl.classList.toggle('is-active', authMode === 'register');
  authUsernameEl.focus();
}

// 顶栏显示当前用户与额度
function renderUserInfo() {
  userInfoEl.textContent = authSession
    ? authSession.username + ' · 额度 ' + authSession.quota
    : '';
}

// 从服务端刷新额度（本地只是展示，服务端才是权威值）
async function refreshQuota() {
  if (!authSession) {
    return;
  }
  try {
    const resp = await fetch('/api/me', {
      headers: { 'Authorization': 'Bearer ' + authSession.token }
    });
    if (!resp.ok) {
      return;
    }
    const data = await resp.json();
    authSession.username = data.username;
    authSession.quota = data.quota;
    saveAuthSession(authSession);
    renderUserInfo();
  } catch (e) {
    // 刷新失败不打扰用户
  }
}

// 加载某个用户的会话列表
function loadUserSessions() {
  sessions = [];
  activeId = null;
  if (loadSessions()) {
    renderSessionList();
    renderSession();
  } else {
    newSession();
  }
}

// 退出登录：清掉本地状态。服务端那条会话记录会在过期后自然失效
function logout() {
  saveAuthSession(null);
  sessions = [];
  activeId = null;
  renderSessionList();
  renderSession();
  renderUserInfo();
  authMode = 'login';
  showAuth();
}

// 提交登录或注册
async function submitAuth() {
  const username = authUsernameEl.value.trim();
  const password = authPasswordEl.value;
  authErrorEl.textContent = '';

  if (!username || !password) {
    authErrorEl.textContent = '请填写用户名和密码';
    return;
  }

  try {
    if (authMode === 'register') {
      const resp = await fetch('/api/register', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username, password, code: authCodeEl.value.trim() })
      });
      const data = await resp.json();
      if (!resp.ok) {
        authErrorEl.textContent = data.error || '注册失败';
        return;
      }
      // 注册成功：切回登录，密码清空让用户重新输入
      authMode = 'login';
      authPasswordEl.value = '';
      showAuth();
      authErrorEl.textContent = '注册成功，请登录';
      return;
    }

    const resp = await fetch('/api/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username, password })
    });
    const data = await resp.json();
    if (!resp.ok) {
      authErrorEl.textContent = data.error || '登录失败';
      return;
    }

    saveAuthSession({ token: data.token, username: data.username, quota: data.quota });
    authPasswordEl.value = '';
    authEl.hidden = true;
    renderUserInfo();
    loadUserSessions();
    refreshQuota();
  } catch (err) {
    authErrorEl.textContent = '请求失败：' + err;
  }
}

tabLoginEl.addEventListener('click', () => {
  authMode = 'login';
  showAuth();
});
tabRegisterEl.addEventListener('click', () => {
  authMode = 'register';
  showAuth();
});
authSubmitEl.addEventListener('click', submitAuth);
authPasswordEl.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    e.preventDefault();
    submitAuth();
  }
});
logoutEl.addEventListener('click', logout);

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

newSessionEl.addEventListener('click', newSession);

// 启动：恢复登录状态。没有会话就显示登录遮罩，登录后再加载该用户的对话
authSession = loadAuthSession();
if (authSession) {
  authEl.hidden = true;
  renderUserInfo();
  loadUserSessions();
  refreshQuota();
} else {
  sessions = [];
  activeId = null;
  renderSessionList();
  renderSession();
  showAuth();
}
inputEl.focus();
