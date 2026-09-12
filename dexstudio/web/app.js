/* DexStudio 前端外壳(B2)。
 *
 * 与 C 宿主的唯一通道是 WebView2 的 postMessage:
 *   JS → C:  window.chrome.webView.postMessage({id, cmd, args})
 *   C → JS:  window.chrome.webView.addEventListener('message', e => e.data)
 * 用 id 做请求/响应配对(ds() 返回 Promise)。**不开窗口也能验证**同样的命令:
 *   dexstudio.exe --command '{"cmd":"app.info"}'
 */
'use strict';

let seq = 0;
const pending = new Map();
const $ = (id) => document.getElementById(id);

/* WebView2 注入的桥是 chrome.webview(全小写)。index.html 顶部已经把可用实例
 * 存进 window.__ds_bridge,这里取它;独立打开页面(非 WebView2)时退化成一个
 * 只会报错的桩,好让界面不至于整片空白。 */
const bridge = window.__ds_bridge || null;
if (!bridge) {
  document.addEventListener('DOMContentLoaded', () => {
    const st = $('status');
    if (st) { st.textContent = '未在 WebView2 中运行(没有消息桥)'; st.className = 'status err'; }
  });
}

function log(cls, text) {
  const el = document.createElement('div');
  el.className = 'line ' + cls;
  el.textContent = text;
  const box = $('log');
  box.appendChild(el);
  box.scrollTop = box.scrollHeight;
}

/* 唯一的 RPC 入口。超时 5 秒,避免"消息丢了"时界面永远转圈。 */
function ds(cmd, args) {
  return new Promise((resolve, reject) => {
    if (!bridge) { reject(new Error('没有消息桥(未在 WebView2 中运行)')); return; }
    const id = ++seq;
    const t = setTimeout(() => {
      pending.delete(id);
      reject(new Error('超时:' + cmd));
    }, 5000);
    pending.set(id, {
      resolve: (v) => { clearTimeout(t); resolve(v); },
      reject: (e) => { clearTimeout(t); reject(e); },
    });
    const payload = { id, cmd, args: args || {} };
    log('tx', '→ ' + JSON.stringify(payload));
    bridge.postMessage(payload);
  });
}

if (bridge) bridge.addEventListener('message', (ev) => {
  const m = ev.data || {};
  log(m.ok ? 'rx' : 'er', '← ' + JSON.stringify(m));
  const p = pending.get(m.id);
  if (!p) { log('dim', '(未匹配的响应,可能已超时)'); return; }
  pending.delete(m.id);
  if (m.ok) p.resolve(m.result);
  else p.reject(new Error(m.error || '未知错误'));
});

/* ------------------------------------------------------------ 渲染 */

function card(k, v, cls) {
  return `<div class="card"><div class="k">${k}</div><div class="v ${cls || ''}">${v}</div></div>`;
}

async function refresh() {
  const info = await ds('app.info');
  $('version').textContent = info.version || '';
  const st = $('status');
  if (info.engine) {
    st.textContent = '引擎已连接 · ' + (info.components || []).length + ' 种组件';
    st.className = 'status ok';
  } else {
    st.textContent = '引擎不可用';
    st.className = 'status err';
  }
  $('obj-count').textContent = info.objects || 0;
  $('scene-path').textContent = info.root
    ? info.root + (info.scene ? '  ·  ' + info.scene.split(/[\\/]/).pop() : '')
    : '未打开项目';
  $('cards').innerHTML =
    card('项目', info.project_name || '(未打开)') +
    card('实体', info.objects || 0) +
    card('撤销 / 重做', (info.undo || 0) + ' / ' + (info.redo || 0)) +
    card('引擎', info.engine ? '已连接' : '不可用', info.engine ? 'ok' : 'err') +
    card('脏标记', info.dirty ? '有未保存改动' : '已保存', info.dirty ? '' : 'ok');
  if (!info.engine) {
    $('cards').innerHTML += card('原因', info.engine_error || '', 'err');
  }
  await refreshEntities();
  await refreshSchema();
}

async function refreshEntities() {
  try {
    const list = await ds('entity.list');
    const ul = $('entity-list');
    ul.innerHTML = '';
    (list || []).forEach((e) => {
      const li = document.createElement('li');
      li.innerHTML = `<span>${e.name || '(无名)'}</span>` +
        `<span class="meta">#${e.id} (${Math.round(e.x)},${Math.round(e.y)})</span>`;
      li.onclick = () => ds('entity.get', { id: e.id }).then((d) =>
        log('dim', JSON.stringify(d)));
      ul.appendChild(li);
    });
    if (!list || !list.length) {
      ul.innerHTML = '<li class="meta">(空场景 —— 用「添加实体」开始)</li>';
    }
    $('obj-count').textContent = (list || []).length;
  } catch (e) { log('er', 'entity.list: ' + e.message); }
}

async function refreshSchema() {
  try {
    const schema = await ds('comp.schema');
    const box = $('schema');
    box.innerHTML = '';
    (schema || []).forEach((c) => {
      const d = document.createElement('div');
      d.className = 'comp';
      d.innerHTML = `<div class="name">${c.name}</div>` +
        (c.fields || []).map((f) =>
          `<div class="field">${f.name}: ${f.type}${f.persist ? '' : ' (不存盘)'}</div>`
        ).join('');
      box.appendChild(d);
    });
    if (!schema || !schema.length) box.textContent = '引擎未连接';
  } catch (e) { log('er', 'comp.schema: ' + e.message); }
}

/* ------------------------------------------------------------ 交互 */

async function guard(fn, what) {
  try { await fn(); await refresh(); }
  catch (e) { log('er', what + ' 失败:' + e.message); }
}

function wire() {
  $('btn-new').onclick = () => {
    const dir = prompt('项目目录(相对于 DexStudio 所在目录)', 'mygame');
    if (dir) guard(() => ds('project.new', { dir, name: dir.split(/[\\/]/).pop() }),
                   'project.new');
  };
  $('btn-open').onclick = () => {
    const dir = prompt('要打开的项目目录', 'examples/dexgame');
    if (dir) guard(() => ds('project.open', { dir }), 'project.open');
  };
  $('btn-save').onclick = () => guard(() => ds('project.save'), 'project.save');
  $('btn-add').onclick = () => {
    const name = prompt('实体名字', 'player' + (seq % 10));
    if (name) guard(() => ds('entity.add', { name }), 'entity.add');
  };
  $('btn-undo').onclick = () => guard(() => ds('undo'), 'undo');
  $('btn-redo').onclick = () => guard(() => ds('redo'), 'redo');
  $('btn-scene-new').onclick = () =>
    guard(() => ds('scene.new', { name: $('scene-name').value || 'scene' }), 'scene.new');
  $('btn-scene-save').onclick = () => guard(() => ds('scene.save'), 'scene.save');
  $('btn-scene-json').onclick = () =>
    ds('scene.json').then((j) => log('dim', JSON.stringify(j, null, 2)))
                    .catch((e) => log('er', e.message));
  $('btn-selfcheck').onclick = selfcheck;
}

/* 连通性自检:走一遍"读状态 → 建实体 → 读回 → 撤销 → 重做",把结果打在页面上。
 * 这正是 dexstudio.exe --selftest 在无窗口时跑的同一批命令。 */
async function selfcheck() {
  const out = $('selfcheck');
  const lines = [];
  const t = (name, ok, extra) => {
    lines.push((ok ? '  PASS  ' : '  FAIL  ') + name + (extra ? '  ' + extra : ''));
    out.textContent = lines.join('\n');
  };
  try {
    const info = await ds('app.info');
    t('app.info', !!info.version, 'v' + info.version);
    t('引擎连接', !!info.engine || info.engine_error === '' ? !!info.engine : false,
      info.engine ? '' : info.engine_error);
    const schema = await ds('comp.schema');
    t('组件自省', (schema || []).length > 0, (schema || []).length + ' 种');
    const before = (await ds('entity.list')).length;
    const added = await ds('entity.add', { name: '__selfcheck__' });
    t('entity.add', !!added.id, 'id=' + added.id);
    const after = (await ds('entity.list')).length;
    t('entity.list 计数 +1', after === before + 1, before + ' → ' + after);
    const got = await ds('entity.get', { id: added.id });
    t('entity.get 组件', Object.keys(got.comps || {}).length > 0,
      Object.keys(got.comps || {}).join(','));
    await ds('comp.set', { id: added.id, comp: 'transform', field: 'x', value: 42 });
    const got2 = await ds('entity.get', { id: added.id });
    t('comp.set transform.x = 42', got2.comps.transform.x === 42,
      'x=' + got2.comps.transform.x);
    await ds('undo');
    t('undo 后实体消失',
      (await ds('entity.list')).filter((e) => e.name === '__selfcheck__').length === 0);
    await ds('redo');
    t('redo 后实体回来',
      (await ds('entity.list')).filter((e) => e.name === '__selfcheck__').length === 1);
    await ds('entity.remove', { id: added.id });
    t('entity.remove', (await ds('entity.list')).length === before);
    const bad = await ds('entity.get', { id: 999999 }).then(() => false)
                                                    .catch((e) => /不存在/.test(e.message));
    t('无效实体带得出原因', bad === true);
  } catch (e) {
    t('自检中断', false, e.message);
  }
  lines.push(g_fail_line(lines));
  out.textContent = lines.join('\n');
}

function g_fail_line(lines) {
  const f = lines.filter((l) => l.indexOf('FAIL') >= 0).length;
  return f ? '结果:有 ' + f + ' 项失败' : '结果:全部通过';
}

window.addEventListener('DOMContentLoaded', () => {
  wire();
  refresh()
    .then(() => {
      /* 告诉宿主"界面已就绪":宿主 --wv-selftest 靠它判定整条链通不通
         (窗口 + WebView2 + 本地页面 + JS→C→JS 往返),不需要人看屏幕。 */
      return ds('ui.ready');
    })
    .catch((e) => { log('er', '初始化失败:' + e.message); });
});
