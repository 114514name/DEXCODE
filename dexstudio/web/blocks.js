/* DexStudio 的**积木编辑器**(给零基础用户的 Scratch 式玩法)。
 *
 * 设计目标(用户原话:"完全就是把代码换了一种形式!必须修改"):
 *   · 不连线、不打字、不出现变量与表达式;
 *   · 每一段脚本 = 一个**帽子积木**(当游戏开始 / 每一帧 / 当按下空格)+ 一摞动作;
 *   · 积木读起来是中文句子,每个空都是**下拉**(数值也给预设);
 *   · "如果…那么"是 C 形积木,身子里还能放动作;
 *   · 点左栏的积木就加进去(不用拖),加错了用 ▲▼✕ 改。
 *
 * 所有语义都在 C(ds_blocks.c):句式、空、默认值、校验、代码生成。这里只画。
 */
'use strict';

const Blocks = (function () {
  let types = [];                 // blocks.types 的目录
  let typeMap = {};
  let scripts = [];               // 当前脚本(blocks.info)
  let sel = { script: 0, block: 0 };   // 选中的一段/一块(子积木往选中的"如果"里放)
  let mode = 'blocks';
  let lastSource = '';
  let previewTimer = 0;

  const el = (id) => document.getElementById(id);

  /* ---------- 小的 DOM 工具 ---------- */
  function mk(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  /* ---------- 句子渲染 ----------
   * 目录里的 text 形如 "让 {0} 往 {1} 走,速度 {2}",{n} 就是第 n 个空的控件。 */
  function sentence(t, props, onSet) {
    const wrap = mk('span', 'blk-sentence');
    const parts = String(t.text || '').split(/\{(\d+)\}/);
    parts.forEach((piece, i) => {
      if (i % 2 === 0) {
        if (piece) wrap.appendChild(mk('span', 'blk-text', piece));
        return;
      }
      const slot = (t.slots || [])[parseInt(piece, 10)];
      if (!slot) return;
      wrap.appendChild(slotControl(t, slot, props, onSet));
    });
    return wrap;
  }

  function slotOptions(slot, cur) {
    const list = (slot.options || []).slice();
    if (!list.some((o) => String(o.value) === String(cur)) && cur !== undefined
        && cur !== null && String(cur) !== '' && String(cur) !== '__custom__') {
      list.push({ value: String(cur), label: String(cur) });
    }
    return list;
  }

  function slotControl(t, slot, props, onSet) {
    const cur = props[slot.name] === undefined ? slot.value : props[slot.name];
    /* 数值槽:选"自定义…"就换成数字输入框 */
    if (slot.kind === 'num' && String(cur) !== '' && String(cur) !== '__custom__'
        && !(slot.options || []).some((o) => String(o.value) === String(cur))) {
      const inp = mk('input', 'blk-num');
      inp.type = 'number';
      inp.value = cur;
      inp.onchange = () => onSet(slot.name, inp.value === '' ? '0' : inp.value);
      return inp;
    }
    const sel = mk('select', 'blk-slot');
    slotOptions(slot, cur).forEach((o) => {
      const op = mk('option', null, o.label === undefined ? o.value : o.label);
      op.value = String(o.value);
      if (String(o.value) === String(cur)) op.selected = true;
      sel.appendChild(op);
    });
    sel.onchange = () => {
      if (sel.value === '__custom__') {
        const v = prompt('填一个数字(积木里也能自己定数值)', '100');
        if (v === null) { render(); return; }
        onSet(slot.name, v);
        return;
      }
      onSet(slot.name, sel.value);
    };
    return sel;
  }

  /* ---------- 左栏:积木盒子 ---------- */
  function renderPalette() {
    const box = el('block-palette');
    if (!box) return;
    box.innerHTML = '';
    const cats = {};
    types.forEach((t) => { (cats[t.cat] = cats[t.cat] || []).push(t); });
    ['事件', '动作', '如果'].forEach((cat) => {
      if (!cats[cat]) return;
      box.appendChild(mk('div', 'cat', cat === '事件' ? '① 起头(每段脚本的第一块)'
                             : (cat === '动作' ? '② 让它做点什么' : '③ 加个条件(里面还能放动作)')));
      cats[cat].forEach((t) => {
        const b = mk('button', 'blk-item cat-' + (t.cat === '事件' ? 'hat'
                                                 : (t.cat === '如果' ? 'if' : 'act')));
        /* 面板里用同一句话渲染,但空是"占位符"(不可改) */
        const parts = String(t.text).split(/\{(\d+)\}/);
        parts.forEach((piece, i) => {
          if (i % 2 === 0) { if (piece) b.appendChild(mk('span', null, piece)); return; }
          const slot = (t.slots || [])[parseInt(piece, 10)];
          if (!slot) return;
          const hint = (slot.options || []).find((o) => String(o.value) === String(slot.value));
          b.appendChild(mk('span', 'blk-hole',
                           '[' + ((hint && hint.label) || slot.value || '…') + ']'));
        });
        b.title = t.type;
        b.onclick = () => addBlock(t.type);
        box.appendChild(b);
      });
    });
  }

  /* ---------- 中间:脚本区 ---------- */
  function renderScripts() {
    const box = el('block-area');
    if (!box) return;
    box.innerHTML = '';
    if (!scripts.length) {
      const hint = mk('div', 'blk-empty');
      hint.appendChild(mk('p', 'hint',
        '还没有积木。点右边「一键示例」就能立刻有一个会动的小人,'));
      hint.appendChild(mk('p', 'hint',
        '或者从左边点「每一帧」再点动作积木,自己拼一段。'));
      box.appendChild(hint);
      return;
    }
    scripts.forEach((s) => {
      const card = mk('div', 'script');
      const t = typeMap[s.event] || { text: s.event, slots: [], cat: '事件' };
      const hat = mk('div', 'hat' + (sel.script === s.id && !sel.block ? ' sel' : ''));
      hat.appendChild(mk('span', 'hat-label', '▶'));
      hat.appendChild(sentence(t, s.props || {}, (name, value) => {
        setSlot(s.id, 0, name, value);
      }));
      const ops = mk('span', 'blk-ops');
      const del = mk('button', 'link', '删这段');
      del.onclick = () => removeScript(s.id);
      ops.appendChild(del);
      hat.appendChild(ops);
      hat.onclick = (ev) => {
        if (ev.target.tagName === 'SELECT' || ev.target.tagName === 'BUTTON') return;
        sel = { script: s.id, block: 0 };
        renderScripts();
      };
      card.appendChild(hat);
      card.appendChild(renderStack(s, s.blocks || [], 0));
      box.appendChild(card);
    });
  }

  function renderStack(s, list, parentBlock) {
    const stack = mk('div', 'stack');
    stack.dataset.script = String(s.id);
    stack.dataset.parent = String(parentBlock);
    if (!list.length) {
      stack.appendChild(mk('div', 'blk-drop',
                           parentBlock ? '(点左栏的动作积木,放进这个「如果」里)'
                                       : '(点左栏的动作积木,加到这里)'));
    }
    list.forEach((b, i) => {
      const t = typeMap[b.type];
      if (!t) return;
      const row = mk('div', 'block cat-' + (t.cat === '如果' ? 'if' : 'act')
                     + (sel.block === b.block_id ? ' sel' : ''));
      row.dataset.block = String(b.block_id);      /* 自检/调试用:认得出是哪一块 */
      row.dataset.type = b.type;
      row.appendChild(sentence(t, b.props || {}, (name, value) => {
        setSlot(s.id, b.block_id, name, value);
      }));
      const ops = mk('span', 'blk-ops');
      const up = mk('button', 'link', '▲');
      up.title = '上移';
      up.onclick = (ev) => { ev.stopPropagation(); moveBlock(s.id, b.block_id, -1); };
      const dn = mk('button', 'link', '▼');
      dn.title = '下移';
      dn.onclick = (ev) => { ev.stopPropagation(); moveBlock(s.id, b.block_id, 1); };
      const rm = mk('button', 'link', '✕');
      rm.title = '删掉这块';
      rm.onclick = (ev) => { ev.stopPropagation(); removeBlock(s.id, b.block_id); };
      ops.appendChild(up); ops.appendChild(dn); ops.appendChild(rm);
      row.appendChild(ops);
      row.onclick = (ev) => {
        if (ev.target.tagName === 'SELECT' || ev.target.tagName === 'BUTTON'
            || ev.target.tagName === 'INPUT') return;
        sel = { script: s.id, block: b.block_id };
        renderScripts();
      };
      if (t.body) {
        const wrap = mk('div', 'if-body');
        wrap.appendChild(mk('div', 'if-label', '那么:'));
        wrap.appendChild(renderStack(s, b.body || [], b.block_id));
        row.appendChild(wrap);
      }
      stack.appendChild(row);
    });
    return stack;
  }

  /* ---------- 代码预览(让用户看到"积木 → 代码"的对应关系,但不要求他读) ---------- */
  async function renderPreview() {
    const box = el('block-code');
    if (!box) return;
    if (mode !== 'blocks') return;
    try {
      const r = await ds('blocks.generate');
      lastSource = r.source || '';
      box.textContent = lastSource;
    } catch (e) {
      box.textContent = '(还没法生成:' + e.message + ')';
    }
  }
  function schedulePreview() {
    clearTimeout(previewTimer);
    previewTimer = setTimeout(renderPreview, 250);
  }

  /* ---------- 动作(全走 C 模型) ---------- */
  async function refresh() {
    try {
      if (!types.length) {
        types = await ds('blocks.types');
        typeMap = {};
        types.forEach((t) => { typeMap[t.type] = t; });
      }
      const info = await ds('blocks.info');
      scripts = info.scripts || [];
    } catch (e) {
      log('er', 'blocks.info: ' + e.message);
      scripts = [];
    }
    renderPalette();
    renderScripts();
    schedulePreview();
  }

  function targetScript() {
    if (sel.script) return sel.script;
    return scripts.length ? scripts[0].id : 0;
  }

  async function addBlock(type) {
    const t = typeMap[type];
    if (!t) return;
    try {
      if (t.cat === '事件') {                 /* 帽子积木 = 新开一段脚本 */
        const r = await call('blocks.script.add', { event: type }, '加一段脚本');
        sel = { script: r.id, block: 0 };
      } else {
        let sid = targetScript();
        if (!sid) {                            /* 还没有脚本:先建"每一帧" */
          const r = await call('blocks.script.add', { event: 'on_update' },
                               '先建一段「每一帧」');
          sid = r.id;
        }
        await call('blocks.add', { id: sid, type, in_block: sel.block || 0 },
                   '加积木');
        sel = { script: sid, block: 0 };
      }
      await refresh();
      toast('加好了:' + t.text.replace(/\{\d+\}/g, '…'), 'ok');
    } catch (e) { /* 已提示 */ }
  }

  async function setSlot(sid, bid, name, value) {
    try {
      await call('blocks.set', { id: sid, block_id: bid, name, value }, '改积木');
      await refresh();
    } catch (e) { /* 已提示 */ }
  }

  async function removeBlock(sid, bid) {
    try {
      await call('blocks.remove', { id: sid, block_id: bid }, '删积木');
      if (sel.block === bid) sel.block = 0;
      await refresh();
    } catch (e) { /* 已提示 */ }
  }

  async function moveBlock(sid, bid, dir) {
    try {
      await call('blocks.move', { id: sid, block_id: bid, dir }, '移动积木');
      await refresh();
    } catch (e) { /* 已提示 */ }
  }

  async function removeScript(sid) {
    if (!(await uiConfirm('删掉这一段积木?', '可以撤销', false))) return;
    try {
      await call('blocks.script.remove', { id: sid }, '删脚本');
      if (sel.script === sid) sel = { script: 0, block: 0 };
      await refresh();
    } catch (e) { /* 已提示 */ }
  }

  async function addScript() {
    const r = await uiDialog({
      title: '新的一段积木',
      hint: '一段脚本 = 一个"什么时候做" + 一摞"做什么"',
      fields: [{ name: 'event', label: '什么时候', type: 'select',
        options: types.filter((t) => t.cat === '事件').map((t) => ({
          value: t.type, label: t.text.replace(/\{\d+\}/g, '…') })),
        value: 'on_update' }],
      ok: '新建',
    });
    if (!r) return;
    try {
      const made = await call('blocks.script.add', { event: r.event }, '加一段脚本');
      sel = { script: made.id, block: 0 };
      await refresh();
    } catch (e) { /* 已提示 */ }
  }

  async function template(id) {
    try {
      await call('blocks.template', { id }, '一键示例');
      sel = { script: 0, block: 0 };
      await refresh();
      toast('示例积木加好了 —— 点顶栏「运行」看看效果', 'ok');
    } catch (e) { /* 已提示 */ }
  }

  async function generate() {
    try {
      const r = await call('blocks.generate', {}, '生成代码');
      lastSource = r.source || '';
      renderPreview();
      toast('已生成 scripts/logic.dex(编译/运行也会做这一步)', 'ok');
      return r;
    } catch (e) { return null; }
  }

  async function validate() {
    let r;
    try {
      r = await ds('blocks.validate');
    } catch (e) { return null; }
    showTab('build');
    const box = el('buildout');
    const parts = ['<div class="diag ' + (r.ok ? (r.errors ? 'err' : 'ok') : 'err') + '">'
      + (r.ok ? (r.warnings ? '积木能用,但有 ' + r.warnings + ' 条提醒'
                           : '积木检查通过:每一块都填好了')
              : '积木有 ' + r.errors + ' 处必须修的地方') + '</div>'];
    (r.issues || []).forEach((it) => {
      parts.push('<div class="diag ' + (it.level === 'warn' ? 'warn' : 'err') + '">'
        + (it.level === 'warn' ? '提醒 ' : '错误 ') + esc(it.msg) + '</div>');
    });
    box.innerHTML = parts.join('');
    if (!r.ok) toast('积木有 ' + r.errors + ' 处要修(见「编译」页签)', 'err');
    else toast('积木检查通过', 'ok');
    return r;
  }

  /* ---------- 模式(积木 / 节点) ----------
   * 注意:这里只切**视图**;写进 project.json 的 logic_mode 由右侧单选/调用方做
   * (否则页面自测一进来就把用户的项目偏好改了)。 */
  function setMode_(m) {
    mode = m === 'graph' ? 'graph' : 'blocks';
    document.body.classList.toggle('blocks-mode', mode === 'blocks');
    renderModeBadge();
    schedulePreview();
  }

  function renderModeBadge() {
    const b = el('block-mode-info');
    if (b) {
      b.textContent = mode === 'blocks'
        ? '当前用「积木」生成游戏逻辑'
        : '当前用「节点图」生成游戏逻辑(进阶)';
    }
  }

  function esc(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  return {
    refresh, renderPalette, renderScripts, renderPreview, setMode: setMode_,
    addScript, template, generate, validate,
    get scripts() { return scripts; },
    get types() { return types; },
    get mode() { return mode; },
    get source() { return lastSource; },
    stats: () => ({ scripts: scripts.length, types: types.length,
                    mode, blocks: scripts.reduce((n, s) => n + (s.blocks || []).length
                                                  + (s.blocks || []).reduce(
                                                      (k, b) => k + (b.body || []).length, 0),
                                                  0) }),
  };
})();

window.addEventListener('DOMContentLoaded', () => {
  if (document.getElementById('block-area')) Blocks.refresh();
});
