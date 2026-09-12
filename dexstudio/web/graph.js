/* DexStudio 逻辑图编辑器(B4):UE 蓝图式的节点画布。
 *
 * 与视口一样,**图数据全在 C 模型层**(scripts/logic.json),这里只画:
 *   - 节点目录来自 graph.types → 节点长什么样、有哪些引脚/属性都由 C 说了算
 *     (与属性面板靠 comp.schema 自省是同一套路:引擎/模型加节点类型,前端不用改)
 *   - 拖动/连线/删除都翻译成 graph.node.* / graph.link / graph.unlink
 *
 * 交互:
 *   左键拖节点 / 拖空白处 = 框选? (一期:拖动节点、拖引脚连线)
 *   中键或空格拖 = 平移 · 滚轮 = 缩放 · Delete = 删选中节点
 *   从输出引脚拖到输入引脚 = 连线;从输入引脚拖走 = 断开该输入
 */
'use strict';

const Graph = (function () {
  let canvas, ctx, cw = 0, ch = 0;
  let types = [];              // graph.types 的结果
  let typeMap = {};            // type → def
  let doc = { nodes: [], links: [] };
  let sel = 0;                 // 选中的节点 id
  let view = { x: 40, y: 40, zoom: 1 };   // 图坐标 → 屏幕: p = (g - view) * zoom
  let drag = null;             // {mode:'node'|'pan'|'link', ...}
  let pending = null;          // 拖动中的连线 {from, from_pin, x, y}
  let hoverPin = null;
  let dirtyLocal = false;

  const NODE_W = 168;
  const ROW_H = 20;
  const HEAD_H = 24;

  /* ---------- 尺寸与坐标 ---------- */

  function resize() {
    if (!canvas) return;
    const r = canvas.parentElement.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    cw = Math.max(1, Math.round(r.width));
    ch = Math.max(1, Math.round(r.height));
    canvas.width = Math.round(cw * dpr);
    canvas.height = Math.round(ch * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  const toScreen = (gx, gy) => ({
    x: (gx - view.x) * view.zoom, y: (gy - view.y) * view.zoom,
  });
  const toGraph = (sx, sy) => ({
    x: sx / view.zoom + view.x, y: sy / view.zoom + view.y,
  });

  function eventPos(ev) {
    const r = canvas.getBoundingClientRect();
    return { x: ev.clientX - r.left, y: ev.clientY - r.top };
  }

  /* ---------- 节点几何 ---------- */

  function defOf(node) { return typeMap[node.type] || null; }

  function nodeSize(node) {
    const d = defOf(node);
    if (!d) return { w: NODE_W, h: HEAD_H + ROW_H };
    const rows = Math.max(d.in.length, d.out.length, 1) + (d.props.length ? 1 : 0);
    return { w: NODE_W, h: HEAD_H + rows * ROW_H + 6 };
  }

  /* 引脚位置(图坐标)。输入在左、输出在右;顺序:输入先 exec 再数据 */
  function pinPos(node, pin, isOut, index) {
    const s = nodeSize(node);
    const d = defOf(node);
    const n = isOut ? (d ? d.out.length : 1) : (d ? d.in.length : 1);
    const y = node.y + HEAD_H + ROW_H * (index + 0.5);
    void n;
    return { x: node.x + (isOut ? s.w : 0), y: y, h: pin.y };
  }

  function pinAt(node, pin) {
    const d = defOf(node);
    if (!d) return null;
    let i;
    for (i = 0; i < d.in.length; i++) {
      if (d.in[i].name === pin) return pinPos(node, d.in[i], false, i);
    }
    for (i = 0; i < d.out.length; i++) {
      if (d.out[i].name === pin) return pinPos(node, d.out[i], true, i);
    }
    return null;
  }

  function nodeAt(gx, gy) {
    for (let i = doc.nodes.length - 1; i >= 0; i--) {
      const n = doc.nodes[i];
      const s = nodeSize(n);
      if (gx >= n.x && gx <= n.x + s.w && gy >= n.y && gy <= n.y + s.h) return n;
    }
    return null;
  }

  /* 找最近的引脚(阈值按屏幕像素折算,缩放后也好点) */
  function pinNear(gx, gy) {
    const thr = 10 / view.zoom;
    let best = null, bestD = thr;
    doc.nodes.forEach((n) => {
      const d = defOf(n);
      if (!d) return;
      d.in.forEach((p, i) => {
        const pos = pinPos(n, p, false, i);
        const dist = Math.hypot(pos.x - gx, pos.y - gy);
        if (dist < bestD) { bestD = dist; best = { node: n, pin: p, isOut: false }; }
      });
      d.out.forEach((p, i) => {
        const pos = pinPos(n, p, true, i);
        const dist = Math.hypot(pos.x - gx, pos.y - gy);
        if (dist < bestD) { bestD = dist; best = { node: n, pin: p, isOut: true }; }
      });
    });
    return best;
  }

  /* ---------- 绘制 ---------- */

  function draw() {
    if (!ctx) return;
    ctx.clearRect(0, 0, cw, ch);
    ctx.fillStyle = '#161622';
    ctx.fillRect(0, 0, cw, ch);
    drawGrid();
    /* 先画线再画节点,节点盖住线头 */
    doc.links.forEach(drawLink);
    if (pending) drawPendingLink();
    doc.nodes.forEach(drawNode);
  }

  function drawGrid() {
    const step = 24 * view.zoom;
    if (step < 6) return;
    ctx.save();
    ctx.strokeStyle = 'rgba(137,180,250,0.08)';
    ctx.lineWidth = 1;
    const off = toScreen(0, 0);
    for (let x = off.x % step; x < cw; x += step) {
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, ch); ctx.stroke();
    }
    for (let y = off.y % step; y < ch; y += step) {
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(cw, y); ctx.stroke();
    }
    ctx.restore();
  }

  function pinColor(t) {
    return t === 'exec' ? '#f9e2af' : (t === 'string' ? '#94e2d5' : '#89b4fa');
  }

  function drawNode(n) {
    const d = defOf(n);
    const s = nodeSize(n);
    const p = toScreen(n.x, n.y);
    const w = s.w * view.zoom, h = s.h * view.zoom;
    ctx.save();
    ctx.fillStyle = n.id === sel ? '#2f3b5c' : '#252537';
    ctx.strokeStyle = n.id === sel ? '#f9e2af' : '#3b3b58';
    ctx.lineWidth = n.id === sel ? 2 : 1;
    roundRect(p.x, p.y, w, h, 6);
    ctx.fill();
    ctx.stroke();
    /* 标题 */
    ctx.fillStyle = (d && d.cat === '事件') ? '#f9e2af'
      : (d && d.cat === '条件') ? '#94e2d5' : '#89b4fa';
    ctx.font = Math.max(9, 12 * view.zoom) + 'px "Microsoft YaHei UI", sans-serif';
    ctx.textBaseline = 'middle';
    ctx.fillText(d ? d.title : n.type, p.x + 8, p.y + HEAD_H * view.zoom / 2);
    /* 引脚 + 属性摘要 */
    if (d) {
      d.in.forEach((pin, i) => {
        const q = pinPos(n, pin, false, i);
        const sp = toScreen(q.x, q.y);
        ctx.fillStyle = pinColor(pin.type);
        ctx.beginPath();
        ctx.arc(sp.x, sp.y, Math.max(2, 4 * view.zoom), 0, Math.PI * 2);
        ctx.fill();
        ctx.fillStyle = '#a6adc8';
        ctx.fillText(pin.name, sp.x + 8, sp.y);
      });
      d.out.forEach((pin, i) => {
        const q = pinPos(n, pin, true, i);
        const sp = toScreen(q.x, q.y);
        ctx.fillStyle = pinColor(pin.type);
        ctx.beginPath();
        ctx.arc(sp.x, sp.y, Math.max(2, 4 * view.zoom), 0, Math.PI * 2);
        ctx.fill();
        const tw = ctx.measureText(pin.name).width;
        ctx.fillStyle = '#a6adc8';
        ctx.fillText(pin.name, sp.x - 8 - tw, sp.y);
      });
      /* 属性摘要:让节点在画布上就"看得出在干什么" */
      if (d.props.length) {
        const y = p.y + (HEAD_H + ROW_H * Math.max(d.in.length, d.out.length, 1))
          * view.zoom + 4;
        ctx.fillStyle = '#7f849c';
        ctx.font = Math.max(8, 11 * view.zoom) + 'px Consolas, monospace';
        const props = n.props || {};
        const txt = d.props.map((pr) => String(props[pr.name] === undefined ? '' :
          props[pr.name])).join(' ').slice(0, 30);
        ctx.fillText(txt, p.x + 8, y + 6 * view.zoom);
      }
    }
    ctx.restore();
  }

  function roundRect(x, y, w, h, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
  }

  function drawLink(l) {
    const a = doc.nodes.find((n) => n.id === l.from);
    const b = doc.nodes.find((n) => n.id === l.to);
    if (!a || !b) return;
    const pa = pinAt(a, l.from_pin);
    const pb = pinAt(b, l.to_pin);
    if (!pa || !pb) return;
    const sa = toScreen(pa.x, pa.y), sb = toScreen(pb.x, pb.y);
    const exec = l.from_pin === 'out' || l.from_pin === 'true'
      || l.from_pin === 'false';
    ctx.save();
    ctx.strokeStyle = exec ? '#f9e2af' : '#89b4fa';
    ctx.lineWidth = exec ? 2 : 1.4;
    bezier(sa, sb);
    ctx.stroke();
    ctx.restore();
  }

  function bezier(a, b) {
    const dx = Math.max(30, Math.abs(b.x - a.x) * 0.5);
    ctx.beginPath();
    ctx.moveTo(a.x, a.y);
    ctx.bezierCurveTo(a.x + dx, a.y, b.x - dx, b.y, b.x, b.y);
  }

  function drawPendingLink() {
    const a = doc.nodes.find((n) => n.id === pending.from);
    if (!a) return;
    const pa = pinAt(a, pending.from_pin);
    if (!pa) return;
    const sa = toScreen(pa.x, pa.y);
    const sb = toScreen(pending.gx, pending.gy);
    ctx.save();
    ctx.strokeStyle = '#a6e3a1';
    ctx.lineWidth = 2;
    ctx.setLineDash([5, 4]);
    bezier(sa, sb);
    ctx.stroke();
    ctx.restore();
  }

  /* ---------- 与模型同步 ---------- */

  async function refresh() {
    if (!types.length) {
      try { types = await ds('graph.types'); } catch (e) { log('er', e.message); }
      typeMap = {};
      types.forEach((t) => { typeMap[t.type] = t; });
    }
    try {
      const info = await ds('graph.info');
      doc = info.graph || { nodes: [], links: [] };
      doc.nodes = doc.nodes || [];
      doc.links = doc.links || [];
    } catch (e) { log('er', 'graph.info: ' + e.message); }
    draw();
  }

  async function addNode(type, gx, gy) {
    try {
      const r = await call('graph.node.add', { type, x: gx, y: gy }, '加节点');
      sel = r.id;
      await refresh();
      await Graph.renderInspector();
      return r.id;
    } catch (e) { /* 已提示 */ }
    return 0;
  }

  async function removeSelected() {
    if (!sel) return;
    try {
      await call('graph.node.remove', { id: sel }, '删节点');
      sel = 0;
      await refresh();
      await Graph.renderInspector();
    } catch (e) { /* 已提示 */ }
  }

  async function save() {
    try {
      await call('graph.save', {}, '保存逻辑图');
      log('dim', '逻辑图已保存');
    } catch (e) { /* 已提示 */ }
  }

  async function generate() {
    try {
      const r = await call('graph.generate', {}, '生成代码');
      log('dim', '已生成 ' + r.path);
      showTab('log');
      log('dim', r.source);
    } catch (e) { /* 已提示 */ }
  }

  /* ---------- 鼠标 ---------- */

  function wireMouse() {
    canvas.addEventListener('contextmenu', (ev) => ev.preventDefault());

    canvas.addEventListener('wheel', (ev) => {
      ev.preventDefault();
      const p = eventPos(ev);
      const before = toGraph(p.x, p.y);
      const f = ev.deltaY < 0 ? 1.12 : 1 / 1.12;
      view.zoom = Math.max(0.25, Math.min(3, view.zoom * f));
      view.x = before.x - p.x / view.zoom;
      view.y = before.y - p.y / view.zoom;
      draw();
    }, { passive: false });

    canvas.addEventListener('mousedown', (ev) => {
      const p = eventPos(ev);
      const g = toGraph(p.x, p.y);
      if (ev.button === 1 || (ev.button === 0 && ev.altKey)) {
        drag = { mode: 'pan', sx: p.x, sy: p.y, vx: view.x, vy: view.y };
        return;
      }
      if (ev.button === 2) {              /* 右键:删节点 或 断开输入 */
        const pin = pinNear(g.x, g.y);
        if (pin && !pin.isOut) {
          ds('graph.unlink', { to: pin.node.id, to_pin: pin.pin.name })
            .then(refresh).catch((e) => log('er', e.message));
          return;
        }
        const n = nodeAt(g.x, g.y);
        if (n) { sel = n.id; removeSelected(); }
        return;
      }
      if (ev.button !== 0) return;
      const pin = pinNear(g.x, g.y);
      if (pin) {
        sel = pin.node.id;
        if (pin.isOut) {
          pending = { from: pin.node.id, from_pin: pin.pin.name, gx: g.x, gy: g.y };
        } else {
          /* 从输入引脚拖走 = 断开这条输入 */
          ds('graph.unlink', { to: pin.node.id, to_pin: pin.pin.name })
            .then(() => { refresh(); Graph.renderInspector(); })
            .catch((e) => log('er', e.message));
        }
        draw();
        return;
      }
      const n = nodeAt(g.x, g.y);
      if (n) {
        sel = n.id;
        drag = { mode: 'node', id: n.id, dx: g.x - n.x, dy: g.y - n.y };
        Graph.renderInspector();
        draw();
        return;
      }
      sel = 0;
      drag = { mode: 'pan', sx: p.x, sy: p.y, vx: view.x, vy: view.y };
      Graph.renderInspector();
      draw();
    });

    window.addEventListener('mousemove', (ev) => {
      if (!canvas || !drag) return;
      const p = eventPos(ev);
      const g = toGraph(p.x, p.y);
      if (drag.mode === 'pan') {
        view.x = drag.vx - (p.x - drag.sx) / view.zoom;
        view.y = drag.vy - (p.y - drag.sy) / view.zoom;
        draw();
      } else if (drag.mode === 'node') {
        const n = doc.nodes.find((k) => k.id === drag.id);
        if (n) { n.x = Math.round(g.x - drag.dx); n.y = Math.round(g.y - drag.dy); }
        draw();
      }
      if (pending) { pending.gx = g.x; pending.gy = g.y; draw(); }
    });

    window.addEventListener('mouseup', async (ev) => {
      if (!canvas) return;
      const p = eventPos(ev);
      const g = toGraph(p.x, p.y);
      if (pending) {
        const pin = pinNear(g.x, g.y);
        const from = pending;
        pending = null;
        if (pin && !pin.isOut) {
          try {
            await call('graph.link', { from: from.from, from_pin: from.from_pin,
                                       to: pin.node.id, to_pin: pin.pin.name }, '连线');
          } catch (e) { /* 原因已显示在输出面板 */ }
        }
        await refresh();
        Graph.renderInspector();
        return;
      }
      if (drag && drag.mode === 'node') {
        const n = doc.nodes.find((k) => k.id === drag.id);
        drag = null;
        if (n) {
          try {
            await call('graph.node.move', { id: n.id, x: n.x, y: n.y }, '移动节点');
          } catch (e) { /* 已提示 */ }
          await refresh();
        }
        return;
      }
      drag = null;
      draw();
    });
  }

  function init(c) {
    canvas = c;
    ctx = canvas.getContext('2d');
    wireMouse();
    window.addEventListener('resize', () => { resize(); draw(); });
    resize();
    draw();
  }

  /* ---------- 属性面板(graph.inspector) ---------- */

  function renderInspector() {
    const box = $('graph-inspector');
    if (!box) return;
    const n = doc.nodes.find((k) => k.id === sel);
    box.innerHTML = '';
    if (!n) {
      box.innerHTML = '<p class="hint">在画布上点一个节点(右键加点/删点,拖引脚连线)。</p>';
      return;
    }
    const d = defOf(n);
    const head = document.createElement('div');
    head.className = 'comp';
    head.innerHTML = '<div class="head"><span>' + (d ? d.title : n.type) +
      '</span><span class="mini hint">#' + n.id + '</span></div>';
    box.appendChild(head);
    const props = n.props || {};
    (d ? d.props : []).forEach((pr) => {
      const row = document.createElement('div');
      row.className = 'field';
      const lab = document.createElement('label');
      lab.textContent = pr.name;
      row.appendChild(lab);
      let inp;
      if (pr.name === 'op') {
        inp = document.createElement('select');
        ['<', '<=', '==', '!=', '>=', '>', '+', '-', '*', '/', '%', '^']
          .forEach((op) => {
            const o = document.createElement('option');
            o.value = op; o.textContent = op;
            inp.appendChild(o);
          });
        inp.value = props.op || (n.type === 'compare' ? '<' : '+');
      } else if (pr.name === 'as') {
        inp = document.createElement('select');
        [['f', '小数(float)'], ['i', '整数(int)']].forEach(([v, t]) => {
          const o = document.createElement('option');
          o.value = v; o.textContent = t;
          inp.appendChild(o);
        });
        inp.value = props.as || 'f';
      } else if (pr.name === 'axis') {
        inp = document.createElement('select');
        [['x', 'X'], ['y', 'Y']].forEach(([v, t]) => {
          const o = document.createElement('option');
          o.value = v; o.textContent = t;
          inp.appendChild(o);
        });
        inp.value = props.axis || 'x';
      } else if (pr.type === 'string') {
        inp = document.createElement('input');
        inp.type = 'text';
        inp.value = props[pr.name] === undefined ? '' : String(props[pr.name]);
      } else {
        inp = document.createElement('input');
        inp.type = 'number';
        inp.step = pr.type === 'int' ? '1' : '0.5';
        inp.value = props[pr.name] === undefined ? 0 : props[pr.name];
      }
      inp.onchange = async () => {
        let v = inp.value;
        if (inp.type === 'number') v = parseFloat(v) || 0;
        try {
          await call('graph.node.set', { id: n.id, props: { [pr.name]: v } },
                     '改节点属性');
          await refresh();
        } catch (e) { /* 已提示 */ }
      };
      row.appendChild(inp);
      box.appendChild(row);
    });
    if (!d || !d.props.length) {
      const p = document.createElement('p');
      p.className = 'hint';
      p.textContent = (d && d.cat === '事件')
        ? '事件节点:从这里拉出执行流。' : '(这个节点没有属性)';
      box.appendChild(p);
    }
  }

  /* ---------- 节点面板(左栏的"可加节点") ---------- */

  function renderPalette() {
    const box = $('graph-palette');
    if (!box) return;
    box.innerHTML = '';
    const cats = {};
    types.forEach((t) => {
      (cats[t.cat] = cats[t.cat] || []).push(t);
    });
    Object.keys(cats).forEach((cat) => {
      const h = document.createElement('div');
      h.className = 'cat';
      h.textContent = cat;
      box.appendChild(h);
      cats[cat].forEach((t) => {
        const b = document.createElement('button');
        b.className = 'nodebtn';
        b.textContent = t.title;
        b.title = t.type;
        b.onclick = () => addNode(t.type, view.x + 60, view.y + 60);
        box.appendChild(b);
      });
    });
  }

  return {
    init, refresh, draw, resize, renderInspector, renderPalette,
    save, generate, removeSelected, addNode,
    select: (id) => { sel = id; draw(); renderInspector(); },
    get selected() { return sel; },
    get doc() { return doc; },
    /* 自检用 */
    stats: () => ({ nodes: doc.nodes.length, links: doc.links.length,
                    types: types.length, sel }),
    center: () => { view = { x: 40, y: 40, zoom: 1 }; draw(); },
  };
})();

window.addEventListener('DOMContentLoaded', () => {
  const c = $('graph');
  if (c) Graph.init(c);
});
