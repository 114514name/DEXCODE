"""Python 版 DEXC 虚拟机 — 供 IDE 调试使用。

与 vm/vm.c 语义一致,但指令使用汇编 IR(Insn),pc 为函数内指令下标,
每条指令携带源码行号,因此天然支持"源码级断点/单步"。

功能:
- 断点(按行)、单步进入/跳过/跳出、继续、重启
- 调用栈、各帧局部变量、值栈
- 通过 ctypes 调用 DLL 原生函数(NCALL)
"""

import ctypes
import os
import tempfile

from . import opcodes as O
from .ir import (
    ConstOperand, LocalOperand, LabelOperand, FuncOperand, NativeOperand,
)

INT64_MASK = (1 << 64) - 1
INT64_SIGN = 1 << 63

# 结构体值语义(P3)的深度上限;环已由编译期"拒绝递归类型"排除,这里仅作防御
MAX_OBJ_DEPTH = 64


def copy_value(v, depth=0):
    """结构体的值拷贝。与 C VM 的 copy_value 语义一致。

    源码表面把结构体写成值,因此赋值/传参时必须拷贝,避免两个局部槽共享同一对象。
    字符串与数字是不可变的,直接返回。"""
    if not isinstance(v, dict) or depth >= MAX_OBJ_DEPTH:
        return v
    return {"type": v["type"],
            "fields": [copy_value(x, depth + 1) for x in v["fields"]]}


def wrap64(x):
    """把整型算术结果包装回有符号 64 位(与 C VM 的 int64 溢出一致)。"""
    x = int(x) & INT64_MASK
    if x >= INT64_SIGN:
        x -= (1 << 64)
    return x


class VMRuntimeError(Exception):
    pass


class Frame:
    __slots__ = ("func", "func_idx", "locals", "base", "ret_pc")

    def __init__(self, func, func_idx, locals_, base, ret_pc):
        self.func = func          # AsmFunc
        self.func_idx = func_idx
        self.locals = locals_     # list[值]
        self.base = base          # 本帧在值栈中的起点(索引)
        self.ret_pc = ret_pc      # 返回后执行的指令下标(在调用者内)


class PyVM:
    def __init__(self, prog):
        self.prog = prog
        self.funcs = prog.funcs
        self.natives = prog.natives
        self.func_index = {f.name: i for i, f in enumerate(self.funcs)}
        self.native_index = {n.name: i for i, n in enumerate(self.natives)}

        # 预解析标签 → 指令下标(每函数)
        for f in self.funcs:
            labels = {}
            for idx, insn in enumerate(f.insns):
                if insn.op is None:
                    labels[insn.label] = idx
            f._label_index = labels

        self.stack = []           # 值栈(list,顶部在末尾)
        self.frames = []          # 帧栈(frames[0] 为 main)
        self.pc = 0               # 当前函数内指令下标
        self.done = False
        self.error = None
        self.output = []          # print 输出(逐行)

        self.breakpoints = set()          # 行号集合
        self._step_mode = None            # None/'into'/'over'/'out'
        self._step_arg = None
        self._step_start_line = 0
        self._stop_reason = None
        self._skip_bp_once = True         # 继续后第一次经过断点行不触发
        self._libs = {}                   # 库路径 -> ctypes 句柄
        self._lib_map = {l.path: l for l in (prog.libs or [])}  # 库路径 -> LibInfo
        self._lib_temp = {}               # 静态库路径 -> 解包出的临时文件
        self._native_fns = {}             # 原生名 -> 已配置的函数

        mainf = self.funcs[0]
        self.frames.append(Frame(mainf, 0, [None] * max(1, mainf.nlocals), 0, 0))
        self.pc = 0

    # ---------- 查询 ----------
    @property
    def depth(self):
        return len(self.frames)

    def current_frame(self):
        return self.frames[-1] if self.frames else None

    def current_func(self):
        return self.current_frame().func if self.frames else None

    def current_line(self):
        f = self.current_func()
        if f is None:
            return 0
        if 0 <= self.pc < len(f.insns):
            return f.insns[self.pc].line
        return 0

    def frame_info(self):
        """返回各帧调试信息:函数名、行号、局部变量(栈顶帧在前)。"""
        info = []
        for rank, fr in enumerate(reversed(self.frames)):
            insns = fr.func.insns
            if rank == 0:  # 活动帧:当前 pc
                line = insns[self.pc].line if 0 <= self.pc < len(insns) else 0
            else:
                line = 0
            locals_ = {}
            for i in range(fr.func.nlocals):
                locals_[f"%{i}"] = fr.locals[i] if i < len(fr.locals) else None
            info.append({"func": fr.func.name, "line": line, "locals": locals_})
        return info

    # ---------- 单步控制 ----------
    def step_into(self):
        self._step_mode = "into"
        self._step_arg = None
        self._step_start_line = self.current_line()
        self._skip_bp_once = True

    def step_over(self):
        self._step_mode = "over"
        self._step_arg = (len(self.frames), self.pc)
        self._step_start_line = self.current_line()
        self._skip_bp_once = True

    def step_out(self):
        self._step_mode = "out"
        self._step_arg = len(self.frames)
        self._step_start_line = self.current_line()
        self._skip_bp_once = True

    def run(self):
        self._step_mode = None
        self._step_arg = None
        self._skip_bp_once = True  # 继续后第一次经过断点行时不触发(避免原地重停)

    def _should_stop(self):
        line = self.current_line()
        skip_once = self._skip_bp_once
        self._skip_bp_once = False  # 每次求值后清除(恢复后仅第一次经过断点行被跳过)
        # 步进操作期间,忽略"起始行"的断点,避免原地反复停住
        bp_active = line in self.breakpoints and not skip_once and line != self._step_start_line
        if self._step_mode is None:
            if line in self.breakpoints and not skip_once:
                self._stop_reason = "breakpoint"
                return True
            return False
        if self._step_mode == "into":
            self._stop_reason = "step"
            return True
        if self._step_mode == "over":
            depth0, pc0 = self._step_arg
            if len(self.frames) < depth0 or (len(self.frames) == depth0 and self.pc != pc0):
                self._stop_reason = "step"
                return True
            if bp_active:
                self._stop_reason = "breakpoint"
                return True
            return False
        if self._step_mode == "out":
            if len(self.frames) < self._step_arg:
                self._stop_reason = "step"
                return True
            if bp_active:
                self._stop_reason = "breakpoint"
                return True
            return False
        return False

    # ---------- 值运算 ----------
    @staticmethod
    def _truthy(v):
        if isinstance(v, str):
            return v != ""
        return bool(v)

    @staticmethod
    def _cmp(a, b):
        """返回 -1/0/1;不可比较返回 None。"""
        if isinstance(a, dict) or isinstance(b, dict):
            # 对象仅支持 ==/!=:按类型名 + 字段递归比较
            if not (isinstance(a, dict) and isinstance(b, dict)):
                return None
            if a["type"] != b["type"] or len(a["fields"]) != len(b["fields"]):
                return None
            for x, y in zip(a["fields"], b["fields"]):
                c = PyVM._cmp(x, y)
                if c is None:
                    return None
                if c != 0:
                    return c
            return 0
        try:
            if isinstance(a, str) and isinstance(b, str):
                return -1 if a < b else (1 if a > b else 0)
            if isinstance(a, (int, float)) and isinstance(b, (int, float)):
                return -1 if a < b else (1 if a > b else 0)
            if isinstance(a, str) != isinstance(b, str):
                return None
            return -1 if a < b else (1 if a > b else 0)
        except TypeError:
            return None

    def _binop(self, op, a, b):
        if op in (O.ADD, O.SUB, O.MUL, O.DIV, O.MOD):
            if isinstance(a, str) or isinstance(b, str):
                raise VMRuntimeError("unsupported operand types for arithmetic")
            if op == O.MOD:
                if not (isinstance(a, int) and isinstance(b, int)):
                    raise VMRuntimeError("MOD requires integer operands")
                if b == 0:
                    raise VMRuntimeError("division by zero")
                return wrap64(a % b)
            if op == O.DIV:
                if b == 0:
                    raise VMRuntimeError("division by zero")
                return a / b
            if isinstance(a, int) and isinstance(b, int):
                if op == O.ADD:
                    return wrap64(a + b)
                if op == O.SUB:
                    return wrap64(a - b)
                return wrap64(a * b)
            # 至少一个浮点
            if op == O.ADD:
                return float(a) + float(b)
            if op == O.SUB:
                return float(a) - float(b)
            return float(a) * float(b)

        c = self._cmp(a, b)
        if c is None:
            if op == O.EQ:
                return 0
            if op == O.NE:
                return 1
            raise VMRuntimeError("cannot order values of these types")
        if op == O.EQ:
            return 1 if c == 0 else 0
        if op == O.NE:
            return 1 if c != 0 else 0
        if op == O.LT:
            return 1 if c < 0 else 0
        if op == O.LE:
            return 1 if c <= 0 else 0
        if op == O.GT:
            return 1 if c > 0 else 0
        return 1 if c >= 0 else 0

    # ---------- 字符串拼接 ----------
    @staticmethod
    def _fmt_num(v):
        """数字 → 字符串,格式与 C VM 的 %g 近似一致。"""
        if isinstance(v, bool):
            return "1" if v else "0"
        if isinstance(v, float):
            if v == int(v) and abs(v) < 1e16:
                return str(int(v))
            return repr(v)
        return str(v)

    def _concat(self, a, b):
        s = (a if isinstance(a, str) else self._fmt_num(a)) + \
            (b if isinstance(b, str) else self._fmt_num(b))
        return s

    # ---------- 原生调用 ----------
    def _resolve_native(self, native):
        fn = self._native_fns.get(native.name)
        if fn is not None:
            return fn
        h = self._libs.get(native.lib)
        if h is None:
            path = native.lib
            lib = self._lib_map.get(native.lib)
            if lib is not None and lib.is_static:
                # 静态链接:把内嵌字节解包到临时文件再加载(无需外部 DLL)
                if native.lib not in self._lib_temp:
                    suffix = os.path.splitext(lib.path)[1] or ".dll"
                    fd, tmp = tempfile.mkstemp(suffix=suffix)
                    with os.fdopen(fd, "wb") as f:
                        f.write(lib.data)
                    self._lib_temp[native.lib] = tmp
                path = self._lib_temp[native.lib]
            try:
                h = ctypes.CDLL(path)
            except OSError:
                h = ctypes.WinDLL(path)
            self._libs[native.lib] = h
        fn = getattr(h, native.name)
        tmap = {O.NAT_INT: ctypes.c_int64, O.NAT_FLOAT: ctypes.c_double, O.NAT_STR: ctypes.c_char_p}
        fn.argtypes = [tmap[c] for c in native.param_types]
        fn.restype = {
            O.NAT_VOID: None, O.NAT_INT: ctypes.c_int64,
            O.NAT_FLOAT: ctypes.c_double, O.NAT_STR: ctypes.c_char_p,
        }[native.ret_type]
        self._native_fns[native.name] = fn
        return fn

    def _ncall(self, native):
        arity = native.arity
        if len(self.stack) < arity:
            raise VMRuntimeError("stack underflow (native arguments)")
        base = len(self.stack) - arity
        args = self.stack[base:]
        del self.stack[base:]
        cargs = []
        for k in range(arity):
            v = args[k]
            t = native.param_types[k]
            if t == O.NAT_INT:
                if isinstance(v, bool):
                    v = int(v)
                if isinstance(v, int):
                    cargs.append(v)
                elif isinstance(v, float):
                    cargs.append(int(v))
                else:
                    raise VMRuntimeError(f"native '{native.name}' arg {k + 1} expects int")
            elif t == O.NAT_FLOAT:
                if isinstance(v, (int, float)) and not isinstance(v, bool):
                    cargs.append(float(v))
                else:
                    raise VMRuntimeError(f"native '{native.name}' arg {k + 1} expects float")
            elif t == O.NAT_STR:
                if isinstance(v, str):
                    cargs.append(v.encode("utf-8"))
                else:
                    raise VMRuntimeError(f"native '{native.name}' arg {k + 1} expects string")
        fn = self._resolve_native(native)
        res = fn(*cargs)
        if native.ret_type == O.NAT_VOID:
            return 0
        if native.ret_type == O.NAT_INT:
            return wrap64(res)
        if native.ret_type == O.NAT_FLOAT:
            return float(res)
        return res.decode("utf-8") if res else ""

    # ---------- 执行 ----------
    def step_once(self):
        if self.done or self.error:
            return
        f = self.current_frame()
        insns = f.func.insns
        if self.pc < 0 or self.pc >= len(insns):
            self.error = "pc out of code section"
            return
        insn = insns[self.pc]
        op = insn.op
        if op is None:  # 标签
            self.pc += 1
            return
        operand = insn.operand

        try:
            if op == O.PUSH:
                self.stack.append(operand.value)
                self.pc += 1
            elif op == O.LOAD:
                self.stack.append(f.locals[operand.index])
                self.pc += 1
            elif op == O.STORE:
                f.locals[operand.index] = copy_value(self.stack.pop())
                self.pc += 1
            elif op in (O.ADD, O.SUB, O.MUL, O.DIV, O.MOD, O.EQ, O.NE,
                        O.LT, O.LE, O.GT, O.GE):
                if len(self.stack) < 2:
                    raise VMRuntimeError("stack underflow")
                b = self.stack.pop()
                a = self.stack.pop()
                self.stack.append(self._binop(op, a, b))
                self.pc += 1
            elif op == O.CONCAT:
                if len(self.stack) < 2:
                    raise VMRuntimeError("stack underflow")
                b = self.stack.pop()
                a = self.stack.pop()
                self.stack.append(self._concat(a, b))
                self.pc += 1
            elif op == O.NEG:
                v = self.stack.pop()
                if isinstance(v, str):
                    raise VMRuntimeError("cannot negate a string")
                self.stack.append(-v)
                self.pc += 1
            elif op == O.NOT:
                v = self.stack.pop()
                self.stack.append(0 if self._truthy(v) else 1)
                self.pc += 1
            elif op in (O.AND, O.OR):
                if len(self.stack) < 2:
                    raise VMRuntimeError("stack underflow")
                b = self.stack.pop()
                a = self.stack.pop()
                self.stack.append(1 if (self._truthy(a) and self._truthy(b)
                                        if op == O.AND
                                        else self._truthy(a) or self._truthy(b)) else 0)
                self.pc += 1
            elif op in (O.JMP, O.JZ, O.JNZ):
                target = f.func._label_index[operand.name]
                if op == O.JMP:
                    self.pc = target
                else:
                    v = self.stack.pop()
                    take = (not self._truthy(v)) if op == O.JZ else self._truthy(v)
                    self.pc = target if take else self.pc + 1
            elif op == O.CALL:
                fi = self.func_index[operand.name]
                callee = self.funcs[fi]
                arity = callee.arity
                if len(self.stack) < arity:
                    raise VMRuntimeError("stack underflow (missing arguments)")
                base = len(self.stack) - arity
                loc = [None] * max(1, callee.nlocals)
                for i in range(arity):
                    loc[i] = copy_value(self.stack[base + i])
                del self.stack[base:]
                self.frames.append(Frame(callee, fi, loc, base, self.pc + 1))
                self.pc = 0
            elif op == O.CALL_NAME:
                # 栈 [args..., name, argc]:弹 argc、name,剩余 argc 个即实参
                if not self.stack:
                    raise VMRuntimeError("stack underflow")
                argc = self.stack.pop()
                if not isinstance(argc, int):
                    raise VMRuntimeError("call() arity must be an int")
                if not self.stack:
                    raise VMRuntimeError("stack underflow (call name)")
                name = self.stack.pop()
                if not isinstance(name, str):
                    raise VMRuntimeError("call() name must be a string")
                fi = self.func_index.get(name)
                if fi is None:
                    raise VMRuntimeError(f"unknown function '{name}' (call)")
                callee = self.funcs[fi]
                if argc != callee.arity:
                    raise VMRuntimeError(
                        f"function '{name}' expects {callee.arity} argument(s), got {argc}")
                if len(self.stack) < argc:
                    raise VMRuntimeError("stack underflow (call arguments)")
                base = len(self.stack) - argc
                loc = [None] * max(1, callee.nlocals)
                for i in range(argc):
                    loc[i] = copy_value(self.stack[base + i])
                del self.stack[base:]
                self.frames.append(Frame(callee, fi, loc, base, self.pc + 1))
                self.pc = 0
            elif op == O.RET:
                if not self.stack:
                    raise VMRuntimeError("stack underflow on RET")
                rv = self.stack.pop()
                if len(self.frames) == 1:  # main 返回 → 结束
                    self.done = True
                    self._stop_reason = "end"
                    return
                fr = self.frames.pop()
                del self.stack[fr.base:]
                self.stack.append(rv)
                self.pc = fr.ret_pc
            elif op == O.NCALL:
                ni = self.native_index[operand.name]
                native = self.natives[ni]
                self.stack.append(self._ncall(native))
                self.pc += 1
            elif op == O.MAKE_OBJ:
                n = operand.nfields
                if len(self.stack) < n:
                    raise VMRuntimeError("stack underflow (object fields)")
                fields = self.stack[-n:] if n else []
                if n:
                    del self.stack[-n:]
                self.stack.append({"type": operand.name, "fields": fields})
                self.pc += 1
            elif op == O.GET_FIELD:
                v = self.stack.pop()
                if not isinstance(v, dict):
                    raise VMRuntimeError("GET_FIELD on non-object")
                self.stack.append(v["fields"][operand.index])
                self.pc += 1
            elif op == O.SET_FIELD:
                if len(self.stack) < 2:
                    raise VMRuntimeError("stack underflow")
                val = self.stack.pop()
                obj = self.stack.pop()
                if not isinstance(obj, dict):
                    raise VMRuntimeError("SET_FIELD on non-object")
                obj["fields"][operand.index] = val
                self.pc += 1
            elif op == O.PRINT:
                v = self.stack.pop()
                self.output.append(_format_value(v))
                self.pc += 1
            elif op == O.POP:
                self.stack.pop()
                self.pc += 1
            elif op == O.DUP:
                self.stack.append(self.stack[-1])
                self.pc += 1
            elif op == O.NOP:
                self.pc += 1
            elif op == O.HALT:
                self.done = True
                self._stop_reason = "end"
                return
            else:
                raise VMRuntimeError(f"unknown opcode 0x{op:02X}")
        except VMRuntimeError as e:
            self.error = f"runtime error in function '{f.func.name}': {e}"
            self._stop_reason = "error"

    def dispose(self):
        """释放 ctypes 库句柄并删除静态库解包出的临时文件。

        pyvm 此前用 tempfile.mkstemp 安全地创建临时文件却从不删除,
        长时间反复调试会不断累积残留。IDE 停止调试时调用本方法清理。"""
        self._native_fns.clear()
        self._libs.clear()
        for tmp in getattr(self, "_lib_temp", {}).values():
            try:
                os.unlink(tmp)
            except OSError:
                pass
        self._lib_temp.clear()

    def run_until_stop(self, step_limit=10_000_000):
        """执行直到断点/单步条件满足或结束。"""
        while not self.done and self.error is None:
            self.step_once()
            if self._should_stop():
                return True
            step_limit -= 1
            if step_limit <= 0:
                self.error = "execution limit exceeded"
                self._stop_reason = "error"
                return False
        return False


def _format_value(v, nested=False):
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, float):
        return ("%g" % v)
    if isinstance(v, str):
        return ('"' + v + '"') if nested else v
    if isinstance(v, dict):
        inner = ", ".join(_format_value(x, True) for x in v["fields"])
        return "%s(%s)" % (v["type"], inner)
    return str(v)


def run_program(prog, breakpoints=()):
    """便捷入口:直接运行到结束,返回 (output_lines, error)。"""
    vm = PyVM(prog)
    vm.breakpoints = set(breakpoints)
    vm.run()
    vm.run_until_stop()
    out, err = vm.output, vm.error
    vm.dispose()          # 释放库句柄 + 删除静态库临时文件
    return out, err
