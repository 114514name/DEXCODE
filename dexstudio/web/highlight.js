/* DexStudio 自写的轻量代码高亮(决策 #9:不引第三方 JS)。
 *
 * 输入一段 DexLang 源码,输出带 <span class="tok-x"> 的 HTML(行号单独一列)。
 * 只做**词法级**着色:注释 / 字符串 / 数字 / 关键字 / 类型 / 调用 / 运算符。
 * 刻意不做语法分析 —— 高亮不需要,而"猜错了"的代价是颜色难看,不是功能错误。
 *
 * 与 dexide(旧 tkinter IDE)的着色保持一致的关键字表(见 dexlang/tokens.py)。
 */
'use strict';

const Highlight = (function () {
  const KEYWORDS = new Set([
    'let', 'if', 'else', 'while', 'func', 'return', 'print', 'true', 'false',
    'include', 'refer', 'extern', 'release', 'type',
  ]);
  const TYPES = new Set(['int', 'float', 'string', 'void', 'bool']);

  function esc(s) {
    return String(s).replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
  }

  /* 把一行切成 token。返回 [{t, k}] */
  function scanLine(line, state) {
    const out = [];
    let i = 0;
    while (i < line.length) {
      const c = line[i];
      /* 块注释状态跨行 */
      if (state.block) {
        const end = line.indexOf('*/', i);
        if (end < 0) { out.push({ t: line.slice(i), k: 'cmt' }); return out; }
        out.push({ t: line.slice(i, end + 2), k: 'cmt' });
        i = end + 2;
        state.block = false;
        continue;
      }
      /* 注释: `//` 与 `#` 到行尾;块注释见下面那一支 */
      if (c === '#' || (c === '/' && line[i + 1] === '/')) {
        out.push({ t: line.slice(i), k: 'cmt' });
        return out;
      }
      if (c === '/' && line[i + 1] === '*') {
        const end = line.indexOf('*/', i + 2);
        if (end < 0) {
          out.push({ t: line.slice(i), k: 'cmt' });
          state.block = true;
          return out;
        }
        out.push({ t: line.slice(i, end + 2), k: 'cmt' });
        i = end + 2;
        continue;
      }
      /* 字符串(支持 \" 转义) */
      if (c === '"') {
        let j = i + 1;
        while (j < line.length) {
          if (line[j] === '\\') { j += 2; continue; }
          if (line[j] === '"') { j++; break; }
          j++;
        }
        out.push({ t: line.slice(i, j), k: 'str' });
        i = j;
        continue;
      }
      /* 数字 */
      if (c >= '0' && c <= '9') {
        let j = i;
        while (j < line.length && /[0-9a-fA-FxX._]/.test(line[j])) j++;
        out.push({ t: line.slice(i, j), k: 'num' });
        i = j;
        continue;
      }
      /* 标识符 / 关键字 / 类型 / 调用 */
      if (/[A-Za-z_\u4e00-\u9fa5]/.test(c)) {
        let j = i;
        while (j < line.length && /[A-Za-z0-9_\u4e00-\u9fa5]/.test(line[j])) j++;
        const word = line.slice(i, j);
        let k = 'id';
        if (KEYWORDS.has(word)) k = 'kw';
        else if (TYPES.has(word)) k = 'ty';
        else if (line[j] === '(') k = 'fn';
        out.push({ t: word, k });
        i = j;
        continue;
      }
      /* 运算符/括号/标点 */
      if (/[+\-*/%=<>!&|^~?:;,.(){}\[\]]/.test(c)) {
        let j = i;
        while (j < line.length && /[+\-*/%=<>!&|^~?:]/.test(line[j])) j++;
        if (j === i) j = i + 1;
        out.push({ t: line.slice(i, j), k: 'op' });
        i = j;
        continue;
      }
      /* 空白与其它原样输出 */
      let j = i;
      while (j < line.length && !/[A-Za-z0-9_"#+\-*/%=<>!&|^~?:;,.(){}\[\]\u4e00-\u9fa5]/
        .test(line[j])) j++;
      if (j === i) j = i + 1;
      out.push({ t: line.slice(i, j), k: '' });
      i = j;
    }
    return out;
  }

  /* 整段源码 → 每行一个 HTML 字符串数组 */
  function lines(src) {
    const state = { block: false };
    return String(src === undefined || src === null ? '' : src)
      .replace(/\r\n?/g, '\n')
      .split('\n')
      .map((line) => scanLine(line, state)
        .map((tk) => (tk.k ? '<span class="tok-' + tk.k + '">' + esc(tk.t) + '</span>'
                           : esc(tk.t)))
        .join(''));
  }

  /* 渲染进一个 <pre>:左边行号列,右边代码;可以给某一行加高亮 */
  function render(pre, src, markLine) {
    const ls = lines(src);
    const marks = markLine || {};
    const html = ls.map((h, i) => {
      const n = i + 1;
      const cls = marks[n] ? ' cline' + (marks[n] === 2 ? ' cline-warn' : '') : '';
      return '<div class="cl' + cls + '" data-line="' + n + '">' +
        '<span class="ln">' + n + '</span>' + (h || ' ') + '</div>';
    }).join('');
    pre.innerHTML = html;
  }

  return { lines, render, KEYWORDS: Array.from(KEYWORDS) };
})();
