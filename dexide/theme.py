"""DEXIDE 主题:Catppuccin Mocha 暗色(与 JASON IDE 一致)+ DexLang 语法配色。"""

# ============================================================
# Catppuccin Mocha palette
# ============================================================
C = {
    'base':     '#1e1e2e', 'mantle':   '#181825', 'crust':    '#11111b',
    'surface0': '#313244', 'surface1': '#45475a', 'surface2': '#585b70',
    'overlay0': '#6c7086', 'text':     '#cdd6f4', 'subtext0': '#a6adc8',
    'subtext1': '#bac2de', 'blue':     '#89b4fa', 'lavender': '#b4befe',
    'mauve':    '#cba6f7', 'pink':     '#f5c2e7', 'red':      '#f38ba8',
    'green':    '#a6e3a1', 'yellow':   '#f9e2af', 'peach':    '#fab387',
    'teal':     '#94e2d5', 'sky':      '#89dceb',
}

# ============================================================
# 语法着色标签
# ============================================================
HL = {
    'keyword':    C['mauve'],    # 流程/声明关键字
    'type':       C['teal'],     # 类型名 int/float/string/void
    'number':     C['peach'],    # 数字
    'string':     C['green'],    # 字符串
    'comment':    C['surface2'], # 注释
    'operator':   C['sky'],      # 运算符
    'func':       C['yellow'],   # DexLang 函数名/调用
    'native':     C['red'],      # 原生(DLL)函数调用
    'builtin':    C['lavender'], # 内置(print)
    'variable':   C['blue'],     # 变量
    'label':      C['pink'],     # 汇编标签
    'library':    C['teal'],     # include/refer 的库路径
    'extern':     C['lavender'], # extern 关键字
    'deflib':     C['pink'],     # .dexdef 中的 refer 路径
    # ---- 以下为"完整着色"补充(此前这些 token 全部落到 operator) ----
    'decl':       C['mauve'],    # func/let/type/extern/release 声明关键字
    'param':      C['subtext1'], # 函数参数名(形参)
    'delim':      C['overlay0'], # 标点 ( ) [ ] ; , :
}

# 关键字 → 高亮类别。
# 注意:词法器为每个关键字给出了**独立的 token 种类**(TokKind.LET / PRINT /
# INCLUDE / ...),并不都是 IDENT。因此编辑器按 token 种类着色(见 editor.py 的
# 映射表),本表只用于文本层面的辅助判断与悬停文档。
_TAG_FLOW = {'if', 'else', 'while', 'return', 'true', 'false'}
_TAG_DECL = {'func', 'let', 'type'}
_TAG_EXTERN = {'include', 'refer'}
_TAG_PRINT = {'print'}
_TAG_TYPE = {'int', 'float', 'string', 'void'}
_TAG_NATIVE = {'extern', 'release'}


def TAG_FOR_KEYWORD(word):
    if word in _TAG_FLOW:
        return 'keyword'
    if word in _TAG_DECL:
        return 'decl'
    if word in _TAG_EXTERN:
        return 'builtin'
    if word in _TAG_PRINT:
        return 'builtin'
    if word in _TAG_TYPE:
        return 'type'
    if word in _TAG_NATIVE:
        return 'extern'
    return None


# ============================================================
# 悬停文档(语法提示)
# ============================================================
KW_DOCS = {
    'func':   ('func 函数名(参数...) { ... }', '定义函数。参数与 let 变量一样是函数作用域局部变量。'),
    'let':    ('let 变量 = 表达式;', '声明局部变量并初始化。需在使用前声明。'),
    'if':     ('if 条件 { ... } else { ... }', '条件分支。条件为假时执行 else(可选)。'),
    'else':   ('else { ... }', '否则分支。与 if 配对。'),
    'while':  ('while 条件 { ... }', '当条件为真时反复执行循环体。'),
    'return': ('return 表达式;', '返回函数值并结束当前函数。仅允许在函数内使用。'),
    'print':  ('print 表达式, ...;', '输出一个或多个值,每个值单独一行。'),
    'include':('include "库名";', '引入库定义(搜索 库名.dexdef)。仅允许在顶层。'),
    'refer':  ('refer "路径";', '显式指向一个 .dexdef 定义文件。仅允许在顶层。'),
    'true':   ('true', '布尔真值,即整数 1。'),
    'false':  ('false', '布尔假值,即整数 0。'),
    'int':    ('int', '64 位整数类型(定义文件参数/返回类型)。'),
    'float':  ('float', '64 位浮点类型(定义文件参数/返回类型)。'),
    'string': ('string', '字符串类型(定义文件参数/返回类型)。'),
    'void':   ('void', '无返回值(定义文件返回类型)。'),
    'extern': ('extern func 名(参数) -> 类型;', '声明 DLL 导出函数接口(仅用于 .dexdef)。'),
}
