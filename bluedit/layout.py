"""bluedit.layout — 自动整理(图布局)。

思路:
  1. exec 主链从「入口」分层(entry 在最左,沿 exec 连线从左到右)——"从前到后"。
  2. 纯 value 数据源(纯文本/数字/布尔/取值/运算…无 exec 输入)从 exec 消费者的
     value 输入反向分层,穿插在 exec 列的左侧,让数据线自左向右流入。
  3. 层内用"重心排序"(barycenter)按上游连线顺序排列,尽量让线不交叉。
  4. 按列分配 x、层内分配 y,间距取该列最大节点宽/高,保证块绝不重叠。
"""

GAP_X = 70      # 列间水平间距(在列最大宽度之外再留的间隔)
GAP_Y = 40      # 行间垂直间距


def auto_layout(scene, start_ids, width_fn=None, height_fn=None):
    """整理一个场景。start_ids:入口节点 id 列表(镜头=entry,MENU=starts)。
    width_fn/height_fn:节点宽高函数(传画布的,否则用默认估算)。"""
    if not scene or not scene.nodes:
        return
    wfn = width_fn or (lambda n: _default_w(n))
    hfn = height_fn or (lambda n: _default_h(n))

    # 1) exec 分层(最长路径,entry=0)
    exec_layer = {}
    for sid in start_ids:
        _assign_exec(scene, scene.node(sid), 0.0, exec_layer)

    # 2) value 数据源分层(反向 BFS,取最左约束)
    value_layer = {}
    for n in scene.nodes:
        if n.id in exec_layer:
            base = exec_layer[n.id]
            for i in range(len(n.inputs)):
                src = scene.find_value_source(n, i)
                if src is not None and src.id not in exec_layer:
                    _assign_value(scene, src, base - 0.5, value_layer, exec_layer)

    # 3) 统一层号
    layer = {}
    for n in scene.nodes:
        if n.id in exec_layer:
            layer[n.id] = exec_layer[n.id]
        elif n.id in value_layer:
            layer[n.id] = value_layer[n.id]
        else:
            layer[n.id] = 0.0          # 孤立节点放最左列

    # 4) 层 → 列
    cols = sorted(set(layer.values()))
    col_map = {v: i for i, v in enumerate(cols)}
    groups = {}
    for n in scene.nodes:
        groups.setdefault(col_map[layer[n.id]], []).append(n.id)

    # 5) 层内排序(重心),从最左列逐列处理
    order = {}
    for col in range(len(cols)):
        nids = groups[col]
        groups[col] = _order(scene, nids, order)
        for i, nid in enumerate(groups[col]):
            order[nid] = i

    # 6) 分配坐标
    widths = {n.id: wfn(scene.node(n.id)) for n in scene.nodes}
    heights = {n.id: hfn(scene.node(n.id)) for n in scene.nodes}
    xpos = {}
    cx = 0
    for col in range(len(cols)):
        mxw = max(widths[nid] for nid in groups[col])
        xpos[col] = cx
        cx += mxw + GAP_X
    for col in range(len(cols)):
        y = 0
        for nid in groups[col]:
            n = scene.node(nid)
            n.x = xpos[col]
            n.y = y
            y += heights[nid] + GAP_Y


def _assign_exec(scene, node, depth, exec_layer, guard=0):
    """沿 exec 输出递归分层(最长路径;防环/防深)。"""
    if node is None or guard > 800:
        return
    if depth > exec_layer.get(node.id, -1e9):
        exec_layer[node.id] = depth
    else:
        return
    for i in range(len(node.outputs)):
        if node.outputs[i].kind != 'exec':
            continue
        nxt = scene.find_exec_target(node, i)
        if nxt is not None:
            _assign_exec(scene, nxt, depth + 1, exec_layer, guard + 1)


def _assign_value(scene, node, depth, value_layer, exec_layer, guard=0):
    """value 数据源反向分层(沿 value 输入继续向左;取更左的约束)。"""
    if node is None or guard > 800:
        return
    if depth < value_layer.get(node.id, 1e9):
        value_layer[node.id] = depth
    else:
        return
    for i in range(len(node.inputs)):
        src = scene.find_value_source(node, i)
        if src is not None and src.id not in exec_layer:
            _assign_value(scene, src, depth - 0.5, value_layer, exec_layer, guard + 1)


def _exec_parents(scene, node):
    """连到节点 exec 输入的父节点(exec 输入统一是第 0 输入)。"""
    out = []
    for l in scene.links:
        if l.to_node == node.id and l.to_pin == 0:
            p = scene.node(l.from_node)
            if p is not None:
                out.append(p)
    return out


def _order(scene, nids, order):
    """层内排序:按上游(exec 父 / value 来源)在左侧各列内的平均序号(barycenter)。"""
    def bary(nid):
        n = scene.node(nid)
        refs = []
        for p in _exec_parents(scene, n):
            if p.id in order:
                refs.append(order[p.id])
        for i in range(len(n.inputs)):
            src = scene.find_value_source(n, i)
            if src is not None and src.id in order:
                refs.append(order[src.id])
        if not refs:
            return nid            # 无上游引用:保持创建顺序
        return sum(refs) / len(refs)
    return sorted(nids, key=bary)


def _default_w(n):
    if getattr(n, "type", -1) == 26:            # N_TEXTLIT 纯文本
        ln = len(n.lit_text or "")
        return min(max(230, 60 + ln * 8), 460)
    return 230


def _default_h(n):
    from .model import N_LONG, N_CHOICE
    rows = max(len(n.inputs), len(n.outputs), 1)
    h = 34 + rows * 26 + 8
    if getattr(n, "type", -1) == N_LONG:
        h += len(n.lines) * 22
    elif getattr(n, "type", -1) == N_CHOICE:
        h += max(0, len(n.options) - 1) * 6
    return h
