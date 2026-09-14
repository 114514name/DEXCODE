/* DexStudio 场景编辑器的几块视图:层级树 / 图层 / 属性面板 + 瓦片调色板。
 *
 * 这里是**纯视图**:数据全部来自 DS.entities / DS.outline / DS.schema(都是 C 模型
 * 层给的),编辑动作全部翻译成 ds_command。属性面板完全由 `comp.schema` 生成 ——
 * 而且现在是**按元数据生成控件**:enum→下拉、bool→复选、color→取色器、
 * image/audio→资源下拉、entity→实体下拉、readonly→禁用并说明原因。
 * (以前"int 一律数字框、字符串一律文本框",于是用户得自己记住
 *  collider.kind 的 0/1/2 是什么意思、贴图路径怎么写。)
 */
'use strict';

/* ------------------------------------------------------------ 层级树 */

/* 父子关系按 transform.parent 缩进显示:以前树是平铺的,parent 字段形同不存在。 */
function renderTree() {
  const ul = $('tree');
  const filter = ($('tree-filter').value || '').toLowerCase();
  ul.innerHTML = '';
  const all = DS.entities;
  const kids = new Map();
  const rootIds = [];
  const byIdMap = new Map(all.map((e) => [e.id, e]));
  const parentOf = (e) => {
    const p = Number((e.parent === undefined ? -1 : e.parent));
    return (p > 0 && byIdMap.has(p) && p !== e.id) ? p : -1;
  };
  all.forEach((e) => {
    const p = parentOf(e);
    if (p < 0) rootIds.push(e.id);
    else {
      if (!kids.has(p)) kids.set(p, []);
      kids.get(p).push(e.id);
    }
  });
  const matches = (e) => !filter || (e.name || '').toLowerCase().indexOf(filter) >= 0;
  const hit = new Set();
  all.forEach((e) => {
    if (!matches(e)) return;
    /* 过滤时把祖先也留下(否则缩进看起来像断了) */
    let cur = e.id;
    let guard = 0;
    while (cur > 0 && guard++ < 64) {
      hit.add(cur);
      const node = byIdMap.get(cur);
      if (!node) break;
      const p = parentOf(node);
      if (p < 0 || hit.has(p)) break;
      cur = p;
    }
  });
  let painted = 0;
  const walk = (id, depth) => {
    const e = byIdMap.get(id);
    if (!e || !hit.has(id)) return;
    if (matches(e)) {
      const o = outlineOf(e.id) || {};
      const li = document.createElement('li');
      if (isSelected(e.id)) li.className = 'sel';
      li.draggable = true;
      li.dataset.id = String(e.id);
      li.style.paddingLeft = (6 + depth * 12) + 'px';
      const dot = '<span class="dot ' + (o.kind || '') + '"></span>';
      li.innerHTML = '<span class="nm">' +
        (depth ? '<span class="tw">└</span>' : '') + dot + esc(e.name || '(无名)') +
        '</span><span class="meta">' + Math.round(e.x) + ',' + Math.round(e.y) + '</span>';
      li.onclick = (ev) => select([e.id], ev.shiftKey || ev.ctrlKey || ev.metaKey);
      li.ondblclick = () => centerOn(e.id);
      li.ondragstart = (ev) => {
        ev.dataTransfer.setData('text/plain', String(e.id));
        ev.dataTransfer.effectAllowed = 'move';
      };
      li.ondragover = (ev) => {
        if (ev.dataTransfer.types.indexOf('text/plain') >= 0) {
          ev.preventDefault();
          li.classList.add('drop');
        }
      };
      li.ondragleave = () => li.classList.remove('drop');
      li.ondrop = async (ev) => {
        ev.preventDefault();
        li.classList.remove('drop');
        const src = parseInt(ev.dataTransfer.getData('text/plain'), 10);
        if (!src || src === e.id) return;
        try {
          await call('entity.set_parent', { id: src, parent: e.id }, '设为子实体');
          await refresh();
          toast((byIdMap.get(src) || {}).name + ' 现在是 ' + e.name + ' 的子实体', 'ok');
        } catch (err) { /* 已提示 */ }
      };
      ul.appendChild(li);
      painted++;
    }
    (kids.get(id) || []).forEach((k) => walk(k, depth + 1));
  };
  rootIds.forEach((id) => walk(id, 0));
  if (!painted) {
    ul.innerHTML = '<li class="meta">' +
      (DS.entities.length ? '(过滤后为空)' : '(空场景 —— 点「+ 实体」开始)') + '</li>';
  }
}

function centerOn(id) {
  const o = outlineOf(id);
  if (!o) return;
  setView({
    x: o.x + o.w / 2 - (DS.vw / 2) / DS.view.zoom,
    y: o.y + o.h / 2 - (DS.vh / 2) / DS.view.zoom,
  });
}

/* 设了贴图之后"看不见图"的另一个来源:实体的包围盒**不在当前视野里**
 * (比如刚给 (0,0) 的实体设了图,而轴心 0.5 让它的左上角跑到视口外面;
 *  再比如实体掉到了 y=3000)。这条只做一件事:不在视野里就把它挪进来。 */
function ensureVisible(id) {
  const o = outlineOf(id);
  if (!o) return false;
  const v = DS.view;
  const x0 = v.x, y0 = v.y;
  const x1 = v.x + DS.vw / v.zoom, y1 = v.y + DS.vh / v.zoom;
  const pad = 8 / v.zoom;
  const inside = o.x >= x0 + pad && o.y >= y0 + pad
    && o.x + o.w <= x1 - pad && o.y + o.h <= y1 - pad;
  if (inside) return false;
  centerOn(id);
  return true;
}

/* ------------------------------------------------------------ 图层 */

function renderLayers() {
  const ul = $('layers');
  const counts = new Map();
  DS.outline.forEach((o) => {
    const k = o.layer || 0;
    counts.set(k, (counts.get(k) || 0) + 1);
  });
  ul.innerHTML = '';
  if (!counts.size) {
    ul.innerHTML = '<li class="meta">(没有实体)</li>';
    return;
  }
  const keys = Array.from(counts.keys()).sort((a, b) => a - b);
  keys.forEach((k, i) => {
    const ids = DS.outline.filter((o) => (o.layer || 0) === k).map((o) => o.id);
    const li = document.createElement('li');
    li.innerHTML = '<span>图层 ' + k + '<span class="meta">  ' +
      (i === 0 ? '(最先画/在最下面)' : (i === keys.length - 1 ? '(最后画/在最上面)' : '')) +
      '</span></span>' +
      '<span class="meta">' + counts.get(k) + ' 个 ' +
      '<button class="link" data-shift="-1" title="整层往下挪(数字 -1)">↓</button>' +
      '<button class="link" data-shift="1" title="整层往上挪(数字 +1)">↑</button>' +
      '</span>';
    li.onclick = (ev) => {
      if (ev.target.dataset && ev.target.dataset.shift) return;
      select(ids, ev.shiftKey || ev.ctrlKey || ev.metaKey);
    };
    li.querySelectorAll('button[data-shift]').forEach((b) => {
      b.onclick = (ev) => {
        ev.stopPropagation();
        shiftLayer(k, parseInt(b.dataset.shift, 10), ids);
      };
    });
    ul.appendChild(li);
  });
  const note = document.createElement('li');
  note.className = 'meta';
  note.textContent = '(顺序 = 组件的 layer / order 字段:数字小的先画。↑↓ 整层挪一格)';
  ul.appendChild(note);
}

/* 整层上移/下移:改这一层所有实体的 layer 字段(一条撤销记录)。
 * 图层不是独立对象,就是组件字段,所以"新建图层"= 把实体的 layer 设成一个新数字。 */
async function shiftLayer(k, delta, ids) {
  const targets = ids || DS.outline.filter((o) => (o.layer || 0) === k).map((o) => o.id);
  const items = [];
  targets.forEach((id) => {
    const e = byId(id);
    const comps = (e && e.comps) || [];
    if (comps.indexOf('sprite') >= 0) {
      items.push({ id, comp: 'sprite', field: 'layer', value: k + delta });
    }
    if (comps.indexOf('tilemap') >= 0) {
      items.push({ id, comp: 'tilemap', field: 'layer', value: k + delta });
    }
  });
  if (!items.length) { toast('这一层里没有可调的实体', 'warn'); return; }
  try {
    await call('comp.set_many', { items }, '调整图层');
    await refresh();
  } catch (e) { /* 已提示 */ }
}

/* ------------------------------------------------------------ 属性面板 */

async function renderInspector() {
  const box = $('inspector');
  const multi = DS.sel.length > 1;
  const id = DS.sel.length ? DS.sel[DS.sel.length - 1] : 0;
  $('ent-ops').hidden = !id;
  $('comp-add-row').hidden = !id;
  $('sel-name').textContent = !id ? '未选中'
    : (multi ? '选中 ' + DS.sel.length + ' 个(显示最后一个)' : '#' + id);
  box.innerHTML = '';
  renderParentSelect(id);
  if (!id) {
    box.innerHTML = '<p class="hint">在层级树或视口里点一个实体。</p>';
    $('comp-add-sel').innerHTML = '';
    return;
  }
  const e = byId(id);
  if (e) $('ent-name').value = e.name || '';
  let detail;
  try {
    detail = await ds('entity.get', { id });
  } catch (err) {
    box.innerHTML = '<p class="hint">' + esc(err.message) + '</p>';
    return;
  }
  const comps = detail.comps || {};
  /* 选中的精灵:把它的贴图**提前解码好**。拖动时画布上要自己画一份跟手的
   * (见 viewport.js 的拖动预览),不预热的话第一次拖的前几帧只有框线没有图。 */
  if (comps.sprite && comps.sprite.tex_path && typeof Viewport.prewarm === 'function')
    Viewport.prewarm(comps.sprite.tex_path);
  const have = Object.keys(comps);
  /* 挂组件下拉:自省出来的组件里去掉已经挂上的,并带上中文说明 */
  const sel = $('comp-add-sel');
  sel.innerHTML = '';
  DS.schema.filter((c) => have.indexOf(c.name) < 0).forEach((c) => {
    const op = document.createElement('option');
    op.value = c.name;
    op.textContent = COMP_HINT[c.name] ? (c.name + '  —  ' + COMP_HINT[c.name]) : c.name;
    sel.appendChild(op);
  });
  $('btn-comp-add').disabled = !sel.options.length;

  if (multi) {
    const note = document.createElement('div');
    note.className = 'batch-note';
    note.textContent = '多选:' + DS.sel.length + ' 个实体。下面的字段会同时写给' +
      '所有挂了这个组件的实体(一条撤销记录)。';
    box.appendChild(note);
  }

  have.forEach((cname) => {
    const spec = DS.schema.find((c) => c.name === cname) || { fields: [] };
    const div = document.createElement('div');
    div.className = 'comp';
    div.dataset.comp = cname;
    const head = document.createElement('div');
    head.className = 'head';
    head.innerHTML = '<span>' + esc(cname) +
      (COMP_HINT[cname] ? ' <span class="hint">' + esc(COMP_HINT[cname]) + '</span>' : '') +
      '</span>' +
      '<span class="mini"><button class="link" data-rm="' + esc(cname) + '">移除</button></span>';
    div.appendChild(head);
    spec.fields.forEach((f) => {
      div.appendChild(fieldRow(id, cname, f, comps[cname] ? comps[cname][f.name] : null));
    });
    if (!spec.fields.length) {
      const p = document.createElement('div');
      p.className = 'field';
      p.innerHTML = '<label>(无字段)</label>';
      div.appendChild(p);
    }
    box.appendChild(div);
  });
  box.querySelectorAll('button[data-rm]').forEach((b) => {
    b.onclick = async () => {
      try {
        await call('comp.remove', { id, comp: b.dataset.rm }, '移除组件');
        await refresh();
      } catch (err) { /* 已提示 */ }
    };
  });
}

/* 组件的中文说明(自省只给名字;这层说明是给"不知道这些是什么"的人看的) */
const COMP_HINT = {
  transform: '位置',
  sprite: '画一张图',
  camera: '决定视口看哪儿',
  animation: '逐帧动画',
  collider: '碰撞形状',
  body: '物理运动',
  tilemap: '瓦片地图',
  audio: '声音',
};

/* 父级下拉:值只有一个,而且程序全知道 —— 以前是让用户手填实体 id */
function renderParentSelect(id) {
  const sel = $('ent-parent');
  if (!sel) return;
  const e = id ? byId(id) : null;
  const cur = e ? Number(e.parent === undefined ? -1 : e.parent) : -1;
  sel.innerHTML = '';
  const none = document.createElement('option');
  none.value = '-1';
  none.textContent = '(没有父实体)';
  sel.appendChild(none);
  DS.entities.forEach((o) => {
    if (o.id === id) return;
    const op = document.createElement('option');
    op.value = String(o.id);
    op.textContent = o.name || ('#' + o.id);
    if (o.id === cur) op.selected = true;
    sel.appendChild(op);
  });
  sel.disabled = !id;
}

/* 一个字段 = 一行:标签(中文 + 单位) + 按元数据生成的控件 */
function fieldRow(id, comp, f, value) {
  const row = document.createElement('div');
  row.className = 'field' + (f.readonly ? ' readonly' : '') + (f.kind === 'flags' ? ' flags' : '');
  const lab = document.createElement('label');
  lab.innerHTML = esc(f.label || f.name) +
    (f.unit ? '<span class="unit">(' + esc(f.unit) + ')</span>' : '') +
    (f.persist === false || f.readonly ? '<span class="ro">只读</span>' : '');
  lab.title = comp + '.' + f.name + '  ' + (f.hint || '') +
    (f.readonly ? '' : '(改这里会立刻生效)');
  row.appendChild(lab);

  const targets = DS.sel.length > 1 ? DS.sel.slice() : [id];
  const isParent = (comp === 'transform' && f.name === 'parent');
  const inp = makeFieldControl(f, value, DS.options, async (v) => {
    try {
      if (isParent) {
        await setParent(parseInt(v, 10));
        return;
      }
      if (targets.length > 1) {
        /* 批量:只写给真的挂了这个组件的实体(一条撤销记录) */
        const items = targets
          .filter((t) => ((byId(t) || {}).comps || []).indexOf(comp) >= 0)
          .map((t) => ({ id: t, comp, field: f.name, value: v }));
        await call('comp.set_many', { items }, '设置 ' + comp + '.' + f.name);
      } else {
        await call('comp.set', { id, comp, field: f.name, value: v },
                   '设置 ' + comp + '.' + f.name);
      }
      row.classList.remove('err');
      await refresh();
    } catch (err) {
      row.classList.add('err');
      log('er', comp + '.' + f.name + ':' + err.message);
    }
  });
  if (f.readonly) {
    const tag = (inp.tagName || '').toLowerCase();
    if (tag === 'input' || tag === 'select') {
      inp.disabled = true;
    } else if (inp.querySelectorAll) {
      Array.prototype.forEach.call(inp.querySelectorAll('input,select'),
                                   (x) => { x.disabled = true; });
    }
    inp.title = f.hint || '这个字段由引擎维护,不用手改';
  }
  row.appendChild(inp);
  return row;
}

/* 拖拽移动之后批量写回(一次一条撤销记录) */
async function moveSelection(idPositions) {
  const items = [];
  Object.keys(idPositions).forEach((id) => {
    const p = idPositions[id];
    items.push({ id: parseInt(id, 10), comp: 'transform', field: 'x', value: p.x });
    items.push({ id: parseInt(id, 10), comp: 'transform', field: 'y', value: p.y });
  });
  if (!items.length) return;
  try {
    await call('comp.set_many', { items }, '移动实体');
    await refresh({ noRender: false });
  } catch (e) { /* 已提示 */ }
}

/* ------------------------------------------------------------ 瓦片调色板 */

async function updatePalette() {
  const sec = $('palette-sec');
  const id = DS.sel.length ? DS.sel[DS.sel.length - 1] : 0;
  const e = id ? byId(id) : null;
  const hasTile = e && (e.comps || []).indexOf('tilemap') >= 0;
  if (!hasTile) {
    sec.hidden = true;
    DS.tilemap = null;
    return;
  }
  sec.hidden = false;
  try {
    DS.tilemap = await ds('tilemap.info', { id });
  } catch (err) {
    sec.hidden = true;
    DS.tilemap = null;
    log('er', 'tilemap.info: ' + err.message);
    return;
  }
  const t = DS.tilemap;
  $('palette-info').textContent = t.cols + '×' + t.rows + ' 格 · ' +
    (t.tw | 0) + '×' + (t.th | 0) + ' 像素/格' + (t.visible ? '' : ' · 已隐藏');
  $('atlas-cols').value = t.atlas_cols || 8;
  $('atlas-first').value = t.atlas_tile || 0;
  const img = $('palette');
  const wrap = img.parentElement;
  if (t.atlas_url) {
    if (img.getAttribute('src') !== t.atlas_url) img.setAttribute('src', t.atlas_url);
    img.hidden = false;
    /* 图集读不出来时**说清楚**:以前就是一块"图片损坏"的裂图标,用户完全不知道
     * 是路径不对、文件不在 res/ 里,还是格式不支持。 */
    img.onerror = () => {
      toast('图集读不出来:' + (t.tex_path || '(空)') +
            ' —— 请确认它就在项目的 res/ 里(不要用项目外的绝对路径)', 'err');
      log('er', 'palette: 读不到图集 ' + t.atlas_url);
    };
    const cells = wrap.querySelector('.cells') ||
      (() => { const d = document.createElement('div'); d.className = 'cells';
               wrap.appendChild(d); return d; })();
    const build = () => buildPaletteCells(cells, t);
    if (img.complete && img.naturalWidth) build();
    else img.onload = build;
  } else {
    img.hidden = true;
    img.onerror = null;
    const cells = wrap.querySelector('.cells');
    if (cells) cells.innerHTML = '';
    /* 没有图集时给出**可点的下一步**,而不是只打印一句提示 */
    toast(t.tex_path
      ? ('图集「' + t.tex_path + '」不在项目里,面板显示不了预览 —— ' +
         '在右边「图集」字段里改选一张 res/ 里的图片')
      : '这个瓦片地图还没有图集:在右边「图集」字段里选一张图片', 'warn');
  }
  pickTile(DS.tile);
}

/* 图集格:图集里从 atlas_tile 起按 atlas_cols 列排布,每格 tw×th 像素 */
function buildPaletteCells(box, t) {
  const img = $('palette');
  box.innerHTML = '';
  const iw = img.naturalWidth || img.clientWidth;
  const ih = img.naturalHeight || img.clientHeight;
  const cw = Math.max(1, Math.round(t.tw || 16));
  const chh = Math.max(1, Math.round(t.th || 16));
  const cols = Math.max(1, t.atlas_cols || 8);
  const total = Math.floor(iw / cw) * Math.floor(ih / chh);
  for (let i = 0; i < total; i++) {
    const d = document.createElement('div');
    d.className = 'cell';
    d.dataset.tile = String((t.atlas_tile || 0) + i);
    d.style.left = ((i % cols) * cw) + 'px';
    d.style.top = (Math.floor(i / cols) * chh) + 'px';
    d.style.width = cw + 'px';
    d.style.height = chh + 'px';
    d.title = '图块 ' + d.dataset.tile;
    d.onclick = () => pickTile(parseInt(d.dataset.tile, 10));
    box.appendChild(d);
  }
  box.style.width = iw + 'px';
  box.style.height = ih + 'px';
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}
