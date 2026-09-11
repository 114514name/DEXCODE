"""递归下降语法分析器:Token 流 → AST。"""

from . import ast
from .errors import DexError
from .tokens import TokKind


class Parser:
    def __init__(self, tokens, filename="<source>"):
        self.tokens = tokens
        self.pos = 0
        self.filename = filename

    # ---------- 基础工具 ----------
    def _peek(self, n=0):
        i = self.pos + n
        return self.tokens[i] if i < len(self.tokens) else self.tokens[-1]

    def _advance(self):
        tok = self.tokens[self.pos]
        if tok.kind != TokKind.EOF:
            self.pos += 1
        return tok

    def _at(self, kind):
        return self._peek().kind == kind

    def _check(self, kind):
        if self._at(kind):
            self._advance()
            return True
        return False

    def _expect(self, kind, what):
        tok = self._peek()
        if tok.kind != kind:
            raise DexError(f"expected {what}, got {tok.lexeme!r}", tok.line, tok.col, "parser")
        return self._advance()

    def _loc(self, node, tok):
        node.line = tok.line
        node.col = tok.col
        return node

    def _raise(self, msg):
        tok = self._peek()
        raise DexError(msg, tok.line, tok.col, "parser")

    # ---------- 程序 / 语句 ----------
    def parse_program(self):
        stmts = []
        while not self._at(TokKind.EOF):
            stmts.append(self.parse_statement())
        return ast.Program(stmts=stmts)

    def parse_statement(self):
        start = self._peek()
        tok = start

        if tok.kind == TokKind.LET:
            self._advance()
            name = self._expect(TokKind.IDENT, "variable name").lexeme
            self._expect(TokKind.EQ, "'='")
            value = self.parse_expression()
            self._expect(TokKind.SEMI, "';'")
            return self._loc(ast.Let(name=name, value=value), start)

        if tok.kind == TokKind.PRINT:
            self._advance()
            exprs = [self.parse_expression()]
            while self._check(TokKind.COMMA):
                exprs.append(self.parse_expression())
            self._expect(TokKind.SEMI, "';'")
            return self._loc(ast.Print(exprs=exprs), start)

        if tok.kind == TokKind.IF:
            self._advance()
            cond = self.parse_expression()
            then = self.parse_block()
            els = None
            if self._check(TokKind.ELSE):
                if self._at(TokKind.IF):
                    els = [self.parse_statement()]  # else if
                else:
                    els = self.parse_block()
            return self._loc(ast.If(cond=cond, then=then, els=els), start)

        if tok.kind == TokKind.WHILE:
            self._advance()
            cond = self.parse_expression()
            body = self.parse_block()
            return self._loc(ast.While(cond=cond, body=body), start)

        if tok.kind == TokKind.FUNC:
            self._advance()
            name = self._expect(TokKind.IDENT, "function name").lexeme
            self._expect(TokKind.LPAREN, "'('")
            params = []
            param_types = []
            if not self._at(TokKind.RPAREN):
                pname = self._expect(TokKind.IDENT, "parameter name").lexeme
                ptype = ""
                if self._at(TokKind.COLON):
                    self._advance()
                    ptype = self._expect(TokKind.IDENT, "parameter type").lexeme
                params.append(pname)
                param_types.append(ptype)
                while self._check(TokKind.COMMA):   # _check 消费逗号
                    pname = self._expect(TokKind.IDENT, "parameter name").lexeme
                    ptype = ""
                    if self._at(TokKind.COLON):
                        self._advance()
                        ptype = self._expect(TokKind.IDENT, "parameter type").lexeme
                    params.append(pname)
                    param_types.append(ptype)
            self._expect(TokKind.RPAREN, "')'")
            ret_type = ""
            if self._at(TokKind.ARROW):
                self._advance()
                ret_type = self._expect(TokKind.IDENT, "return type").lexeme
            body = self.parse_block()
            return self._loc(ast.Func(name=name, params=params, body=body,
                                      param_types=param_types, ret_type=ret_type), start)

        if tok.kind == TokKind.RETURN:
            self._advance()
            value = None
            if not self._at(TokKind.SEMI):
                value = self.parse_expression()
            self._expect(TokKind.SEMI, "';'")
            return self._loc(ast.Return(value=value), start)

        # 自定义数据类型:type Name { field: type; ... }
        if tok.kind == TokKind.TYPE:
            self._advance()
            name = self._expect(TokKind.IDENT, "type name").lexeme
            self._expect(TokKind.LBRACE, "'{'")
            fields = []
            while not self._at(TokKind.RBRACE):
                fname = self._expect(TokKind.IDENT, "field name").lexeme
                self._expect(TokKind.COLON, "':'")
                ftype = self._expect(TokKind.IDENT, "field type").lexeme
                self._expect(TokKind.SEMI, "';'")
                fields.append((fname, ftype))
            self._expect(TokKind.RBRACE, "'}'")
            return self._loc(ast.TypeDef(name=name, fields=fields), start)

        # 库引入:include "库名";  /  refer "路径";
        if tok.kind == TokKind.INCLUDE or tok.kind == TokKind.REFER:
            kind = "include" if tok.kind == TokKind.INCLUDE else "refer"
            self._advance()
            target = self._expect(TokKind.STRING, "library name/path string").value
            self._expect(TokKind.SEMI, "';'")
            return self._loc(ast.LibRef(kind=kind, target=target), start)

        # 赋值 / 成员赋值: IDENT ('=' | '.'字段... '=') expr ';'
        if tok.kind == TokKind.IDENT and self._peek(1).kind in (TokKind.EQ, TokKind.DOT):
            lhs = self._parse_lvalue(start)
            self._expect(TokKind.EQ, "'='")
            value = self.parse_expression()
            self._expect(TokKind.SEMI, "';'")
            if isinstance(lhs, ast.GetField):
                return self._loc(ast.SetField(obj=lhs.obj, field=lhs.field, value=value), start)
            return self._loc(ast.Assign(name=lhs.name, value=value), start)

        # 表达式语句
        expr = self.parse_expression()
        self._expect(TokKind.SEMI, "';'")
        return self._loc(ast.ExprStmt(expr=expr), start)

    def parse_block(self):
        self._expect(TokKind.LBRACE, "'{'")
        stmts = []
        while not self._at(TokKind.RBRACE):
            if self._at(TokKind.EOF):
                self._raise("expected '}' before end of file")
            stmts.append(self.parse_statement())
        self._advance()  # '}'
        return stmts

    # ---------- 表达式(优先级爬升) ----------
    def parse_expression(self):
        return self._parse_logical_or()

    def _parse_logical_or(self):
        start = self._peek()
        node = self._parse_logical_and()
        while self._at(TokKind.OR):
            self._advance()
            right = self._parse_logical_and()
            node = self._loc(ast.BinOp(op="||", left=node, right=right), start)
        return node

    def _parse_logical_and(self):
        start = self._peek()
        node = self._parse_equality()
        while self._at(TokKind.AND):
            self._advance()
            right = self._parse_equality()
            node = self._loc(ast.BinOp(op="&&", left=node, right=right), start)
        return node

    def _parse_equality(self):
        start = self._peek()
        node = self._parse_comparison()
        while True:
            if self._at(TokKind.EQEQ):
                self._advance()
                op = "=="
            elif self._at(TokKind.NE):
                self._advance()
                op = "!="
            else:
                break
            right = self._parse_comparison()
            node = self._loc(ast.BinOp(op=op, left=node, right=right), start)
        return node

    def _parse_comparison(self):
        start = self._peek()
        node = self._parse_term()
        while True:
            if self._at(TokKind.LT):
                self._advance()
                op = "<"
            elif self._at(TokKind.LE):
                self._advance()
                op = "<="
            elif self._at(TokKind.GT):
                self._advance()
                op = ">"
            elif self._at(TokKind.GE):
                self._advance()
                op = ">="
            else:
                break
            right = self._parse_term()
            node = self._loc(ast.BinOp(op=op, left=node, right=right), start)
        return node

    def _parse_term(self):
        start = self._peek()
        node = self._parse_factor()
        while True:
            if self._at(TokKind.PLUS):
                self._advance()
                op = "+"
            elif self._at(TokKind.MINUS):
                self._advance()
                op = "-"
            else:
                break
            right = self._parse_factor()
            node = self._loc(ast.BinOp(op=op, left=node, right=right), start)
        return node

    def _parse_factor(self):
        start = self._peek()
        node = self._parse_unary()
        while True:
            if self._at(TokKind.STAR):
                self._advance()
                op = "*"
            elif self._at(TokKind.SLASH):
                self._advance()
                op = "/"
            elif self._at(TokKind.PERCENT):
                self._advance()
                op = "%"
            else:
                break
            right = self._parse_unary()
            node = self._loc(ast.BinOp(op=op, left=node, right=right), start)
        return node

    def _parse_unary(self):
        start = self._peek()
        tok = start
        if tok.kind == TokKind.BANG:
            self._advance()
            operand = self._parse_unary()
            return self._loc(ast.UnaryOp(op="!", operand=operand), start)
        if tok.kind == TokKind.MINUS:
            self._advance()
            operand = self._parse_unary()
            return self._loc(ast.UnaryOp(op="-", operand=operand), start)
        return self._parse_primary()

    def _parse_lvalue(self, start):
        """解析可赋值左值:IDENT 或 IDENT.field[.field...](返回 Name 或 GetField)。"""
        name = self._expect(TokKind.IDENT, "name").lexeme
        node = ast.Name(name=name)
        while self._check(TokKind.DOT):
            field = self._expect(TokKind.IDENT, "field name").lexeme
            node = ast.GetField(obj=node, field=field)
        return node

    def _parse_primary(self):
        start = self._peek()
        tok = start

        if tok.kind == TokKind.INT or tok.kind == TokKind.FLOAT:
            self._advance()
            return self._loc(ast.Literal(value=tok.value), start)
        if tok.kind == TokKind.STRING:
            self._advance()
            return self._loc(ast.Literal(value=tok.value), start)
        if tok.kind == TokKind.TRUE:
            self._advance()
            return self._loc(ast.Literal(value=1), start)
        if tok.kind == TokKind.FALSE:
            self._advance()
            return self._loc(ast.Literal(value=0), start)

        if tok.kind == TokKind.IDENT:
            self._advance()
            if self._at(TokKind.LPAREN):
                self._advance()
                args = []
                if not self._at(TokKind.RPAREN):
                    args.append(self.parse_expression())
                    while self._check(TokKind.COMMA):
                        args.append(self.parse_expression())
                self._expect(TokKind.RPAREN, "')'")
                return self._loc(ast.Call(name=tok.lexeme, args=args), start)
            if self._at(TokKind.LBRACE):
                # 消歧:仅当 '{' 后紧跟 "IDENT :" 才是结构体字面量 Name{field: v};
                # 否则(如 if/while 的语句块 '{')按普通标识符结束表达式。
                nxt = self._peek(1)
                if nxt.kind == TokKind.IDENT and self._peek(2).kind == TokKind.COLON:
                    self._advance()
                    fields = []
                    while not self._at(TokKind.RBRACE):
                        fname = self._expect(TokKind.IDENT, "field name").lexeme
                        self._expect(TokKind.COLON, "':'")
                        fval = self.parse_expression()
                        fields.append((fname, fval))
                        if not self._check(TokKind.COMMA):
                            break
                    self._expect(TokKind.RBRACE, "'}'")
                    return self._loc(ast.StructLit(type_name=tok.lexeme, fields=fields), start)
            node = ast.Name(name=tok.lexeme)
            while self._at(TokKind.DOT):
                self._advance()
                field = self._expect(TokKind.IDENT, "field name").lexeme
                node = ast.GetField(obj=node, field=field)
            return self._loc(node, start)

        if tok.kind == TokKind.LPAREN:
            self._advance()
            node = self.parse_expression()
            self._expect(TokKind.RPAREN, "')'")
            return node

        self._raise(f"unexpected token {tok.lexeme!r} in expression")
