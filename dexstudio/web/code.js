/* DexStudio 的「代码」页签(B5):看脚本、编译、运行、点错误跳行。
 *
 * 高亮用自写的 highlight.js(决策 #9:不引第三方 JS);编译/运行的**逻辑在 C**
 * (build.compile / build.run / build.stop),这里只显示结果 —— 诊断的行列号是 C 侧
 * 解析出来的,所以"点一下跳到那一行"只是把 line 传给视口。
 */
'use strict';

const Code = (function () {
  let files = [];            // scripts/*.dex
  let current = '';          // 当前文件名
  let text = '';             // 当前文件内容
  let marks = {};            // 行号 → 1 error / 2 warning
  let lastBuild = null;

  function el(id) { return document.getElementById(id); }

  /* ---------- 文件列表 ---------- */

  async function refreshFiles(pick) {
    try {
      files = await ds('project.scripts');
    } catch (e) {
      files = [];
      log('er', 'project.scripts: ' + e.message);
    }
    const sel = el('code-file');
    sel.innerHTML = '';
    files.forEach((f) => {
      const o = document.createElement('option');
      o.value = f;
      o.textContent = f;
      sel.appendChild(o);
    });
    if (!files.length) {
      const o = document.createElement('option');
      o.value = '';
      o.textContent = '(没有脚本:先新建项目)';
      sel.appendChild(o);
      await show('');
      return;
    }
    const want = pick && files.indexOf(pick) >= 0 ? pick
      : (files.indexOf(current) >= 0 ? current : files[0]);
    sel.value = want;
    await show(want);
  }

  /* ---------- 显示一个文件 ---------- */

  async function show(name) {
    current = name || '';
    marks = {};
    if (!current) {
      text = '';
      Highlight.render(el('code'), '');
      el('code-info').textContent = '';
      return;
    }
    try {
      const r = await ds('file.read', { path: 'scripts/' + current });
      text = r.text || '';
    } catch (e) {
      text = '';
      log('er', 'file.read: ' + e.message);
    }
    /* 上一次编译的诊断如果就是这个文件,顺手标出来 */
    if (lastBuild && lastBuild.diag) {
      lastBuild.diag.forEach((d) => {
        if (d.file && d.file.replace(/\\/g, '/').endsWith('/scripts/' + current)) {
          marks[d.line] = d.level === 'error' ? 1 : 2;
        }
      });
    }
    Highlight.render(el('code'), text, marks);
    const lines = text ? text.split('\n').length : 0;
    el('code-info').textContent = current + ' · ' + lines + ' 行' +
      (codeOf(current) ? ' · 由逻辑图生成(改图,别改这里)' : '');
  }

  const codeOf = (name) => name === 'logic.dex';

  /* 跳到某一行(错误定位) */
  function goto(file, line) {
    const base = (file || '').replace(/\\/g, '/').split('/').pop();
    setMode('code').then(async () => {
      if (base && base !== current && files.indexOf(base) >= 0) await show(base);
      else await show(current);
      const node = el('code').querySelector('.cl[data-line="' + line + '"]');
      if (node) {
        node.scrollIntoView({ block: 'center' });
        node.classList.add('flash');
        setTimeout(() => node.classList.remove('flash'), 1200);
      }
    });
  }

  /* ---------- 编译 / 运行 ---------- */

  async function compile() {
    showTab('build');
    el('buildout').textContent = '编译中…';
    let r;
    try {
      r = await call('build.compile', {}, '编译');
    } catch (e) {
      el('buildout').innerHTML = '<div class="diag err">编译失败:' + esc(e.message) +
        '</div>';
      return null;
    }
    lastBuild = r;
    renderBuild(r);
    await show(current);           /* 重新标一遍诊断行 */
    return r;
  }

  async function run() {
    showTab('build');
    el('buildout').textContent = '编译中…';
    const b = await compile();
    if (!b || !b.ok) return null;
    try {
      const r = await call('build.run', { detach: 1 }, '运行');
      el('buildout').innerHTML += '<div class="diag ok">已启动游戏窗口(pid ' +
        r.pid + ',exe ' + esc(r.exe) + ')。关掉窗口或点「停止」结束。</div>';
      return r;
    } catch (e) {
      el('buildout').innerHTML += '<div class="diag err">运行失败:' +
        esc(e.message) + '</div>';
      return null;
    }
  }

  async function stop() {
    try {
      const r = await call('build.stop', {}, '停止');
      log('dim', r.stopped ? '已停止游戏进程' : '没有在运行的游戏');
    } catch (e) { /* 已提示 */ }
  }

  /* 运行状态轮询:游戏窗口关掉之后把按钮/输出更新一下(2 秒一次,很便宜) */
  async function poll() {
    try {
      const st = await ds('build.status');
      const running = !!st.running;
      el('btn-run').disabled = running;
      el('btn-stop').disabled = !running;
      el('code-info').title = running ? ('游戏运行中 pid=' + st.pid) : '';
    } catch (e) { /* 忽略 */ }
  }

  /* ---------- 输出面板 ---------- */

  function renderBuild(r) {
    const box = el('buildout');
    const parts = [];
    parts.push('<div class="diag ' + (r.ok ? 'ok' : 'err') + '">' +
      (r.ok ? '编译成功' : '编译失败') + ' · 退出码 ' + r.code +
      ' · 错误 ' + r.errors + ' · 警告 ' + r.warnings +
      (r.bytecode_exists ? ' · ' + esc(r.bytecode.replace(/\\/g, '/').split('/').pop())
                         : ' · 没有产出字节码') + '</div>');
    (r.diag || []).forEach((d) => {
      const base = (d.file || '').replace(/\\/g, '/').split('/').pop();
      parts.push('<div class="diag ' + (d.level === 'error' ? 'err' : 'warn') +
        ' jump" data-file="' + esc(base) + '" data-line="' + d.line + '">' +
        '[' + esc(d.phase) + '] ' + esc(base) + ':' + d.line + ':' + d.col + '  ' +
        esc(d.msg) + '</div>');
    });
    if (r.out) {
      parts.push('<div class="raw">' + esc(r.out) + '</div>');
    }
    box.innerHTML = parts.join('');
    box.querySelectorAll('.jump').forEach((n) => {
      n.onclick = () => goto(n.dataset.file, parseInt(n.dataset.line, 10));
    });
  }

  function esc(s) {
    return String(s === undefined || s === null ? '' : s)
      .replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;',
                                    '"': '&quot;' }[c]));
  }

  function init() {
    el('code-file').onchange = () => show(el('code-file').value);
    el('btn-code-reload').onclick = () => show(current);
    el('btn-code-compile').onclick = compile;
    el('btn-code-run').onclick = run;
    el('btn-code-stop').onclick = stop;
    setInterval(poll, 2000);
  }

  return {
    init, refreshFiles, show, compile, run, stop, goto, renderBuild,
    get text() { return text; },
    get current() { return current; },
    get files() { return files; },
    get lastBuild() { return lastBuild; },
    get marks() { return marks; },
  };
})();

window.addEventListener('DOMContentLoaded', () => {
  if (document.getElementById('code')) Code.init();
});
