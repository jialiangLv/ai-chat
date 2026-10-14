/* ==============================================================================
   极简 Markdown 渲染器。把模型输出的 Markdown 文本转成安全的 HTML 字符串。

   安全铁律：**先把 HTML 转义，再做 Markdown 变换**，而且变换只产出白名单标签。
   顺序绝不能反——
     先变换再转义：会把我们自己生成的 <strong> 也转义掉，渲染直接失效
     先转义再变换：模型给的任何 HTML 都已被中和成纯文本，安全性是构造出来的
   因此本文件里所有输出到 HTML 的文本，都必然经过 escapeHtml。

   只支持模型实际会吐的常用子集；不认识的语法原样显示成纯文本，宁可不渲染也不渲染错。

   「先转义再变换」的代价，改代码时要记住：
   块级解析看到的已经是转义后的文本，所以凡是标记符本身属于 HTML 特殊字符的，
   都要按转义后的形式去匹配。目前只有引用块受影响——它的 > 此时已经是 &gt;。
   其余标记符（# - * ` [ ] ( )）都不是 HTML 特殊字符，不受影响。
   ============================================================================== */

// 把文本里的 HTML 特殊字符转义成实体。& 必须最先替换，否则会把后面生成的实体再转义一次
function escapeHtml(text) {
  return String(text)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

// 代码块的语言标识只保留常见字符，避免拼进 class 属性时出意外
function sanitizeLang(lang) {
  return lang.replace(/[^a-zA-Z0-9+#._-]/g, '').slice(0, 20);
}

// 渲染一行内的语法（行内代码、链接、粗体、斜体）。入参必须是**已转义**的文本
function renderInline(escaped) {
  // 先把行内代码整段摘出来单独处理，否则里面的 * 会被当成粗体标记
  return escaped
    .split(/(`[^`\n]*`)/)
    .map((part) => {
      if (part.length >= 2 && part.startsWith('`') && part.endsWith('`')) {
        return '<code>' + part.slice(1, -1) + '</code>';
      }
      return part
        // 链接：只认 http/https，从根上排掉 javascript: 之类的伪协议
        .replace(
          /\[([^\]\n]*)\]\((https?:\/\/[^\s)]+)\)/g,
          '<a href="$2" target="_blank" rel="noopener noreferrer">$1</a>'
        )
        .replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>')
        .replace(/(^|[^*\w])\*([^*\n]+)\*/g, '$1<em>$2</em>');
    })
    .join('');
}

// 判断一行是不是块级语法的开头，用于决定普通段落在哪里断开。
// 入参同样是**已转义**的文本，所以引用块这一项要匹配 &gt;
function isBlockStart(line) {
  return (
    /^\s*```/.test(line) ||
    /^\s*#{1,6}\s/.test(line) ||
    /^\s*[-*+]\s/.test(line) ||
    /^\s*\d+\.\s/.test(line) ||
    /^\s*&gt;/.test(line) ||
    /^\s*(---|\*\*\*)\s*$/.test(line)
  );
}

// 把一段 Markdown 文本渲染成 HTML。流式过程中可以反复用最新的完整文本调用它，
// 语法不完整时会退化成原样显示，等标记闭合后自然变正确——不需要特殊处理
function renderMarkdown(text) {
  const lines = escapeHtml(text).split('\n');
  const out = [];
  let i = 0;
  let listOpen = null;  // 记录当前打开的列表类型，用于合并连续的列表项

  // 列表结束时要补上闭合标签
  function closeList() {
    if (listOpen) {
      out.push('</' + listOpen + '>');
      listOpen = null;
    }
  }

  while (i < lines.length) {
    const line = lines[i];

    // 围栏代码块：从 ``` 读到下一个 ```，未闭合时就把剩下的全当代码
    const fence = line.match(/^\s*```(\S*)\s*$/);
    if (fence) {
      closeList();
      const code = [];
      i += 1;
      while (i < lines.length && !/^\s*```\s*$/.test(lines[i])) {
        code.push(lines[i]);
        i += 1;
      }
      i += 1;  // 跳过收尾的 ```

      const lang = sanitizeLang(fence[1]);
      out.push(
        '<div class="md-code">' +
          '<button class="md-copy" type="button">复制</button>' +
          '<pre><code' + (lang ? ' class="language-' + lang + '"' : '') + '>' +
          code.join('\n') +
          '</code></pre>' +
        '</div>'
      );
      continue;
    }

    // 水平线
    if (/^\s*(---|\*\*\*)\s*$/.test(line)) {
      closeList();
      out.push('<hr>');
      i += 1;
      continue;
    }

    // 标题：原样用 # 的层级会让 h1 比页面标题还大，所以整体降三级并封顶到 h5
    const heading = line.match(/^(#{1,6})\s+(.*)$/);
    if (heading) {
      closeList();
      const level = Math.min(heading[1].length + 2, 5);
      out.push('<h' + level + '>' + renderInline(heading[2]) + '</h' + level + '>');
      i += 1;
      continue;
    }

    // 引用块：把连续的 > 行合并成一个块。
    // 注意匹配的是 &gt; 而不是 >——文本在进入本函数前已整体转义，此时所有 > 都成了 &gt;。
    // 这是「先转义再变换」的必然结果：只有 > 属于 HTML 特殊字符，其余标记符不受影响
    if (/^\s*&gt;/.test(line)) {
      closeList();
      const quote = [];
      while (i < lines.length && /^\s*&gt;/.test(lines[i])) {
        quote.push(lines[i].replace(/^\s*&gt;\s?/, ''));
        i += 1;
      }
      out.push('<blockquote>' + quote.map(renderInline).join('<br>') + '</blockquote>');
      continue;
    }

    // 无序列表
    const bullet = line.match(/^\s*[-*+]\s+(.*)$/);
    if (bullet) {
      if (listOpen !== 'ul') {
        closeList();
        out.push('<ul>');
        listOpen = 'ul';
      }
      out.push('<li>' + renderInline(bullet[1]) + '</li>');
      i += 1;
      continue;
    }

    // 有序列表
    const numbered = line.match(/^\s*\d+\.\s+(.*)$/);
    if (numbered) {
      if (listOpen !== 'ol') {
        closeList();
        out.push('<ol>');
        listOpen = 'ol';
      }
      out.push('<li>' + renderInline(numbered[1]) + '</li>');
      i += 1;
      continue;
    }

    // 空行
    if (/^\s*$/.test(line)) {
      closeList();
      i += 1;
      continue;
    }

    // 普通段落：把连续的非空行合成一段，行内换行用 <br> 保留
    closeList();
    const paragraph = [];
    while (i < lines.length && !/^\s*$/.test(lines[i]) && !isBlockStart(lines[i])) {
      paragraph.push(lines[i]);
      i += 1;
    }
    out.push('<p>' + paragraph.map(renderInline).join('<br>') + '</p>');
  }

  closeList();
  return out.join('');
}
