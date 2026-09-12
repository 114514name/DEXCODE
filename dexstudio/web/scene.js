/* DexStudio 场景编辑器的三块视图:层级树 / 图层 / 属性面板 + 瓦片调色板。
 *
 * 这里是**纯视图**:数据全部来自 DS.entities / DS.outline / DS.schema(都是 C 模型
 * 层给的),编辑动作全部翻译成 ds_command。属性面板完全由 `comp.schema` 生成 ——
 * 引擎加一个组件,这里一行都不用改。 */
'use strict';

/* ------------------------------------------------------------ 层级树 */

function renderTree() {
  const ul = $('tree');
  const filter = ($('tree-filter').value || '').toLowerCase();
  ul.innerHTML = '';
  const list = DS.entities.filter((e) =>
    !filter || (e.name || '').toLowerCase().indexOf(filter) >= 0);
  list.forEach((e) => {
    const o = outlineOf(e.id) || {};
    const li = document.createElement('li');
    if (isSelected(e.id)) li.className = 'sel';
    const dot = '<span class="dot ' + (o.kind || '') + '"></span>';
    li.innerHTML = '<span class="nm">' + dot + esc(e.name || '(无名)') + '</span>' +
      '<span class="meta">' + Math.round(e.x) + ',' + Math.round(e.y) + '</span>';
    li.onclick = (ev) => select([e.id], ev.shiftKey || ev.ctrlKey || ev.metaKey);
    li.ondblclick = () => centerOn(e.id);
    ul.appendChild(li);
  });
  if (!list.length) {
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
  Array.from(counts.keys()).sort((a, b) => a - b).forEach((k) => {
    const ids = DS.outline.filter((o) => (o.layer || 0) === k).map((o) => o.id);
    const li = document.createElement('li');
    li.innerHTML = '<span>图层 ' + k + '</span>' +
      '<span class="meta">' + counts.get(k) + ' 个</span>';
    li.onclick = (ev) => select(ids, ev.shiftKey || ev.ctrlKey || ev.metaKey);
    ul.appendChild(li);
  });
  /* 全部图层共用的格子顺序由 sprite/tilemap 的 layer 字段决定 —— 这里顺便
   * 提示一句,免得用户以为图层面板能排序 */
  const note = document.createElement('li');
  note.className = 'meta';
  note.textContent = '(顺序由组件的 layer / order 字段决定)';
  ul.appendChild(note);
}

/* ------------------------------------------------------------ 属性面板 */

const TYPE_LABEL = { 1: 'float', 2: 'int/bool', 3: 'string' };

/* 哪些字段其实是"路径"?改完要能看出错在哪(引擎会立刻尝试加载)。 */
const PATH_FIELDS = { tex_path: 1, path: 1 };

async function renderInspector() {
  const box = $('inspector');
  const id = DS.sel.length === 1 ? DS.sel[0] : (DS.sel.length ? DS.sel[DS.sel.length - 1] : 0);
  const multi = DS.sel.length > 1;
  $('ent-ops').hidden = !id;
  $('comp-add-row').hidden = !id;
  $('sel-name').textContent = !id ? '未选中'
    : (multi ? '选中 ' + DS.sel.length + ' 个(显示最后一个)' : '#' + id);
  box.innerHTML = '';
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
  const have = Object.keys(comps);
  /* 挂组件下拉:自省出来的组件里去掉已经挂上的 */
  const sel = $('comp-add-sel');
  sel.innerHTML = '';
  DS.schema.filter((c) => have.indexOf(c.name) < 0).forEach((c) => {
    const op = document.createElement('option');
    op.value = c.name;
    op.textContent = c.name;
    sel.appendChild(op);
  });
  $('btn-comp-add').disabled = !sel.options.length;

  have.forEach((cname) => {
    const spec = DS.schema.find((c) => c.name === cname) || { fields: [] };
    const div = document.createElement('div');
    div.className = 'comp';
    const head = document.createElement('div');
    head.className = 'head';
    head.innerHTML = '<span>' + esc(cname) + '</span>' +
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

function fieldRow(id, comp, f, value) {
  const row = document.createElement('div');
  row.className = 'field';
  const lab = document.createElement('label');
  lab.innerHTML = esc(f.name) + (f.persist ? '' : ' <span class="rt">运行期</span>');
  lab.title = f.name + ' (' + (TYPE_LABEL[f.type] || f.type) +
    (f.persist ? ',存盘' : ',不存盘') + ')';
  row.appendChild(lab);
  const inp = document.createElement('input');
  if (f.type === 3) {
    inp.type = 'text';
    inp.value = (value === null || value === undefined) ? '' : String(value);
    if (PATH_FIELDS[f.name]) inp.title = '文件路径(相对项目根目录):设置后引擎会立刻加载';
  } else if (f.type === 2 && isBoolField(f.name)) {
    inp.type = 'checkbox';
    inp.checked = !!value;
  } else {
    inp.type = 'number';
    inp.step = (f.type === 1) ? '0.5' : '1';
    inp.value = (value === null || value === undefined) ? '' : value;
  }
  inp.onchange = async () => {
    let v = inp.value;
    if (inp.type === 'checkbox') v = inp.checked;
    else if (inp.type === 'number') v = parseFloat(v) || 0;
    try {
      await call('comp.set', { id, comp, field: f.name, value: v },
                 '设置 ' + comp + '.' + f.name);
      row.classList.remove('err');
      await refresh();
    } catch (err) {
      row.classList.add('err');
      log('er', comp + '.' + f.name + ':' + err.message);
    }
  };
  row.appendChild(inp);
  return row;
}

/* 引擎里"int 但其实当布尔用"的字段(自省只给类型码,没有语义标签)*/
const BOOL_FIELDS = {
  active: 1, loop: 1, is_trigger: 1, sleeping: 1, flip: 1, visible: 1,
  play_on_start: 1,
};
function isBoolField(name) { return !!BOOL_FIELDS[name]; }

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
    const cells = wrap.querySelector('.cells') ||
      (() => { const d = document.createElement('div'); d.className = 'cells';
               wrap.appendChild(d); return d; })();
    const build = () => buildPaletteCells(cells, t);
    if (img.complete && img.naturalWidth) build();
    else img.onload = build;
  } else {
    img.hidden = true;
    const cells = wrap.querySelector('.cells');
    if (cells) cells.innerHTML = '';
    log('warn', '这个瓦片地图还没有图集(tex_path)—— 用属性面板设一个 PNG,或点「新建瓦片地图」。');
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
