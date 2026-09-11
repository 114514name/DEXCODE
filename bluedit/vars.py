"""bluedit.vars — 变量统计与作用域处理。

变量由编辑器实时统计(扫描所有镜头的 DEFVAR/SETVAR/GETVAR 节点 + value 引脚引用),
供下拉选择。导出时:全局变量用原名,局部变量加场景前缀避免跨镜头冲突。
"""

from .model import (N_DEFVAR, N_SETVAR, N_GETVAR, Var)


def collect_vars(project):
    """扫描所有镜头/菜单节点,统计变量列表(按名字去重)。"""
    result = {}
    for sc in project.scenes:
        for n in sc.nodes:
            if n.type == N_DEFVAR:
                v = result.get(n.var_name)
                if v is None:
                    result[n.var_name] = Var(n.var_name, n.var_type,
                                             n.scope, n.var_value)
                else:
                    v.type = n.var_type
                    v.scope = n.scope
            elif n.type in (N_SETVAR, N_GETVAR):
                if n.var_name and n.var_name not in result:
                    result[n.var_name] = Var(n.var_name, n.var_type, n.scope)
            for pin in list(n.inputs) + list(n.outputs):
                if pin.kind == 'value' and pin.source == 'variable' and pin.variable:
                    if pin.variable not in result:
                        result[pin.variable] = Var(pin.variable, pin.var_type, 0)
    # 菜单场景里的变量节点(菜单内操作用全局变量)
    for mn in getattr(project, "menus", []) or []:
        for n in mn.nodes:
            if n.type == N_DEFVAR:
                v = result.get(n.var_name)
                if v is None:
                    result[n.var_name] = Var(n.var_name, n.var_type, 0, n.var_value)
                else:
                    v.type = n.var_type
            elif n.type in (N_SETVAR, N_GETVAR):
                if n.var_name and n.var_name not in result:
                    result[n.var_name] = Var(n.var_name, n.var_type, 0)
            for pin in list(n.inputs) + list(n.outputs):
                if pin.kind == 'value' and pin.source == 'variable' and pin.variable:
                    if pin.variable not in result:
                        result[pin.variable] = Var(pin.variable, pin.var_type, 0)
    return list(result.values())


def var_names(project, scene_idx=None):
    """返回变量名列表(带类型后缀),供下拉框使用。"""
    vs = collect_vars(project)
    out = []
    for v in vs:
        label = v.name
        tname = {0: "int", 1: "float", 2: "str", 3: "bool"}.get(v.type, "?")
        out.append(f"{label}  ({tname})")
    out.sort()
    return out


def var_value(var_name, project):
    """按名字取变量对象。"""
    for v in collect_vars(project):
        if v.name == var_name:
            return v
    return None


def storage_name(var_name, scene_idx, scope):
    """导出时的存储名:全局用原名,局部加场景前缀。"""
    if scope == 1:
        return f"_s{scene_idx}_{var_name}"
    return var_name


def type_detect_literal(s):
    """猜测字面值类型:纯数字→int,含小数点→float,true/false→bool,否则 string。"""
    if s in ("true", "false", "True", "False"):
        return 3
    try:
        int(s)
        return 0
    except Exception:
        pass
    try:
        float(s)
        return 1
    except Exception:
        return 2
