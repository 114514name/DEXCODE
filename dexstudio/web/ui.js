/* DexStudio 的界面零件:小对话框、提示条、以及"按字段元数据造控件"。
 *
 * 为什么要有这个文件:
 *   1. 以前新建/打开项目、资源改名走的是 window.prompt/confirm —— 用户要**手打路径**,
 *      而且 prompt 是浏览器原生框(样式、中文、多字段都做不了);
 *   2. 属性面板过去"int 一律数字框",于是 collider.kind / body.motion / tint / 贴图路径
 *      全靠用户记数字和敲字符串。现在控件由 C 侧给的字段元数据(kind/enum/min/max/
 *      unit/readonly)决定 —— 枚举变下拉、布尔变复选、颜色变取色器、资源变下拉。
 * 这里只做"造控件",不做任何业务判断(与 app.js 的分工一致)。
 */
'use strict';

/* ------------------------------------------------------------ 提示条 */

let toastTimer = 0;
function toast(msg, kind) {
  const el = $('toast');
  if (!el) return;
  el.className = 'toast' + (kind ? ' ' + kind : '');
  el.textContent = msg;
  el.hidden = false;
  clearTimeout(toastTimer);
  /* 错误留久一点(用户往往正好没看屏幕) */
  toastTimer = setTimeout(() => { el.hidden = true; },
                          kind === 'err' ? 6000 : 3000);
}

/* ------------------------------------------------------------ 小对话框 */

/* uiDialog({title, hint, fields:[{name,label,type,value,options,min,max,step,
 *                                placeholder,choices}], ok, cancel, danger})
 *   type: text | number | select | checkbox | choices(单选按钮组) | info
 * 返回 Promise:确定 → {name: value, …};取消/Esc/点背景 → null */
let dialogFocus = null;

function uiDialog(opts) {
  const o = opts || {};
  const box = $('modal');
  const body = $('modal-body');
  const title = $('modal-title');
  const hint = $('modal-hint');
  const ok = $('modal-ok');
  const cancel = $('modal-cancel');
  return new Promise((resolve) => {
    const fields = o.fields || [];
    const read = {};
    title.textContent = o.title || '';
    if (o.hint) { hint.textContent = o.hint; hint.hidden = false; }
    else hint.hidden = true;
    body.innerHTML = '';
    fields.forEach((f) => {
      const row = document.createElement('div');
      row.className = 'field';
      const lab = document.createElement('label');
      lab.textContent = f.label || f.name || '';
      row.appendChild(lab);
      let inp;
      if (f.type === 'info') {
        const p = document.createElement('div');
        p.className = 'hint';
        p.textContent = f.value || '';
        row.removeChild(lab);
        row.style.display = 'block';
        row.appendChild(p);
        body.appendChild(row);
        return;
      }
      if (f.type === 'select') {
        inp = document.createElement('select');
        (f.options || []).forEach((op) => {
          const e = document.createElement('option');
          e.value = String(op.value);
          e.textContent = op.label === undefined ? op.value : op.label;
          if (String(op.value) === String(f.value)) e.selected = true;
          inp.appendChild(e);
        });
      } else if (f.type === 'checkbox') {
        inp = document.createElement('input');
        inp.type = 'checkbox';
        inp.checked = !!f.value;
      } else if (f.type === 'choices') {
        inp = document.createElement('div');
        inp.className = 'choices';
        (f.choices || []).forEach((c, i) => {
          const l = document.createElement('label');
          const r = document.createElement('input');
          r.type = 'radio';
          r.name = 'choice_' + (f.name || 'x');
          r.value = String(c.value);
          if (String(c.value) === String(f.value) || (!f.value && i === 0)) r.checked = true;
          const span = document.createElement('span');
          span.textContent = c.label;
          const desc = document.createElement('span');
          desc.className = 'hint';
          desc.textContent = c.desc ? '— ' + c.desc : '';
          l.appendChild(r); l.appendChild(span); l.appendChild(desc);
          inp.appendChild(l);
        });
      } else {
        inp = document.createElement('input');
        inp.type = f.type === 'number' ? 'number' : 'text';
        if (f.min !== undefined) inp.min = f.min;
        if (f.max !== undefined) inp.max = f.max;
        if (f.step !== undefined) inp.step = f.step;
        if (f.placeholder) inp.placeholder = f.placeholder;
        inp.value = f.value === undefined || f.value === null ? '' : f.value;
      }
      inp.dataset.field = f.name || '';
      row.appendChild(inp);
      body.appendChild(row);
      read[f.name] = () => {
        if (f.type === 'checkbox') return inp.checked;
        if (f.type === 'choices') {
          const on = inp.querySelector('input:checked');
          return on ? on.value : '';
        }
        if (f.type === 'number') {
          const v = parseFloat(inp.value);
          return isNaN(v) ? 0 : v;
        }
        return inp.value;
      };
      if (!dialogFocus && f.type !== 'checkbox') dialogFocus = inp;
    });
    ok.textContent = o.ok || '确定';
    cancel.textContent = o.cancel || '取消';
    ok.className = o.danger ? 'primary err' : 'primary';
    const close = (val) => {
      box.hidden = true;
      document.removeEventListener('keydown', onkey, true);
      ok.onclick = null; cancel.onclick = null; box.onclick = null;
      resolve(val);
    };
    const onkey = (ev) => {
      if (ev.key === 'Escape') { ev.preventDefault(); close(null); }
      else if (ev.key === 'Enter' && ev.target && ev.target.tagName !== 'TEXTAREA') {
        ev.preventDefault();
        submit();
      }
    };
    const submit = () => {
      const out = {};
      Object.keys(read).forEach((k) => { out[k] = read[k](); });
      close(out);
    };
    ok.onclick = submit;
    cancel.onclick = () => close(null);
    box.onclick = (ev) => { if (ev.target === box) close(null); };
    document.addEventListener('keydown', onkey, true);
    box.hidden = false;
    const first = dialogFocus;
    dialogFocus = null;
    if (first) setTimeout(() => { first.focus(); if (first.select) first.select(); }, 0);
  });
}

/* 确认框(替代 confirm)。danger=1 时确定按钮标红。 */
async function uiConfirm(title, hint, danger) {
  const r = await uiDialog({
    title, hint, danger: !!danger,
    fields: [], ok: danger ? '确定删除' : '确定',
  });
  return r !== null;
}

/* ------------------------------------------------------------ 按元数据造字段控件
 *
 * meta 是 C 侧 comp.schema 给的一条字段描述:
 *   {name,type,label,kind,unit,hint,readonly,step,min,max,enum:[{value,label}]}
 * options 是场景选项(ds_model_scene_options):entities / images / audios。
 * onCommit(value) 由调用方决定怎么写回去(单条 comp.set 或批量 comp.set_many)。
 */
function makeFieldControl(meta, value, options, onCommit) {
  const kind = meta.kind || (meta.type === 'string' ? 'text' : 'number');
  const opts = options || { entities: [], images: [], audios: [] };

  /* 资源下拉:选项来自 res/,而且**总是**带一个"浏览/导入"入口 ——
   * 用户不该为了设一张贴图去手打 res/xxx.png。 */
  function resourceSelect(list, current, noun, onPick) {
    const wrap = document.createElement('div');
    wrap.className = 'ctl-res';
    const sel = document.createElement('select');
    const empty = document.createElement('option');
    empty.value = '';
    empty.textContent = list.length ? '(未设置)' : '(res/ 里还没有' + noun + ')';
    sel.appendChild(empty);
    let found = false;
    list.forEach((p) => {
      const o = document.createElement('option');
      o.value = p;
      o.textContent = p.replace(/^res\//, '');
      if (p === current) { o.selected = true; found = true; }
      sel.appendChild(o);
    });
    if (current && !found) {           /* 值不在 res/ 里(手写过的路径)也显示出来 */
      const o = document.createElement('option');
      o.value = current;
      o.textContent = current + '(不在 res/ 里)';
      o.selected = true;
      sel.appendChild(o);
    }
    sel.onchange = () => onCommit(sel.value);
    wrap.appendChild(sel);
    const btn = document.createElement('button');
    btn.className = 'link';
    btn.textContent = '导入…';
    btn.title = '从磁盘选一个文件复制进 res/';
    btn.onclick = onPick;
    wrap.appendChild(btn);
    return wrap;
  }

  switch (kind) {
    case 'bool': {
      const inp = document.createElement('input');
      inp.type = 'checkbox';
      inp.checked = !!value;
      inp.onchange = () => onCommit(inp.checked);
      return inp;
    }
    case 'enum': {
      const sel = document.createElement('select');
      (meta.enum || []).forEach((e) => {
        const o = document.createElement('option');
        o.value = String(e.value);
        o.textContent = e.label;
        if (Number(value) === Number(e.value)) o.selected = true;
        sel.appendChild(o);
      });
      sel.onchange = () => onCommit(parseInt(sel.value, 10));
      return sel;
    }
    case 'flags': {
      /* 位掩码:一个复选 = 一位(例如 sprite.flip 的横向/纵向) */
      const wrap = document.createElement('div');
      wrap.className = 'flagbox';
      (meta.enum || []).forEach((e) => {
        const l = document.createElement('label');
        l.className = 'chk';
        const c = document.createElement('input');
        c.type = 'checkbox';
        c.checked = (Number(value) & e.value) !== 0;
        c.onchange = () => {
          let v = Number(value) || 0;
          if (c.checked) v |= e.value; else v &= ~e.value;
          value = v;
          onCommit(v);
        };
        const s = document.createElement('span');
        s.textContent = e.label;
        l.appendChild(c); l.appendChild(s);
        wrap.appendChild(l);
      });
      return wrap;
    }
    case 'color': {
      const wrap = document.createElement('div');
      wrap.className = 'flagbox';
      const inp = document.createElement('input');
      inp.type = 'color';
      /* 引擎的颜色是 **0xAARRGGBB**(高位是 alpha;见 dexgame.h 的 DG_RGBA)。
       * 取色器只给 0xRRGGBB,直接写进去 alpha=0 → **整个精灵变全透明**,
       * 用户看到的就是"设了颜色之后图没了"。所以这里必须补上不透明的 alpha。 */
      const n = (Number(value) >>> 0);
      inp.value = '#' + (n & 0xFFFFFF).toString(16).padStart(6, '0');
      const hexText = document.createElement('span');
      hexText.className = 'hint';
      hexText.textContent = inp.value + (n === 0xFFFFFFFF ? '(原色)' : '');
      inp.onchange = () => {
        hexText.textContent = inp.value;
        const rgb = inp.value.slice(1);
        onCommit((0xFF000000 | parseInt(rgb, 16)) >>> 0);
      };
      wrap.appendChild(inp); wrap.appendChild(hexText);
      return wrap;
    }
    case 'image':
      return resourceSelect(opts.images || [], value || '', '图片', () => importRes(true));
    case 'audio':
      return resourceSelect(opts.audios || [], value || '', '声音', () => importRes(false));
    case 'entity': {
      const sel = document.createElement('select');
      const none = document.createElement('option');
      none.value = '-1';
      none.textContent = '(没有父实体)';
      sel.appendChild(none);
      let found = false;
      (opts.entities || []).forEach((e) => {
        const o = document.createElement('option');
        o.value = String(e.id);
        o.textContent = e.name || ('#' + e.id);
        if (Number(value) === Number(e.id)) { o.selected = true; found = true; }
        sel.appendChild(o);
      });
      if (Number(value) > 0 && !found) {
        const o = document.createElement('option');
        o.value = String(value);
        o.textContent = '#' + value + '(已失效)';
        o.selected = true;
        sel.appendChild(o);
      }
      sel.onchange = () => onCommit(parseInt(sel.value, 10));
      return sel;
    }
    case 'readonly': {
      const inp = document.createElement('input');
      inp.type = 'text';
      inp.value = value === null || value === undefined ? '' : String(value);
      inp.disabled = true;
      return inp;
    }
    default: {
      if (meta.type === 'string') {
        const inp = document.createElement('input');
        inp.type = 'text';
        inp.value = value === null || value === undefined ? '' : String(value);
        inp.onchange = () => onCommit(inp.value);
        return inp;
      }
      const inp = document.createElement('input');
      inp.type = 'number';
      inp.step = meta.step ? String(meta.step) : '1';
      if (meta.min !== undefined) inp.min = String(meta.min);
      if (meta.max !== undefined) inp.max = String(meta.max);
      inp.value = value === null || value === undefined ? '' : value;
      /* 空/非法**不提交**(以前 parseFloat(v)||0 会把"想删掉重输"变成 0) */
      inp.onchange = () => {
        const raw = inp.value.trim();
        if (raw === '' || isNaN(parseFloat(raw))) {
          inp.value = value === null || value === undefined ? '' : value;
          toast('这里要填一个数字(已还原)');
          return;
        }
        onCommit(parseFloat(raw));
      };
      return inp;
    }
  }
}
