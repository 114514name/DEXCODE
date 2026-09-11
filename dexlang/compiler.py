"""编译器(降级):AST → 汇编 IR。

- 每个函数编译为一个 AsmFunc;`main`(顶层语句)为函数表中第 0 个函数
- 局部变量按声明顺序分配槽位(参数占 0..arity-1)
- `if/while/&&/||` 展开为跳转指令序列
- `include "库"` / `refer "定义文件"` 加载 .dexdef,注册原生(DLL)函数
- 编译期检查:未定义变量/函数、重复声明、实参个数、原生调用类型、
  顶层 return、字面量除零、重复引入、不可达代码(警告)等
"""

import os

from . import opcodes as O
from .errors import DexError
from .defparser import parse_def
from .ir import (
    AsmFunc, Insn, ConstOperand, LocalOperand, LabelOperand,
    FuncOperand, NativeOperand, NativeFunc, LibInfo, AssemblyProgram,
    TypeOperand, FieldOperand,
)

_BINOP_OPCODES = {
    "+": O.ADD, "-": O.SUB, "*": O.MUL, "/": O.DIV, "%": O.MOD,
    "==": O.EQ, "!=": O.NE, "<": O.LT, "<=": O.LE, ">": O.GT, ">=": O.GE,
}

_TYPE_NAMES = {O.NAT_INT: "int", O.NAT_FLOAT: "float", O.NAT_STR: "string", O.NAT_VOID: "void"}


def _search_lang_file(target, source_path, include_dirs, ext):
    """在源目录与 include 目录(及其直接子目录)中搜索 <target><ext>。
    返回绝对路径,找不到返回 None。"""
    source_dir = os.path.dirname(os.path.abspath(source_path)) if source_path else os.getcwd()
    candidates = []
    if source_path:
        candidates.append(os.path.join(source_dir, target + ext))
    for d in include_dirs or []:
        base = os.path.abspath(d)
        candidates.append(os.path.join(base, target + ext))
        try:
            for name in sorted(os.listdir(base)):
                sub = os.path.join(base, name)
                if os.path.isdir(sub):
                    candidates.append(os.path.join(sub, target + ext))
        except OSError:
            pass
    for p in candidates:
        if os.path.exists(p):
            return os.path.abspath(p)
    return None


def _resolve_def(target, kind, source_path, include_dirs):
    """把 include/refer 的 target 解析为库文件绝对路径。

    include:优先 <target>.dexdef(原生 DLL 定义),再找 <target>.dex(语言模块)。
    refer:直接解析定义文件路径(相对源目录)。
    """
    if kind == "refer":
        source_dir = os.path.dirname(os.path.abspath(source_path)) if source_path else os.getcwd()
        path = target if os.path.isabs(target) else os.path.join(source_dir, target)
        if not path.endswith(".dexdef"):
            path += ".dexdef"
        if not os.path.exists(path):
            raise DexError(f"definition file not found: {path}", phase="compiler")
        return os.path.abspath(path)

    p = _search_lang_file(target, source_path, include_dirs, ".dexdef")
    if p:
        return p
    p = _search_lang_file(target, source_path, include_dirs, ".dex")
    if p:
        return p
    raise DexError(
        f"cannot find library '{target}' (searched *.dexdef / *.dex "
        f"in source dir and include dirs)",
        phase="compiler")


class CompileUnit:
    """一次编译的产物:普通函数(主函数在前)+ 原生函数 + 库表 + 警告。"""

    def __init__(self):
        self.funcs: list = []           # AsmFunc,索引即函数编号,main 在前
        self.func_index: dict = {}      # DexLang 函数名 -> 编号
        self.natives: list = []         # NativeFunc,索引即原生函数编号
        self.native_index: dict = {}    # 原生函数名 -> 编号
        self.libs: list = []            # List[LibInfo](去重,索引即库编号)
        self.lib_index: dict = {}       # 库路径 -> 编号
        self.warnings: list = []        # 编译期警告
        self.loaded_defs: set = set()   # 已加载的定义文件(防止重复引入)
        self.consts: list = []
        self.const_index: dict = {}
        self.types: dict = {}           # 自定义类型名 -> [(字段名, 类型名)]
        self.type_order: list = []      # 类型声明顺序(MAKE_OBJ 用)
        self.type_nodes: dict = {}      # 类型名 -> TypeDef 节点(报错定位用)
        self.func_ret_types: dict = {}  # 语言函数名 -> 返回类型标注(空串=未知)
        self.func_nodes: dict = {}      # 语言函数名 -> Func AST 节点(用于推断返回类型)
        self.lib_release: dict = {}     # 库路径 -> 该库的字符串释放函数名(约定式)
        self.module_funcs: list = []    # 语言模块(.dex)中收集的 Func 节点

    def add_type(self, name, fields, node):
        if name in self.types:
            raise DexError(f"duplicate type '{name}'", node.line, node.col, "compiler")
        self.types[name] = list(fields)
        self.type_order.append(name)
        self.type_nodes[name] = node

    def check_recursive_types(self):
        """拒绝「递归类型」(类型直接或间接包含自身)。

        这是 P3(结构体值语义)的前提,也是让引用计数成为完备方案的关键:
        值语义下 `a.v = a` 要成立,字段 v 的类型就必须等于 a 的类型,即类型递归。
        因此排除递归类型后,环在类型层面**不可表达**,无需环收集器。
        (Rust 出于同样的"无限大小"原因拒绝递归的按值类型。)"""
        state = {}   # 0=未访问 1=在栈上 2=已完成

        def visit(tname, path):
            st = state.get(tname, 0)
            if st == 2:
                return
            if st == 1:
                chain = " -> ".join(path + [tname])
                node = self.type_nodes.get(tname)
                line, col = (node.line, node.col) if node else (0, 0)
                raise DexError(
                    f"recursive type is not allowed: {chain}\n"
                    f"    a value type cannot contain itself (it would have infinite size); "
                    f"hold the field as a separate variable instead",
                    line, col, "compiler")
            state[tname] = 1
            for _fname, ftype in self.types.get(tname, []):
                if ftype in self.types:
                    visit(ftype, path + [tname])
            state[tname] = 2

        for tname in self.type_order:
            visit(tname, [])

    def field_index(self, tname, fname, node=None):
        """返回类型 tname 中字段 fname 的索引;不存在则报错。"""
        fields = self.types.get(tname)
        line, col = (node.line, node.col) if node else (0, 0)
        if fields is None:
            raise DexError(f"unknown type '{tname}'", line, col, "compiler")
        for i, (fn, _ft) in enumerate(fields):
            if fn == fname:
                return i
        raise DexError(f"type '{tname}' has no field '{fname}'", line, col, "compiler")

    def warn(self, msg):
        self.warnings.append(msg)

    def add_native(self, name, lib: LibInfo, param_types, ret_type, node):
        if name in self.func_index or name in self.native_index:
            raise DexError(f"duplicate or conflicting function '{name}'",
                           node.line, node.col, "compiler")
        if lib.path not in self.lib_index:
            self.lib_index[lib.path] = len(self.libs)
            self.libs.append(lib)
        self.natives.append(NativeFunc(
            name=name, lib=lib.path, param_types=list(param_types), ret_type=ret_type))
        self.native_index[name] = len(self.natives) - 1

    def load_definition(self, def_path, node, rel_lib=False):
        if def_path in self.loaded_defs:
            return   # 已加载过(主文件与语言模块可能重复 include),去重跳过
        self.loaded_defs.add(def_path)
        with open(def_path, "r", encoding="utf-8-sig") as f:
            text = f.read()
        def_file = parse_def(text, def_path)
        if def_file.lib_path is None:
            raise DexError(
                "definition file must contain 'refer \"...dll\"' to point to the native library",
                node.line, node.col, "compiler")
        lib_dir = os.path.dirname(def_path)
        lib_path = def_file.lib_path
        if not os.path.isabs(lib_path):
            abs_lib = os.path.normpath(os.path.join(lib_dir, lib_path))
        else:
            abs_lib = lib_path
        # 动态链接 + rel_lib(相对路径模式):保留 dexdef 里的原样相对路径(如 "libdexxgal.dll"),
        # VM 运行时从工作目录解析 → 发布文件夹换机可用。静态始终记录绝对路径(VM 用内嵌数据,不加载 path)。
        record_path = lib_path if (rel_lib and not def_file.is_static) else os.path.abspath(abs_lib)

        lib = LibInfo(path=record_path, is_static=def_file.is_static, data=b"")
        if def_file.is_static:
            # 静态链接:读取 DLL 字节内嵌进字节码,运行时无需外部 DLL
            try:
                with open(os.path.abspath(abs_lib), "rb") as f:
                    lib.data = f.read()
            except OSError as e:
                raise DexError(
                    f"cannot read library '{os.path.abspath(abs_lib)}' for static linking: {e}",
                    node.line, node.col, "compiler")
        for nd in def_file.natives:
            self.add_native(nd.name, lib, nd.param_types, nd.ret_type, node)

        # 约定式释放函数:定义文件里形如 `extern func f(s: string) -> void;` 的函数
        # 视为该库的字符串释放器。编译器会在调用本库「返回 string」的原生函数后
        # 自动插入 f(返回值),因此原生库可以返回堆分配的字符串而不泄漏。
        # 详见 docs/SPEC.md 4.5「字符串所有权」。
        for nd in def_file.natives:
            if nd.ret_type == O.NAT_VOID and nd.param_types == [O.NAT_STR]:
                self.lib_release[lib.path] = nd.name
                # 立刻记到库表上,使其进入字节码的 DXRL trailer。
                # 必须在此处设置:to_program() 可能在任何函数体编译之前被调用。
                lib.release_name = nd.name
                break

    def load_lang_module(self, path, node, source_path, include_dirs, rel_lib=False):
        """include 一个 .dex 语言模块:合并其中的类型与函数声明。

        模块文件只允许声明(include/refer/type/func),出现可执行语句会报错。
        重复 include 同一模块会被去重跳过。"""
        if path in self.loaded_defs:
            return
        self.loaded_defs.add(path)
        with open(path, "r", encoding="utf-8-sig") as f:
            text = f.read()
        from .lexer import Lexer
        from .parser import Parser
        from .ast import Func, TypeDef, LibRef
        tokens = Lexer(text, path).tokenize()
        mod = Parser(tokens, path).parse_program()
        for st in mod.stmts:
            if isinstance(st, LibRef):
                dep = _resolve_def(st.target, st.kind, path, include_dirs)
                if dep.endswith(".dex"):
                    self.load_lang_module(dep, st, path, include_dirs, rel_lib)
                else:
                    self.load_definition(dep, st, rel_lib)
            elif isinstance(st, TypeDef):
                self.add_type(st.name, st.fields, st)
            elif isinstance(st, Func):
                for mf in self.module_funcs:
                    if mf.name == st.name:
                        raise DexError(f"duplicate function '{st.name}'",
                                       st.line, st.col, "compiler")
                self.module_funcs.append(st)
                self.func_nodes[st.name] = st
                if st.ret_type:
                    self.func_ret_types[st.name] = st.ret_type
            else:
                raise DexError(
                    "library file may only contain declarations "
                    "(include/refer/type/func)", st.line, st.col, "compiler")

    def to_program(self) -> AssemblyProgram:
        return AssemblyProgram(funcs=self.funcs, natives=self.natives, libs=self.libs)


class FuncCompiler:
    def __init__(self, unit, func, params, is_main=False):
        self.unit = unit
        self.func = func
        self.is_main = is_main
        self.locals = {}
        self.var_types = {}   # 变量名 -> 静态类型名(int/float/string/None)
        self.next_local = 0
        self.label_count = 0
        self._line = 0
        self._infer_stack = set()   # 正在推断返回类型的函数名(防递归)
        for p in params:
            self.declare(p, func)
        self.func.nlocals = self.next_local

    # ---------- 语句块(含不可达代码警告) ----------
    def stmts(self, nodes):
        from .ast import Return
        for i, s in enumerate(nodes):
            self.stmt(s)
            if isinstance(s, Return) and i + 1 < len(nodes):
                nxt = nodes[i + 1]
                self.unit.warn(
                    f"unreachable statement at {nxt.line}:{nxt.col} (after 'return')")

    # ---------- 局部变量 ----------
    def declare(self, name, func):
        if name in self.locals:
            raise DexError(f"duplicate variable '{name}'", func.line, func.col, "compiler")
        self.locals[name] = self.next_local
        self.next_local += 1

    def local(self, name, node):
        idx = self.locals.get(name)
        if idx is None:
            raise DexError(f"undefined variable '{name}'", node.line, node.col, "compiler")
        return idx

    # ---------- 发射 ----------
    def new_label(self):
        name = f"L{self.func.name}_{self.label_count}"
        self.label_count += 1
        return name

    def emit(self, op, operand=None):
        self.func.insns.append(Insn(op=op, operand=operand, line=self._line))

    def place_label(self, name):
        self.func.insns.append(Insn(op=None, label=name, line=self._line))

    # ---------- 语句 ----------
    def stmt(self, node):
        from .ast import (Let, Assign, Print, If, While, Return, ExprStmt,
                          LibRef, TypeDef, SetField)
        self._line = node.line
        if isinstance(node, Let):
            self.expr(node.value)
            self.declare(node.name, node)
            self.var_types[node.name] = self.infer(node.value)
            self.emit(O.STORE, LocalOperand(self.locals[node.name]))
        elif isinstance(node, Assign):
            slot = self.local(node.name, node)
            self.expr(node.value)
            self.var_types[node.name] = self.infer(node.value)
            self.emit(O.STORE, LocalOperand(slot))
        elif isinstance(node, SetField):
            # 成员赋值:p.x = v  ->  栈 [obj, value]  SET_FIELD idx
            tname = self.infer(node.obj)
            if tname not in self.unit.types:
                raise DexError(f"cannot determine type of field '{node.field}'",
                               node.line, node.col, "compiler")
            fields = self.unit.types[tname]
            idx = self.unit.field_index(tname, node.field, node)
            self._check_field_assign(tname, node.field, fields[idx][1], node.value, node)
            self.expr(node.obj)
            self.expr(node.value)
            self.emit(O.SET_FIELD, FieldOperand(idx))
        elif isinstance(node, Print):
            for e in node.exprs:
                self.expr(e)
                self.emit(O.PRINT)
        elif isinstance(node, If):
            self._if(node)
        elif isinstance(node, While):
            self._while(node)
        elif isinstance(node, Return):
            if self.is_main:
                raise DexError("'return' is only allowed inside a function",
                               node.line, node.col, "compiler")
            if node.value is not None:
                self.expr(node.value)
            else:
                self.emit(O.PUSH, ConstOperand(0))
            self.emit(O.RET)
        elif isinstance(node, ExprStmt):
            self.expr(node.expr)
            self.emit(O.POP)
        elif isinstance(node, LibRef):
            raise DexError("'include'/'refer' are only allowed at the top level",
                           node.line, node.col, "compiler")
        elif isinstance(node, TypeDef):
            raise DexError("'type' definitions are only allowed at the top level",
                           node.line, node.col, "compiler")
        else:
            raise DexError(f"unsupported statement {type(node).__name__}", node.line, node.col, "compiler")

    def _if(self, node):
        else_label = self.new_label()
        end_label = self.new_label()
        self.expr(node.cond)
        self.emit(O.JZ, LabelOperand(else_label))
        self.stmts(node.then)
        if node.els:
            self.emit(O.JMP, LabelOperand(end_label))
            self.place_label(else_label)
            self.stmts(node.els)
            self.place_label(end_label)
        else:
            self.place_label(else_label)

    def _while(self, node):
        loop_label = self.new_label()
        end_label = self.new_label()
        self.place_label(loop_label)
        self.expr(node.cond)
        self.emit(O.JZ, LabelOperand(end_label))
        self.stmts(node.body)
        self.emit(O.JMP, LabelOperand(loop_label))
        self.place_label(end_label)

    # ---------- 表达式 ----------
    def expr(self, node):
        from .ast import Literal, Name, BinOp, UnaryOp, Call, StructLit, GetField
        self._line = node.line
        if isinstance(node, Literal):
            self.emit(O.PUSH, ConstOperand(node.value))
        elif isinstance(node, Name):
            slot = self.local(node.name, node)
            self.emit(O.LOAD, LocalOperand(slot))
        elif isinstance(node, StructLit):
            # 结构体字面量:按类型声明顺序压入各字段值,再 MAKE_OBJ
            fields = self.unit.types.get(node.type_name)
            if fields is None:
                raise DexError(f"unknown type '{node.type_name}'",
                               node.line, node.col, "compiler")
            given = dict(node.fields)
            for fname, ftype in fields:
                if fname in given:
                    self.expr(given[fname])
                else:
                    self.emit(O.PUSH, ConstOperand(self._default_value(ftype, node)))
            self.emit(O.MAKE_OBJ, TypeOperand(node.type_name, len(fields)))
        elif isinstance(node, GetField):
            self.expr(node.obj)
            tname = self.infer(node.obj)
            if tname not in self.unit.types:
                raise DexError(f"cannot determine type of field '{node.field}'",
                               node.line, node.col, "compiler")
            idx = self.unit.field_index(tname, node.field, node)
            self.emit(O.GET_FIELD, FieldOperand(idx))
        elif isinstance(node, BinOp):
            self._binop(node)
        elif isinstance(node, UnaryOp):
            if node.op == "!":
                self.expr(node.operand)
                self.emit(O.NOT)
            elif node.op == "-":
                self.expr(node.operand)
                self.emit(O.NEG)
            else:
                raise DexError(f"unsupported unary operator '{node.op}'", node.line, node.col, "compiler")
        elif isinstance(node, Call):
            self._call(node)
        else:
            raise DexError(f"unsupported expression {type(node).__name__}", node.line, node.col, "compiler")

    def _binop(self, node):
        op = node.op
        if op == "&&":
            false_l, end_l = self.new_label(), self.new_label()
            self.expr(node.left)
            self.emit(O.JZ, LabelOperand(false_l))
            self.expr(node.right)
            self.emit(O.JZ, LabelOperand(false_l))
            self.emit(O.PUSH, ConstOperand(1))
            self.emit(O.JMP, LabelOperand(end_l))
            self.place_label(false_l)
            self.emit(O.PUSH, ConstOperand(0))
            self.place_label(end_l)
        elif op == "||":
            true_l, end_l = self.new_label(), self.new_label()
            self.expr(node.left)
            self.emit(O.JNZ, LabelOperand(true_l))
            self.expr(node.right)
            self.emit(O.JNZ, LabelOperand(true_l))
            self.emit(O.PUSH, ConstOperand(0))
            self.emit(O.JMP, LabelOperand(end_l))
            self.place_label(true_l)
            self.emit(O.PUSH, ConstOperand(1))
            self.place_label(end_l)
        else:
            from .ast import Literal
            # 字符串拼接:当 '+' 的操作数静态判定为字符串时,降级为 CONCAT(数字自动转字符串)
            if op == "+" and (self.infer(node.left) == "string"
                              or self.infer(node.right) == "string"):
                self.expr(node.left)
                self.expr(node.right)
                self.emit(O.CONCAT)
                return
            opcode = _BINOP_OPCODES.get(op)
            if opcode is None:
                raise DexError(f"unsupported binary operator '{op}'", node.line, node.col, "compiler")
            # 编译期检查:字面量除零 / 取模零
            if op in ("/", "%") and isinstance(node.right, Literal) and node.right.value == 0:
                raise DexError(
                    f"division/modulo by literal zero ({op} 0)", node.line, node.col, "compiler")
            self.expr(node.left)
            self.expr(node.right)
            self.emit(opcode)

    # ---------- 字段赋值类型校验 ----------
    def _check_field_assign(self, tname, fname, ftype, value_node, node):
        """校验 `obj.field = value` 的值类型与被赋字段的类型兼容。

        结构体是引用语义且 VM 的 SET_FIELD 不做运行期类型检查,所以若不在这里拦,
        `a.v = a`(把对象赋给 int 字段)会被静默接受,构造出环状对象,
        随后 print / == 递归到 C 栈耗尽而崩溃。
        采用"能推断出明确冲突才报错"的策略:类型未知时放行,避免误伤动态用法。"""
        vtype = self.infer(value_node)
        if vtype is None:
            return                      # 无法静态判定,交给运行期
        if vtype == ftype:
            return
        # int 字面量可隐式提升为 float,以及数值字段间的常见放宽
        if ftype == "float" and vtype == "int":
            return
        if ftype in ("int", "bool") and vtype in ("int", "bool"):
            return
        if vtype in self.unit.types or ftype in self.unit.types:
            # 涉及自定义类型的赋值必须是同一类型
            raise DexError(
                f"cannot assign a value of type '{vtype}' to field '{fname}' of type '{ftype}'",
                node.line, node.col, "compiler")
        if ftype in ("int", "float", "string") and vtype in ("int", "float", "string"):
            raise DexError(
                f"cannot assign a value of type '{vtype}' to field '{fname}' of type '{ftype}'",
                node.line, node.col, "compiler")

    def _infer_return_type(self, fnode):
        """从函数体的 return 语句推断返回类型(无标注时使用)。

        DexLang 的 `func mk(n) { ... return s; }` 不写 `-> string`,此前
        `infer` 对这类调用一律返回 None,导致 `mk(1) + mk(2)` 被降级成 ADD
        而不是 CONCAT,运行期报 "unsupported operand types for arithmetic"。
        取所有 return 表达式的类型:全部一致才采用,否则保守返回 None
        (宁可少优化,不可错降级)。

        依赖调用方已把函数体内的 let/赋值类型收集进 self.var_types
        (见 compile_program 的预跑阶段),否则局部变量类型未知会退化为 None。
        `_infer_stack` 防环:递归函数推断自身返回类型会无限递归。"""
        name = getattr(fnode, "name", None)
        if name in self._infer_stack:
            return None
        self._infer_stack.add(name)
        try:
            from .ast import Return
            types = []

            def walk(stmts):
                for st in stmts:
                    if isinstance(st, Return):
                        types.append(self.infer(st.value) if st.value is not None else "int")
                    for attr in ("then", "els", "body"):
                        sub = getattr(st, attr, None)
                        if isinstance(sub, list):
                            walk(sub)

            walk(fnode.body)
            known = {t for t in types if t is not None}
            if len(known) == 1 and None not in types:
                return known.pop()
            return None
        finally:
            self._infer_stack.discard(name)

    def collect_var_types(self, stmts):
        """按执行顺序收集 let/简单赋值带来的变量静态类型(供返回类型推断)。

        只记录能推断出类型的赋值;重复赋值若类型不一致则抹掉该变量类型,
        避免用过期信息做出错误的 CONCAT/ADD 降级决策。"""
        from .ast import Let, Assign

        def walk(nodes):
            for st in nodes:
                if isinstance(st, Let):
                    self.var_types[st.name] = self.infer(st.value)
                elif isinstance(st, Assign):
                    t = self.infer(st.value)
                    if st.name in self.var_types and self.var_types[st.name] != t:
                        self.var_types[st.name] = None
                    else:
                        self.var_types[st.name] = t
                for attr in ("then", "els", "body"):
                    sub = getattr(st, attr, None)
                    if isinstance(sub, list):
                        walk(sub)

        walk(stmts)

    def _default_value(self, ftype, node):
        """结构体字段缺省时的默认值(基础类型);自定义类型字段缺省报错。"""
        if ftype == "int" or ftype == "bool":
            return 0
        if ftype == "float":
            return 0.0
        if ftype == "string":
            return ""
        raise DexError(
            f"field of custom type '{ftype}' must be provided explicitly",
            node.line, node.col, "compiler")

    # ---------- 轻量类型推断(用于字符串拼接等) ----------
    def infer(self, node):
        """尽力推断表达式静态类型:"int"/"float"/"string"/自定义类型名/None(未知)。"""
        from .ast import Literal, Name, BinOp, UnaryOp, Call, StructLit, GetField
        if isinstance(node, Literal):
            v = node.value
            if isinstance(v, str):
                return "string"
            if isinstance(v, float):
                return "float"
            return "int"   # int 字面量;true/false 视为 1/0
        if isinstance(node, Name):
            return self.var_types.get(node.name)
        if isinstance(node, StructLit):
            return node.type_name
        if isinstance(node, GetField):
            tname = self.infer(node.obj)
            if tname in self.unit.types:
                for fname, ftype in self.unit.types[tname]:
                    if fname == node.field:
                        return ftype
            return None
        if isinstance(node, UnaryOp):
            return self.infer(node.operand)
        if isinstance(node, BinOp):
            if node.op == "+":
                lt, rt = self.infer(node.left), self.infer(node.right)
                if lt == "string" or rt == "string":
                    return "string"
                if "float" in (lt, rt):
                    return "float"
                if lt == "int" and rt == "int":
                    return "int"
                return None
            return None
        if isinstance(node, Call):
            rt = self.unit.func_ret_types.get(node.name)
            if rt:
                return rt
            ni = self.unit.native_index.get(node.name)
            if ni is not None:
                return _TYPE_NAMES.get(self.unit.natives[ni].ret_type)
            # 无返回类型标注的普通函数:返回类型已由 compile_program 的
            # 预跑阶段推断并写入 func_ret_types;这里不再二次推断。
            return None
        return None

    def _check_native_literal(self, fname, native, idx, lit):
        """原生调用的字面量实参类型检查(尽力而为,动态值无法静态判定)。"""
        ptype = native.param_types[idx]
        value = lit.value
        argno = idx + 1
        if ptype == O.NAT_STR:
            if not isinstance(value, str):
                vt = _TYPE_NAMES[O.NAT_INT if isinstance(value, int) else O.NAT_FLOAT]
                raise DexError(
                    f"argument {argno} of native '{fname}' expects a string, got a {vt} literal",
                    lit.line, lit.col, "compiler")
        elif ptype == O.NAT_INT:
            if isinstance(value, str):
                raise DexError(
                    f"argument {argno} of native '{fname}' expects an int, got a string literal",
                    lit.line, lit.col, "compiler")
            if isinstance(value, float) and value != int(value):
                raise DexError(
                    f"argument {argno} of native '{fname}' expects an int, "
                    f"got non-integral float literal {value!r}",
                    lit.line, lit.col, "compiler")
        elif ptype == O.NAT_FLOAT:
            if isinstance(value, str):
                raise DexError(
                    f"argument {argno} of native '{fname}' expects a number, got a string literal",
                    lit.line, lit.col, "compiler")
        # int 字面量 → float 参数允许隐式提升

    def _call(self, node):
        from .ast import Literal
        # 内置动态调用:call("函数名", arg, ...) —— 运行时按名字查函数,用于信号分发等
        if node.name == "call":
            self._call_builtin(node)
            return

        fi = self.unit.func_index.get(node.name)
        if fi is not None:
            callee = self.unit.funcs[fi]
            if len(node.args) != callee.arity:
                raise DexError(
                    f"function '{node.name}' expects {callee.arity} argument(s), got {len(node.args)}",
                    node.line, node.col, "compiler")
            for a in node.args:
                self.expr(a)
            self.emit(O.CALL, FuncOperand(node.name))
            return

        ni = self.unit.native_index.get(node.name)
        if ni is not None:
            native = self.unit.natives[ni]
            if len(node.args) != native.arity:
                raise DexError(
                    f"native function '{node.name}' expects {native.arity} argument(s), "
                    f"got {len(node.args)}",
                    node.line, node.col, "compiler")
            for idx, a in enumerate(node.args):
                if isinstance(a, Literal):
                    self._check_native_literal(node.name, native, idx, a)
            for a in node.args:
                self.expr(a)
            self._emit_ncall(native, node)
            return

        raise DexError(f"undefined function '{node.name}'", node.line, node.col, "compiler")

    def _emit_ncall(self, native, node):
        """发射 NCALL;返回 string 且本库声明了释放函数时,登记到库表供 VM 释放。

        释放**必须由 VM 执行**:原生返回的 const char* 是原始缓冲区指针,
        只存在于 VM 自己的值栈上;编译器既看不到它,也无法用字节码表达它
        (DUP 复制的是 VM 已 strdup 的副本,把它交给释放器会双重释放)。
        因此这里只把「本库的释放函数」记进 LibInfo,由 VM 在复制完字符串后调用。"""
        self.emit(O.NCALL, NativeOperand(native.name))
        if native.ret_type != O.NAT_STR:
            return
        # 默认契约(见 docs/SPEC.md 4.5):原生返回的 const char* 视为**借用**,
        # 由 VM 复制进自己的堆;不声明释放函数即表示该库返回的是静态/常驻缓冲区,
        # VM 不会去释放它 —— 这是绝大多数现有库的形态,因此不产生任何警告。
        #
        # 只有显式声明了 (string)->void 释放函数的库才会走释放路径:
        # release_name 已在 load_definition 阶段写入库表,由 VM 读取 DXRL 后调用。
        _ = self.unit.lib_release.get(native.lib)

    def _call_builtin(self, node):
        """内置 call(name, args...):动态按名调用语言函数。"""
        if len(node.args) < 1:
            raise DexError("call() expects at least a function name as its first argument",
                           node.line, node.col, "compiler")
        name_arg = node.args[0]
        args = node.args[1:]
        # 栈布局:CALL_NAME 弹出 argc,再弹 name,剩余 argc 个即实参(顺序与声明一致)
        for a in args:
            self.expr(a)
        self.expr(name_arg)
        self.emit(O.PUSH, ConstOperand(len(args)))
        self.emit(O.CALL_NAME)

    # ---------- 收尾 ----------
    def finish_function(self):
        self.func.nlocals = self.next_local
        if self.func.insns and self.func.insns[-1].op == O.RET:
            return
        self.emit(O.PUSH, ConstOperand(0))
        self.emit(O.RET)

    def finish_main(self):
        self.func.nlocals = self.next_local
        self.emit(O.HALT)


def compile_program(ast, source_path=None, include_dirs=None, rel_lib=False) -> CompileUnit:
    """AST → CompileUnit(汇编 IR + 原生函数 + 库表)。

    source_path: 源码文件路径(用于解析 include/refer 的相对路径)
    include_dirs: include 搜索目录列表
    rel_lib: 动态库路径保留相对原样(发布自包含;VM 从运行目录解析)
    """
    from .ast import Func, LibRef, TypeDef

    unit = CompileUnit()

    # 0) 顶层:先收集自定义类型定义(供全程序引用)
    for s in ast.stmts:
        if isinstance(s, TypeDef):
            unit.add_type(s.name, s.fields, s)
    unit.check_recursive_types()

    # 1) 顶层:处理库引入,收集函数声明(main 恒为 0 号,支持前向引用/递归)
    decls = []
    seen = {"main"}
    for s in ast.stmts:
        if isinstance(s, LibRef):
            def_path = _resolve_def(s.target, s.kind, source_path, include_dirs)
            if def_path.endswith(".dex"):
                unit.load_lang_module(def_path, s, source_path, include_dirs, rel_lib)
            else:
                unit.load_definition(def_path, s, rel_lib)
        elif isinstance(s, Func):
            if s.name in seen:
                raise DexError(f"duplicate function '{s.name}'", s.line, s.col, "compiler")
            seen.add(s.name)
            decls.append(s)
            unit.func_nodes[s.name] = s
            if s.ret_type:
                unit.func_ret_types[s.name] = s.ret_type

    # 合并语言模块(.dex)中收集的函数声明
    for mf in unit.module_funcs:
        if mf.name in seen:
            raise DexError(f"duplicate function '{mf.name}'", mf.line, mf.col, "compiler")
        seen.add(mf.name)
        decls.append(mf)

    funcs = [AsmFunc(name="main", arity=0)]
    for d in decls:
        funcs.append(AsmFunc(name=d.name, arity=len(d.params)))
    unit.funcs = funcs
    unit.func_index = {f.name: i for i, f in enumerate(funcs)}

    # 2) 预跑:推断无返回类型标注的函数的返回类型。
    #    必须在编译任何函数体之前完成 —— infer() 依赖 func_ret_types 来决定
    #    `+` 走 CONCAT 还是 ADD。每个函数用一个临时 FuncCompiler 做纯类型遍历
    #    (不 emit 指令),跑完即丢弃,避免污染真正编译时的槽位分配。
    for d in decls:
        if d.ret_type:
            continue
        probe = FuncCompiler(unit, AsmFunc(name=d.name, arity=len(d.params)), d.params)
        for p, pt in zip(d.params, d.param_types):
            if pt:
                probe.var_types[p] = pt
        probe.collect_var_types(d.body)
        rt = probe._infer_return_type(d)
        if rt:
            unit.func_ret_types[d.name] = rt

    # 3) 编译各具名函数
    for f, d in zip(funcs[1:], decls):
        fc = FuncCompiler(unit, f, d.params)
        for p, pt in zip(d.params, d.param_types):
            if pt:
                fc.var_types[p] = pt
        fc.stmts(d.body)
        fc.finish_function()

    # 4) 编译 main(顶层非函数语句;include/refer/type 已处理)
    fc = FuncCompiler(unit, funcs[0], [], is_main=True)
    fc.stmts([s for s in ast.stmts if not isinstance(s, (Func, LibRef, TypeDef))])
    fc.finish_main()

    return unit
