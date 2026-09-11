"""DEXIDE 项目分析器。

- 用真实工具链(词法/语法/编译)分析每个 .dex 文件,收集编译错误/警告
- 提取函数(参数/arity/行号/调用点)、变量作用域
- 自动解析 include/refer 引入的 .dexdef,聚合原生函数(库自动分析)
- 构建函数调用关系图(调用者/被调用者)
- 提供自动补全与悬停提示数据
"""

import os
import re
from collections import defaultdict

from dexlang import Lexer, Parser, compile_program, DexError
from dexlang import ast as A
from dexlang import opcodes as O
from dexlang.compiler import _resolve_def
from dexlang.defparser import parse_def

from .theme import KW_DOCS, TAG_FOR_KEYWORD


class Problem:
    __slots__ = ("severity", "line", "col", "msg")

    def __init__(self, severity, line, col, msg):
        self.severity = severity  # 'error' | 'warning' | 'info'
        self.line = line or 0
        self.col = col or 0
        self.msg = msg

    def __repr__(self):
        return f"Problem({self.severity},{self.line}:{self.col},{self.msg})"


class FuncSymbol:
    __slots__ = ("name", "arity", "params", "line", "file", "calls",
                 "callees", "callers", "usages", "var_names")

    def __init__(self, name, arity, params, line, file):
        self.name = name
        self.arity = arity
        self.params = params
        self.line = line
        self.file = file
        self.calls = []       # [(callee, line, col)]
        self.callees = set()  # 被调用者(函数或原生)
        self.callers = set()  # 调用者
        self.usages = []      # 调用点(line, col)
        self.var_names = list(params)  # 参数 + let 变量(函数作用域)


class NativeSymbol:
    __slots__ = ("name", "lib", "sig", "arity", "param_types", "ret_type",
                 "def_file", "line")

    def __init__(self, name, lib, sig, arity, param_types, ret_type, def_file, line):
        self.name = name
        self.lib = lib
        self.sig = sig
        self.arity = arity
        self.param_types = param_types
        self.ret_type = ret_type
        self.def_file = def_file
        self.line = line

    @property
    def ret_name(self):
        return O.CODE_TO_TYPE.get(self.ret_type, "?")


class FileAnalysis:
    def __init__(self, path):
        self.path = path
        self.funcs = {}       # name -> FuncSymbol
        self.natives = {}     # name -> NativeSymbol
        self.includes = []    # (line, kind, target, resolved_path)
        self.top_vars = {}    # name -> line
        self.problems = []    # Problem
        self.libs = []        # (dll_path, def_file)

    def iter_all_funcs(self):
        for f in self.funcs.values():
            yield f


_WARN_RE = re.compile(r"^unreachable statement at (\d+):(\d+)")


def _collect_body(func_node):
    """收集函数体中的 let 变量与函数调用点。"""
    lets = []
    calls = []

    def expr(n):
        if isinstance(n, A.Call):
            calls.append((n.name, n.line, n.col))
            for a in n.args:
                expr(a)
        elif isinstance(n, A.BinOp):
            expr(n.left)
            expr(n.right)
        elif isinstance(n, A.UnaryOp):
            expr(n.operand)

    def stmts(nodes):
        for s in nodes:
            stmt(s)

    def stmt(s):
        if isinstance(s, A.Let):
            lets.append((s.name, s.line, s.col))
            expr(s.value)
        elif isinstance(s, A.Assign):
            expr(s.value)
        elif isinstance(s, A.Print):
            for e in s.exprs:
                expr(e)
        elif isinstance(s, A.If):
            expr(s.cond)
            stmts(s.then)
            if s.els:
                stmts(s.els)
        elif isinstance(s, A.While):
            expr(s.cond)
            stmts(s.body)
        elif isinstance(s, A.Return):
            if s.value is not None:
                expr(s.value)
        elif isinstance(s, A.ExprStmt):
            expr(s.expr)

    stmts(func_node.body)
    return lets, calls


class ProjectAnalyzer:
    def __init__(self, root, include_dirs=None):
        self.root = os.path.abspath(root)
        dirs = list(include_dirs or [])
        # 自动把项目根(或其上级目录)下的 libs/ 加入 include 搜索,
        # 这样 include "math" 能解析到 <root>/libs/math/math.dexdef
        for base in (self.root, os.path.dirname(self.root)):
            lib = os.path.join(base, "libs")
            if os.path.isdir(lib) and lib not in dirs:
                dirs.append(lib)
        self.include_dirs = dirs
        self.files = {}          # path -> FileAnalysis
        self.def_cache = {}      # def_path -> parsed natives dict
        self._all_natives = {}   # name -> NativeSymbol(聚合)
        self._callees = defaultdict(set)
        self._callers = defaultdict(set)

    # ---------- 文件发现 ----------
    def scan(self):
        found = []
        for dirpath, dirnames, filenames in os.walk(self.root):
            dirnames[:] = [d for d in dirnames
                           if not d.startswith((".", "_")) and d not in (".venv", "vm", "dexide", "__pycache__", "tests", "docs", ".git")]
            for fn in filenames:
                if fn.endswith((".dex", ".dexdef")):
                    found.append(os.path.join(dirpath, fn))
        return sorted(found)

    # ---------- 定义文件 ----------
    def _load_def(self, def_path):
        if def_path in self.def_cache:
            return self.def_cache[def_path]
        natives = {}
        lib_path = None
        try:
            with open(def_path, "r", encoding="utf-8-sig") as f:
                text = f.read()
            def_file = parse_def(text, def_path)
            lib_path = def_file.lib_path
            if lib_path and not os.path.isabs(lib_path):
                lib_path = os.path.abspath(os.path.join(os.path.dirname(def_path), lib_path))
            for nd in def_file.natives:
                natives[nd.name] = NativeSymbol(
                    name=nd.name, lib=lib_path or "", sig=nd.sig,
                    arity=nd.arity, param_types=list(nd.param_types),
                    ret_type=nd.ret_type, def_file=def_path, line=0)
        except DexError as e:
            natives = {}
        self.def_cache[def_path] = (natives, lib_path)
        return self.def_cache[def_path]

    # ---------- 单文件分析 ----------
    def analyze_file(self, path):
        path = os.path.abspath(path)
        try:
            with open(path, "r", encoding="utf-8-sig") as f:
                text = f.read()
        except OSError:
            return FileAnalysis(path)

        fa = FileAnalysis(path)
        if not path.endswith(".dex"):
            # 非 DexLang 源文件(如 .dxasm 汇编 / .dexdef 定义)不做编译分析
            self.files[path] = fa
            return fa

        try:
            toks = Lexer(text, path).tokenize()
            ast = Parser(toks, path).parse_program()
            unit = compile_program(ast, source_path=path, include_dirs=self.include_dirs)
        except DexError as e:
            fa.problems.append(Problem("error", e.line, e.col, e.message))
            return fa

        for w in unit.warnings:
            m = _WARN_RE.match(w)
            if m:
                fa.problems.append(Problem("warning", int(m.group(1)), int(m.group(2)), w))
            else:
                fa.problems.append(Problem("warning", 0, 0, w))

        for node in ast.stmts:
            if isinstance(node, A.Func):
                sym = FuncSymbol(node.name, len(node.params), list(node.params),
                                 node.line, path)
                lets, calls = _collect_body(node)
                sym.calls = calls
                sym.var_names = list(node.params) + [n for n, _, _ in lets]
                fa.funcs[node.name] = sym
            elif isinstance(node, A.LibRef):
                try:
                    def_path = _resolve_def(node.target, node.kind, path, self.include_dirs)
                    fa.includes.append((node.line, node.kind, node.target, def_path))
                    if def_path.endswith(".dex"):
                        # 语言模块:函数/类型已由 compile_program 收集,无需原生解析
                        continue
                    natives, lib = self._load_def(def_path)
                    if lib:
                        fa.libs.append((lib, def_path))
                    for nname, nsym in natives.items():
                        fa.natives[nname] = nsym
                except DexError as e:
                    fa.problems.append(Problem("error", node.line, node.col, e.message))
            elif isinstance(node, A.Let):
                fa.top_vars[node.name] = node.line

        self.files[path] = fa
        return fa

    # ---------- 全量分析 ----------
    def reanalyze(self):
        paths = self.scan()
        for p in paths:
            self.analyze_file(p)
        self._build_graph()

    def analyze_path(self, path):
        fa = self.analyze_file(path)
        self._build_graph()
        return fa

    def _build_graph(self):
        self._callers.clear()
        self._callees.clear()
        self._all_natives = {}
        for fa in self.files.values():
            for nname, nsym in fa.natives.items():
                self._all_natives[nname] = nsym
            for fsym in fa.iter_all_funcs():
                for callee, _line, _col in fsym.calls:
                    self._callees[fsym.name].add(callee)
                    self._callers[callee].add(fsym.name)
                    fsym.callees.add(callee)
        for fa in self.files.values():
            for fsym in fa.iter_all_funcs():
                fsym.callers = set(self._callers.get(fsym.name, ()))

    # ---------- 补全与悬停 ----------
    def all_func_names(self):
        names = set()
        for fa in self.files.values():
            names.update(fa.funcs)
        return names

    def native_names(self):
        return set(self._all_natives)

    def autocomplete(self, path, func_name, prefix):
        """返回候选 [(word, kind, detail)]。kind: keyword/func/native/variable/builtin。"""
        cands = []
        fa = self.files.get(path)
        # 关键字 + 内置
        for kw in KW_DOCS:
            if kw.startswith(prefix):
                cands.append((kw, "keyword", "keyword"))
        for b in ("print",):
            if b.startswith(prefix):
                cands.append((b, "builtin", "内置函数"))
        # 项目内 DexLang 函数
        for n in sorted(self.all_func_names()):
            if n.startswith(prefix):
                cands.append((n, "func", "函数"))
        # 原生函数
        for n in sorted(self.native_names()):
            if n.startswith(prefix):
                nsym = self._all_natives[n]
                cands.append((n, "native", f"原生 {nsym.sig}"))
        # 变量(当前函数作用域 + 顶层)
        if fa:
            if func_name and func_name in fa.funcs:
                for v in fa.funcs[func_name].var_names:
                    if v.startswith(prefix):
                        cands.append((v, "variable", "局部变量"))
            for v in fa.top_vars:
                if v.startswith(prefix):
                    cands.append((v, "variable", "顶层变量"))
        # 去重保序
        seen = set()
        out = []
        for c in cands:
            if c[0] not in seen:
                seen.add(c[0])
                out.append(c)
        return out

    def hover(self, path, word):
        """返回 (标题, 详情) 或 None。"""
        doc = KW_DOCS.get(word)
        if doc:
            return doc
        fa = self.files.get(path)
        if fa and word in fa.funcs:
            fs = fa.funcs[word]
            params = ", ".join(fs.params) if fs.params else "无"
            detail = (f"参数: {params}  (arity={fs.arity})\n"
                      f"调用者: {', '.join(sorted(fs.callers)) or '无'}\n"
                      f"被调用: {', '.join(sorted(fs.callees)) or '无'}")
            return (f"函数 {word}({params})", detail)
        if word in self._all_natives:
            ns = self._all_natives[word]
            ret = O.CODE_TO_TYPE.get(ns.ret_type, "?")
            pts = ",".join(O.CODE_TO_TYPE.get(c, "?") for c in ns.param_types)
            return (f"原生函数 {word}({pts}) -> {ret}",
                    f"签名: {ns.sig}\n来自库: {os.path.basename(ns.lib)}\n"
                    f"定义文件: {os.path.basename(ns.def_file)}")
        return None
