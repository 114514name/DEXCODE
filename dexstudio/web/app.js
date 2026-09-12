/* DexStudio 前端外壳。
 *
 * 与 C 宿主的唯一通道是 WebView2 的 postMessage:
 *   JS → C:  window.chrome.webview.postMessage({id, cmd, args})
 *   C → JS:  message 事件里的 e.data
 * 用 id 做请求/响应配对(ds() 返回 Promise)。**不开窗口也能验证**同样的命令:
 *   dexstudio.exe --command '{"cmd":"app.info"}'
 *
 * 分工(见 docs/DEXGAME_DESIGN.md §9.1 决策 #10):
 *   - C(模型层):项目 / 场景 / 撤销 / 字段读写 / 瓦片 / 离屏渲染 / 下拉候选
 *   - 这里(视图层):只做"把状态画出来"和"把用户动作翻译成命令"
 * 所以本文件里不该出现任何业务规则(比如"哪个字段该不该存盘")。
 *
 * 本次修复的两条规矩(docs/DEXSTUDIO_UX_ISSUES.md):
 *   1. **按钮必须真的接线**:每个可见控件都要有处理器 + 失败提示,"点了没反应"
 *      是最难查的一类故障(以前「挂组件」「改名」两个按钮就是死的)。
 *   2. **凡是程序能列出来的都给控件**:项目/场景/实体/组件/字段/资源/按键一律
 *      下拉或对话框,不让用户手打路径与名字。
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

/* 带错误提示的调用:失败时写进输出面板 + 弹一条提示条并抛出。
 * 为什么一定要弹:命令的失败原因在输出面板里,而用户经常根本没看那儿
 * (面板还可能是收起的),于是表现就是"点了没反应"。 */
async function call(cmd, args, what) {
  try {
    const r = await ds(cmd, args);
    /* 宿主可以在结果里带一句「顺手帮你做了什么」的说明(note)。
     * 为什么要显示:有些修正是**顺带**做的(比如换贴图时把旧模板留下的 32×32
     * 裁切改成整张图),不说出来用户会以为程序偷偷改了他的东西。 */
    if (r && r.note && typeof toast === 'function') toast(r.note, 'ok');
    return r;
  } catch (e) {
    const msg = (what || cmd) + '失败:' + e.message;
    log('er', msg);
    if (typeof toast === 'function') toast(msg, 'err');
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
  schema: [],          // comp.schema(带 label/kind/enum 等人类语义)
  options: { entities: [], images: [], audios: [], schema: [] },  // scene.options
  entities: [],        // entity.list
  outline: [],         // scene.outline(世界包围盒)
  scenes: [],          // 项目里的场景名(scene.list)
  recent: [],          // 最近打开的项目
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
  mode: 'scene',       // scene | graph | code(三个编辑器共用同一个窗口)
  res: [],             // res/ 下的文件
  resSel: '',          // 资源面板里选中的文件
  quietLog: true,      // 默认**不打**协议流量(真人话才看得见),调试时勾上
  busy: false,         // 正在跑一批命令(拖拽中),抑制重复刷新
};

const byId = (id) => DS.entities.find((e) => e.id === id) || null;
const outlineOf = (id) => DS.outline.find((o) => o.id === id) || null;
const selected = () => DS.sel.slice();
const baseName = (p) => String(p || '').replace(/\\/g, '/').split('/').pop();
const schemaOf = (comp) => DS.schema.find((c) => c.name === comp) || null;

/* ------------------------------------------------------------ 刷新 */

/* 一次把界面重画一遍。**所有**改动之后都走它:状态只有 C 模型一份,
 * 前端不做增量推断(撤销/重做会让实体 id 全变,增量更新必然出错)。
 *
 * 刷新合并:一批命令(比如批量写字段 + 撤销)会连着触发好几次 refresh,
 * 每次都发 8 条命令 + 离屏渲染一帧太浪费。这里让后到的调用**等当前这次**,
 * 结束后再补跑一次 —— 语义不变(调用方拿到的永远是刷新后的界面),但次数收敛。 */
let refreshBusy = null;
let refreshAgain = false;

async function refresh(opts) {
  const o = opts || {};
  if (DS.busy && !o.force) return;
  if (refreshBusy) {
    refreshAgain = true;
    return refreshBusy;
  }
  refreshBusy = refreshOnce(o);
  try {
    await refreshBusy;
  } finally {
    refreshBusy = null;
  }
  if (refreshAgain) {
    refreshAgain = false;
    await refresh(o);
  }
}

async function refreshOnce(o) {
  let info;
  try {
    info = await ds('app.info');
    DS.schema = await ds('comp.schema');
    DS.options = await ds('scene.options');
    DS.entities = await ds('entity.list');
    DS.outline = await ds('scene.outline');
    if (info.root) {
      try {
        const st = await ds('project.state');
        DS.scenes = st.scenes || [];
      } catch (e) { DS.scenes = []; }
    } else {
      DS.scenes = [];
    }
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
  renderScenes();
  renderTree();
  renderLayers();
  await refreshResources();
  renderRecover();
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
      (info.root ? baseName(info.root) : '(未打开项目)') +
      ' · ' + DS.entities.length + ' 实体 · 撤销 ' + (info.undo || 0);
    st.className = 'status ok';
  } else {
    st.textContent = '引擎不可用:' + (info.engine_error || '');
    st.className = 'status err';
  }
  $('scene-path').textContent = info.scene
    ? baseName(info.scene) + '   ' + (info.root || '')
    : (info.root ? '(这个项目还没有场景)' : '未打开项目');
  $('obj-count').textContent = DS.entities.length;
  $('btn-undo').disabled = !(info.undo > 0);
  $('btn-redo').disabled = !(info.redo > 0);
  $('vp-hint').hidden = !!info.root;
  $('tile-pick').textContent = DS.tile < 0 ? '橡皮/空' : ('图块 ' + DS.tile);
  $('viewinfo').textContent =
    '视口 ' + Math.round(DS.view.x) + ',' + Math.round(DS.view.y) +
    ' · ' + (Math.round(DS.view.zoom * 100) / 100) + '× · ' +
    DS.vw + '×' + DS.vh +
    (DS.sel.length ? ' · 选中 ' + DS.sel.length : '');
}

/* 场景下拉 + "游戏从这里开始"复选框 */
function renderScenes() {
  const sel = $('scene-sel');
  if (!sel) return;
  const cur = DS.info && DS.info.scene ? baseName(DS.info.scene) : '';
  const names = (DS.scenes || []).map((n) => String(n).replace(/\.json$/, ''));
  sel.innerHTML = '';
  if (!names.length) {
    const o = document.createElement('option');
    o.value = '';
    o.textContent = '(这个项目还没有场景)';
    sel.appendChild(o);
  } else {
    names.forEach((n) => {
      const o = document.createElement('option');
      o.value = n;
      o.textContent = n;
      if (n === cur) o.selected = true;
      sel.appendChild(o);
    });
  }
  const start = DS.info ? String(DS.info.start_scene || '') : '';
  const startName = start ? String(start).replace(/\.json$/, '').split('/').pop() : '';
  $('chk-start-scene').checked = !!cur && cur === startName;
  $('chk-start-scene').disabled = !cur;
  $('btn-scene-rename').disabled = !cur;
  $('btn-scene-del').disabled = !cur;
  /* 逻辑模式(积木/节点)单选:状态在 project.json 的 logic_mode */
  const lm = (DS.info && DS.info.logic_mode) === 'graph' ? 'graph' : 'blocks';
  if ($('lm-blocks')) $('lm-blocks').checked = (lm === 'blocks');
  if ($('lm-graph')) $('lm-graph').checked = (lm === 'graph');
}

/* 最近项目下拉 */
function renderRecent(items) {
  DS.recent = items || DS.recent;
  const sel = $('recent');
  sel.innerHTML = '';
  const head = document.createElement('option');
  head.value = '';
  head.textContent = DS.recent.length ? '最近项目…' : '(还没有最近项目)';
  sel.appendChild(head);
  DS.recent.forEach((it) => {
    const o = document.createElement('option');
    o.value = it.path;
    o.textContent = (it.exists ? '' : '✗ ') + (it.name || baseName(it.path));
    sel.appendChild(o);
  });
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

/* ------------------------------------------------------------ 恢复提示 */

/* 有一份**比场景文件新的自动保存** = 上次退出时还有没保存的改动。
 * 可能是崩溃(clean=0),也可能是正常关掉但没存盘(clean=1)——
 * 两种都问用户要不要恢复,但话要说准(不能一律说"崩溃")。 */
function renderRecover() {
  const bar = $('recover-bar');
  const info = DS.info || {};
  if (!info.recoverable) {
    bar.hidden = true;
    return;
  }
  bar.hidden = false;
  const why = info.recover_kind === 'crash'
    ? '上次似乎没有正常退出(可能崩溃了)'
    : '上次退出时还有没保存的改动';
  $('recover-text').textContent =
    why + ' —— 有一份比场景文件新的自动保存' +
    (info.autosave_seq ? '(' + info.autosave_seq + ' 次自动保存)' : '') +
    '。要恢复它吗?';
}

/* ------------------------------------------------------------ 资源面板 */

/* 图片/声音走**虚拟主机**显示与试听(宿主把项目根映射成 dexstudio-proj.local),
 * 所以缩略图与试听都不经过消息通道。
 *
 * `?v=` 是必须的:URL 长得一样时浏览器会**缓存住之前的失败**(比如项目还没打开、
 * 或上一次指向的是别的项目) —— 表现就是"图导入成功了,缩略图永远是个裂图标"。
 * 每次刷新资源列表都换一个序号,强制重新取。 */
let resSeq = 0;
const resUrl = (name) => 'https://dexstudio-proj.local/res/' +
  encodeURIComponent(name) + '?v=' + resSeq;

function humanSize(n) {
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1024 / 1024).toFixed(2) + ' MB';
}

async function refreshResources() {
  const box = $('res-list');
  if (!box) return;
  resSeq++;                       /* 破缓存:见 resUrl 的说明 */
  if (!DS.info || !DS.info.root) {
    DS.res = [];
    box.innerHTML = '<div class="hint">未打开项目</div>';
    $('res-count').textContent = '0';
    return;
  }
  try {
    const r = await ds('res.list');
    DS.res = r.files || [];
  } catch (e) {
    DS.res = [];
    log('er', 'res.list: ' + e.message);
  }
  $('res-count').textContent = DS.res.length;
  box.innerHTML = '';
  if (!DS.res.length) {
    box.innerHTML = '<div class="hint">res/ 还是空的 —— 点「导入…」</div>';
    return;
  }
  DS.res.forEach((f) => {
    const d = document.createElement('div');
    d.className = 'res ' + f.kind + (f.name === DS.resSel ? ' sel' : '');
    d.title = f.name + ' · ' + humanSize(f.size) + ' · ' + f.kind;
    if (f.kind === 'image') {
      d.innerHTML = '<img src="' + resUrl(f.name) + '" alt="">' +
        '<div class="nm">' + esc(f.name) + '</div>' +
        '<div class="kb">' + humanSize(f.size) + '</div>';
    } else {
      d.innerHTML = '<div class="ph">' + (f.kind === 'audio' ? '♪' : '▤') + '</div>' +
        '<div class="nm">' + esc(f.name) + '</div>' +
        '<div class="kb">' + humanSize(f.size) + '</div>';
    }
    d.onclick = () => applyRes(f);
    d.oncontextmenu = (ev) => {
      ev.preventDefault();
      resMenu(f);
    };
    box.appendChild(d);
  });
}

/* 点资源:图片→给选中实体的 sprite.tex_path;声音→试听一下 */
async function applyRes(f) {
  DS.resSel = f.name;
  refreshResources();
  if (f.kind === 'audio') {
    try {
      const a = new Audio(resUrl(f.name));
      a.volume = 0.6;
      a.play().then(() => log('dim', '试听 ' + f.name))
              .catch((e) => toast('试听失败:' + (e && e.message ? e.message : e) +
                                  '(文件格式或浏览器不允许自动播放)', 'err'));
    } catch (e) { toast('试听失败:' + e.message, 'err'); }
    return;
  }
  if (f.kind !== 'image') {
    log('dim', f.name + ':' + humanSize(f.size));
    return;
  }
  if (!DS.sel.length) {
    toast('先在层级树里选一个实体,再把 ' + f.name + ' 设成它的贴图', 'warn');
    return;
  }
  const id = DS.sel[DS.sel.length - 1];
  /* 刻意**现查一次**实体,而不是查本地缓存 DS.entities:
   * 缓存一旦过期,这里会静默 return(用户看到的是"点了没反应")。 */
  let e;
  try {
    e = await ds('entity.get', { id });
  } catch (err) {
    toast('实体已失效(' + id + '),先在层级树里重选一个', 'err');
    return;
  }
  try {
    if (!e.comps || !Object.prototype.hasOwnProperty.call(e.comps, 'sprite')) {
      await call('comp.add', { id, comp: 'sprite' }, '挂 sprite');
    }
    await call('comp.set', { id, comp: 'sprite', field: 'tex_path',
                             value: 'res/' + f.name }, '设置贴图');
    /* 图比屏幕还大时**自动缩小到合适大小** —— 零基础用户点一张缩略图,
     * 期望的是"我的角色出现了",而不是一张 300×400 的图盖满整个视口
     * (而且会以为"没显示出来")。用 transform 的缩放,并在提示条里说清楚。 */
    const info = await new Promise((resolve) => {
      const im = new Image();
      im.onload = () => resolve({ w: im.naturalWidth, h: im.naturalHeight });
      im.onerror = () => resolve(null);
      im.src = resUrl(f.name);
    });
    if (info && Math.max(info.w, info.h) > 256) {
      const k = Math.round((128 / Math.max(info.w, info.h)) * 100) / 100;
      await call('comp.set_many', { items: [
        { id, comp: 'transform', field: 'sx', value: k },
        { id, comp: 'transform', field: 'sy', value: k },
      ] }, '按贴图大小缩放');
      toast('这张图 ' + info.w + '×' + info.h + ',已按 ' + k +
            ' 倍缩小(右边「缩放 X / 缩放 Y」可以改)', 'ok');
    }
    log('dim', f.name + ' → ' + (e.name || id) + ' 的贴图');
    await refresh();
  } catch (err) { /* 原因已提示 */ }
}

/* 右键资源:改名 / 删除。改名会**连引用一起改**(以前改完名字贴图全裂)。 */
async function resMenu(f) {
  let refs = 0;
  try {
    const r = await ds('res.refs', { name: f.name });
    refs = r.refs || 0;
  } catch (e) { /* 数不出来不影响改名 */ }
  const pick = await uiDialog({
    title: '资源 ' + f.name,
    hint: refs ? ('res/' + f.name + ' 被 ' + refs + ' 处引用(改名时会一起更新)')
               : ('res/' + f.name + ' · ' + humanSize(f.size)),
    fields: [
      { name: 'act', label: '要做什么', type: 'choices', value: 'rename',
        choices: [{ value: 'rename', label: '改名' },
                  { value: 'delete', label: '删除' }] },
      { name: 'to', label: '新文件名', type: 'text', value: f.name },
    ],
    ok: '确定',
  });
  if (!pick) return;
  try {
    if (pick.act === 'delete') {
      if (!(await uiConfirm('删除 res/' + f.name + '?',
                            refs ? ('有 ' + refs + ' 处引用指向它,删掉之后那些贴图/声音会失效')
                                 : '这个文件会从 res/ 里删掉', true))) return;
      const r = await call('res.delete', { name: f.name, force: 1 }, '删除资源');
      toast('已删除 res/' + f.name + (r.refs ? '(有 ' + r.refs + ' 处引用已失效)' : ''), 'ok');
    } else if (pick.to && pick.to !== f.name) {
      const r = await call('res.rename',
                           { name: f.name, to: pick.to, update_refs: 1 }, '资源改名');
      toast('已改名 → ' + r.name +
            (r.updated ? '(同步更新了 ' + r.updated + ' 处引用)' : ''), 'ok');
    }
    await refresh();
  } catch (e) { /* 原因已提示 */ }
}

/* 导入资源:可以一次选多个(以前只能一个一个来) */
async function importRes() {
  if (!DS.info || !DS.info.root) {
    toast('先在左上角新建/打开一个项目', 'warn');
    return [];
  }
  let picked;
  try {
    picked = await ds('res.pick', { multi: 1 });
  } catch (e) {
    log('er', '选文件失败:' + e.message);
    return [];
  }
  const paths = (picked && picked.paths && picked.paths.length)
    ? picked.paths : (picked && picked.path ? [picked.path] : []);
  if (!picked || !picked.picked || !paths.length) return [];
  const done = [];
  for (const p of paths) {
    try {
      const r = await call('res.import', { src: p }, '导入资源');
      done.push(r.name);
    } catch (e) { /* 单个失败(重名等)不打断其余的,原因已提示 */ }
  }
  if (done.length) toast('已导入 ' + done.length + ' 个资源:' + done.join('、'), 'ok');
  await refreshResources();
  return done;
}

/* 自动保存的节拍:策略在 C(存什么/存哪儿/什么时候算脏),这里只负责"按时敲一下" */
function startAutosave() {
  setInterval(async () => {
    const info = DS.info || {};
    if (!info.root || !info.dirty) return;
    try {
      const r = await ds('autosave.tick');
      if (r.saved) log('dim', '已自动保存(第 ' + r.seq + ' 次)');
    } catch (e) { /* 忽略:自动保存失败不该打断编辑 */ }
  }, 30000);
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

/* ------------------------------------------------------------ 项目动作 */

async function doNewProject() {
  if (!bridge) return;
  const picked = await ds('project.pick').catch((e) => {
    toast('选文件夹失败:' + e.message, 'err');
    return null;
  });
  if (!picked) return;
  if (!picked.picked || !picked.path) {
    toast('没有选文件夹(项目会建在你选的那个文件夹里面)', 'warn');
    return;
  }
  const parent = picked.path;
  const r = await uiDialog({
    title: '在 ' + parent + ' 里新建项目',
    hint: '会建出 project.json + scenes/ + scripts/ + res/,建完可以直接点「运行」',
    fields: [
      { name: 'name', label: '项目名', type: 'text', value: '我的游戏' },
      { name: 'dir', label: '文件夹名', type: 'text', value: '我的游戏' },
      { name: 'tpl', label: '模板', type: 'choices', value: 'starter',
        choices: [
          { value: 'starter', label: '能跑的最小项目', desc: '相机 + 玩家(精灵/碰撞/物理)' },
          { value: 'empty', label: '空项目', desc: '什么都没有,自己搭' },
        ] },
    ],
    ok: '创建',
  });
  if (!r) return;
  const dir = parent.replace(/[\\/]+$/, '') + '\\' + (r.dir || r.name || 'game');
  try {
    /* 模板也交给 C:它会一起把**示例积木**写好(新用户建完项目就有一段能动的玩法) */
    const made = await call('project.new',
                            { dir, name: r.name || r.dir, template: r.tpl },
                            '新建项目');
    DS.sel = [];
    await refresh();
    await refreshRecent();
    log('dim', '项目建好了:' + (made.root || dir));
    /* 建完直接切到「积木」:新用户第一眼看到的是"已经会动的小人",而不是空场景 */
    await setMode('blocks');
    toast('项目已建好并带了一段示例积木 —— 点顶栏「运行」就能看到效果', 'ok');
  } catch (e) { /* 已提示 */ }
}

/* "能跑的最小项目"模板:相机 + 玩家(精灵/碰撞/物理),全部走模型命令 */
async function makeStarterScene() {
  const cam = await call('entity.add', { name: '相机', comps: ['transform', 'camera'] }, '加相机');
  const pl = await call('entity.add',
                        { name: '玩家', comps: ['transform', 'sprite', 'collider', 'body'] },
                        '加玩家');
  await call('comp.set_many', { items: [
    { id: cam.id, comp: 'transform', field: 'x', value: 0 },
    { id: cam.id, comp: 'transform', field: 'y', value: 0 },
    { id: pl.id, comp: 'transform', field: 'x', value: 100 },
    { id: pl.id, comp: 'transform', field: 'y', value: 100 },
    { id: pl.id, comp: 'collider', field: 'kind', value: 0 },
    { id: pl.id, comp: 'collider', field: 'hw', value: 16 },
    { id: pl.id, comp: 'collider', field: 'hh', value: 16 },
    { id: pl.id, comp: 'body', field: 'motion', value: 2 },
  ] }, '布置模板实体');
  select([pl.id]);
}

async function doOpenProject() {
  if (!bridge) return;
  const picked = await ds('project.pick').catch((e) => {
    toast('选文件夹失败:' + e.message, 'err');
    return null;
  });
  if (!picked || !picked.picked || !picked.path) return;
  await openProjectAt(picked.path);
}

async function openProjectAt(dir) {
  try {
    await call('project.open', { dir }, '打开项目');
    DS.sel = [];
    await refresh();
    await refreshRecent();
    toast('已打开 ' + baseName(dir), 'ok');
  } catch (e) { /* 已提示 */ }
}

async function refreshRecent() {
  try {
    const r = await ds('project.recent');
    renderRecent(r.items || []);
  } catch (e) { /* 没有最近列表也能用 */ }
}

/* ------------------------------------------------------------ 场景动作 */

async function doSceneNew() {
  const suggest = '场景' + (DS.scenes.length + 1);
  const r = await uiDialog({
    title: '新建场景', hint: '新场景会立刻建好并切换过去(旧场景不会被动)',
    fields: [{ name: 'name', label: '场景名', type: 'text', value: suggest }],
    ok: '新建',
  });
  if (!r || !r.name) return;
  try {
    await call('scene.new', { name: r.name }, '新建场景');
    DS.sel = [];
    await refresh();
    toast('已新建场景 ' + r.name, 'ok');
  } catch (e) { /* 已提示 */ }
}

async function doSceneRename() {
  const cur = DS.info && DS.info.scene ? baseName(DS.info.scene).replace(/\.json$/, '') : '';
  if (!cur) return;
  const r = await uiDialog({
    title: '场景改名', fields: [{ name: 'name', label: '新名字', type: 'text', value: cur }],
    ok: '改名',
  });
  if (!r || !r.name || r.name === cur) return;
  try {
    await call('scene.rename', { name: cur, to: r.name }, '场景改名');
    await refresh();
    toast('已改名为 ' + r.name, 'ok');
  } catch (e) { /* 已提示 */ }
}

async function doSceneDelete() {
  const cur = DS.info && DS.info.scene ? baseName(DS.info.scene).replace(/\.json$/, '') : '';
  if (!cur) return;
  if (!(await uiConfirm('删除场景「' + cur + '」?',
                        '文件会从磁盘上删掉,场景里的实体一起没了', true))) return;
  try {
    await call('scene.delete', { name: cur }, '删除场景');
    DS.sel = [];
    await refresh();
  } catch (e) { /* 已提示 */ }
}

async function loadScene(name) {
  if (!name) return;
  const cur = DS.info && DS.info.scene ? baseName(DS.info.scene).replace(/\.json$/, '') : '';
  if (name === cur) return;
  if (DS.info && DS.info.dirty &&
      !(await uiConfirm('切到「' + name + '」?', '当前场景有没保存的改动(可以先点「保存」)'))) {
    renderScenes();
    return;
  }
  try {
    await call('scene.load', { path: 'scenes/' + name + '.json' }, '切换场景');
    DS.sel = [];
    await refresh();
  } catch (e) { renderScenes(); }
}

/* ------------------------------------------------------------ 实体动作 */

/* 加实体:先问模板(组件一次性挂好),不让用户"先建空壳再一个个挂组件" */
async function doAddEntity() {
  const r = await uiDialog({
    title: '添加实体', hint: '模板会一次性把组件挂好,省得一个个加',
    fields: [
      { name: 'name', label: '名字', type: 'text', value: 'obj' + (DS.entities.length + 1) },
      { name: 'tpl', label: '模板', type: 'choices', value: 'sprite',
        choices: [
          { value: 'sprite', label: '精灵', desc: '位置 + 贴图(点左边资源缩略图选图)' },
          { value: 'player', label: '玩家', desc: '精灵 + 矩形碰撞 + 物理体' },
          { value: 'camera', label: '相机', desc: '决定视口看哪儿' },
          { value: 'tilemap', label: '瓦片图层', desc: '位置 + 瓦片地图(再点「新建瓦片地图…」)' },
          { value: 'empty', label: '空实体', desc: '只有位置' },
        ] },
    ],
    ok: '添加',
  });
  if (!r) return;
  const tpl = {
    empty: ['transform'],
    sprite: ['transform', 'sprite'],
    camera: ['transform', 'camera'],
    tilemap: ['transform', 'tilemap'],
    player: ['transform', 'sprite', 'collider', 'body'],
  }[r.tpl] || ['transform'];
  try {
    const made = await call('entity.add', { name: r.name, comps: tpl }, '添加实体');
    /* **不预设 sw/sh**:那两个字段是"图集里取哪一块",取多少就画多大(1:1)。
     * 以前模板硬写 32×32,配 300×400 的贴图就只画左上角一小块 —— 用户看到的是
     * "设了贴图但画面里没图"。保持 0(=整张贴图),大小交给「缩放 X/Y」。 */
    if (r.tpl === 'player') {
      await call('comp.set_many', { items: [
        { id: made.id, comp: 'collider', field: 'kind', value: 0 },
        { id: made.id, comp: 'collider', field: 'hw', value: 16 },
        { id: made.id, comp: 'collider', field: 'hh', value: 16 },
        { id: made.id, comp: 'body', field: 'motion', value: 2 },
      ] }, '设置玩家模板');
    }
    await refresh();
    select([made.id]);
    log('dim', '新实体 ' + made.name + ' #' + made.id);
  } catch (e) { /* 已提示 */ }
}

async function doDelete() {
  if (!DS.sel.length) { toast('没有选中实体', 'warn'); return; }
  const n = DS.sel.length;
  const names = DS.sel.map((id) => (byId(id) || {}).name).filter(Boolean);
  if (!(await uiConfirm('删除 ' + n + ' 个实体?',
                        names.slice(0, 6).join('、') + (names.length > 6 ? ' …' : '') +
                        '(可以用 Ctrl+Z 撤销)', true))) return;
  for (const id of selected()) {
    try { await call('entity.remove', { id }, '删除实体'); }
    catch (e) { break; }
  }
  DS.sel = [];
  await afterEdit('删除 ' + n + ' 个实体');
}

async function doDuplicate(ids) {
  const list = ids && ids.length ? ids : selected();
  if (!list.length) { toast('没有选中实体', 'warn'); return; }
  const r = await call('entity.duplicate', { ids: list, dx: 8, dy: 8 }, '复制实体');
  await refresh();
  select(r.ids);
  log('dim', '复制出 ' + r.ids.length + ' 个实体');
}

async function doCopy() {
  if (!DS.sel.length) { toast('没有选中实体', 'warn'); return; }
  const r = await call('entity.copy', { ids: selected() }, '复制到剪贴板');
  toast('已复制 ' + r.count + ' 个实体(Ctrl+V 粘贴)', 'ok');
}

async function doPaste() {
  try {
    const r = await call('entity.paste', { dx: 16, dy: 16 }, '粘贴');
    await refresh();
    select(r.ids);
    log('dim', '粘贴出 ' + r.ids.length + ' 个实体');
  } catch (e) { /* 已在 call 里提示 */ }
}

async function doRename() {
  const id = DS.sel.length ? DS.sel[DS.sel.length - 1] : 0;
  if (!id) { toast('没有选中实体', 'warn'); return; }
  const e = byId(id);
  const name = ($('ent-name').value || '').trim();
  if (!name) { toast('名字不能为空', 'warn'); return; }
  if (e && name === e.name) return;
  try {
    const r = await call('entity.rename', { id, name }, '改名');
    await refresh();
    toast('已改名为 ' + name +
          (r.refs ? '(逻辑图里 ' + r.refs + ' 处引用一起改了)' : ''), 'ok');
  } catch (err) { /* 已提示 */ }
}

async function setParent(parentId) {
  const id = DS.sel.length ? DS.sel[DS.sel.length - 1] : 0;
  if (!id) return;
  if (parentId === id) { toast('不能让实体做自己的父级', 'warn'); return; }
  try {
    await call('entity.set_parent', { id, parent: parentId }, '设置父级');
    await refresh();
  } catch (e) { /* 已提示 */ }
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

/* ------------------------------------------------------------ 模式切换 */

async function setMode(mode) {
  DS.mode = (mode === 'graph' || mode === 'code' || mode === 'blocks') ? mode : 'scene';
  document.body.classList.toggle('mode-graph', DS.mode === 'graph');
  document.body.classList.toggle('mode-code', DS.mode === 'code');
  document.body.classList.toggle('mode-blocks', DS.mode === 'blocks');
  $('mode-scene').classList.toggle('on', DS.mode === 'scene');
  $('mode-blocks').classList.toggle('on', DS.mode === 'blocks');
  $('mode-graph').classList.toggle('on', DS.mode === 'graph');
  $('mode-code').classList.toggle('on', DS.mode === 'code');
  if (DS.mode === 'blocks') {
    showTab('log');
    if (typeof Blocks !== 'undefined') {
      Blocks.setMode(DS.info && DS.info.logic_mode === 'graph' ? 'graph' : 'blocks');
      await Blocks.refresh();
    }
  } else if (DS.mode === 'graph') {
    showTab('log');
    if (typeof Graph !== 'undefined') {
      Graph.resize();
      await Graph.refresh();
      Graph.renderPalette();
      Graph.renderInspector();
      Graph.draw();
    }
  } else if (DS.mode === 'code') {
    showTab('build');
    if (typeof Code !== 'undefined') await Code.refreshFiles();
  } else if (typeof Viewport !== 'undefined') {
    Viewport.resize();
    Viewport.drawOverlay();
  }
}

/* ------------------------------------------------------------ 输出面板页签 */

function showTab(which) {
  const map = { log: 'log', self: 'selfcheck', sel: 'selinfo', build: 'buildout' };
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

/* 画布像素统计(自检用):"画布上到底有没有东西"只有实测像素才知道。 */
function canvasStats(el) {
  if (!el || !el.width) return { colors: 0, nonBg: 0, sampled: 0 };
  const ctx = el.getContext('2d');
  const d = ctx.getImageData(0, 0, el.width, el.height).data;
  const seen = new Set();
  let nonBg = 0, sampled = 0;
  const stride = Math.max(1, Math.floor(d.length / 4 / 20000)) * 4;
  for (let i = 0; i < d.length; i += stride) {
    sampled++;
    const r = d[i], g = d[i + 1], b = d[i + 2];
    if (!(r === 22 && g === 22 && b === 34)) nonBg++;   /* #161622 = 图布背景 */
    seen.add((r << 16) | (g << 8) | b);
  }
  return { colors: seen.size, nonBg, sampled };
}

function canvasHasColor(el, hex) {
  if (!el || !el.width) return false;
  const ctx = el.getContext('2d');
  const d = ctx.getImageData(0, 0, el.width, el.height).data;
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  for (let i = 0; i < d.length; i += 4) {
    if (Math.abs(d[i] - r) <= 6 && Math.abs(d[i + 1] - g) <= 6 &&
        Math.abs(d[i + 2] - b) <= 6) return true;
  }
  return false;
}

/* 元素**实际**是否隐藏:光看 hidden 属性不够 —— 作者样式里的 display 会盖掉
 * 浏览器默认的 [hidden]{display:none}(这正是"恢复提示条永远显示"的根因)。 */
function hiddenNow(el) {
  if (!el) return true;
  return getComputedStyle(el).display === 'none';
}

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
    /* 自测**自己**把 app.info 记下来:后面"有没有打开项目"的分支依赖 DS.info.root,
     * 而 DS.info 本来是 refresh() 写的 —— 自测可能在 refresh 之前就跑(宿主等的是
     * ui.ready),于是带着项目跑也整段被跳过,白丢一堆断言。 */
    DS.info = info;
    DS.vw = info.view_w || 1024;
    DS.vh = info.view_h || 640;
    if (info.view) DS.view = info.view;
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
    if (ctrl && ev.key.toLowerCase() === 'd') {
      ev.preventDefault();
      if (DS.mode === 'graph' && typeof Graph !== 'undefined') Graph.duplicateSelected();
      else doDuplicate();
      return;
    }
    if (ctrl && ev.key.toLowerCase() === 'c') { ev.preventDefault(); doCopy(); return; }
    if (ctrl && ev.key.toLowerCase() === 'v') { ev.preventDefault(); doPaste(); return; }
    if (ctrl && ev.key.toLowerCase() === 'a') {
      ev.preventDefault();
      if (DS.mode === 'graph' && typeof Graph !== 'undefined') Graph.selectAll();
      else select(DS.entities.map((e) => e.id));
      return;
    }
    if (typing) return;
    if (ev.key === 'F1') { ev.preventDefault(); setMode('scene'); return; }
    if (ev.key === 'F2') { ev.preventDefault(); setMode('blocks'); return; }
    if (ev.key === 'F3') { ev.preventDefault(); setMode('graph'); return; }
    if (ev.key === 'F4') { ev.preventDefault(); setMode('code'); return; }
    if (ev.key === 'F5') { ev.preventDefault(); Code.run(); return; }
    if (ctrl && ev.key.toLowerCase() === 'b') { ev.preventDefault(); Code.compile(); return; }
    if (DS.mode === 'graph' && (ev.key === 'Delete' || ev.key === 'Backspace')) {
      ev.preventDefault();
      Graph.removeSelected();
      return;
    }
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
  if (t === 'brush' && !DS.tilemap) {
    toast('瓦片刷子要一个带「瓦片地图」组件的实体:先选中它,再点「新建瓦片地图…」', 'warn');
  }
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
  if (!DS.info || !DS.info.root) { toast('还没有打开项目', 'warn'); return; }
  try {
    const r = await call('project.save', {}, '保存');
    log('dim', '已保存:' + (r.root || DS.info.root));
    await refresh();
    toast('已保存(场景 + 逻辑图 + 项目)', 'ok');
  } catch (e) { /* 已提示 */ }
}

/* 新建瓦片地图:文件名/大小/图集全部有默认值和下拉,不用手打路径 */
async function doTilemapNew() {
  const id = DS.sel.length ? DS.sel[DS.sel.length - 1] : 0;
  if (!id) { toast('先选中一个实体(会自动给它挂「瓦片地图」组件)', 'warn'); return; }
  const e = byId(id) || {};
  if (!(e.comps || []).includes('tilemap')) {
    try {
      await call('comp.add', { id, comp: 'tilemap' }, '挂瓦片地图组件');
      await refresh();
    } catch (err) { return; }
  }
  const cur = DS.tilemap || {};
  const r = await uiDialog({
    title: '新建瓦片地图',
    hint: '瓦片数据放在 CSV 文件里(场景文件保持干净,地图也能单独替换)',
    fields: [
      { name: 'file', label: '文件名', type: 'text', value: 'tiles.csv' },
      { name: 'cols', label: '列数', type: 'number', value: 32, min: 1, max: 512, step: 1 },
      { name: 'rows', label: '行数', type: 'number', value: 18, min: 1, max: 512, step: 1 },
      { name: 'tex', label: '图集', type: 'select',
        options: [{ value: '', label: '(稍后再设)' }].concat(
          (DS.options.images || []).map((p) => ({ value: p, label: p.replace(/^res\//, '') }))),
        value: cur.tex_path || '' },
    ],
    ok: '创建',
  });
  if (!r) return;
  try {
    const made = await call('tilemap.create', {
      id,
      path: 'res/' + String(r.file || 'tiles.csv').replace(/^res\//, ''),
      cols: r.cols, rows: r.rows, tex_path: r.tex || '',
    }, '新建瓦片地图');
    toast('瓦片地图已建好:' + made.cols + '×' + made.rows + ' —— 切到瓦片刷子开始画', 'ok');
    await refresh();
    setTool('brush');
  } catch (e) { /* 已提示 */ }
}

/* 帮助:第一次用的人最需要的东西 —— 顺序、每一个按钮干什么、快捷键。
 * (以前这些散落在 tooltip 里,新用户只能靠试。) */
async function showHelp() {
  const inProj = DS.info && DS.info.root;
  await uiDialog({
    title: 'DexStudio 怎么用',
    fields: [
      { name: 'i1', type: 'info', value:
        (inProj ? '当前项目:' + DS.info.root : '现在还没有打开项目') },
      { name: 'i2', type: 'info', value:
        '① 左上「新建项目…」选一个文件夹 → 建好就能直接运行\n' +
        '② 「场景」页签:顶栏「+ 实体」挑模板,点左边资源缩略图给它设贴图\n' +
        '③ 「积木」页签(推荐):点左栏积木拼玩法 —— 每个空都是下拉,不用打字\n' +
        '     想让东西动起来:点「每一帧」→ 再点「让 … 往 … 走」\n' +
        '     想加条件:点「如果…」,再点动作积木就放进它的肚子里\n' +
        '④ 顶栏「运行」= 存盘 + 编译 + 开一个独立游戏窗口\n' +
        '⑤ 觉得积木不够用了,再切到「节点」页签(进阶,UE 蓝图式)或「代码」' },
      { name: 'i3', type: 'info', value:
        '快捷键\n' +
        '  F1 场景 · F2 积木 · F3 节点 · F4 代码    Ctrl+S 保存\n' +
        '  Ctrl+B 编译 · F5 运行                    Ctrl+Z / Ctrl+Y 撤销重做\n' +
        '  Ctrl+D 复制实体/节点 · Delete 删除\n' +
        '  1 选择工具 · 2 瓦片刷子 · F 缩放到全部\n' +
        '  视口:滚轮缩放 / 中键或右键拖动平移 / Shift 点选多个\n' +
        '  场景树:拖一行到另一行 = 设成它的子实体' },
      { name: 'i4', type: 'info', value:
        '容易踩的点\n' +
        '  · 场景里存的是**项目相对路径**(res/xxx.png),项目可以整个搬走\n' +
        '  · 贴图太大?点缩略图会自动缩小;也可以在「缩放 X / 缩放 Y」里改(1 = 原大小)\n' +
        '  · 实体"不见了":选中它按右边「定位」,或在场景树里双击\n' +
        '  · 「编译」会把当前场景与玩法一起存盘,游戏读的就是你看到的那份\n' +
        '  · 资源改名会连引用一起改;删掉被引用的资源会先拦一下' },
    ],
    ok: '知道了', cancel: '关闭',
  });
}

/* ------------------------------------------------------------ 接线 */

function wire() {
  $('btn-new').onclick = doNewProject;
  $('btn-open').onclick = doOpenProject;
  $('recent').onchange = () => {
    const p = $('recent').value;
    if (p) openProjectAt(p);
  };
  $('btn-save').onclick = saveAll;
  $('btn-reload').onclick = async () => {
    const sc = DS.info && DS.info.scene;
    if (!sc) { toast('没有场景可重载', 'warn'); return; }
    if (DS.info.dirty && !(await uiConfirm('从磁盘重载?',
        '内存里没保存的改动会丢掉', true))) return;
    try {
      await call('scene.load', { path: sc }, '重载场景');
      DS.sel = [];
      await refresh();
      toast('已重载 ' + baseName(sc), 'ok');
    } catch (e) { /* 已提示 */ }
  };
  $('btn-add').onclick = doAddEntity;
  $('btn-dup').onclick = () => doDuplicate();
  $('btn-del').onclick = doDelete;
  $('btn-undo').onclick = () => undoRedo('undo');
  $('btn-redo').onclick = () => undoRedo('redo');
  $('scene-sel').onchange = () => loadScene($('scene-sel').value);
  $('btn-scene-new').onclick = doSceneNew;
  $('btn-scene-rename').onclick = doSceneRename;
  $('btn-scene-del').onclick = doSceneDelete;
  $('chk-start-scene').onchange = async (e) => {
    const cur = DS.info && DS.info.scene ? baseName(DS.info.scene).replace(/\.json$/, '') : '';
    if (!cur) return;
    if (!e.target.checked) {          /* 总得有一个起始场景 */
      e.target.checked = true;
      toast('至少要有一个起始场景(想换:切到那个场景再勾它)', 'warn');
      return;
    }
    try {
      await call('scene.set_start', { name: cur }, '设为起始场景');
      toast('游戏启动时会加载「' + cur + '」', 'ok');
      await refresh();
    } catch (err) { e.target.checked = false; }
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
  $('chk-debug').onchange = (e) => { DS.quietLog = !e.target.checked; };
  $('btn-selfcheck').onclick = selfcheck;
  $('btn-res-import').onclick = () => importRes();
  $('btn-res-refresh').onclick = () => refreshResources();
  $('btn-recover').onclick = async () => {
    try {
      await call('recover.apply', {}, '恢复自动保存');
      DS.sel = [];
      await refresh();
      toast('已从上一次的自动保存恢复(记得点「保存」)', 'ok');
    } catch (e) { /* 已提示 */ }
  };
  $('btn-recover-drop').onclick = async () => {
    try {
      await call('recover.discard', {}, '丢弃自动保存');
      renderRecover();
      toast('已丢弃自动保存', 'ok');
    } catch (e) { /* 已提示 */ }
  };
  $('mode-scene').onclick = () => setMode('scene');
  $('mode-blocks').onclick = () => setMode('blocks');
  $('mode-graph').onclick = () => setMode('graph');
  $('mode-code').onclick = () => setMode('code');
  /* --- 积木模式 --- */
  $('btn-block-new').onclick = () => Blocks.addScript();
  $('btn-block-check').onclick = () => Blocks.validate();
  $('btn-block-gen').onclick = () => Blocks.generate();
  $('btn-tpl-move').onclick = () => Blocks.template('move_jump');
  $('btn-tpl-camera').onclick = () => Blocks.template('camera');
  $('btn-tpl-sound').onclick = () => Blocks.template('sound');
  $('lm-blocks').onchange = async () => {
    await ds('logic.mode', { mode: 'blocks' });
    await Blocks.setMode('blocks');
    toast('现在用「积木」生成游戏逻辑', 'ok');
  };
  $('lm-graph').onchange = async () => {
    await ds('logic.mode', { mode: 'graph' });
    await Blocks.setMode('graph');
    toast('现在用「节点图」生成游戏逻辑(进阶)', 'ok');
  };
  $('btn-compile').onclick = () => Code.compile();
  $('btn-run').onclick = () => Code.run();
  $('btn-stop').onclick = () => Code.stop();
  $('btn-help').onclick = showHelp;
  $('btn-graph-save').onclick = () => Graph.save();
  $('btn-graph-check').onclick = () => Graph.check();
  $('btn-graph-gen').onclick = () => Graph.generate();
  $('btn-graph-dup').onclick = () => Graph.duplicateSelected();
  $('btn-graph-del').onclick = () => Graph.removeSelected();
  $('graph-filter').oninput = () => Graph.renderPalette();
  $('btn-graph-new').onclick = async () => {
    if (!(await uiConfirm('清空整张逻辑图?', '可以撤销', true))) return;
    try {
      await call('graph.new', {}, '清空逻辑图');
      await Graph.refresh();
      Graph.renderInspector();
    } catch (e) { /* 已提示 */ }
  };
  $('btn-graph-fit').onclick = () => { Graph.center(); };
  $('btn-clear-log').onclick = () => { $('log').innerHTML = ''; logLines = 0; };
  document.querySelectorAll('.tabs .tab').forEach((b) => {
    b.onclick = () => showTab(b.dataset.tab);
  });
  $('btn-tile-apply').onclick = applyAtlas;
  $('btn-atlas-auto').onclick = () => {
    /* 图集列数其实能从"图片宽度 ÷ 格子宽度"算出来 —— 不该让用户去数 */
    const img = $('palette');
    if (!DS.tilemap || !img || !img.naturalWidth) {
      toast('先给瓦片地图选一张图集,再点自动算', 'warn');
      return;
    }
    const tw = Math.max(1, Math.round(DS.tilemap.tw || 16));
    const cols = Math.max(1, Math.round(img.naturalWidth / tw));
    $('atlas-cols').value = cols;
    applyAtlas();
    toast('按图片宽度算出 ' + cols + ' 列(' + img.naturalWidth + '÷' + tw + ')', 'ok');
  };
  $('btn-tilemap-new').onclick = doTilemapNew;
  $('btn-tile-clear').onclick = async () => {
    if (!DS.tilemap) return;
    if (!(await uiConfirm('把整张瓦片图清空?', '一格都不剩(可以撤销)', true))) return;
    const cols = DS.tilemap.cols, rows = DS.tilemap.rows;
    const csv = Array.from({ length: rows }, () => Array(cols).fill(-1).join(',')).join('\n');
    try {
      await call('tilemap.csv', { id: DS.tilemap.id, csv }, '清空瓦片');
      await refresh();
    } catch (e) { /* 已提示 */ }
  };
  $('btn-tile-empty').onclick = () => pickTile(-1);
  /* 实体:改名(按钮 + 回车)、父级下拉 */
  $('btn-rename').onclick = doRename;
  $('btn-locate').onclick = () => {
    const id = DS.sel.length ? DS.sel[DS.sel.length - 1] : 0;
    if (!id) { toast('先选中一个实体', 'warn'); return; }
    centerOn(id);                       /* 视口对准它(找不到实体时最有用) */
    toast('已把视口对准选中实体', 'ok');
  };
  $('ent-name').onkeydown = (ev) => { if (ev.key === 'Enter') doRename(); };
  $('ent-parent').onchange = () => setParent(parseInt($('ent-parent').value, 10));
  /* 挂组件:以前这个按钮**没有处理器**(点了没反应,于是根本挂不上组件) */
  $('btn-comp-add').onclick = async () => {
    const id = DS.sel.length ? DS.sel[DS.sel.length - 1] : 0;
    const comp = $('comp-add-sel').value;
    if (!id) { toast('先选中一个实体', 'warn'); return; }
    if (!comp) { toast('先在下拉里选一个组件', 'warn'); return; }
    try {
      await call('comp.add', { id, comp }, '挂组件 ' + comp);
      await refresh();
      toast('已挂上 ' + comp + ',右边可以填它的字段了', 'ok');
    } catch (e) { /* 已提示 */ }
  };
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
  $('tile-pick').textContent = t < 0 ? '橡皮/空' : ('图块 ' + t);
}

/* ------------------------------------------------------------ 界面自测
 *
 * 无窗口、无人值守地验证**界面这一层**是否真的可用。宿主 `--wv-selftest` 会调它
 * 并断言结果。(这正是 AGENTS.md 里"自动化离屏证明代替肉眼看屏幕"的做法。)
 *
 * 本次加了两类断言(以前的 68 项全过却漏掉了真问题):
 *   1. **computed display**:光断言 `.hidden` 属性不够 —— CSS 级联可以让它失效;
 *   2. **真的点一下按钮**:只检查"按钮存在"证明不了它接线了。 */
window.__ds_selftest = async function () {
  const out = { pass: [], fail: [], skip: [] };
  const t = (name, ok, extra) => {
    (ok ? out.pass : out.fail).push(name + (extra ? '  ' + extra : ''));
  };
  const wasQuiet = DS.quietLog;
  DS.quietLog = true;
  let tmpId = 0;
  try {
    /* 离屏/无人值守启动时窗口尺寸是后设上来的:断言之前先按容器尺寸对齐一次,
     * 并等一帧让布局稳定(否则画布可能是 0×0,像素断言全废)。 */
    Viewport.resize();
    if (typeof Graph !== 'undefined') Graph.resize();
    await new Promise((r) => requestAnimationFrame(() => r()));
    await new Promise((r) => setTimeout(r, 30));
    /* --- 通道与状态 --- */
    t('消息桥存在', !!bridge);
    t('app.info 有版本', !!(DS.info && DS.info.version), 'v' + (DS.info || {}).version);
    t('引擎可用', !!(DS.info && DS.info.engine),
      DS.info && DS.info.engine ? '' : (DS.info || {}).engine_error);
    t('组件自省有 8 种', DS.schema.length >= 8, DS.schema.length + ' 种');

    /* --- 条件显示:hidden 必须真的隐藏(不看属性,看 computed display) --- */
    ['recover-bar', 'brush-tools', 'ent-ops', 'comp-add-row'].forEach((id) => {
      const el = $(id);
      el.hidden = true;
      t('hidden 真的隐藏了 #' + id + '(computed display)',
        getComputedStyle(el).display === 'none',
        'display=' + getComputedStyle(el).display);
    });
    select([]);
    await renderInspector();
    t('没选中实体时不显示改名行', hiddenNow($('ent-ops')));
    t('没选中实体时不显示挂组件行', hiddenNow($('comp-add-row')));
    setTool('select');
    t('选择工具时收起刷子选项', hiddenNow($('brush-tools')));
    DS.info = Object.assign({}, DS.info, { recoverable: false });
    renderRecover();
    t('没有可恢复内容时收起恢复条', hiddenNow($('recover-bar')));

    /* --- 字段元数据到位(前端据此出下拉/复选,而不是手打) --- */
    {
      const collider = schemaOf('collider');
      const kind = collider && collider.fields.find((f) => f.name === 'kind');
      t('collider.kind 是枚举下拉', !!kind && kind.kind === 'enum'
        && (kind.enum || []).length >= 3, kind && JSON.stringify(kind.enum));
      const sprite = schemaOf('sprite');
      const tex = sprite && sprite.fields.find((f) => f.name === 'tex_path');
      t('sprite.tex_path 是资源下拉', !!tex && tex.kind === 'image', tex && tex.kind);
      const tint = sprite && sprite.fields.find((f) => f.name === 'tint');
      t('sprite.tint 是取色器', !!tint && tint.kind === 'color', tint && tint.kind);
      const rot = schemaOf('transform').fields.find((f) => f.name === 'rot');
      t('engine 没实现的字段是只读', !!rot && !!rot.readonly, rot && rot.hint);
      t('场景选项带实体下拉候选', (DS.options.entities || []).length === DS.entities.length,
        (DS.options.entities || []).length + ' vs ' + DS.entities.length);
    }

    /* --- 离屏渲染真的到了画布上 --- */
    const ii = Viewport.imgInfo();
    t('渲染图已解码', ii.ready && ii.w > 0, ii.w + '×' + ii.h);
    t('渲染图尺寸 = 引擎视口', ii.w === DS.vw && ii.h === DS.vh,
      ii.w + '×' + ii.h + ' vs ' + DS.vw + '×' + DS.vh);
    t('渲染图走虚拟主机', ii.src.indexOf('dexstudio-preview.local') >= 0, ii.src);
    const px = Viewport.pixelStats();
    /* 这一条只证明"离屏渲染的那一帧真的画到画布上了"。空场景本来就是一片清屏色,
     * 所以颜色种数不能当判据 —— 真正的"画了东西"断言在后面有贴图时做。 */
    t('渲染图真的画到画布上', px.nonBg > px.sampled * 0.25,
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
    t('改名行出现了', !hiddenNow($('ent-ops')));
    t('父级下拉列出了除自己以外的所有实体',
      $('ent-parent').options.length ===
        DS.entities.filter((e) => e.id !== added.id).length + 1,
      $('ent-parent').options.length + ' 项');
    Viewport.drawOverlay();
    t('视口画出了选中框', Viewport.hasColor('#f9e2af'));

    /* --- 「挂组件」按钮真的能挂上(以前它没有处理器) --- */
    {
      const before = (byId(added.id) || {}).comps || [];
      $('comp-add-sel').value = 'camera';
      $('btn-comp-add').click();
      await new Promise((r) => setTimeout(r, 150));
      const after = ((await ds('entity.get', { id: added.id })).comps) || {};
      t('点「挂组件」真的挂上了组件',
        before.indexOf('camera') < 0 && !!after.camera,
        Object.keys(after).join(','));
      await refresh();
    }

    /* --- 「改名」按钮真的能改名(以前它也没有处理器) --- */
    {
      $('ent-name').value = '__uitest_renamed__';
      $('btn-rename').click();
      await new Promise((r) => setTimeout(r, 150));
      const got = await ds('entity.get', { id: added.id });
      t('点「改名」真的改了名字', got.name === '__uitest_renamed__', got.name);
      await ds('entity.rename', { id: added.id, name: '__uitest__' });
      await refresh();
    }

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

    /* --- 父子关系(以前只能手填实体 id) --- */
    {
      const parentId = (DS.entities.find((e) => e.name === '__uitest__') || {}).id;
      const child = await ds('entity.add', { name: '__uitest_child__' });
      const r = await ds('entity.set_parent', { id: child.id, parent: parentId });
      t('entity.set_parent 可用', !!r && typeof r.parent === 'number',
        JSON.stringify(r));
      await refresh();
      select([child.id]);
      await renderInspector();
      t('父级下拉选中了父实体',
        parseInt($('ent-parent').value, 10) === parentId, $('ent-parent').value);
      await ds('entity.remove', { id: child.id });
      await refresh();
    }

    /* --- 瓦片地图:新建 → 画一格 → 读回来 --- */
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
      await refresh();
      const back = DS.entities.find((e) => e.name === '__tiletest__');
      info = await ds('tilemap.info', { id: back ? back.id : tm.id });
      t('撤销 paint 后 CSV 复原(paint 一条撤销)',
        info.tiles[0] === -1 && info.tiles[1] === -1,
        info.tiles.slice(0, 4).join(','));
    }
    t('「新建瓦片地图」按钮存在', !!$('btn-tilemap-new'));
    t('帮助入口存在', !!$('btn-help'));

    /* --- 图层面板:整层上移/下移(以前只能手改数字) --- */
    {
      const tmEnt = DS.entities.find((e) => e.name === '__tiletest__');
      const tmOut = tmEnt ? outlineOf(tmEnt.id) : null;
      if (!tmEnt || !tmOut) {
        out.skip.push('图层上下移:没有可用的测试实体');
      } else {
        const layer = tmOut.layer || 0;
        const row = Array.prototype.find.call(
          document.querySelectorAll('#layers li'),
          (li) => (li.textContent || '').indexOf('图层 ' + layer) === 0);
        const up = row && row.querySelector('button[data-shift="1"]');
        t('图层行有上下移按钮', !!up, row ? row.textContent.trim() : '(没找到那一行)');
        if (up) {
          up.click();
          await new Promise((r) => setTimeout(r, 200));
          const after = (outlineOf(tmEnt.id) || {}).layer;
          t('点 ↑ 把整层挪了一格', after === layer + 1, layer + ' → ' + after);
          const back = Array.prototype.find.call(
            document.querySelectorAll('#layers li'),
            (li) => (li.textContent || '').indexOf('图层 ' + (layer + 1)) === 0);
          const down = back && back.querySelector('button[data-shift="-1"]');
          if (down) {
            down.click();
            await new Promise((r) => setTimeout(r, 200));
          }
        }
      }
    }

    /* --- 清理:把测试实体**和它建的 CSV** 都删掉 --- */
    const ids = DS.entities.map((e) => e.id);
    for (const id of ids) {
      const e = byId(id);
      if (e && /__uitest__|__tiletest__/.test(e.name || '')) {
        await ds('entity.remove', { id });
      }
    }
    DS.sel = [];
    await refresh();
    t('清理后无残留测试实体',
      !DS.entities.some((e) => /__uitest__|__tiletest__/.test(e.name || '')));
    if (DS.info && DS.info.root) {
      try {
        const rl = await ds('res.list');
        if ((rl.files || []).some((f) => f.name === '__uitest__.csv')) {
          await ds('res.delete', { name: '__uitest__.csv', force: 1 });
        }
      } catch (e) { /* 没有就算了 */ }
      await refreshResources();
      t('清理后没在 res/ 里留下自测的 CSV',
        !(DS.res || []).some((f) => f.name === '__uitest__.csv'),
        (DS.res || []).map((f) => f.name).join(','));
    }

    /* --- 积木模式:给零基础用户的那一套(不连线、不打字) --- */
    await setMode('blocks');
    t('切到积木模式', document.body.classList.contains('mode-blocks'));
    t('积木盒子列出了积木(且与 C 的目录一致)',
      document.querySelectorAll('#block-palette .blk-item').length === Blocks.types.length
      && Blocks.types.length >= 12,
      document.querySelectorAll('#block-palette .blk-item').length + ' vs ' + Blocks.types.length);
    t('积木模式信息条有内容', ($('block-mode-info').textContent || '').length > 0,
      $('block-mode-info').textContent);
    {
      /* 自检**不能破坏用户已有的积木**:记下原来的脚本 id,结尾删掉自己加的 */
      const keepIds = (Blocks.scripts || []).map((s) => s.id);
      const before = Blocks.stats();
      /* 积木默认要指向一个真实实体,所以先建一个自测用的 */
      const bEnt = await ds('entity.add', { name: '__blocktest__' });
      await refresh();
      /* ① 一键示例:一段"每帧往右走 + 按跳键就跳" */
      await Blocks.template('move_jump');
      const after = Blocks.stats();
      t('点「一键示例」就有一段会动的积木',
        after.scripts === before.scripts + 1 && after.blocks >= before.blocks + 2,
        JSON.stringify(before) + ' → ' + JSON.stringify(after));
      /* ② **真的点左栏积木**(不是调 API):应该加进当前那段 */
      const n1 = Blocks.stats().blocks;
      const item = Array.prototype.find.call(
        document.querySelectorAll('#block-palette .blk-item'),
        (b) => b.title === 'move');
      t('左栏能找到「让…走」这块积木', !!item, item ? item.textContent : '');
      if (item) {
        item.click();
        await new Promise((r) => setTimeout(r, 250));
      }
      t('点一下左栏积木就加进去了', Blocks.stats().blocks === n1 + 1,
        n1 + ' → ' + Blocks.stats().blocks);
      /* ③ 下拉改值:把刚加的这块的实体/方向改掉 */
      {
        const sel2 = document.querySelector('#block-area .block select.blk-slot');
        t('积木里的空是下拉(不是输入框)', !!sel2, sel2 ? sel2.outerHTML.slice(0, 90) : '');
        if (sel2 && sel2.options.length > 1) {
          const target = sel2.options[1].value;
          sel2.value = target;
          sel2.dispatchEvent(new Event('change'));
          await new Promise((r) => setTimeout(r, 250));
          const dump = JSON.stringify(Blocks.scripts);
          t('改下拉真的写进了模型', dump.indexOf('"' + target + '"') >= 0,
            target + ' / ' + dump.slice(0, 120));
        }
      }
      /* ④ 如果积木:点它 → 张开肚子;再点动作积木 → 放进肚子里 */
      const ifItem = Array.prototype.find.call(
        document.querySelectorAll('#block-palette .blk-item'),
        (b) => b.title === 'if_key');
      const jumpItem = Array.prototype.find.call(
        document.querySelectorAll('#block-palette .blk-item'),
        (b) => b.title === 'jump');
      if (ifItem && jumpItem) {
        /* 先数一遍"肚子里有多少积木"(要和点击后对比,别依赖 DOM 顺序) */
        const bodies = () => (Blocks.scripts || []).reduce((n, s) =>
          n + (s.blocks || []).reduce((k, b) => k + (b.body || []).length, 0), 0);
        const bodies0 = bodies();
        ifItem.click();
        await new Promise((r) => setTimeout(r, 250));
        const ifRows = Array.prototype.slice.call(
          document.querySelectorAll('#block-area .block.cat-if'));
        t('「如果」积木出现在脚本里并张开肚子',
          ifRows.length > 0 && !!ifRows[0].querySelector('.if-body'),
          ifRows.length + ' 个如果积木');
        /* 选中**刚加的那块**「如果」,再点动作积木 → 应该进它的肚子 */
        const last = ifRows.length ? ifRows[ifRows.length - 1] : null;
        if (last) {
          last.click();
          await new Promise((r) => setTimeout(r, 80));
          jumpItem.click();
          await new Promise((r) => setTimeout(r, 300));
          t('动作积木被放进了「如果」的肚子里(不是并排放在下面)',
            bodies() === bodies0 + 1, bodies0 + ' → ' + bodies());
        }
      } else {
        out.skip.push('并没有找到「如果按着…」积木');
      }
      /* ⑤ 生成代码:积木 → DexLang(编译/运行用的就是它)。
       * 没有项目时"生成到文件"没地方放 —— 那就照实跳过。 */
      if (DS.info && DS.info.root) {
        const g = await Blocks.generate();
        t('积木生成出可编译的代码',
          !!g && g.source.indexOf('func logic_update') >= 0
          && g.source.indexOf('eng_set_f(eng_find(') >= 0,
          g ? g.source.split('\n').slice(0, 3).join(' / ') : 'no source');
        t('生成的代码会被编译(run 之前的那一步)',
          !!g && g.source.indexOf('include "dexgame";') >= 0);
      } else {
        out.skip.push('没有打开项目:跳过积木生成落盘检查');
      }
      /* ⑥ 收尾:删掉自测加的脚本/实体,回到原来那几段 */
      for (const s of (Blocks.scripts || []).slice()) {
        if (keepIds.indexOf(s.id) < 0) {
          await ds('blocks.script.remove', { id: s.id });
        }
      }
      await ds('entity.remove', { id: bEnt.id });
      await refresh();
      await Blocks.refresh();
      t('自检收尾:积木回到原样', Blocks.stats().scripts === keepIds.length,
        Blocks.stats().scripts + ' vs ' + keepIds.length);
    }
    await setMode('scene');
    t('积木模式能切回场景', !document.body.classList.contains('mode-blocks'));

    /* --- 逻辑图模式:节点面板 / 下拉 / 加节点 / 属性 / 连线 / 校验 / 生成 --- */
    await setMode('graph');
    t('切到逻辑图模式', document.body.classList.contains('mode-graph'));
    /* 逻辑图引用的是**实体名**,所以先建一个实体给节点引用(校验会检查它存在) */
    const gEnt = await ds('entity.add', { name: '__graphtest__' });
    await refresh();
    const gcanvas = $('graph');
    t('逻辑图画布有尺寸', !!gcanvas && gcanvas.clientWidth > 0 && gcanvas.clientHeight > 0,
      (gcanvas ? gcanvas.clientWidth + '×' + gcanvas.clientHeight : 'no canvas') +
      ' · 窗口 ' + window.innerWidth + '×' + window.innerHeight +
      ' · 视口容器 ' + $('viewport').clientWidth + '×' + $('viewport').clientHeight);
    const btns = document.querySelectorAll('#graph-palette .nodebtn').length;
    t('节点面板列出全部节点类型', btns === Graph.stats().types && btns > 15,
      btns + ' vs ' + Graph.stats().types);
    const n0 = Graph.stats().nodes;
    const l0 = Graph.stats().links;

    const evId = await Graph.addNode('on_update', 60, 60);
    const sfId = await Graph.addNode('set_field', 320, 60);
    const nmId = await Graph.addNode('num', 60, 260);
    t('加节点进了图', Graph.stats().nodes === n0 + 3, Graph.stats().nodes);
    const gpx = canvasStats(gcanvas);
    t('画布上画出了节点', gpx.nonBg > 500, JSON.stringify(gpx));
    Graph.select(sfId);
    t('正在编辑的节点有选中框', canvasHasColor(gcanvas, '#f9e2af'));
    const grows = document.querySelectorAll('#graph-inspector .field, #graph-inspector .field-ctl')
      .length;
    t('节点属性面板生成字段', grows >= 4, grows + ' 个');
    {
      const sels = document.querySelectorAll('#graph-inspector select').length;
      t('节点属性里的引用型字段都是下拉', sels >= 4, sels + ' 个下拉');
    }
    t('逻辑图信息条有内容', ($('graph-info').textContent || '').indexOf('节点') >= 0,
      $('graph-info').textContent);
    await ds('graph.node.set', { id: sfId, props: {
      obj: '__graphtest__', comp: 'transform', field: 'x', value: 3, as: 'f' } });
    await ds('graph.link', { from: evId, from_pin: 'out', to: sfId, to_pin: 'exec' });
    await ds('graph.link', { from: nmId, from_pin: 'v', to: sfId, to_pin: 'value' });
    await Graph.refresh();
    Graph.select(sfId);
    t('连线进了图(执行流 + 数据线)', Graph.stats().links === l0 + 2,
      Graph.stats().links);
    t('画布上画出了连线', canvasHasColor(gcanvas, '#89b4fa'));
    {
      const pin = Graph.pinScreenPos(nmId, 'v');
      const rect = gcanvas.getBoundingClientRect();
      const evd = (type, x, y, buttons) => gcanvas.dispatchEvent(
        new MouseEvent(type, { bubbles: true, clientX: rect.left + x,
                               clientY: rect.top + y, button: 0, buttons }));
      if (!pin) {
        t('能取到输出引脚的屏幕位置', false, 'pinScreenPos 返回 null');
      } else {
        evd('mousedown', pin.x, pin.y, 1);
        const st0 = Graph.dragState();
        t('从输出引脚拖出预览线', !!st0.pending, JSON.stringify(st0));
        const tx = pin.x + 120, ty = pin.y + 80;
        evd('mousemove', tx, ty, 1);
        const st1 = Graph.dragState();
        const moved = st1.pending
          && Math.abs(st1.pending.gx - st0.pending.gx) > 20
          && Math.abs(st1.pending.gy - st0.pending.gy) > 20;
        t('预览线跟着鼠标走(不是连完才出现)', moved,
          JSON.stringify(st0) + ' → ' + JSON.stringify(st1));
        evd('mouseup', tx, ty, 0);
        t('松开后预览线消失', !Graph.dragState().pending);
        await Graph.refresh();
        Graph.select(sfId);
      }
    }
    /* --- 逻辑图多选:框选 → 一起复制 / 一起删除 / 一起拖动 --- */
    {
      Graph.selectMany([sfId, nmId], false);
      const st = Graph.stats();
      t('逻辑图能多选(框选 / Shift 点选)', st.selCount === 2, JSON.stringify(st));
      const beforeNodes = Graph.stats().nodes;
      await Graph.duplicateSelected();
      t('一次复制多个节点', Graph.stats().nodes === beforeNodes + 2, Graph.stats().nodes);
      const copied = Graph.selection.slice();
      await ds('graph.node.remove', { ids: copied });
      await Graph.refresh();
      t('一次删除多个节点(一条撤销)', Graph.stats().nodes === beforeNodes,
        Graph.stats().nodes);
      /* 拖动一组选中节点:两个节点要一起动(走 graph.node.move_many) */
      Graph.selectMany([sfId, nmId], false);
      const p0a = (Graph.doc.nodes.find((n) => n.id === sfId) || {}).x;
      const p0b = (Graph.doc.nodes.find((n) => n.id === nmId) || {}).x;
      const c = Graph.nodeScreenPos(sfId);
      const rect2 = gcanvas.getBoundingClientRect();
      const evd2 = (type, x, y, buttons) => gcanvas.dispatchEvent(
        new MouseEvent(type, { bubbles: true, clientX: rect2.left + x,
                               clientY: rect2.top + y, button: 0, buttons }));
      if (c) {
        evd2('mousedown', c.x, c.y, 1);
        evd2('mousemove', c.x + 64, c.y + 40, 1);
        evd2('mouseup', c.x + 64, c.y + 40, 0);
        await new Promise((r) => setTimeout(r, 200));
        const p1a = (Graph.doc.nodes.find((n) => n.id === sfId) || {}).x;
        const p1b = (Graph.doc.nodes.find((n) => n.id === nmId) || {}).x;
        t('拖一个节点时整组一起动', p1a !== p0a && p1b !== p0b,
          p0a + '/' + p0b + ' → ' + p1a + '/' + p1b);
        await Graph.refresh();
        Graph.select(sfId);
      } else {
        t('能取到节点的屏幕位置', false, 'nodeScreenPos 返回 null');
      }
    }
    if (DS.info && DS.info.root) {
      const ck = await ds('graph.validate');      t('graph.validate 可用', typeof ck.count === 'number', '问题 ' + ck.count);
      const gen = await ds('graph.generate');
      t('生成代码落盘', !!gen.path && gen.source.indexOf('func logic_update') >= 0,
        gen.path);
      t('生成的代码含刚连的字段写入',
        gen.source.indexOf('"transform", "x"') >= 0, gen.source);
    } else {
      out.skip.push('没有打开项目:跳过 graph.generate 落盘检查');
    }
    for (const id of [evId, sfId, nmId]) await ds('graph.node.remove', { id });
    await Graph.refresh();
    t('自检收尾:图回到原样',
      Graph.stats().nodes === n0 && Graph.stats().links === l0, Graph.stats().nodes);
    await ds('entity.remove', { id: gEnt.id });
    await setMode('scene');
    t('切回场景模式', !document.body.classList.contains('mode-graph'));

    /* --- 代码页签:自写高亮 / 文件列表 / 编译输出与错误跳转 --- */
    t('高亮器认关键字与字符串',
      (() => {
        const ls = Highlight.lines('let x = 1; # 注释\nprint "s"; foo(1);');
        return ls[0].indexOf('tok-kw') >= 0 && ls[0].indexOf('tok-cmt') >= 0
          && ls[1].indexOf('tok-str') >= 0 && ls[1].indexOf('tok-fn') >= 0;
      })(), Highlight.lines('let x = 1;').join('').slice(0, 120));
    t('高亮器不把注释里的引号当字符串',
      Highlight.lines('let a = 1; # "x"\nprint a;')[0].indexOf('tok-str') < 0);
    t('高亮器逐行编号', Highlight.lines('a\nb\nc').length === 3);
    await setMode('code');
    t('切到代码模式', document.body.classList.contains('mode-code'));
    const cwrap = document.querySelector('.code-wrap');
    t('代码面板可见且有尺寸',
      !!cwrap && cwrap.clientWidth > 0 && cwrap.clientHeight > 0,
      cwrap ? cwrap.clientWidth + '×' + cwrap.clientHeight : 'none');
    t('代码页签有「外部编辑」入口', !!$('btn-code-open'));
    Code.renderBuild({
      ok: false, code: 1, errors: 1, warnings: 1, bytecode_exists: false,
      bytecode: 'x.dexbc',
      diag: [
        { level: 'error', phase: 'parser', file: 'C:\\p\\scripts\\bad.dex',
          line: 3, col: 1, msg: "expected '}' before end of file" },
        { level: 'warning', phase: 'compiler', file: 'C:\\p\\scripts\\bad.dex',
          line: 5, col: 5, msg: 'unreachable statement' },
      ],
      out: '汇编(IR): …',
    });
    t('编译输出列出诊断(可点)', document.querySelectorAll('#buildout .jump').length === 2,
      document.querySelectorAll('#buildout .jump').length);
    t('错误/警告分色',
      document.querySelectorAll('#buildout .diag.err').length === 2
      && document.querySelectorAll('#buildout .diag.warn').length === 1);
    t('诊断里带上行列号',
      (document.querySelector('#buildout .jump') || {}).textContent.indexOf('3:1') >= 0,
      (document.querySelector('#buildout .jump') || {}).textContent);
    document.querySelector('#buildout .jump').click();
    await new Promise((r) => setTimeout(r, 60));
    t('点诊断会切到代码模式', document.body.classList.contains('mode-code'));
    if (DS.info && DS.info.root) {
      const r = await Code.compile();
      t('界面里能一键编译', !!r && r.ok === true, r && r.out);
      t('编译成功时输出里有字节码行', !!r && r.out.indexOf('字节码') >= 0, r && r.out);
      t('编译结果报出起始场景', !!r && typeof r.start_scene === 'string', r && r.start_scene);
      t('代码页签列出了脚本', Code.files.length >= 1, Code.files);
      t('代码页签渲染了高亮的源码',
        document.querySelectorAll('#code .ln').length > 0
        && !!document.querySelector('#code .tok-kw'), Code.current);
    } else {
      out.skip.push('没有打开项目:跳过界面里的 build.compile 检查');
    }
    await setMode('scene');
    t('收尾回到场景模式', !document.body.classList.contains('mode-code'));

    /* --- 项目/场景/资源面板 --- */
    t('左栏有资源面板', !!document.getElementById('res-sec'));
    t('项目/场景下拉都在',
      !!$('scene-sel') && !!$('recent') && !!$('btn-new') && !!$('btn-open'));
    await refreshResources();
    const rcount = parseInt(document.getElementById('res-count').textContent, 10);
    t('资源计数与 res.list 一致', rcount === DS.res.length,
      rcount + ' vs ' + DS.res.length);
    if (DS.info && DS.info.root) {
      const rl = await ds('res.list');
      t('界面里的 res.list 可用', typeof rl.count === 'number', rl.count);
      t('场景下拉列出了场景', $('scene-sel').options.length >= 1
        && DS.scenes.length >= 1, JSON.stringify(DS.scenes));
      const tick = await ds('autosave.tick');
      t('界面里能敲自动保存节拍', typeof tick.saved === 'boolean', tick);
      t('自动保存的节拍已经挂上(30 秒一次)', startAutosave.toString().indexOf('30000') > 0);
      try {
        const resp = await fetch('https://dexstudio-proj.local/project.json');
        const txt = await resp.text();
        t('项目根映射出去了(资源缩略图靠它)',
          resp.ok && txt.indexOf('"name"') >= 0, resp.status);
      } catch (e) {
        t('项目根映射出去了(资源缩略图靠它)', false, String(e));
      }
      {
        const imgs = (DS.res || []).filter((f) => f.kind === 'image');
        const auds = (DS.res || []).filter((f) => f.kind === 'audio');
        /* 声音:点一下要真的能播。以前只 log 失败,页面自测根本不管 ——
         * 而用户遇到的正是"试听没反应"(虚拟主机 URL 或解码失败都会这样)。 */
        if (!auds.length) {
          out.skip.push('没有音频资源:跳过试听检查');
        } else {
          const aurl = resUrl(auds[0].name);
          const aok = await new Promise((res) => {
            const a = new Audio();
            a.volume = 0;
            const done = (v) => res(v);
            a.oncanplay = () => done(true);
            a.onerror = () => done(false);
            setTimeout(() => done(false), 8000);
            a.src = aurl;
            a.play().catch(() => done(false));
          });
          t('声音能解码并播放(' + auds[0].name + ')', aok, aurl);
        }
        if (!imgs.length) {
          out.skip.push('没有图片资源:跳过缩略图解码检查');
        } else {
          const texSize = {};
          for (const img of imgs) {
            const url = resUrl(img.name);
            const ok = await new Promise((res) => {
              const im = new Image();
              const done = (v) => res(v);
              im.onload = () => {
                texSize[img.name] = { w: im.naturalWidth, h: im.naturalHeight };
                done(im.naturalWidth > 0 && im.naturalHeight > 0);
              };
              im.onerror = () => done(false);
              setTimeout(() => done(false), 5000);
              im.src = url;
            });
            t('缩略图真的解码了  ' + img.name, ok, url);
          }
          const keepSel = DS.sel;
          const e0 = await ds('entity.add', { name: '__uitest_tex__' });
          try {
            await ds('comp.add', { id: e0.id, comp: 'sprite' });
            DS.sel = [e0.id];
            await applyRes(imgs[0]);
            const g = await ds('entity.get', { id: e0.id });
            const sp = (g.comps || {}).sprite || {};
            t('点缩略图能给实体设上贴图(tex_path 是项目相对路径)',
              sp.tex_path === 'res/' + imgs[0].name, sp.tex_path);
            t('设置的贴图真的被引擎加载了(texture >= 0)',
              typeof sp.texture === 'number' && sp.texture >= 0, sp.texture);
            /* 换上贴图之后,选中框必须变成**整张图那么大**(而不是旧模板留下的
             * 32×32 小方块),否则用户看到的还是"框线一动、图没了"。
             * (画面里像素层面的证明在模型层的 test_sprite_scale /
             *  test_outline_matches_pixels:那边是世界坐标,不受画布大小影响。) */
            {
              const ob = (await ds('scene.outline')).filter((o) => o.id === e0.id)[0];
              const g4 = await ds('entity.get', { id: e0.id });
              const tr4 = (g4.comps || {}).transform || {};
              const ts = texSize[imgs[0].name] || { w: 0, h: 0 };
              const k = tr4.sx || 1;
              t('换上贴图后选中框 = 整张贴图 × 缩放(不是 32×32 的小方块)',
                !!ob && ob.kind === 'sprite'
                && Math.abs(ob.w - ts.w * k) <= 2 && Math.abs(ob.h - ts.h * k) <= 2,
                JSON.stringify(ob) + ' 贴图=' + JSON.stringify(ts) + ' k=' + k);
              t('换贴图时自动改成画整张贴图(裁切清 0)', sp.sw === 0 && sp.sh === 0,
                'sw=' + sp.sw + ' sh=' + sp.sh);
              t('选中框以实体位置为中心(和引擎画图是同一个公式)',
                !!ob && Math.abs(ob.x + ob.w / 2 - tr4.x) < 0.6
                && Math.abs(ob.y + ob.h / 2 - tr4.y) < 0.6,
                JSON.stringify([ob && ob.x, ob && ob.y, ob && ob.w, ob && ob.h,
                                tr4.x, tr4.y]));
            }
          } finally {
            await ds('entity.remove', { id: e0.id });
            DS.sel = keepSel;
            await refresh();
          }
        }
      }
    } else {
      out.skip.push('没有打开项目:跳过 res.list/autosave 的界面检查');
    }
    /* 恢复提示:合成一个"可恢复"状态验渲染与按钮 */
    const keepInfo = DS.info;
    DS.info = Object.assign({}, keepInfo,
      { recoverable: true, autosave_seq: 3, recover_kind: 'crash' });
    renderRecover();
    const barEl = document.getElementById('recover-bar');
    const txtEl = document.getElementById('recover-text');
    t('有可恢复的自动保存时弹出提示条',
      !barEl.hidden && !hiddenNow(barEl) && txtEl.textContent.indexOf('自动保存') >= 0,
      txtEl.textContent);
    t('崩溃留下的自动保存说"没有正常退出"',
      txtEl.textContent.indexOf('没有正常退出') >= 0, txtEl.textContent);
    DS.info = Object.assign({}, keepInfo,
      { recoverable: true, autosave_seq: 1, recover_kind: 'unsaved' });
    renderRecover();
    t('正常退出但没存盘时说"没保存的改动"',
      txtEl.textContent.indexOf('没保存的改动') >= 0, txtEl.textContent);
    DS.info = Object.assign({}, keepInfo, { recoverable: false });
    renderRecover();
    t('没有可恢复内容时提示条收起',
      document.getElementById('recover-bar').hidden
      && hiddenNow(document.getElementById('recover-bar')));
    DS.info = keepInfo;
    t('恢复提示的两个按钮都在',
      !!document.getElementById('btn-recover')
      && !!document.getElementById('btn-recover-drop'));
    /* 输出面板默认不刷协议流量(真人话才看得见) */
    t('默认不打协议流量(quietLog=true)', DS.quietLog === true);
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
  startAutosave();
  setTool('select');
  setMode('scene');
  $('chk-debug').checked = !DS.quietLog;
  try {
    await refresh();
    await refreshRecent();
    log('dim', '就绪。快捷键:1/2 切换工具 · F 适应 · Ctrl+Z/Y 撤销重做 · Ctrl+D 复制 · Delete 删除');
    await ds('ui.ready');
  } catch (e) {
    log('er', '初始化失败:' + e.message);
  }
});
