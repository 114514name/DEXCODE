/* DexStudio 视口。
 *
 * 画面从哪来:引擎离屏渲染一帧(1024×640)落成 BMP,宿主把预览目录映射成了
 * https://dexstudio-preview.local/,所以这里只是 `<img>` + `drawImage`。
 * 为什么不把像素塞进 JSON:1024×640×4 = 2.6 MB,每条消息 3.4 MB(base64)
 * 会把这个通道彻底压垮。
 *
 * 坐标换算(引擎的相机约定:相机 (x,y) = 屏幕左上角的世界坐标,zoom = 每世界单位像素数):
 *   世界 → 图内像素:  px = (wx - view.x) * zoom
 *   图内像素 → 画布:  cx = px * scale + ox   (scale/ox 由"把图按比例装进视口"得出)
 * 于是反算:          wx = view.x + (cx - ox) / scale / zoom
 *
 * 交互:滚轮缩放(以光标为锚点)、中键/空格/右键拖动平移、左键选择与框选、
 * 拖动移动选中实体、瓦片刷子。
 */
'use strict';

const Viewport = (function () {
  const img = new Image();
  let imgReady = false;
  let imgSeq = -1;
  let canvas, ctx, cw = 0, ch = 0;
  /* 把渲染图装进视口的比例与偏移(letterbox) */
  let fit = { s: 1, ox: 0, oy: 0 };
  /* 交互状态 */
  let drag = null;
  let marquee = null;
  let hover = null;      /* 瓦片刷子:悬停的格子 */
  let painted = null;    /* 拖动连续画:本次已经画过的格子 */
  let localPos = null;   /* 拖动移动中的临时位置 {id:{x,y}} */
  const space = { down: false };

  img.onload = () => {
    imgReady = true;
    resize();
    draw();
  };

  function init(c) {
    canvas = c;
    ctx = canvas.getContext('2d');
    wireMouse();
    window.addEventListener('resize', () => { resize(); draw(); });
    /* 容器尺寸变化就跟着重算。为什么不能只靠 window resize:
     * ① 离屏/无人值守启动时,窗口尺寸是**创建之后**才设上来的,页面首帧看到的
     *    容器可能是 0×0(实测:离屏自测里画布恒为 0×0,像素断言全废);
     * ② 模式切换(场景↔逻辑↔代码)会换布局,不一定触发 window resize。 */
    if (typeof ResizeObserver !== 'undefined') {
      new ResizeObserver(() => { resize(); draw(); }).observe(canvas.parentElement);
    }
    window.addEventListener('keydown', (e) => {
      if (e.code === 'Space') { space.down = true; }
    });
    window.addEventListener('keyup', (e) => {
      if (e.code === 'Space') { space.down = false; }
    });
    resize();
    draw();
  }

  function resize() {
    if (!canvas) return;
    const r = canvas.parentElement.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    cw = Math.max(1, Math.round(r.width));
    ch = Math.max(1, Math.round(r.height));
    canvas.width = Math.round(cw * dpr);
    canvas.height = Math.round(ch * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    /* 图按比例装进视口(保持像素方形,不拉伸) */
    const s = Math.min(cw / DS.vw, ch / DS.vh);
    fit = { s, ox: (cw - DS.vw * s) / 2, oy: (ch - DS.vh * s) / 2 };
  }

  /* 让浏览器把新的 BMP 读进来再重画 */
  async function show(seq) {
    if (!canvas) return;
    imgSeq = seq;
    /* ?t= 破缓存。BMP 每次都是同一个路径,不加时间戳会被缓存住。 */
    img.src = 'https://dexstudio-preview.local/preview.bmp?t=' + seq;
    try { await img.decode(); imgReady = true; } catch (e) { /* onload 会兜底 */ }
    draw();
  }

  /* ---------- 坐标 ---------- */

  function toWorld(cx, cy) {
    const v = DS.view;
    return {
      x: v.x + (cx - fit.ox) / fit.s / v.zoom,
      y: v.y + (cy - fit.oy) / fit.s / v.zoom,
    };
  }
  function toCanvas(wx, wy) {
    const v = DS.view;
    return {
      x: (wx - v.x) * v.zoom * fit.s + fit.ox,
      y: (wy - v.y) * v.zoom * fit.s + fit.oy,
    };
  }
  function centerWorld() {
    return toWorld(fit.ox + DS.vw * fit.s / 2, fit.oy + DS.vh * fit.s / 2);
  }
  function eventPos(ev) {
    const r = canvas.getBoundingClientRect();
    return { x: ev.clientX - r.left, y: ev.clientY - r.top };
  }
  function snap(v) {
    if (!DS.snap || DS.grid <= 1) return v;
    return Math.round(v / DS.grid) * DS.grid;
  }

  /* ---------- 绘制 ---------- */

  function draw() {
    if (!ctx) return;
    ctx.clearRect(0, 0, cw, ch);
    ctx.fillStyle = '#14141f';
    ctx.fillRect(0, 0, cw, ch);
    if (imgReady) {
      ctx.imageSmoothingEnabled = DS.view.zoom * fit.s < 1;  /* 放大时保持硬像素 */
      ctx.drawImage(img, fit.ox, fit.oy, DS.vw * fit.s, DS.vh * fit.s);
    } else {
      ctx.fillStyle = '#2d2d44';
      ctx.fillRect(fit.ox, fit.oy, DS.vw * fit.s, DS.vh * fit.s);
      ctx.fillStyle = '#a6adc8';
      ctx.font = '12px "Microsoft YaHei UI", sans-serif';
      ctx.fillText('等待引擎渲染…', fit.ox + 12, fit.oy + 20);
    }
    if (DS.showGrid) drawGrid();
    drawSelection();
    drawMarquee();
    drawBrush();
  }

  function drawGrid() {
    const g = DS.grid;
    if (g <= 0) return;
    const step = g * DS.view.zoom * fit.s;
    /* 世界坐标是 g 的整数倍的那些线 */
    const w0 = toWorld(fit.ox, fit.oy), w1 = toWorld(cw, ch);
    ctx.save();
    ctx.strokeStyle = 'rgba(137,180,250,0.14)';
    ctx.lineWidth = 1;
    /* 缩得很小时只隐藏密集网格，原点轴仍然保留，避免缩放后失去方向感。 */
    if (step >= 4) {
      for (let x = Math.ceil(w0.x / g) * g; x <= w1.x; x += g) {
        const p = toCanvas(x, 0).x;
        ctx.beginPath(); ctx.moveTo(p, fit.oy); ctx.lineTo(p, fit.oy + DS.vh * fit.s); ctx.stroke();
      }
      for (let y = Math.ceil(w0.y / g) * g; y <= w1.y; y += g) {
        const p = toCanvas(0, y).y;
        ctx.beginPath(); ctx.moveTo(fit.ox, p); ctx.lineTo(fit.ox + DS.vw * fit.s, p); ctx.stroke();
      }
    }
    /* 世界原点:十字 + 亮一点 */
    const o = toCanvas(0, 0);
    ctx.strokeStyle = 'rgba(249,226,175,0.5)';
    ctx.beginPath();
    if (o.x >= fit.ox && o.x <= fit.ox + DS.vw * fit.s) {
      ctx.moveTo(o.x, fit.oy); ctx.lineTo(o.x, fit.oy + DS.vh * fit.s);
    }
    if (o.y >= fit.oy && o.y <= fit.oy + DS.vh * fit.s) {
      ctx.moveTo(fit.ox, o.y); ctx.lineTo(fit.ox + DS.vw * fit.s, o.y);
    }
    ctx.stroke();
    ctx.restore();
  }

  function drawSelection() {
    const ids = DS.sel;
    if (!ids.length) return;
    ctx.save();
    ctx.lineWidth = 1.5;
    ids.forEach((id, i) => {
      const o = outlineOf(id);
      if (!o) return;
      let p = { x: o.x, y: o.y, w: o.w, h: o.h };
      if (localPos && localPos[id]) {
        p = { x: localPos[id].x, y: localPos[id].y, w: o.w, h: o.h };
      }
      const a = toCanvas(p.x, p.y);
      const b = toCanvas(p.x + p.w, p.y + p.h);
      ctx.strokeStyle = (i === ids.length - 1) ? '#f9e2af' : '#89b4fa';
      ctx.strokeRect(a.x, a.y, b.x - a.x, b.y - a.y);
    });
    /* 选中的最后一个再画个角标,配合属性面板"显示最后一个" */
    const last = outlineOf(ids[ids.length - 1]);
    if (last) {
      const lp = localPos && localPos[ids[ids.length - 1]];
      const p = toCanvas(lp ? lp.x : last.x, lp ? lp.y : last.y);
      ctx.fillStyle = '#f9e2af';
      ctx.fillRect(p.x - 3, p.y - 3, 6, 6);
    }
    ctx.restore();
  }

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

  function drawBrush() {
    const t = DS.tilemap;
    if (DS.tool !== 'brush' || !t) return;
    const e = byId(t.id);
    if (!e) return;
    const tw = t.tw || 16, th = t.th || 16;
    const cols = t.cols || 0, rows = t.rows || 0;
    ctx.save();
    ctx.strokeStyle = 'rgba(148,226,213,0.85)';
    ctx.lineWidth = 1;
    const stepx = tw * DS.view.zoom * fit.s;
    const stepy = th * DS.view.zoom * fit.s;
    if (stepx > 3 && cols <= 512) {
      for (let c = 0; c <= cols; c++) {
        const p = toCanvas(e.x + c * tw, 0).x;
        ctx.beginPath();
        ctx.moveTo(p, toCanvas(0, e.y).y);
        ctx.lineTo(p, toCanvas(0, e.y + rows * th).y);
        ctx.stroke();
      }
      for (let r = 0; r <= rows; r++) {
        const p = toCanvas(0, e.y + r * th).y;
        ctx.beginPath();
        ctx.moveTo(toCanvas(e.x, 0).x, p);
        ctx.lineTo(toCanvas(e.x + cols * tw, 0).x, p);
        ctx.stroke();
      }
    }
    if (hover) {
      const a = toCanvas(e.x + hover.col * tw, e.y + hover.row * th);
      const b = toCanvas(e.x + (hover.col + 1) * tw, e.y + (hover.row + 1) * th);
      ctx.fillStyle = (DS.tile < 0 || DS.erase)
        ? 'rgba(243,139,168,0.35)' : 'rgba(249,226,175,0.35)';
      ctx.fillRect(a.x, a.y, b.x - a.x, b.y - a.y);
      ctx.strokeStyle = '#f9e2af';
      ctx.strokeRect(a.x, a.y, b.x - a.x, b.y - a.y);
    }
    ctx.restore();
  }

  /* ---------- 命中测试 ---------- */

  /* 渲染顺序:layer → order → 创建顺序(和引擎的 qsort 判据一致)。
   * 以前按"创建顺序"倒着取第一个命中的,和画面不一致 —— 点最上面的精灵
   * 可能选中它下面那个实体(outline 里的 seq 就是创建顺序)。 */
  function drawOrder() {
    return DS.outline.slice().sort((a, b) =>
      (a.layer || 0) - (b.layer || 0) ||
      (a.order || 0) - (b.order || 0) ||
      (a.seq || 0) - (b.seq || 0));
  }

  function hitTest(wx, wy) {
    const list = drawOrder();
    for (let i = list.length - 1; i >= 0; i--) {
      const o = list[i];
      if (!o.w || !o.h) continue;
      if (wx >= o.x && wx <= o.x + o.w && wy >= o.y && wy <= o.y + o.h) return o.id;
    }
    return 0;
  }

  function tileAt(wx, wy) {
    const t = DS.tilemap;
    if (!t) return null;
    const e = byId(t.id);
    if (!e) return null;
    const tw = t.tw || 16, th = t.th || 16;
    const col = Math.floor((wx - e.x) / tw);
    const row = Math.floor((wy - e.y) / th);
    /* 允许画到现有范围外一点(模型会自动扩),但不允许负数 */
    if (col < 0 || row < 0 || col > 4096 || row > 4096) return null;
    return { col, row };
  }

  /* ---------- 鼠标 ---------- */

  function wireMouse() {
    canvas.addEventListener('wheel', (ev) => {
      ev.preventDefault();
      const p = eventPos(ev);
      const before = toWorld(p.x, p.y);
      const f = ev.deltaY < 0 ? 1.15 : 1 / 1.15;
      const z = Math.max(0.05, Math.min(16, DS.view.zoom * f));
      DS.view.zoom = z;
      DS.view.on = true;
      DS.view.x = before.x - (p.x - fit.ox) / fit.s / z;
      DS.view.y = before.y - (p.y - fit.oy) / fit.s / z;
      ds('view.set', DS.view).catch(() => {});
      scheduleRender();
      draw();
      renderTopbar();
    }, { passive: false });

    canvas.addEventListener('contextmenu', (ev) => ev.preventDefault());

    canvas.addEventListener('mousedown', (ev) => {
      const p = eventPos(ev);
      const w = toWorld(p.x, p.y);
      /* 平移:中键 / 空格+左键 / 右键(非刷子时) */
      if (ev.button === 1 || (ev.button === 0 && space.down) ||
          (ev.button === 2 && DS.tool !== 'brush')) {
        drag = { mode: 'pan', cx: p.x, cy: p.y, vx: DS.view.x, vy: DS.view.y };
        canvas.style.cursor = 'grabbing';
        ev.preventDefault();
        return;
      }
      if (DS.tool === 'brush' && DS.tilemap && (ev.button === 0 || ev.button === 2)) {
        const cell = tileAt(w.x, w.y);
        if (!cell) return;
        drag = { mode: 'paint' };
        painted = {};
        paintCell(cell);
        return;
      }
      if (ev.button !== 0) return;
      const hit = hitTest(w.x, w.y);
      if (hit) {
        if (ev.shiftKey || ev.ctrlKey) select([hit], true);
        else if (!isSelected(hit)) select([hit]);
        /* 拖着已选中的实体移动 */
        const start = {};
        DS.sel.forEach((id) => {
          const e = byId(id);
          if (e) start[id] = { x: e.x, y: e.y };
        });
        drag = { mode: 'move', wx: w.x, wy: w.y, start, positions: null, moved: false };
        canvas.style.cursor = 'move';
      } else {
        marquee = { x0: p.x, y0: p.y, x1: p.x, y1: p.y, add: ev.shiftKey };
        drag = { mode: 'marquee' };
      }
      draw();
    });

    window.addEventListener('mousemove', (ev) => {
      if (!canvas) return;
      const p = eventPos(ev);
      const w = toWorld(p.x, p.y);
      if (!drag) {
        if (DS.tool === 'brush' && DS.tilemap) {
          const cell = tileAt(w.x, w.y);
          const changed = JSON.stringify(cell) !== JSON.stringify(hover);
          hover = cell;
          if (changed) draw();
        }
        return;
      }
      if (drag.mode === 'pan') {
        DS.view.x = drag.vx - (p.x - drag.cx) / fit.s / DS.view.zoom;
        DS.view.y = drag.vy - (p.y - drag.cy) / fit.s / DS.view.zoom;
        DS.view.on = true;
        ds('view.set', DS.view).catch(() => {});
        scheduleRender();
        draw();
        renderTopbar();
      } else if (drag.mode === 'move') {
        let dx = (w.x - drag.wx), dy = (w.y - drag.wy);
        drag.moved = drag.moved || Math.abs(dx) > 0.5 || Math.abs(dy) > 0.5;
        localPos = {};
        drag.positions = {};
        Object.keys(drag.start).forEach((id) => {
          const s = drag.start[id];
          const nx = snap(s.x + dx), ny = snap(s.y + dy);
          drag.positions[id] = { x: nx, y: ny };
          const o = outlineOf(parseInt(id, 10));
          localPos[id] = o
            ? { x: o.x + (nx - s.x), y: o.y + (ny - s.y) }
            : { x: nx, y: ny };
        });
        draw();
      } else if (drag.mode === 'marquee') {
        marquee.x1 = p.x;
        marquee.y1 = p.y;
        draw();
      } else if (drag.mode === 'paint' && DS.paintDrag) {
        const cell = tileAt(w.x, w.y);
        hover = cell;
        if (cell) paintCell(cell);
        else draw();
      }
    });

    window.addEventListener('mouseup', async (ev) => {
      if (!drag) return;
      const mode = drag.mode;
      const moved = drag.moved;
      const marqueeRect = marquee;
      const wasMarquee = marquee ? marquee.add : false;
      const positions = drag.positions;
      const dragRef = drag;
      drag = null;
      marquee = null;
      canvas.style.cursor = 'crosshair';
      if (mode === 'move') {
        localPos = null;
        if (moved && positions) {
          await moveSelection(positions);
        } else if (moved === false) {
          draw();
        }
      } else if (mode === 'marquee') {
        if (marqueeRect) {
          const a = toWorld(Math.min(marqueeRect.x0, marqueeRect.x1),
                            Math.min(marqueeRect.y0, marqueeRect.y1));
          const b = toWorld(Math.max(marqueeRect.x0, marqueeRect.x1),
                            Math.max(marqueeRect.y0, marqueeRect.y1));
          const ids = DS.outline.filter((o) =>
            o.x < b.x && o.x + o.w > a.x && o.y < b.y && o.y + o.h > a.y)
            .map((o) => o.id);
          select(ids, wasMarquee);
        }
      } else if (mode === 'paint' && painted && Object.keys(painted).length) {
        await flushPaint();
      } else if (mode === 'pan') {
        await renderView();
      }
      void dragRef;
      void ev;
    });

    canvas.addEventListener('mouseleave', () => {
      if (hover) { hover = null; draw(); }
    });
  }

  /* ---------- 瓦片刷子 ---------- */

  function paintCell(cell) {
    const key = cell.col + ',' + cell.row;
    if (painted[key]) return;
    painted[key] = true;
    hover = cell;
    const tile = (DS.erase) ? -1 : DS.tile;
    /* 单格立即下发:反馈快(不用等鼠标松开)。拖动时聚合成 tilemap.paint,
     * 于是整笔拖拽只占一条撤销记录。 */
    if (DS.paintDrag) {
      DS.pendingPaint = DS.pendingPaint || [];
      DS.pendingPaint.push({ col: cell.col, row: cell.row, tile });
      drawTilePreview(cell, tile);
    } else {
      ds('tilemap.set', { id: DS.tilemap.id, col: cell.col, row: cell.row, tile })
        .then(() => refresh({ noRender: false }))
        .catch((e) => log('er', '画瓦片失败:' + e.message));
    }
  }

  async function flushPaint() {
    const cells = DS.pendingPaint || [];
    DS.pendingPaint = [];
    if (!cells.length) return;
    try {
      await call('tilemap.paint', { id: DS.tilemap.id, cells }, '画瓦片');
      await refresh();
    } catch (e) { /* 已提示 */ }
  }

  /* 拖动过程中的即时反馈:直接在画布上盖一块半透明色块(不重新渲染引擎) */
  function drawTilePreview(cell, tile) {
    const t = DS.tilemap;
    const e = byId(t.id);
    const tw = t.tw || 16, th = t.th || 16;
    const a = toCanvas(e.x + cell.col * tw, e.y + cell.row * th);
    const b = toCanvas(e.x + (cell.col + 1) * tw, e.y + (cell.row + 1) * th);
    ctx.save();
    ctx.fillStyle = tile < 0 ? 'rgba(243,139,168,0.55)' : 'rgba(166,227,161,0.5)';
    ctx.fillRect(a.x, a.y, b.x - a.x, b.y - a.y);
    ctx.restore();
  }

  /* ---------- 渲染节流 ---------- */

  let renderTimer = 0;
  let renderBusy = false;
  let renderAgain = false;
  function scheduleRender() {
    if (renderTimer) return;
    renderTimer = setTimeout(async () => {
      renderTimer = 0;
      if (renderBusy) { renderAgain = true; return; }
      renderBusy = true;
      await renderView();
      renderBusy = false;
      if (renderAgain) { renderAgain = false; scheduleRender(); }
    }, 60);
  }

  return {
    init, show, draw, resize,
    drawOverlay: draw,
    centerWorld, toWorld, toCanvas,
    scheduleRender,
    get fit() { return fit; },
    /* 自检用:渲染图到底有没有读进来、画布上是不是真的有内容。
     * "窗口开着但画面是空的"只有实测像素才能发现(见 AGENTS.md 的验证习惯)。 */
    imgInfo: () => ({
      ready: imgReady, seq: imgSeq,
      w: img.naturalWidth || 0, h: img.naturalHeight || 0,
      src: String(img.getAttribute('src') || ''),
    }),
    pixelStats: () => {
      if (!ctx) return { colors: 0, nonBg: 0, sampled: 0 };
      const d = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
      const seen = new Set();
      let nonBg = 0, sampled = 0;
      const stride = Math.max(1, Math.floor(d.length / 4 / 20000)) * 4;
      for (let i = 0; i < d.length; i += stride) {
        const r = d[i], g = d[i + 1], b = d[i + 2];
        sampled++;
        if (!(r === 20 && g === 20 && b === 31)) nonBg++;
        seen.add((r << 16) | (g << 8) | b);
      }
      return { colors: seen.size, nonBg, sampled };
    },
    /* 画布上某一个点(画布 CSS 坐标)的颜色 —— 自检用:
     * "实体所在位置的那个像素到底是不是贴图"只有实测才能回答。 */
    pixelAt: (cx, cy) => {
      if (!ctx) return null;
      const dpr = window.devicePixelRatio || 1;
      const x = Math.round(cx * dpr), y = Math.round(cy * dpr);
      if (x < 0 || y < 0 || x >= canvas.width || y >= canvas.height) return null;
      const d = ctx.getImageData(x, y, 1, 1).data;
      return '#' + [d[0], d[1], d[2]].map((v) => v.toString(16).padStart(2, '0')).join('');
    },
    hasColor: (hex) => {
      if (!ctx) return false;
      const d = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
      const r = parseInt(hex.slice(1, 3), 16);
      const g = parseInt(hex.slice(3, 5), 16);
      const b = parseInt(hex.slice(5, 7), 16);
      for (let i = 0; i < d.length; i += 4) {
        if (Math.abs(d[i] - r) <= 4 && Math.abs(d[i + 1] - g) <= 4 &&
            Math.abs(d[i + 2] - b) <= 4) return true;
      }
      return false;
    },
  };
})();

window.addEventListener('DOMContentLoaded', () => {
  const c = $('view');
  if (c) Viewport.init(c);
});
