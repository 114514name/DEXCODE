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

  /* ---------- 重绘合帧 / 背景缓存 ---------- */

  /* 一次 mousemove 画一次画布,会被 125~1000Hz 的鼠标直接拖垮(实测就是用户报的
   * "拖框线掉帧")。所有热路径都改成"标脏 + 每帧画一次"。 */
  let rafPending = 0;
  function requestDraw() {
    if (rafPending) return;
    rafPending = requestAnimationFrame(() => {
      rafPending = 0;
      draw();
    });
  }

  /* 背景(预览图 + 网格)画进一张离屏画布缓存起来:
   * 拖动实体时视图没变,每帧就只剩一次 drawImage,而不是重画整张图 + 几十条网格线。 */
  let bgCache = null, bgKey = '';
  function drawGridInto(c, w, h) {
    const g = DS.grid;
    if (g <= 0) return;
    const step = g * DS.view.zoom * fit.s;
    const w0 = toWorld(fit.ox, fit.oy), w1 = toWorld(fit.ox + w, fit.oy + h);
    c.save();
    c.strokeStyle = 'rgba(137,180,250,0.14)';
    c.lineWidth = 1;
    if (step >= 4) {
      for (let x = Math.ceil(w0.x / g) * g; x <= w1.x; x += g) {
        const px = toCanvas(x, 0).x - fit.ox;
        c.beginPath(); c.moveTo(px, 0); c.lineTo(px, h); c.stroke();
      }
      for (let y = Math.ceil(w0.y / g) * g; y <= w1.y; y += g) {
        const py = toCanvas(0, y).y - fit.oy;
        c.beginPath(); c.moveTo(0, py); c.lineTo(w, py); c.stroke();
      }
    }
    const ox = toCanvas(0, 0).x - fit.ox, oy = toCanvas(0, 0).y - fit.oy;
    c.strokeStyle = 'rgba(249,226,175,0.5)';
    c.beginPath();
    if (ox >= 0 && ox <= w) { c.moveTo(ox, 0); c.lineTo(ox, h); }
    if (oy >= 0 && oy <= h) { c.moveTo(0, oy); c.lineTo(w, oy); }
    c.stroke();
    c.restore();
  }
  function bgCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(DS.vw * fit.s));
    const h = Math.max(1, Math.round(DS.vh * fit.s));
    const key = [imgSeq, fit.s, fit.ox, fit.oy, DS.vw, DS.vh, dpr, DS.showGrid, DS.grid,
                 DS.view.x, DS.view.y, DS.view.zoom].join('|');
    if (bgCache && bgKey === key) return bgCache;
    if (!bgCache) bgCache = document.createElement('canvas');
    bgCache.width = Math.round(w * dpr);
    bgCache.height = Math.round(h * dpr);
    const c = bgCache.getContext('2d');
    c.setTransform(dpr, 0, 0, dpr, 0, 0);
    c.clearRect(0, 0, w, h);
    if (imgReady) {
      c.imageSmoothingEnabled = DS.view.zoom * fit.s < 1;   /* 放大时保持硬像素 */
      c.drawImage(img, 0, 0, w, h);
    } else {
      c.fillStyle = '#2d2d44';
      c.fillRect(0, 0, w, h);
      c.fillStyle = '#a6adc8';
      c.font = '12px "Microsoft YaHei UI", sans-serif';
      c.fillText('等待引擎渲染…', 12, 20);
    }
    if (DS.showGrid) drawGridInto(c, w, h);
    bgKey = key;
    return bgCache;
  }

  /* ---------- 跟手的拖动预览 ---------- */

  /* 引擎那张预览图是"文件里的位置",而框线是跟手的 —— 不处理的话用户看到的就是
   * "图片比框线慢半拍 / 有重影 / 掉帧"。做法(一般编辑器都是这么干的):
   *   ① 按下时让引擎把被拖的精灵**藏起来**(scene.render {hide:[…]});
   *   ② 拖动过程里**一次引擎渲染都不做**,画布上自己画一份跟手的贴图;
   *   ③ 松手时一次写回模型(一条撤销),再渲染一帧。
   * 代价:贴图四角的公式在前端也有一份 —— 所以页面自测里有一条
   * "预览四角与 C 给的 corners 一致"专门钉住这两份不许漂。 */
  let ghost = null;        /* { ids:[], src:{id:{…}}, tf:{id:{x,y,sx,sy,rot}} } */
  const texCache = {};
  const tintCache = {};

  function texImage(path) {
    if (!path) return null;
    if (!texCache[path]) {
      const im = new Image();
      /* 项目根映射成 dexstudio-proj.local;路径按段编码(中文/空格/# 都能取) */
      im.src = 'https://dexstudio-proj.local/' + path.split('/')
        .map(encodeURIComponent).join('/');
      im.onload = () => requestDraw();      /* 图到了再画一次,免得第一帧是空的 */
      texCache[path] = im;
    }
    return texCache[path];
  }

  /* 染色:引擎是"贴图 × 顶点色"(dg_draw.c 的 ps_main),这里用 multiply 复制同一件事,
   * 并且保 alpha(否则透明区会被染成一块方色)。 */
  function tintedImage(im, tint) {
    if (tint === -1 || tint === 0xFFFFFFFF || !im.naturalWidth) return im;
    const key = (im.src || '') + '#' + tint;
    if (tintCache[key]) return tintCache[key];
    const c = document.createElement('canvas');
    c.width = im.naturalWidth;
    c.height = im.naturalHeight;
    const cx = c.getContext('2d');
    cx.drawImage(im, 0, 0);
    const a = (tint >>> 24) & 0xFF, r = (tint >>> 16) & 0xFF;
    const g = (tint >>> 8) & 0xFF, b = tint & 0xFF;
    cx.globalCompositeOperation = 'multiply';
    cx.fillStyle = 'rgba(' + r + ',' + g + ',' + b + ',' + (a / 255) + ')';
    cx.fillRect(0, 0, c.width, c.height);
    cx.globalCompositeOperation = 'destination-in';
    cx.drawImage(im, 0, 0);
    tintCache[key] = c;
    return c;
  }

  /* 贴图四角(世界坐标)。**必须与引擎 dg_scene.c 的 sprite 绘制逐字相同**:
   *   轴心 = 实体世界坐标;未旋转左上 = 轴心 + (-sw·px·kx, -sh·py·ky);
   *   四角 = 轴心 + R(rot)·(偏移 + (u,v)),(u,v) ∈ [0,sw·kx]×[0,sh·ky]
   * rot 单位是度,屏幕上顺时针为正。 */
  function spriteQuad(rec, wx, wy, kx, ky, rot) {
    const rad = rot * Math.PI / 180, co = Math.cos(rad), si = Math.sin(rad);
    const ox = -rec.sw * rec.px * kx, oy = -rec.sh * rec.py * ky;
    const uu = [0, rec.sw * kx, rec.sw * kx, 0];
    const vv = [0, 0, rec.sh * ky, rec.sh * ky];
    const out = [];
    for (let q = 0; q < 4; q++) {
      const lx = ox + uu[q], ly = oy + vv[q];
      out.push({ x: wx + lx * co - ly * si, y: wy + lx * si + ly * co });
    }
    return out;
  }

  /* 选中框的四角(世界坐标):优先用 C 给的 corners(= 引擎那份公式算出来的),
   * 只有拿不到时才退回 AABB。 */
  function cornersOf(o) {
    if (o && o.corners && o.corners.length === 8) {
      const p = [];
      for (let i = 0; i < 4; i++) p.push({ x: o.corners[i * 2], y: o.corners[i * 2 + 1] });
      return p;
    }
    const x = o ? o.x : 0, y = o ? o.y : 0, w = o ? o.w : 0, h = o ? o.h : 0;
    return [{ x, y }, { x: x + w, y }, { x: x + w, y: y + h }, { x, y: y + h }];
  }

  /* 拖动中该画成什么样:跟手的那份变换(不写模型) */
  function ghostTf(id) {
    return ghost && ghost.tf[id] ? ghost.tf[id] : null;
  }
  function currentQuadWorld(o) {
    const g = ghostTf(o.id);
    const rec = ghost && ghost.src[o.id];
    if (!g || !rec) return cornersOf(o);
    if (drag && drag.mode === 'move') {
      /* 平移是纯平移:把 C 给的四角搬过去就够了(不做第二次换算) */
      const dx = g.x - rec.x0, dy = g.y - rec.y0;
      return cornersOf(o).map((p) => ({ x: p.x + dx, y: p.y + dy }));
    }
    if (rec.sw > 0 && rec.sh > 0) return spriteQuad(rec, g.x, g.y, g.sx, g.sy, g.rot);
    return cornersOf(o);
  }

  function drawGhosts() {
    if (!ghost || !drag) return;
    Object.keys(ghost.src).forEach((key) => {
      const rec = ghost.src[key];
      const g = ghost.tf[key];
      if (!rec || !g || !rec.img || !rec.img.naturalWidth) return;
      const q = spriteQuad(rec, g.x, g.y, g.sx, g.sy, g.rot).map((p) => toCanvas(p.x, p.y));
      let p0 = q[0], p1 = q[1], p3 = q[3];
      /* 翻转:引擎是交换 uv,也就是"内容在同一个矩形里镜像" —— 这里交换映射的边 */
      if (rec.flip & 1) { const t = p0; p0 = p1; p1 = t; }
      if (rec.flip & 2) { const t = p0; p0 = p3; p3 = t; }
      const sw = rec.sw, sh = rec.sh;
      const a = (p1.x - p0.x) / sw, b = (p1.y - p0.y) / sw;
      const c = (p3.x - p0.x) / sh, d = (p3.y - p0.y) / sh;
      ctx.save();
      ctx.transform(a, b, c, d, p0.x, p0.y);
      ctx.imageSmoothingEnabled = DS.view.zoom * fit.s < 1;
      ctx.drawImage(tintedImage(rec.img, rec.tint), rec.sx, rec.sy, sw, sh, 0, 0, sw, sh);
      ctx.restore();
    });
  }

  /* ---------- 绘制 ---------- */

  function draw() {
    if (!ctx) return;
    ctx.clearRect(0, 0, cw, ch);
    ctx.fillStyle = '#14141f';
    ctx.fillRect(0, 0, cw, ch);
    {
      const w = Math.max(1, Math.round(DS.vw * fit.s));
      const h = Math.max(1, Math.round(DS.vh * fit.s));
      ctx.drawImage(bgCanvas(), fit.ox, fit.oy, w, h);
    }
    drawGhosts();          /* 跟手的贴图(拖动中才有) */
    drawSelection();
    drawMarquee();
    drawBrush();
  }

  /* 画一个四角多边形(世界坐标) */
  function strokeQuadWorld(pts, style, dash) {
    ctx.save();
    ctx.strokeStyle = style;
    ctx.setLineDash(dash || []);
    ctx.beginPath();
    pts.forEach((p, i) => {
      const c = toCanvas(p.x, p.y);
      if (i === 0) ctx.moveTo(c.x, c.y);
      else ctx.lineTo(c.x, c.y);
    });
    ctx.closePath();
    ctx.stroke();
    ctx.restore();
  }

  /* 手柄:四角 + 四边中点 + 一个旋转手柄(在"上边"外侧)。
   * 位置全部由**四角**推出来(角是 C 给的、和引擎同源),前端不再算第二遍变换。 */
  const HANDLES = [
    { id: 'nw', u: -1, v: -1 }, { id: 'n', u: 0, v: -1 }, { id: 'ne', u: 1, v: -1 },
    { id: 'e', u: 1, v: 0 }, { id: 'se', u: 1, v: 1 }, { id: 's', u: 0, v: 1 },
    { id: 'sw', u: -1, v: 1 }, { id: 'w', u: -1, v: 0 },
  ];

  function handlePoints(o) {
    const q = cornersOf(o);
    const c = q.map((p) => toCanvas(p.x, p.y));
    const mid = (a, b) => ({ x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 });
    const corners = {
      nw: c[0], ne: c[1], se: c[2], sw: c[3],
      n: mid(c[0], c[1]), e: mid(c[1], c[2]),
      s: mid(c[3], c[2]), w: mid(c[0], c[3]),
    };
    /* "上"方向 = 上边中点 - 下边中点 */
    let ux = corners.n.x - corners.s.x, uy = corners.n.y - corners.s.y;
    const ul = Math.hypot(ux, uy) || 1;
    ux /= ul; uy /= ul;
    corners.rotate = { x: corners.n.x + ux * 26, y: corners.n.y + uy * 26 };
    return corners;
  }

  function primaryOutline() {
    const id = DS.sel[DS.sel.length - 1];
    const o = id ? outlineOf(id) : null;
    /* 手柄只对**精灵**(有 corners)给:空实体/相机没有"拉伸"的语义 */
    return (o && o.corners && o.corners.length === 8) ? o : null;
  }

  function drawHandles(o) {
    const h = handlePoints(o);
    ctx.save();
    HANDLES.forEach((k) => {
      const p = h[k.id];
      ctx.fillStyle = '#1e1e2e';
      ctx.strokeStyle = '#f9e2af';
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.rect(p.x - 4, p.y - 4, 8, 8);
      ctx.fill();
      ctx.stroke();
    });
    /* 旋转手柄:连一根短线 + 一个圆点 */
    ctx.strokeStyle = '#f9e2af';
    ctx.beginPath();
    ctx.moveTo(h.n.x, h.n.y);
    ctx.lineTo(h.rotate.x, h.rotate.y);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(h.rotate.x, h.rotate.y, 5, 0, Math.PI * 2);
    ctx.fillStyle = '#1e1e2e';
    ctx.fill();
    ctx.stroke();
    /* 轴心(旋转/缩放都是围着它)—— 画出来用户才知道会怎么变 */
    const pv = o.pivot_x != null
      ? toCanvas(o.pivot_x, o.pivot_y)
      : toCanvas(o.x + o.w / 2, o.y + o.h / 2);
    ctx.strokeStyle = '#f38ba8';
    ctx.beginPath();
    ctx.arc(pv.x, pv.y, 3.5, 0, Math.PI * 2);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(pv.x - 6, pv.y); ctx.lineTo(pv.x + 6, pv.y);
    ctx.moveTo(pv.x, pv.y - 6); ctx.lineTo(pv.x, pv.y + 6);
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
      const primary = (i === ids.length - 1);
      /* 拖动中就用跟手的那份四角画 */
      const q = currentQuadWorld(o);
      strokeQuadWorld(q, primary ? '#f9e2af' : '#89b4fa');
      /* 碰撞盒(逻辑用的那个)单独画虚线:和"围着图的实线"区分开,
       * 又不用把两者混成一个框(用户报过"框线还是模板的 32×32")。 */
      if (o.collider) {
        const cb = o.collider;
        strokeQuadWorld([{ x: cb[0], y: cb[1] }, { x: cb[0] + cb[2], y: cb[1] },
                         { x: cb[0] + cb[2], y: cb[1] + cb[3] },
                         { x: cb[0], y: cb[1] + cb[3] }], 'rgba(148,226,213,0.75)', [4, 3]);
      }
      if (primary && !(drag && drag.mode === 'rotate')) drawHandles(o);
    });
    /* 选中的最后一个再画个角标,配合属性面板"显示最后一个" */
    const last = outlineOf(ids[ids.length - 1]);
    if (last) {
      const q = currentQuadWorld(last);
      const p = toCanvas(q[0].x, q[0].y);
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

  /* 手柄命中:画布坐标 → 哪个手柄(带 6px 容差) */
  function hitHandle(cx, cy) {
    const o = primaryOutline();
    if (!o) return null;
    const h = handlePoints(o);
    if (Math.hypot(cx - h.rotate.x, cy - h.rotate.y) <= 8) return { id: 'rotate' };
    for (let i = 0; i < HANDLES.length; i++) {
      const p = h[HANDLES[i].id];
      if (Math.abs(cx - p.x) <= 6 && Math.abs(cy - p.y) <= 6) return HANDLES[i];
    }
    return null;
  }

  const CURSORS = {
    nw: 'nwse-resize', se: 'nwse-resize', ne: 'nesw-resize', sw: 'nesw-resize',
    n: 'ns-resize', s: 'ns-resize', w: 'ew-resize', e: 'ew-resize',
    rotate: 'grab',
  };

  /* 拖动开始:把被拖的精灵在**引擎那份预览图**里藏掉,并准备"跟手的那份"。
   * 为什么要 src/tf 两份记录:src 是素材(贴图 + 源矩形 + 轴心 + 按下时的变换,
   * 手势期间不变),tf 是拖动中的临时变换(每帧变,不写模型)。
   * 注意 `entity.list` 里的 comps 是**组件名数组**,拿字段值必须 `entity.get`。 */
  async function beginGhost(ids) {
    const src = {}, tf = {};
    for (let k = 0; k < ids.length; k++) {
      const id = ids[k];
      const o = outlineOf(id);
      if (!o) continue;
      let full = null;
      try { full = await ds('entity.get', { id }); } catch (e) { continue; }
      const comps = (full && full.comps) || {};
      const sp = comps.sprite;
      const tr = comps.transform || {};
      const kx0 = tr.sx == null ? 1 : tr.sx;
      const ky0 = tr.sy == null ? 1 : tr.sy;
      const rec = {
        x0: tr.x || 0, y0: tr.y || 0,
        sx0: kx0, sy0: ky0, rot0: tr.rot || 0,
        px: 0.5, py: 0.5, sx: 0, sy: 0, flip: 0, tint: -1, img: null,
        sw: Math.abs(kx0) > 0 ? (o.rw || o.w || 16) / Math.abs(kx0) : (o.w || 16),
        sh: Math.abs(ky0) > 0 ? (o.rh || o.h || 16) / Math.abs(ky0) : (o.h || 16),
      };
      if (sp) {
        rec.px = sp.px == null ? 0.5 : sp.px;
        rec.py = sp.py == null ? 0.5 : sp.py;
        rec.sx = sp.sx || 0;
        rec.sy = sp.sy || 0;
        rec.sw = sp.sw > 0 ? sp.sw : rec.sw;
        rec.sh = sp.sh > 0 ? sp.sh : rec.sh;
        rec.flip = sp.flip || 0;
        rec.tint = sp.tint == null ? -1 : sp.tint;
        rec.img = texImage(sp.tex_path);
      }
      if (!(rec.sw > 0) || !(rec.sh > 0)) continue;
      src[id] = rec;
      tf[id] = { x: rec.x0, y: rec.y0, sx: rec.sx0, sy: rec.sy0, rot: rec.rot0 };
    }
    ghost = { ids: ids.slice(), src, tf };
    /* 让引擎这一帧**不画**这些精灵(否则它的旧位置和这里的跟手版会同屏重影) */
    try {
      const r = await ds('scene.render', { hide: ids });
      await Viewport.show(r.seq);
    } catch (e) { /* 渲染失败就退回旧画面,不影响拖动 */ }
    return ghost;
  }

  /* "按下时"的那份变换(缩放的定格点、旋转的起始角度都靠它) */
  function ghostBefore(id) {
    const rec = ghost && ghost.src[id];
    return rec
      ? { x: rec.x0, y: rec.y0, sx: rec.sx0, sy: rec.sy0, rot: rec.rot0 }
      : null;
  }

  function endGhost() {
    ghost = null;
  }

  /* 松手:一次写回(**一条撤销记录**),然后重新渲染一帧(这次不再隐藏)。
   * 拖动过程里一次模型写、一次引擎渲染都没有 —— 这正是"不卡"的另一半。 */
  async function commitTransform(mode, tf, before, moved) {
    const id = DS.sel[DS.sel.length - 1];
    const items = [];
    const push = (field, value) => items.push({ id, comp: 'transform', field, value });
    if (id && tf && before && moved) {
      if (Math.abs(tf.x - before.x) > 0.0005) push('x', tf.x);
      if (Math.abs(tf.y - before.y) > 0.0005) push('y', tf.y);
      if (Math.abs(tf.sx - before.sx) > 0.0005) push('sx', tf.sx);
      if (Math.abs(tf.sy - before.sy) > 0.0005) push('sy', tf.sy);
      if (Math.abs(tf.rot - before.rot) > 0.0005) push('rot', tf.rot);
    }
    endGhost();
    if (!items.length) {           /* 只是点了一下手柄:把画面还原就行 */
      await renderView();
      return;
    }
    try {
      await call('comp.set_many', { items },
                 mode === 'rotate' ? '旋转实体' : '缩放实体');
      await refresh({ noRender: false });
      log('dim', (mode === 'rotate' ? '旋转' : '缩放') + ' '
          + items.map((it) => it.field + '=' + it.value).join(', '));
    } catch (e) {
      await renderView();          /* 失败也要让画面回到模型的真实状态 */
    }
  }

  /* 拉伸/旋转的拖动计算**定义在 wireMouse 外面**:页面的自检(debugStartScale)
   * 也要走同一份实现 —— 两份公式一定会漂。 */
  let scaleDragTo = null;
  let rotateDragTo = null;

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
      scheduleRender();          /* 渲染那一拍顺带把 view.set 发出去(见 scheduleRender) */
      requestDraw();
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
      /* ① 先看手柄(它在实体外面,不能等到实体命中) */
      const hd = hitHandle(p.x, p.y);
      if (hd) {
        const o = primaryOutline();
        drag = {
          mode: hd.id === 'rotate' ? 'rotate' : 'scale',
          handle: hd,
          outline: o,
          moved: false,
        };
        const id = DS.sel[DS.sel.length - 1];
        /* 按下时的变换由 beginGhost 从 entity.get 里取(comps 在 entity.list 里
         * 只是名字数组,不能当字段表用);异步填好之前拖动不会开始算。 */
        drag.ready = beginGhost(DS.sel).then(() => { drag.before = ghostBefore(id); });
        canvas.style.cursor = hd.id === 'rotate' ? 'grabbing' : CURSORS[hd.id];
        ev.preventDefault();
        return;
      }
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
        /* 移动也要"跟手的那一份":引擎那份先藏起来,画布上自己画 */
        beginGhost(DS.sel);
      } else {
        marquee = { x0: p.x, y0: p.y, x1: p.x, y1: p.y, add: ev.shiftKey };
        drag = { mode: 'marquee' };
      }
      requestDraw();
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
          if (changed) requestDraw();
          return;
        }
        /* 悬停手柄给个光标提示(用户才知道能拉) */
        const hd = DS.tool === 'select' ? hitHandle(p.x, p.y) : null;
        canvas.style.cursor = hd ? CURSORS[hd.id] : 'crosshair';
        return;
      }
      if (drag.mode === 'pan') {
        DS.view.x = drag.vx - (p.x - drag.cx) / fit.s / DS.view.zoom;
        DS.view.y = drag.vy - (p.y - drag.cy) / fit.s / DS.view.zoom;
        DS.view.on = true;
        scheduleRender();          /* 节流的那一拍顺带发 view.set */
        requestDraw();
        renderTopbar();
      } else if (drag.mode === 'move') {
        const dx = (w.x - drag.wx), dy = (w.y - drag.wy);
        drag.moved = drag.moved || Math.abs(dx) > 0.5 || Math.abs(dy) > 0.5;
        drag.positions = {};
        Object.keys(drag.start).forEach((id) => {
          const s = drag.start[id];
          const nx = snap(s.x + dx), ny = snap(s.y + dy);
          drag.positions[id] = { x: nx, y: ny };
          if (ghost && ghost.tf[id]) {
            ghost.tf[id].x = nx;
            ghost.tf[id].y = ny;
          }
        });
        requestDraw();
      } else if (drag.mode === 'scale') {
        scaleDragTo(w, ev.shiftKey);
        requestDraw();
      } else if (drag.mode === 'rotate') {
        rotateDragTo(w, ev.shiftKey);
        requestDraw();
      } else if (drag.mode === 'marquee') {
        marquee.x1 = p.x;
        marquee.y1 = p.y;
        requestDraw();
      } else if (drag.mode === 'paint' && DS.paintDrag) {
        const cell = tileAt(w.x, w.y);
        hover = cell;
        if (cell) paintCell(cell);
        else requestDraw();
      }
    });

    /* 拉伸:被拖的角/边**对面的那条边固定不动**(和一般编辑器一样)。
     * 全部在精灵自己的**未旋转**坐标系里算:把指针转回 -rot,得到局部坐标,
     * 于是"往右拉变宽"这类判断不受旋转影响。
     * 轴心跟着一起算:x/y 不是随便改的 —— 定格点不动 + 轴心在新矩形里的相对
     * 位置不变(rw·px) ⇒ 世界位置 = 定格点 + R(rot)·(定格点到新轴心的偏移)。 */
    scaleDragTo = (w, shift) => {
      const o = drag.outline;
      const id = DS.sel[DS.sel.length - 1];
      const rec = ghost && ghost.src[id];
      const g = ghost && ghost.tf[id];
      if (!o || !rec || !g || !drag.before) return;
      const hd = drag.handle;
      const rot = drag.before.rot;
      const rad = rot * Math.PI / 180, co = Math.cos(rad), si = Math.sin(rad);
      /* 世界 → 局部(以**按下时的轴心**为原点,去掉旋转) */
      let px = w.x, py = w.y;
      if (DS.snap && rot === 0) { px = snap(px); py = snap(py); }
      const dx = px - drag.before.x, dy = py - drag.before.y;
      const lx = dx * co + dy * si;
      const ly = -dx * si + dy * co;
      /* 按下时的矩形(局部) */
      const w0 = rec.sw * drag.before.sx, h0 = rec.sh * drag.before.sy;
      const ox = -w0 * rec.px, oy = -h0 * rec.py;
      /* 定格点(局部):u=-1 → 左边;u=1 → 右边;u=0 → 不动这一轴 */
      const fx = hd.u === -1 ? ox : (hd.u === 1 ? ox + w0 : 0);
      const fy = hd.v === -1 ? oy : (hd.v === 1 ? oy + h0 : 0);
      const minW = Math.max(1, rec.sw * 0.02), minH = Math.max(1, rec.sh * 0.02);
      let nw = hd.u === 0 ? w0 : Math.max(minW, Math.abs(lx - fx));
      let nh = hd.v === 0 ? h0 : Math.max(minH, Math.abs(ly - fy));
      let kx = nw / rec.sw, ky = nh / rec.sh;
      if (shift) {
        /* 等比:按"变化更大的那一轴"统一(Shift 拖就是保持长宽比) */
        const k = Math.abs(kx - drag.before.sx) >= Math.abs(ky - drag.before.sy) ? kx : ky;
        kx = ky = Math.max(0.02, k);
        nw = rec.sw * kx;
        nh = rec.sh * ky;
      }
      kx = Math.round(kx * 1000) / 1000;
      ky = Math.round(ky * 1000) / 1000;
      nw = rec.sw * kx;
      nh = rec.sh * ky;
      /* 定格点在世界里的位置(按下时的轴心 + R·定格点) */
      const fwx = drag.before.x + fx * co - fy * si;
      const fwy = drag.before.y + fx * si + fy * co;
      /* 新轴心相对定格点的偏移 = 新矩形里轴心的位置 */
      const au = (hd.u === -1 ? nw * rec.px : (hd.u === 1 ? -nw * (1 - rec.px) : -fx));
      const av = (hd.v === -1 ? nh * rec.py : (hd.v === 1 ? -nh * (1 - rec.py) : -fy));
      const nwx = fwx + au * co - av * si;
      const nwy = fwy + au * si + av * co;
      if (Math.abs(nwx - drag.before.x) > 0.01 || Math.abs(nwy - drag.before.y) > 0.01)
        drag.moved = true;
      if (kx !== drag.before.sx || ky !== drag.before.sy) drag.moved = true;
      g.sx = kx;
      g.sy = ky;
      g.x = Math.round(nwx * 1000) / 1000;
      g.y = Math.round(nwy * 1000) / 1000;
    };

    /* 旋转:指针相对**轴心**的角度(屏幕上顺时针为正,和引擎的 rot 一致);
     * 按住 Shift 吸附到 15°。 */
    rotateDragTo = (w, shift) => {
      const id = DS.sel[DS.sel.length - 1];
      const g = ghost && ghost.tf[id];
      if (!g || !drag.before) return;
      const dx = w.x - drag.before.x, dy = w.y - drag.before.y;
      if (Math.hypot(dx, dy) < 1) return;
      let deg = Math.atan2(dy, dx) * 180 / Math.PI + 90;
      if (shift) deg = Math.round(deg / 15) * 15;
      deg = Math.round(deg * 10) / 10;
      if (Math.abs(deg - drag.before.rot) > 0.05) drag.moved = true;
      g.rot = deg;
    };

    window.addEventListener('mouseup', async (ev) => {
      if (!drag) return;
      const mode = drag.mode;
      const moved = drag.moved;
      const marqueeRect = marquee;
      const wasMarquee = marquee ? marquee.add : false;
      const positions = drag.positions;
      const dragRef = drag;
      const tf = ghost && ghost.tf[DS.sel[DS.sel.length - 1]];
      const before = drag.before;
      drag = null;
      marquee = null;
      canvas.style.cursor = 'crosshair';
      if (mode === 'move') {
        if (moved && positions) await moveSelection(positions);
        else if (moved === false) requestDraw();
        endGhost();
      } else if (mode === 'scale' || mode === 'rotate') {
        await commitTransform(mode, tf, before, moved);
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
      /* 编辑器视图顺手同步给模型(拖动/滚轮时每帧发一条太浪费 —— 60ms 一次足够,
       * 而且渲染用的视图是 scene.render 的参数,画面不受影响)。 */
      ds('view.set', DS.view).catch(() => {});
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
    /* 自检用:手柄(缩放手柄 8 个 + 旋转手柄)在画布上的位置;没有精灵选中时 null。 */
    handleInfo: () => {
      const o = primaryOutline();
      if (!o) return null;
      const h = handlePoints(o);
      const out = {};
      Object.keys(h).forEach((k) => { out[k] = { x: h[k].x, y: h[k].y }; });
      return out;
    },
    /* 自检用:某个实体"C 给的四角"与"预览用的四角"—— 两者必须一致,
     * 否则拖动的跟手预览会和松手后的框线/图像对不上。 */
    quadOf: (id) => {
      const o = outlineOf(id);
      if (!o) return null;
      return {
        corners: cornersOf(o),
        preview: drag ? currentQuadWorld(o) : null,
        ghost: ghost ? ghost.tf[id] || null : null,
      };
    },
    /* 自检用:拖动中的"跟手贴图"是否真的画上去了(有没有贴图/图到没到) */
    ghostInfo: () => {
      if (!ghost) return null;
      const out = {};
      Object.keys(ghost.src).forEach((k) => {
        const r = ghost.src[k];
        out[k] = { hasTex: !!r.img, ready: !!(r.img && r.img.naturalWidth), tf: ghost.tf[k] };
      });
      return out;
    },
    /* 提前把贴图解码好(选中实体时调用):拖动第一帧就有图,不会先空一下 */
    prewarm: (path) => { texImage(path); },
    /* 自检用:贴图解码好了没 */
    texReady: (path) => {
      const im = texCache[path];
      return !!(im && im.naturalWidth);
    },
    /* 自检可以直接喂一个拖动:派发事件太脆,这里直接调内部函数 */    debugStartScale: async (handleId) => {
      const o = primaryOutline();
      const id = DS.sel[DS.sel.length - 1];
      if (!o || !id) return null;
      const hd = HANDLES.filter((k) => k.id === handleId)[0];
      if (!hd) return null;
      drag = { mode: 'scale', handle: hd, outline: o, moved: false };
      await beginGhost(DS.sel);
      drag.before = ghostBefore(id);
      return drag.before;
    },
    debugStartRotate: async () => {
      const o = primaryOutline();
      const id = DS.sel[DS.sel.length - 1];
      if (!o || !id) return null;
      drag = { mode: 'rotate', handle: { id: 'rotate' }, outline: o, moved: false };
      await beginGhost(DS.sel);
      drag.before = ghostBefore(id);
      return drag.before;
    },
    debugDragTo: (wx, wy, shift) => {
      if (!drag) return null;
      if (drag.mode === 'scale') scaleDragTo({ x: wx, y: wy }, !!shift);
      else if (drag.mode === 'rotate') rotateDragTo({ x: wx, y: wy }, !!shift);
      else return null;
      requestDraw();
      return ghost ? ghost.tf[DS.sel[DS.sel.length - 1]] : null;
    },
    debugEndDrag: async () => {
      if (!drag) return null;
      const mode = drag.mode, moved = drag.moved, before = drag.before;
      const tf = ghost && ghost.tf[DS.sel[DS.sel.length - 1]];
      drag = null;
      await commitTransform(mode, tf, before, moved);
      return { mode, moved, tf, before };
    },
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
