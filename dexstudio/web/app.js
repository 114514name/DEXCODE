/* DexStudio 前端外壳(B3)。
 *
 * 与 C 宿主的唯一通道是 WebView2 的 postMessage:
 *   JS → C:  window.chrome.webview.postMessage({id, cmd, args})
 *   C → JS:  message 事件里的 e.data
 * 用 id 做请求/响应配对(ds() 返回 Promise)。**不开窗口也能验证**同样的命令:
 *   dexstudio.exe --command '{"cmd":"app.info"}'
 *
 * 分工(见 docs/DEXGAME_DESIGN.md §9.1 决策 #10):
 *   - C(模型层):项目 / 场景 / 撤销 / 字段读写 / 瓦片数据 / 离屏渲染
 *   - 这里(视图层):只做"把状态画出来"和"把用户动作翻译成命令"
 * 所以本文件里不该出现任何业务规则(比如"哪个字段该不该存盘")。
 */
'use strict';

const $ = (id) => document.getElementById(id);

/* ------------------------------------------------------------ 桥与 RPC */

const bridge = window.__ds_bridge || null;

let rpcSeq = 0;
const pending = new Map();
let logLines = 0;

function log(cls, text) {
  const box = $('log');
  if (!box) return;
  const el = document.createElement('div');
  el.className = 'line ' + cls;
  el.textContent = text;
  box.appendChild(el);
  /* 只留最近 500 行,免得长时间运行把内存吃满 */
  if (++logLines > 500) { box.removeChild(box.firstChild); logLines--; }
  box.scrollTop = box.scrollHeight;
}

function quiet() { return !!DS.quietLog; }

/* 唯一的 RPC 入口。超时 8 秒(离屏渲染 + 场景存盘偶尔会久一点)。 */
function ds(cmd, args) {
  return new Promise((resolve, reject) => {
    if (!bridge) { reject(new Error('没有消息桥(未在 WebView2 中运行)')); return; }
    const id = ++rpcSeq;
    const t = setTimeout(() => {
      pending.delete(id);
      reject(new Error('超时:' + cmd));
    }, 8000);
    pending.set(id, {
      resolve: (v) => { clearTimeout(t); resolve(v); },
      reject: (e) => { clearTimeout(t); reject(e); },
    });
    const payload = { id, cmd, args: args || {} };
    if (!quiet()) log('tx', '→ ' + JSON.stringify(payload));
    bridge.postMessage(payload);
  });
}

/* 带错误提示的调用:失败时写进输出面板并抛出,让调用方决定要不要继续 */
async function call(cmd, args, what) {
  try {
    return await ds(cmd, args);
  } catch (e) {
    log('er', (what || cmd) + ' 失败:' + e.message);
    throw e;
  }
}

if (bridge) {
  bridge.addEventListener('message', (ev) => {
    const m = ev.data || {};
    if (!quiet()) log(m.ok ? 'rx' : 'er', '← ' + JSON.stringify(m));
    if (m.cmd === 'ui.eval' && m.args) { log('dim', 'eval: ' + m.args.result); return; }
    const p = pending.get(m.id);
    if (!p) return;   /* 宿主主动推的消息(如 ui.ready 的回执) */
    pending.delete(m.id);
    if (m.ok) p.resolve(m.result);
    else p.reject(new Error(m.error || '未知错误'));
  });
}

/* ------------------------------------------------------------ 全局状态 */

const DS = {
  info: null,          // app.info 的结果
  schema: [],          // comp.schema
  entities: [],        // entity.list
  outline: [],         // scene.outline(世界包围盒)
  tilemap: null,       // 选中实体的 tilemap.info
  sel: [],             // 选中实体 id 数组(顺序即点选顺序)
  tool: 'select',      // select | brush
  grid: 16,
  showGrid: true,
  snap: true,
  paintDrag: true,
  erase: false,
  tile: -1,            // 刷子当前图块
  view: { on: true, x: 0, y: 0, zoom: 1 },
  vw: 1024, vh: 640,   // 引擎离屏尺寸(渲染的"屏幕")
  seq: 0,
  names: {},           // id → 名字(撤销后重新选中用)
  pendingPaint: [],    // 拖动刷子时攒下的格子(松手时一条命令提交)
  quietLog: false,
  busy: false,         // 正在跑一批命令(拖拽中),抑制重复刷新
};

const byId = (id) => DS.entities.find((e) => e.id === id) || null;
const outlineOf = (id) => DS.outline.find((o) => o.id === id) || null;
const selected = () => DS.sel.slice();

/* ------------------------------------------------------------ 刷新 */

/* 一次把界面重画一遍。**所有**改动之后都走它:状态只有 C 模型一份,
 * 前端不做增量推断(撤销/重做会让实体 id 全变,增量更新必然出错)。 */
async function refresh(opts) {
  const o = opts || {};
  if (DS.busy && !o.force) return;
  let info;
  try {
    info = await ds('app.info');
    DS.schema = await ds('comp.schema');
    DS.entities = await ds('entity.list');
    DS.outline = await ds('scene.outline');
  } catch (e) {
    log('er', '刷新失败:' + e.message);
    return;
  }
  DS.info = info;
  DS.vw = info.view_w || 1024;
  DS.vh = info.view_h || 640;
  if (info.view) DS.view = info.view;
  /* 选择里已经失效的 id 丢掉 */
  const alive = new Set(DS.entities.map((e) => e.id));
  DS.sel = DS.sel.filter((id) => alive.has(id));
  renderTopbar();
  renderTree();
  renderLayers();
  await renderInspector();
  await updatePalette();
  if (!o.noRender) await renderView();
}

function renderTopbar() {
  const info = DS.info || {};
  $('version').textContent = info.version || '';
  const st = $('status');
  if (info.engine) {
    st.textContent = (info.dirty ? '● 未保存 · ' : '') +
      (info.root ? info.root.split(/[\\/]/).pop() : '(未打开项目)') +
      ' · ' + DS.entities.length + ' 实体 · 撤销 ' + (info.undo || 0);
    st.className = 'status ok';
  } else {
    st.textContent = '引擎不可用:' + (info.engine_error || '');
    st.className = 'status err';
  }
  $('scene-path').textContent = info.scene
    ? info.scene.split(/[\\/]/).pop() + '   ' + (info.root || '')
    : '未打开项目';
  $('obj-count').textContent = DS.entities.length;
  $('btn-undo').disabled = !(info.undo > 0);
  $('btn-redo').disabled = !(info.redo > 0);
  $('vp-hint').hidden = !!info.root;
  $('tile-pick').textContent = DS.tile < 0 ? '空 (-1)' : ('图块 ' + DS.tile);
  $('viewinfo').textContent =
    '视口 ' + Math.round(DS.view.x) + ',' + Math.round(DS.view.y) +
    ' · ' + (Math.round(DS.view.zoom * 100) / 100) + '× · ' +
    DS.vw + '×' + DS.vh +
    (DS.sel.length ? ' · 选中 ' + DS.sel.length : '');
}

/* 离屏渲染一帧,画到视口画布上 */
async function renderView() {
  try {
    const r = await ds('scene.render', {
      x: DS.view.x, y: DS.view.y, zoom: DS.view.zoom,
    });
    DS.seq = r.seq;
    if (typeof Viewport !== 'undefined') await Viewport.show(r.seq);
  } catch (e) {
    log('er', '渲染失败:' + e.message);
  }
}

/* 一次编辑之后的统一收尾:刷新 + 场景标记为脏 */
function afterEdit(msg) {
  if (msg) log('dim', msg);
  return refresh();
}

/* ------------------------------------------------------------ 选中 */

function select(ids, additive) {
  const set = additive ? new Set(DS.sel) : new Set();
  (ids || []).forEach((id) => {
    if (additive && set.has(id)) set.delete(id);
    else set.add(id);
  });
  DS.sel = Array.from(set);
  renderTree();
  renderLayers();
  renderInspector();
  updatePalette();
  renderTopbar();
  if (typeof Viewport !== 'undefined') Viewport.drawOverlay();
}

function isSelected(id) { return DS.sel.indexOf(id) >= 0; }

/* 撤销/重做之后实体 id 会变(场景是整份重建的),所以先记住名字,
 * 回来之后按名字重新选中 —— 否则用户视角是"撤销一下选中项就没了"。 */
async function withNamesThen(fn) {
  const names = DS.sel.map((id) => (byId(id) || {}).name).filter(Boolean);
  await fn();
  await refresh();
  if (names.length) {
    const ids = DS.entities.filter((e) => names.indexOf(e.name) >= 0).map((e) => e.id);
    select(ids.filter((id, i) => ids.indexOf(id) === i));
  }
}

/* ------------------------------------------------------------ 顶栏动作 */

async function doAddEntity(name) {
  const r = await call('entity.add', { name: name || 'obj' }, '添加实体');
  await refresh();
  select([r.id]);
  log('dim', '新实体 ' + r.name + ' #' + r.id);
}

async function doDelete() {
  if (!DS.sel.length) { log('warn', '没有选中实体'); return; }
  for (const id of selected()) {
    try { await call('entity.remove', { id }, '删除实体'); }
    catch (e) { break; }
  }
  DS.sel = [];
  await afterEdit('删除 ' + DS.sel.length + ' 个实体');
}

async function doDuplicate(ids) {
  const list = ids && ids.length ? ids : selected();
  if (!list.length) { log('warn', '没有选中实体'); return; }
  const r = await call('entity.duplicate', { ids: list, dx: 8, dy: 8 }, '复制实体');
  await refresh();
  select(r.ids);
  log('dim', '复制出 ' + r.ids.length + ' 个实体');
}

async function doCopy() {
  if (!DS.sel.length) { log('warn', '没有选中实体'); return; }
  const r = await call('entity.copy', { ids: selected() }, '复制到剪贴板');
  log('dim', '已复制 ' + r.count + ' 个实体到剪贴板');
}

async function doPaste() {
  try {
    const r = await call('entity.paste', { dx: 16, dy: 16 }, '粘贴');
    await refresh();
    select(r.ids);
    log('dim', '粘贴出 ' + r.ids.length + ' 个实体');
  } catch (e) { /* 已在 call 里提示 */ }
}

/* 方向键微调:一次一条撤销记录(comp.set_many) */
async function nudge(dx, dy) {
  if (!DS.sel.length) return;
  const items = [];
  DS.sel.forEach((id) => {
    const e = byId(id);
    if (!e) return;
    items.push({ id, comp: 'transform', field: 'x', value: e.x + dx });
    items.push({ id, comp: 'transform', field: 'y', value: e.y + dy });
  });
  if (!items.length) return;
  try {
    await call('comp.set_many', { items }, '微调位置');
    await refresh();
  } catch (e) { /* 已提示 */ }
}

/* ------------------------------------------------------------ 输出面板页签 */

function showTab(which) {
  const map = { log: 'log', self: 'selfcheck', sel: 'selinfo' };
  Object.keys(map).forEach((k) => { $(map[k]).hidden = (k !== which); });
  document.querySelectorAll('.tabs .tab').forEach((b) => {
    b.classList.toggle('on', b.dataset.tab === which);
  });
  if (which === 'sel') renderSelInfo();
}

function renderSelInfo() {
  const box = $('selinfo');
  const rows = DS.sel.map((id) => {
    const e = byId(id) || {};
    const o = outlineOf(id) || {};
    return '#' + id + '  ' + (e.name || '(无名)') +
      '\n    位置 ' + fmt(e.x) + ',' + fmt(e.y) +
      '  世界 ' + fmt(o.wx) + ',' + fmt(o.wy) +
      '  包围盒 ' + fmt(o.w) + '×' + fmt(o.h) + ' (' + (o.kind || '?') + ')' +
      '\n    组件 ' + ((e.comps || []).join(', ') || '(无)');
  });
  box.textContent = rows.length ? rows.join('\n\n') : '(没有选中实体)';
}

const fmt = (v) => (v === undefined || v === null) ? '-' :
  (Math.abs(v - Math.round(v)) < 1e-6 ? String(Math.round(v)) : v.toFixed(3));

/* ------------------------------------------------------------ 自检 */

/* 走一遍"读状态 → 建实体 → 读回 → 撤销 → 重做",把结果打在页面上。
 * 与 dexstudio.exe --selftest(无窗口)跑的是同一批命令 —— 同一套通道。 */
async function selfcheck() {
  showTab('self');
  const out = $('selfcheck');
  const lines = [];
  const t = (name, ok, extra) => {
    lines.push((ok ? '  PASS  ' : '  FAIL  ') + name + (extra ? '  ' + extra : ''));
    out.textContent = lines.join('\n');
  };
  const wasQuiet = DS.quietLog;
  DS.quietLog = true;
  try {
    const info = await ds('app.info');
    t('app.info', !!info.version, 'v' + info.version);
    t('引擎连接', !!info.engine, info.engine ? '' : info.engine_error);
    const schema = await ds('comp.schema');
    t('组件自省', (schema || []).length > 0, (schema || []).length + ' 种');
    const view = await ds('view.set', { x: 0, y: 0, zoom: 1 });
    t('view.set', view.on === true, 'zoom=' + view.zoom);
    const render = await ds('scene.render');
    t('scene.render', render.seq > 0 && render.w > 0,
      render.w + '×' + render.h + ' seq=' + render.seq);
    const outline = await ds('scene.outline');
    t('scene.outline 是数组', Array.isArray(outline), (outline || []).length + ' 个');
    const before = (await ds('entity.list')).length;
    const added = await ds('entity.add', { name: '__selfcheck__' });
    t('entity.add', !!added.id, 'id=' + added.id);
    await ds('comp.add', { id: added.id, comp: 'sprite' });
    await ds('comp.set', { id: added.id, comp: 'sprite', field: 'sw', value: 12 });
    const dup = await ds('entity.duplicate', { id: added.id, dx: 4, dy: 4 });
    t('entity.duplicate', dup.ids.length === 1, 'id=' + dup.ids[0]);
    const dupInfo = await ds('entity.get', { id: dup.ids[0] });
    t('复制带上组件字段', dupInfo.comps.sprite.sw === 12,
      'sw=' + dupInfo.comps.sprite.sw);
    await ds('entity.copy', { id: added.id });
    const pasted = await ds('entity.paste', { dx: 1, dy: 1 });
    t('entity.paste', pasted.ids.length === 1);
    const many = await ds('comp.set_many', {
      items: [
        { id: added.id, comp: 'transform', field: 'x', value: 42 },
        { id: added.id, comp: 'transform', field: 'y', value: 43 },
      ],
    });
    t('comp.set_many', many.count === 2);
    const got2 = await ds('entity.get', { id: added.id });
    t('comp.set_many 生效', got2.comps.transform.y === 43,
      'y=' + got2.comps.transform.y);
    const grid = await ds('tilemap.info', { id: 999999 }).then(() => false)
      .catch((e) => /没有 tilemap|不存在/.test(e.message));
    t('tilemap.info 错误带原因', grid === true);
    let n = (await ds('entity.list')).length;
    for (let i = 0; i < 6; i++) await ds('undo');
    const back = (await ds('entity.list')).length;
    t('连续撤销回到 ' + before + ' 个', back === before, back + ' 个');
    for (let i = 0; i < 6; i++) await ds('redo');
    const again = (await ds('entity.list')).length;
    t('重做回到 ' + n + ' 个', again === n, again + ' 个');
    for (let i = 0; i < 6; i++) await ds('undo');
    t('清理干净', (await ds('entity.list')).length === before);
    const bad = await ds('entity.get', { id: 999999 }).then(() => false)
      .catch((e) => /不存在/.test(e.message));
    t('无效实体带得出原因', bad === true);
    const bad2 = await ds('nosuchcmd').then(() => false)
      .catch((e) => /未知命令/.test(e.message));
    t('未知命令带得出原因', bad2 === true);
  } catch (e) {
    t('自检中断', false, e.message);
  }
  DS.quietLog = wasQuiet;
  const fails = lines.filter((l) => l.indexOf('FAIL') >= 0).length;
  lines.push(fails ? '结果:' + fails + ' 项失败' : '结果:全部通过(' + lines.length + ' 项)');
  out.textContent = lines.join('\n');
  await refresh();
}

/* ------------------------------------------------------------ 快捷键 */

function wireKeys() {
  window.addEventListener('keydown', (ev) => {
    const tag = (ev.target.tagName || '').toLowerCase();
    const typing = tag === 'input' || tag === 'select' || tag === 'textarea';
    const ctrl = ev.ctrlKey || ev.metaKey;
    if (ctrl && ev.key.toLowerCase() === 's') {
      ev.preventDefault();
      saveAll();
      return;
    }
    if (ctrl && ev.key.toLowerCase() === 'z') { ev.preventDefault(); undoRedo('undo'); return; }
    if (ctrl && (ev.key.toLowerCase() === 'y')) { ev.preventDefault(); undoRedo('redo'); return; }
    if (ctrl && ev.key.toLowerCase() === 'd') { ev.preventDefault(); doDuplicate(); return; }
    if (ctrl && ev.key.toLowerCase() === 'c') { ev.preventDefault(); doCopy(); return; }
    if (ctrl && ev.key.toLowerCase() === 'v') { ev.preventDefault(); doPaste(); return; }
    if (ctrl && ev.key.toLowerCase() === 'a') {
      ev.preventDefault();
      select(DS.entities.map((e) => e.id));
      return;
    }
    if (typing) return;
    if (ev.key === 'Delete' || ev.key === 'Backspace') { ev.preventDefault(); doDelete(); return; }
    if (ev.key === 'Escape') { select([]); return; }
    if (ev.key === '1') { setTool('select'); return; }
    if (ev.key === '2') { setTool('brush'); return; }
    if (ev.key === 'f' || ev.key === 'F') { fitView(); return; }
    const step = ev.shiftKey ? DS.grid : 1;
    if (ev.key === 'ArrowLeft') { ev.preventDefault(); nudge(-step, 0); }
    if (ev.key === 'ArrowRight') { ev.preventDefault(); nudge(step, 0); }
    if (ev.key === 'ArrowUp') { ev.preventDefault(); nudge(0, -step); }
    if (ev.key === 'ArrowDown') { ev.preventDefault(); nudge(0, step); }
  });
}

/* ------------------------------------------------------------ 视图动作 */

async function setView(part) {
  Object.assign(DS.view, part);
  DS.view.on = true;
  if (DS.view.zoom <= 0.01) DS.view.zoom = 0.01;
  try {
    await ds('view.set', DS.view);
    await renderView();
  } catch (e) { log('er', '缩放失败:' + e.message); }
  renderTopbar();
}

function setTool(t) {
  DS.tool = t;
  $('tool-select').classList.toggle('on', t === 'select');
  $('tool-brush').classList.toggle('on', t === 'brush');
  $('brush-tools').hidden = (t !== 'brush');
  if (typeof Viewport !== 'undefined') Viewport.drawOverlay();
}

function fitView() {
  /* "适应":以所有实体的包围盒为中心,缩放刚好装下(留 15% 边距);
   * 空场景就回到原点 1:1。 */
  if (!DS.outline.length) { setView({ x: 0, y: 0, zoom: 1 }); return; }
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  DS.outline.forEach((o) => {
    x0 = Math.min(x0, o.x); y0 = Math.min(y0, o.y);
    x1 = Math.max(x1, o.x + o.w); y1 = Math.max(y1, o.y + o.h);
  });
  const w = Math.max(1e-3, x1 - x0), h = Math.max(1e-3, y1 - y0);
  const z = Math.max(0.05, Math.min(8,
    Math.min(DS.vw / (w * 1.15), DS.vh / (h * 1.15))));
  const cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
  setView({ zoom: z, x: cx - (DS.vw / 2) / z, y: cy - (DS.vh / 2) / z });
}

async function undoRedo(which) { await withNamesThen(() => ds(which)); }

async function saveAll() {
  try {
    const r = await call('project.save', {}, '保存');
    log('dim', '已保存:' + (r.root || DS.info.root || ''));
    await refresh();
  } catch (e) { /* 已提示 */ }
}

/* ------------------------------------------------------------ 接线 */

function wire() {
  $('btn-new').onclick = async () => {
    const dir = prompt('项目目录(相对 DexStudio 所在目录,或绝对路径)', 'mygame');
    if (!dir) return;
    const name = dir.split(/[\\/]/).filter(Boolean).pop() || 'game';
    try {
      await call('project.new', { dir, name }, '新建项目');
      DS.sel = [];
      await refresh();
    } catch (e) { /* 已提示 */ }
  };
  $('btn-open').onclick = async () => {
    const dir = prompt('要打开的项目目录', 'examples/dexgame');
    if (!dir) return;
    try {
      await call('project.open', { dir }, '打开项目');
      DS.sel = [];
      await refresh();
    } catch (e) { /* 已提示 */ }
  };
  $('btn-save').onclick = saveAll;
  $('btn-reload').onclick = async () => {
    const sc = DS.info && DS.info.scene;
    if (!sc) { log('warn', '没有场景可重载'); return; }
    try {
      await call('scene.load', { path: sc }, '重载场景');
      DS.sel = [];
      await refresh();
      log('dim', '已重载 ' + sc);
    } catch (e) { /* 已提示 */ }
  };
  $('btn-add').onclick = () => doAddEntity('obj' + (DS.entities.length + 1));
  $('btn-dup').onclick = () => doDuplicate();
  $('btn-del').onclick = doDelete;
  $('btn-undo').onclick = () => undoRedo('undo');
  $('btn-redo').onclick = () => undoRedo('redo');
  $('btn-scene-new').onclick = async () => {
    const name = $('scene-name').value || 'scene';
    try {
      await call('scene.new', { name }, '新建场景');
      DS.sel = [];
      await refresh();
    } catch (e) { /* 已提示 */ }
  };
  $('btn-scene-list').onclick = async () => {
    try {
      const list = await ds('scene.list');
      log('dim', '场景:' + (list.join(', ') || '(无)'));
    } catch (e) { log('er', e.message); }
  };
  $('btn-scene-json').onclick = async () => {
    try {
      const j = await ds('scene.json');
      log('dim', JSON.stringify(j, null, 2));
      showTab('log');
    } catch (e) { log('er', e.message); }
  };
  $('btn-sel-all').onclick = () => select(DS.entities.map((e) => e.id));
  $('btn-sel-none').onclick = () => select([]);
  $('tree-filter').oninput = renderTree;
  $('tool-select').onclick = () => setTool('select');
  $('tool-brush').onclick = () => setTool('brush');
  $('btn-zoom-in').onclick = () => zoomBy(1.25);
  $('btn-zoom-out').onclick = () => zoomBy(1 / 1.25);
  $('btn-zoom-1').onclick = () => setView({ zoom: 1 });
  $('btn-fit').onclick = fitView;
  $('chk-grid').onchange = (e) => { DS.showGrid = e.target.checked; Viewport.drawOverlay(); };
  $('chk-snap').onchange = (e) => { DS.snap = e.target.checked; };
  $('chk-paint-drag').onchange = (e) => { DS.paintDrag = e.target.checked; };
  $('chk-erase').onchange = (e) => { DS.erase = e.target.checked; };
  $('grid-size').onchange = (e) => {
    DS.grid = Math.max(1, parseInt(e.target.value, 10) || 16);
    Viewport.drawOverlay();
  };
  $('btn-selfcheck').onclick = selfcheck;
  $('btn-clear-log').onclick = () => { $('log').innerHTML = ''; logLines = 0; };
  document.querySelectorAll('.tabs .tab').forEach((b) => {
    b.onclick = () => showTab(b.dataset.tab);
  });
  $('btn-tile-apply').onclick = applyAtlas;
  $('btn-tile-clear').onclick = async () => {
    if (!DS.tilemap) return;
    const cols = DS.tilemap.cols, rows = DS.tilemap.rows;
    const csv = Array.from({ length: rows }, () => Array(cols).fill(-1).join(',')).join('\n');
    try {
      await call('tilemap.csv', { id: DS.tilemap.id, csv }, '清空瓦片');
      await refresh();
    } catch (e) { /* 已提示 */ }
  };
  $('btn-tile-empty').onclick = () => pickTile(-1);
}

function zoomBy(f) {
  /* 以视口中心为锚点缩放 */
  const c = Viewport.centerWorld();
  const z = Math.max(0.05, Math.min(16, DS.view.zoom * f));
  setView({ zoom: z, x: c.x - (DS.vw / 2) / z, y: c.y - (DS.vh / 2) / z });
}

async function applyAtlas() {
  if (!DS.tilemap) return;
  const cols = parseInt($('atlas-cols').value, 10) || 8;
  const first = parseInt($('atlas-first').value, 10) || 0;
  try {
    await call('comp.set', { id: DS.tilemap.id, comp: 'tilemap', field: 'atlas_cols', value: cols }, '设置图集列数');
    await call('comp.set', { id: DS.tilemap.id, comp: 'tilemap', field: 'atlas_tile', value: first }, '设置首块');
    await refresh();
  } catch (e) { /* 已提示 */ }
}

function pickTile(t) {
  DS.tile = t;
  $('btn-tile-empty').classList.toggle('on', t < 0);
  document.querySelectorAll('.palette-wrap .cell').forEach((c) => {
    c.classList.toggle('on', parseInt(c.dataset.tile, 10) === t);
  });
  $('tile-pick').textContent = t < 0 ? '空 (-1)' : ('图块 ' + t);
}

/* ------------------------------------------------------------ 界面自测

 * 无窗口、无人值守地验证**界面这一层**是否真的可用:渲染图读进来了没有、
 * 画布上有没有像素、层级树的行数对不对、世界↔屏幕换算是否自洽、属性面板有没有
 * 生成字段、瓦片刷子能不能落到 CSV 上。宿主 `--wv-selftest` 会调它并断言结果。
 * (这正是 AGENTS.md 里"自动化离屏证明代替肉眼看屏幕"的做法。) */
window.__ds_selftest = async function () {
  const out = { pass: [], fail: [] };
  const t = (name, ok, extra) => {
    (ok ? out.pass : out.fail).push(name + (extra ? '  ' + extra : ''));
  };
  const wasQuiet = DS.quietLog;
  DS.quietLog = true;
  let tmpId = 0;
  try {
    /* --- 通道与状态 --- */
    t('消息桥存在', !!bridge);
    t('app.info 有版本', !!(DS.info && DS.info.version), 'v' + (DS.info || {}).version);
    t('引擎可用', !!(DS.info && DS.info.engine),
      DS.info && DS.info.engine ? '' : (DS.info || {}).engine_error);
    t('组件自省有 8 种', DS.schema.length >= 8, DS.schema.length + ' 种');

    /* --- 离屏渲染真的到了画布上 --- */
    const ii = Viewport.imgInfo();
    t('渲染图已解码', ii.ready && ii.w > 0, ii.w + '×' + ii.h);
    t('渲染图尺寸 = 引擎视口', ii.w === DS.vw && ii.h === DS.vh,
      ii.w + '×' + ii.h + ' vs ' + DS.vw + '×' + DS.vh);
    t('渲染图走虚拟主机', ii.src.indexOf('dexstudio-preview.local') >= 0, ii.src);
    const px = Viewport.pixelStats();
    /* 画布里除了渲染图还有 letterbox 区域,所以只要求"有相当比例非背景色 +
     * 颜色不止几种"(全黑/纯色 = 引擎没画东西)。 */
    t('画布上有内容', px.nonBg > px.sampled * 0.25 && px.colors > 8,
      px.nonBg + '/' + px.sampled + ' 非背景,' + px.colors + ' 种颜色');

    /* --- 世界 ↔ 屏幕换算自洽 --- */
    const w1 = Viewport.toWorld(100, 80);
    const c1 = Viewport.toCanvas(w1.x, w1.y);
    t('世界↔屏幕往返一致',
      Math.abs(c1.x - 100) < 0.01 && Math.abs(c1.y - 80) < 0.01,
      '(' + c1.x.toFixed(2) + ',' + c1.y.toFixed(2) + ')');

    /* --- 层级树 --- */
    renderTree();
    const rows = document.querySelectorAll('#tree li').length;
    /* 空场景时树里会有一行占位提示 */
    t('层级树行数 = 实体数', rows === Math.max(1, DS.entities.length),
      rows + ' vs ' + DS.entities.length);

    /* --- 建一个实体 → 选中 → 属性面板出字段 → 视口画选中框 --- */
    const added = await ds('entity.add', { name: '__uitest__' });
    tmpId = added.id;
    await refresh();
    t('新实体出现在树里',
      Array.prototype.some.call(document.querySelectorAll('#tree li .nm'),
                                (n) => n.textContent.indexOf('__uitest__') >= 0));
    select([added.id]);
    await renderInspector();
    const fields = document.querySelectorAll('#inspector .comp .field').length;
    t('属性面板生成字段', fields >= 6, fields + ' 个字段');
    Viewport.drawOverlay();
    t('视口画出了选中框', Viewport.hasColor('#f9e2af'));

    /* --- 拖动移动(走 comp.set_many)--- */
    await moveSelection({ [added.id]: { x: 123, y: 45 } });
    const moved = byId(added.id) || {};
    t('拖动移动写回位置', Math.abs(moved.x - 123) < 0.001 && Math.abs(moved.y - 45) < 0.001,
      moved.x + ',' + moved.y);

    /* --- 撤销:实体 id 会变,但按名字能重新选中 --- */
    await withNamesThen(() => ds('undo'));
    {
      const back = DS.entities.find((e) => e.name === '__uitest__') || {};
      t('撤销后位置复原', Math.abs(back.x || 0) < 0.001, 'x=' + back.x);
    }

    /* --- 瓦片地图:建 CSV → 画一格 → 读回来 --- */
    const tm = await ds('entity.add', { name: '__tiletest__' });
    await ds('comp.add', { id: tm.id, comp: 'tilemap' });
    const created = await ds('tilemap.create', {
      id: tm.id, path: 'res/__uitest__.csv', cols: 4, rows: 3,
    });
    t('tilemap.create 落盘', !!created.path, created.cols + '×' + created.rows);
    await ds('tilemap.set', { id: tm.id, col: 2, row: 1, tile: 5 });
    let info = await ds('tilemap.info', { id: tm.id });
    const idx = 1 * info.cols + 2;
    t('瓦片写进 CSV 并读回', info.tiles[idx] === 5, 'tiles[' + idx + ']=' + info.tiles[idx]);
    const cells = [{ col: 0, row: 0, tile: 7 }, { col: 1, row: 0, tile: 8 }];
    const painted = await ds('tilemap.paint', { id: tm.id, cells });
    t('tilemap.paint 一次多条', painted.count === 2);
    info = await ds('tilemap.info', { id: tm.id });
    t('paint 结果正确', info.tiles[0] === 7 && info.tiles[1] === 8,
      info.tiles.slice(0, 4).join(','));
    await ds('undo');
    {
      /* 撤销会把场景整份重建 → 实体 id 变了,按名字找回 */
      await refresh();
      const back = DS.entities.find((e) => e.name === '__tiletest__');
      info = await ds('tilemap.info', { id: back ? back.id : tm.id });
      t('撤销 paint 后 CSV 复原(paint 一条撤销)',
        info.tiles[0] === -1 && info.tiles[1] === -1,
        info.tiles.slice(0, 4).join(','));
    }

    /* --- 清理:把测试实体删掉 --- */
    const ids = DS.entities.map((e) => e.id);
    for (const id of ids) {
      const e = byId(id);
      if (e && (e.name === '__uitest__' || e.name === '__tiletest__')) {
        await ds('entity.remove', { id });
      }
    }
    DS.sel = [];
    await refresh();
    t('清理后无残留测试实体',
      !DS.entities.some((e) => /__uitest__|__tiletest__/.test(e.name || '')));
  } catch (e) {
    out.fail.push('自检中断:' + (e && e.message));
  }
  DS.quietLog = wasQuiet;
  out.total = out.pass.length + out.fail.length;
  out.pass_count = out.pass.length;
  out.fails = out.fail.length;
  void tmpId;
  try {
    bridge.postMessage({ cmd: 'ui.selftest', args: out });
  } catch (e) { /* 忽略 */ }
  return out;
};

/* ------------------------------------------------------------ 启动 */

window.addEventListener('DOMContentLoaded', async () => {
  if (!bridge) {
    const st = $('status');
    st.textContent = '未在 WebView2 中运行(没有消息桥)';
    st.className = 'status err';
    log('er', '没有 chrome.webview 桥:这个页面只能在 DexStudio 宿主里用。');
    return;
  }
  wire();
  wireKeys();
  setTool('select');
  try {
    await refresh();
    log('dim', '就绪。快捷键:1/2 切换工具 · F 适应 · Ctrl+Z/Y 撤销重做 · Ctrl+D 复制 · Delete 删除');
    /* 告诉宿主"界面已就绪":宿主 --wv-selftest 靠它判定整条链通不通
       (窗口 + WebView2 + 本地页面 + JS→C→JS 往返),不需要人看屏幕。 */
    await ds('ui.ready');
  } catch (e) {
    log('er', '初始化失败:' + e.message);
  }
});
