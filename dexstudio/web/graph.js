/* DexStudio 逻辑图编辑器:UE 蓝图式的节点画布。
 *
 * 与视口一样,**图数据全在 C 模型层**(scripts/logic.json),这里只画:
 *   - 节点目录来自 graph.types → 节点长什么样、有哪些引脚/属性都由 C 说了算
 *   - 属性下拉的候选来自 graph.options(实体名/组件→字段/动作/按键/运算符/资源)
 *     —— 以前这些全是**手打字符串**:打错一个字就生成 eng_find("player ") 这种
 *     运行期静默无效的代码,用户完全查不出来。
 *
 * 交互:
 *   左键拖节点 / 拖引脚连线 · 中键或 Alt 拖 = 平移 · 滚轮 = 缩放
 *   从输出引脚拖到输入引脚 = 连线;从输入引脚拖走 = 断开该输入
 *   Delete = 删选中节点 · Ctrl+D = 复制节点 · 点「校验」看漏填的属性
 */
'use strict';

const Graph = (function () {
  let canvas, ctx, cw = 0, ch = 0;
  let types = [];              // graph.types 的结果
  let typeMap = {};            // type → def
  let opts = { entities: [], schema: [], actions: [], keys: [], mouse: [],
               ops: [], images: [], audios: [] };   // graph.options
  let doc = { nodes: [], links: [] };
  let sel = 0;                 // 主选中节点(属性面板显示它)
  let selSet = new Set();      // 多选(框选/Shift 点选);操作按整组来
  let marquee = null;          // 框选矩形(屏幕坐标)
  let drag = null;             // {mode:'node'|'pan', ...}
  let view = { x: 40, y: 40, zoom: 1 };   // 图坐标 → 屏幕: p = (g - view) * zoom
  let pending = null;          // 拖动中的连线 {from, from_pin, x, y}
  let hoverPin = null;
  let issueIds = {};           // 校验出来的**错误**节点:id → 1(画红框)
  let issues = [];
  let errCount = 0;            // 其中必须修的(错误)条数;警告不拦生成
  let mouseGraph = null;       // 鼠标在图坐标里的位置(新节点就加在这儿)
  let filter = '';
  let drawQueued = false;

  function requestDraw() {
    if (drawQueued) return;
    drawQueued = true;
    requestAnimationFrame(() => {
      drawQueued = false;
      draw();
    });
  }

  /* ---------- 选中(单选 / 多选) ---------- */

  const isSel = (id) => selSet.has(id);

  function selectOne(id) {
    selSet = new Set(id ? [id] : []);
    sel = id || 0;
  }

  function selectToggle(id) {
    if (!id) return;
    if (selSet.has(id)) selSet.delete(id);
    else selSet.add(id);
    sel = id;
  }

  function selectMany(ids, additive) {
    if (!additive) selSet = new Set();
    (ids || []).forEach((i) => selSet.add(i));
    sel = ids && ids.length ? ids[ids.length - 1] : 0;
  }

  function selectAllNodes() {
    selectMany(doc.nodes.map((n) => n.id), false);
    draw();
    renderInspector();
  }

  function selectedIds() { return Array.from(selSet); }

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
    const y = node.y + HEAD_H + ROW_H * (index + 0.5);
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
    doc.links.forEach(drawLink);
    if (pending) drawPendingLink();
    doc.nodes.forEach(drawNode);
    drawMarquee();
    drawInfo();
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
    ctx.fillStyle = n.id === sel ? '#2f3b5c' : (isSel(n.id) ? '#2a3350' : '#252537');
    ctx.strokeStyle = issueIds[n.id] ? '#f38ba8'
      : (n.id === sel ? '#f9e2af' : (isSel(n.id) ? '#89b4fa' : '#3b3b58'));
    ctx.lineWidth = (n.id === sel || issueIds[n.id]) ? 2 : 1;
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

  /* 框选矩形:和场景视口一样的手感 */
  function drawMarquee() {
    if (!marquee) return;
    ctx.save();
    ctx.strokeStyle = '#89b4fa';
    ctx.fillStyle = 'rgba(137,180,250,0.14)';
    ctx.setLineDash([4, 3]);
    const x = Math.min(marquee.x0, marquee.x1), y = Math.min(marquee.y0, marquee.y1);
    const w = Math.abs(marquee.x1 - marquee.x0), h = Math.abs(marquee.y1 - marquee.y0);
    ctx.fillRect(x, y, w, h);
    ctx.strokeRect(x, y, w, h);
    ctx.restore();
  }

  /* 画布左下角的提示:把"现在能做什么/哪里有问题"直接写在图布上 */
  function drawInfo() {
    const lines = [];
    if (errCount) {
      lines.push('⚠ ' + errCount + ' 处必须修的问题(点「校验」看清单)');
    }
    if (doc.nodes.length === 0) {
      lines.push('从左边「节点面板」点一个**事件**节点开始(比如「每帧」)');
    }
    if (!lines.length) return;
    ctx.save();
    ctx.font = '12px "Microsoft YaHei UI", sans-serif';
    ctx.fillStyle = issues.length ? '#f38ba8' : '#6c7086';
    lines.forEach((s, i) => ctx.fillText(s, 10, ch - 12 - i * 16));
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
      opts = await ds('graph.options');
    } catch (e) { log('er', 'graph.options: ' + e.message); }
    try {
      const info = await ds('graph.info');
      doc = info.graph || { nodes: [], links: [] };
      doc.nodes = doc.nodes || [];
      doc.links = doc.links || [];
    } catch (e) { log('er', 'graph.info: ' + e.message); }
    issueIds = {};
    issues = [];
    errCount = 0;
    renderInfo();
    draw();
  }

  function renderInfo() {
    const el = $('graph-info');
    if (el) {
      el.textContent = '节点 ' + doc.nodes.length + ' · 连线 ' + doc.links.length +
        ' · ' + Math.round(view.zoom * 100) + '%' +
        (errCount ? ' · ⚠ ' + errCount + ' 处必改' : '');
    }
  }

  /* 在鼠标位置附近找个空位加节点(以前永远加在同一个地方,连点几下就叠一起) */
  function nextFreeSpot() {
    let gx, gy;
    if (mouseGraph) { gx = mouseGraph.x - 20; gy = mouseGraph.y - 20; }
    else { gx = view.x + 60; gy = view.y + 60; }
    gx = Math.round(gx / 8) * 8;
    gy = Math.round(gy / 8) * 8;
    let guard = 0;
    while (guard++ < 40) {
      const clash = doc.nodes.some((n) => {
        const s = nodeSize(n);
        return Math.abs(n.x - gx) < 30 && Math.abs(n.y - gy) < 30;
      });
      if (!clash) break;
      gx += 28; gy += 28;
    }
    return { x: gx, y: gy };
  }

  async function addNode(type, gx, gy) {
    let x = gx, y = gy;
    if (x === undefined || y === undefined) {
      const p = nextFreeSpot();
      x = p.x; y = p.y;
    }
    try {
      const r = await call('graph.node.add', { type, x, y }, '加节点');
      sel = r.id;
      await refresh();
      await Graph.renderInspector();
      return r.id;
    } catch (e) { /* 已提示 */ }
    return 0;
  }

  async function removeSelected() {
    const ids = selectedIds();
    if (!ids.length) { toast('先点一个节点(或框选几个)', 'warn'); return; }
    try {
      await call('graph.node.remove', { ids }, '删节点');
      selectOne(0);
      await refresh();
      await Graph.renderInspector();
    } catch (e) { /* 已提示 */ }
  }

  async function duplicateSelected() {
    const ids = selectedIds();
    if (!ids.length) { toast('先点一个节点(或框选几个)', 'warn'); return; }
    try {
      const r = await call('graph.node.duplicate', { ids, dx: 28, dy: 28 }, '复制节点');
      selectMany(r.ids || [], false);
      await refresh();
      await Graph.renderInspector();
      toast('已复制 ' + (r.ids || []).length + ' 个节点(属性一起复制,连线不复制)', 'ok');
    } catch (e) { /* 已提示 */ }
  }

  async function save() {
    try {
      await call('graph.save', {}, '保存逻辑图');
      toast('逻辑图已保存(scripts/logic.json)', 'ok');
    } catch (e) { /* 已提示 */ }
  }

  /* 校验:把"缺什么/引用了谁"列在输出面板,并**在画布上标红**对应节点。
   * 只有 error 会拦住生成;warn(实体不在当前场景)只是提醒。 */
  async function check(quietMode) {
    let r;
    try {
      r = await ds('graph.validate');
    } catch (e) {
      log('er', 'graph.validate: ' + e.message);
      return null;
    }
    issues = r.issues || [];
    issueIds = {};
    errCount = r.errors || 0;
    issues.forEach((it) => { if (it.level !== 'warn') issueIds[it.id] = 1; });
    renderInfo();
    draw();
    if (quietMode) return r;
    showTab('build');
    const box = $('buildout');
    const parts = [
      '<div class="diag ' + (r.ok ? (r.count ? 'warn' : 'ok') : 'err') + '">' +
      (r.errors ? '逻辑图有 ' + r.errors + ' 处必须修的问题(点一下跳到那个节点)'
                : (r.warnings ? '逻辑图能用,但有 ' + r.warnings + ' 条提醒'
                              : '逻辑图检查通过:没有漏填的属性')) + '</div>',
    ];
    issues.forEach((it) => {
      parts.push('<div class="diag ' + (it.level === 'warn' ? 'warn' : 'err') +
        ' issue jump" data-node="' + it.id + '">' +
        (it.level === 'warn' ? '提醒 ' : '错误 ') +
        '#' + it.id + ' [' + esc(it.title) + '] ' + (it.field ? it.field + ':' : '') +
        esc(it.msg) + '</div>');
    });
    box.innerHTML = parts.join('');
    box.querySelectorAll('.jump').forEach((n) => {
      n.onclick = () => {
        const id = parseInt(n.dataset.node, 10);
        Graph.select(id);
        focusNode(id);
      };
    });
    if (!r.ok) toast('逻辑图有 ' + r.errors + ' 处必须修的问题(见「编译」页签)', 'err');
    else if (r.warnings) toast('逻辑图有 ' + r.warnings + ' 条提醒(见「编译」页签)', 'warn');
    else toast('逻辑图检查通过', 'ok');
    return r;
  }

  function focusNode(id) {
    const n = doc.nodes.find((k) => k.id === id);
    if (!n) return;
    view.x = n.x - cw / view.zoom / 2 + 80;
    view.y = n.y - ch / view.zoom / 2 + 40;
    draw();
  }

  async function generate() {
    const r = await check(true);
    if (r && !r.ok) {
      await check(false);
      return null;
    }
    try {
      const g = await call('graph.generate', {}, '生成代码');
      showTab('build');
      $('buildout').innerHTML = '<div class="diag ok">已生成 ' + esc(g.path) +
        '</div><div class="raw">' + esc(g.source) + '</div>';
      toast('已生成 scripts/logic.dex', 'ok');
      return g;
    } catch (e) { /* 已提示(原因在输出面板) */
      showTab('log');
      return null;
    }
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
      renderInfo();
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
        if (n) { selectOne(n.id); removeSelected(); }
        return;
      }
      if (ev.button !== 0) return;
      const pin = pinNear(g.x, g.y);
      if (pin) {
        sel = pin.node.id;
        if (!selSet.has(sel)) selSet.add(sel);
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
        /* 点已选中的节点:保持整组选中(方便一起拖);点别的:单选或 Shift 加选 */
        if (ev.shiftKey) selectToggle(n.id);
        else if (!isSel(n.id)) selectOne(n.id);
        else sel = n.id;
        const start = {};
        selectedIds().forEach((id) => {
          const k = doc.nodes.find((x) => x.id === id);
          if (k) start[id] = { x: k.x, y: k.y };
        });
        drag = { mode: 'node', dx: g.x - n.x, dy: g.y - n.y, start, moved: false };
        Graph.renderInspector();
        draw();
        return;
      }
      /* 空白处:框选(Shift = 追加) */
      marquee = { x0: p.x, y0: p.y, x1: p.x, y1: p.y, gx0: g.x, gy0: g.y,
                  add: !!ev.shiftKey, moved: false };
      drag = { mode: 'pan', sx: p.x, sy: p.y, vx: view.x, vy: view.y, viaMarquee: true };
      draw();
    });

    window.addEventListener('mousemove', (ev) => {
      if (!canvas) return;
      const p = eventPos(ev);
      const g = toGraph(p.x, p.y);
      mouseGraph = g;
      /* 正在拉的那根线要**跟着鼠标走**。注意它用 pending,不是 drag ——
       * 早先这个处理函数开头先判断 drag 是不是空、为空就直接 return,
       * 而拉线时 drag 恰好是 null,于是预览线只在起点画一次、一动不动,
       * 看起来像"连完线才出现一根线"。 */
      if (pending) { pending.gx = g.x; pending.gy = g.y; requestDraw(); }
      if (!drag) return;
      if (marquee) {
        marquee.x1 = p.x;
        marquee.y1 = p.y;
        if (Math.abs(marquee.x1 - marquee.x0) > 3
            || Math.abs(marquee.y1 - marquee.y0) > 3) marquee.moved = true;
        requestDraw();
        return;
      }
      if (drag.mode === 'pan') {
        view.x = drag.vx - (p.x - drag.sx) / view.zoom;
        view.y = drag.vy - (p.y - drag.sy) / view.zoom;
        renderInfo();
        requestDraw();
      } else if (drag.mode === 'node') {
        const dx = g.x - drag.dx, dy = g.y - drag.dy;
        const first = doc.nodes.find((k) => k.id === sel);
        if (first) {
          const moveX = dx - first.x, moveY = dy - first.y;
          drag.moved = drag.moved || Math.abs(moveX) > 0.5 || Math.abs(moveY) > 0.5;
          Object.keys(drag.start).forEach((id) => {
            const k = doc.nodes.find((x) => x.id === parseInt(id, 10));
            const s = drag.start[id];
            if (k) { k.x = Math.round(s.x + moveX); k.y = Math.round(s.y + moveY); }
          });
        }
        requestDraw();
      }
    });

    window.addEventListener('mouseup', async () => {
      if (!canvas) return;
      const g = mouseGraph || { x: 0, y: 0 };
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
      if (marquee) {
        const m = marquee;
        marquee = null;
        drag = null;
        if (m.moved) {
          /* 屏幕矩形 → 图坐标,取与之相交的节点 */
          const a = toGraph(Math.min(m.x0, m.x1), Math.min(m.y0, m.y1));
          const b = toGraph(Math.max(m.x0, m.x1), Math.max(m.y0, m.y1));
          const ids = doc.nodes.filter((n) => {
            const s = nodeSize(n);
            return n.x < b.x && n.x + s.w > a.x && n.y < b.y && n.y + s.h > a.y;
          }).map((n) => n.id);
          selectMany(ids, m.add);
          Graph.renderInspector();
        } else {
          selectOne(0);
          Graph.renderInspector();
        }
        draw();
        return;
      }
      if (drag && drag.mode === 'node') {
        const moved = drag.moved;
        const start = drag.start;
        drag = null;
        if (moved) {
          const items = Object.keys(start).map((id) => {
            const k = doc.nodes.find((x) => x.id === parseInt(id, 10));
            return k ? { id: k.id, x: k.x, y: k.y } : null;
          }).filter(Boolean);
          try {
            await call('graph.node.move_many', { items }, '移动节点');
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
    /* 同视口:容器尺寸变化就重算(离屏自测里窗口尺寸是后设上来的,
     * 页面首帧可能看到 0×0 的容器;模式切换也未必触发 window resize) */
    if (typeof ResizeObserver !== 'undefined') {
      new ResizeObserver(() => { resize(); draw(); }).observe(canvas.parentElement);
    }
    resize();
    draw();
  }

  /* ---------- 属性面板 ---------- */

  /* 一个属性 = 一行。**引用型属性一律下拉**(实体名/组件/字段/动作/按键/鼠标键/
   * 资源路径)——以前全是手打,打错就是运行期静默无效。 */
  function propControl(n, pr, props, commit) {
    const name = pr.name;
    const mk = (tag, cls) => {
      const e = document.createElement(tag);
      if (cls) e.className = cls;
      return e;
    };
    const fill = (sel, list, cur, labelOf) => {
      list.forEach((v) => {
        const o = mk('option');
        const val = (v && typeof v === 'object') ? v.value : v;
        o.value = String(val);
        o.textContent = labelOf ? labelOf(v) : String(val);
        if (String(val) === String(cur)) o.selected = true;
        sel.appendChild(o);
      });
    };
    if (name === 'op') {
      const s = mk('select');
      fill(s, opts.ops || [], props.op || '', null);
      s.onchange = () => commit(s.value);
      return s;
    }
    if (name === 'as') {
      const s = mk('select');
      fill(s, [{ value: 'f', label: '小数 (float)' }, { value: 'i', label: '整数 (int)' }],
           props.as || 'f', (x) => x.label);
      s.onchange = () => commit(s.value);
      return s;
    }
    if (name === 'axis') {
      const s = mk('select');
      fill(s, [{ value: 'x', label: 'X' }, { value: 'y', label: 'Y' }],
           props.axis || 'x', (x) => x.label);
      s.onchange = () => commit(s.value);
      return s;
    }
    if (name === 'action') {
      const s = mk('select');
      fill(s, opts.actions || [], props.action || '', null);
      s.onchange = () => commit(s.value);
      return s;
    }
    if (name === 'key') {
      const s = mk('select');
      fill(s, opts.keys || [], props.key === undefined ? '' : props.key,
           (x) => x.label + '(' + x.value + ')');
      s.onchange = () => commit(parseFloat(s.value));
      return s;
    }
    if (name === 'btn') {
      const s = mk('select');
      fill(s, opts.mouse || [], props.btn === undefined ? '' : props.btn,
           (x) => x.label + '(' + x.value + ')');
      s.onchange = () => commit(parseFloat(s.value));
      return s;
    }
    if (name === 'obj' || name === 'cam') {
      const s = mk('select');
      const ents = (opts.entities || []).map((e) => e.name);
      fill(s, ents, props[name] || '', null);
      if (props[name] && ents.indexOf(props[name]) < 0) {
        const o = mk('option');
        o.value = props[name];
        o.textContent = props[name] + '(场景里没有)';
        o.selected = true;
        s.appendChild(o);
      }
      s.onchange = () => commit(s.value);
      return s;
    }
    if (name === 'comp') {
      const s = mk('select');
      fill(s, (opts.schema || []).map((c) => c.name), props.comp || '', null);
      s.onchange = () => {
        /* 换组件时顺手把字段名改成这个组件第一个字段,免得留下无效组合 */
        commit(s.value);
      };
      return s;
    }
    if (name === 'field') {
      const s = mk('select');
      const comp = (opts.schema || []).find((c) => c.name === (props.comp || 'transform'));
      fill(s, comp ? comp.fields : [], props.field || '', null);
      s.onchange = () => commit(s.value);
      return s;
    }
    if (name === 'path') {
      const s = mk('select');
      const list = (opts.audios || []).slice();
      fill(s, list, props.path || '', (p) => String(p).replace(/^res\//, ''));
      if (props.path && list.indexOf(props.path) < 0) {
        const o = mk('option');
        o.value = props.path;
        o.textContent = props.path + '(不在 res/ 里)';
        o.selected = true;
        s.appendChild(o);
      }
      if (!list.length) {
        const o = mk('option');
        o.value = '';
        o.textContent = '(res/ 里还没有音频,先导入一个)';
        s.appendChild(o);
      }
      s.onchange = () => commit(s.value);
      return s;
    }
    if (pr.type === 'string') {
      const inp = mk('input');
      inp.type = 'text';
      inp.value = props[name] === undefined ? '' : String(props[name]);
      inp.onchange = () => commit(inp.value);
      return inp;
    }
    const inp = mk('input');
    inp.type = 'number';
    inp.step = pr.type === 'int' ? '1' : '0.5';
    inp.value = props[name] === undefined ? 0 : props[name];
    inp.onchange = () => {
      const raw = inp.value.trim();
      if (raw === '' || isNaN(parseFloat(raw))) {
        inp.value = props[name] === undefined ? 0 : props[name];
        toast('这里要填一个数字(已还原)');
        return;
      }
      commit(parseFloat(raw));
    };
    return inp;
  }

  function renderInspector() {
    const box = $('graph-inspector');
    if (!box) return;
    const n = doc.nodes.find((k) => k.id === sel);
    box.innerHTML = '';
    const title = $('graph-sel');
    if (title) {
      title.textContent = n ? ('#' + n.id + ' ' + ((defOf(n) || {}).title || n.type))
                            : '未选中';
    }
    if (!n) {
      box.innerHTML = '<p class="hint">在画布上点一个节点(右键加点/删点,拖引脚连线)。</p>';
      return;
    }
    const d = defOf(n);
    const head = document.createElement('div');
    head.className = 'comp';
    head.innerHTML = '<div class="head"><span>' + esc(d ? d.title : n.type) +
      '</span><span class="mini hint">#' + n.id + '</span></div>';
    box.appendChild(head);
    const props = n.props || {};
    (d ? d.props : []).forEach((pr) => {
      const row = document.createElement('div');
      row.className = 'field';
      const lab = document.createElement('label');
      lab.textContent = PROP_LABEL[pr.name] || pr.name;
      lab.title = pr.name;
      row.appendChild(lab);
      const inp = propControl(n, pr, props, async (v) => {
        try {
          await call('graph.node.set', { id: n.id, props: { [pr.name]: v } }, '改节点属性');
          await refresh();
          await Graph.renderInspector();
        } catch (e) { /* 已提示 */ }
      });
      row.appendChild(inp);
      box.appendChild(row);
    });
    if (!d || !d.props.length) {
      const p = document.createElement('p');
      p.className = 'hint';
      p.textContent = (d && d.cat === '事件')
        ? '事件节点:从这里拉出执行流,指向要执行的动作。' : '(这个节点没有属性)';
      box.appendChild(p);
    }
    /* 这个节点被校验挑出来的问题,直接写在属性面板上方 */
    const mine = issues.filter((it) => it.id === n.id);
    if (mine.length) {
      const div = document.createElement('div');
      div.className = 'batch-note err';
      div.textContent = '⚠ ' + mine.map((it) => it.msg).join(';');
      box.insertBefore(div, box.firstChild);
    }
  }

  /* 属性名 → 中文(自省给的是英文名;这层只影响显示) */
  const PROP_LABEL = {
    obj: '实体', cam: '相机实体', comp: '组件', field: '字段', value: '值',
    delta: '增量', as: '类型', action: '动作', key: '按键', btn: '鼠标键',
    path: '声音', volume: '音量', loop: '循环', op: '运算符', axis: '坐标轴',
    cond: '条件', ox: 'X 偏移', oy: 'Y 偏移',
  };

  /* ---------- 节点面板 ---------- */

  function renderPalette() {
    const box = $('graph-palette');
    if (!box) return;
    const f = ($('graph-filter') && $('graph-filter').value || '').toLowerCase();
    box.innerHTML = '';
    const cats = {};
    types.forEach((t) => {
      if (f && (t.title || '').toLowerCase().indexOf(f) < 0
          && (t.type || '').toLowerCase().indexOf(f) < 0
          && (t.cat || '').toLowerCase().indexOf(f) < 0) return;
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
        b.title = t.type + (NODE_HINT[t.type] ? ' — ' + NODE_HINT[t.type] : '');
        b.onclick = () => addNode(t.type);
        box.appendChild(b);
      });
    });
    if (!box.children.length) box.innerHTML = '<div class="hint">没有匹配的节点</div>';
  }

  const NODE_HINT = {
    on_start: '游戏开始时执行一次',
    on_update: '每帧执行',
    on_draw: '每帧绘制(画面相关)',
    on_action: '按下某个动作时(默认键位:方向键/WASD/空格)',
    on_key: '按下某个键时(按一下触发一次)',
    branch: '条件成立走「真」,否则走「假」',
    set_field: '把实体的某个字段设成某个值',
    add_field: '给字段做累加(常用于移动)',
    destroy: '把实体从场景里移除',
    play_sound: '播一个声音',
    camera_to: '让相机跟着某个实体',
    num: '一个固定数字',
    dt: '上一帧用了多少秒(做"每秒移动 N"用)',
    field: '读实体字段的当前值',
    math: '加减乘除',
    compare: '比较两个数(结果是 1 或 0)',
    action_down: '动作按住期间',
    action_pressed: '动作刚按下的那一帧',
    key_down: '按键按住期间',
    mouse_down: '鼠标键按住期间',
    on_ground: '实体站在地面上',
    mouse_world: '鼠标在世界里的坐标',
  };

  return {
    init, refresh, draw, resize, renderInspector, renderPalette, renderInfo,
    save, generate, check, removeSelected, duplicateSelected, addNode, focusNode,
    selectAll: selectAllNodes, selectMany,
    select: (id) => { selectOne(id); draw(); renderInspector(); },
    get selected() { return sel; },
    get selection() { return selectedIds(); },
    get doc() { return doc; },
    get issues() { return issues; },
    /* 自检用 */
    stats: () => ({ nodes: doc.nodes.length, links: doc.links.length,
                    types: types.length, sel, selCount: selSet.size,
                    issues: issues.length, errors: errCount }),
    center: () => { view = { x: 40, y: 40, zoom: 1 }; renderInfo(); draw(); },
    /* 自检用:拉线时 pending 必须跟着鼠标更新(曾经因为 mousemove 里先判断
     * `!drag` 而完全不动 —— 现象是"连完线才出现")。 */
    dragState: () => ({
      pending: pending ? { gx: pending.gx, gy: pending.gy,
                           from: pending.from, from_pin: pending.from_pin } : null,
      mode: drag ? drag.mode : null,
    }),
    /* 自检用:节点标题的屏幕位置(要往它身上派发鼠标事件) */
    nodeScreenPos: (node_id) => {
      const n = doc.nodes.find((k) => k.id === node_id);
      if (!n) return null;
      const s = nodeSize(n);
      return toScreen(n.x + s.w / 2, n.y + HEAD_H / 2);
    },
    /* 自检用:把某个引脚的位置换算成屏幕坐标(要往它上面派发鼠标事件) */
    pinScreenPos: (node_id, pin_name) => {
      const n = doc.nodes.find((k) => k.id === node_id);
      if (!n) return null;
      const q = pinAt(n, pin_name);
      if (!q) return null;
      return toScreen(q.x, q.y);
    },
    options: () => opts,
  };
})();

window.addEventListener('DOMContentLoaded', () => {
  const c = $('graph');
  if (c) Graph.init(c);
});
