/* ============================================================================
 * dexc_parse.c — 递归下降语法分析器:Token 流 → AST(dexlang/parser.py 的移植)。
 *
 * 报错文本与 Python 版逐字节一致(`expected {what}, got {lexeme!r}`),因为
 * tests/test_dexc.py 会比较两边的错误输出。Python 的空列表为假也照搬:
 * `else { }` 在 Python 里 els == [] 是假值,于是不生成 JMP end —— 所以
 * `_if` 必须用 els.len > 0 判断,而不是"有没有 else 关键字"。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "dexc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Token *toks;
    int n, pos;
    const char *filename;
} Parser;

static Node *node_new(int kind, int line, int col)
{
    Node *n = dx_malloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->kind = kind;
    n->line = line;
    n->col = col;
    return n;
}

static void nl_push(NodeList *l, Node *n)
{
    DXV_PUSH(l->items, l->len, l->cap, n);
}

static void sl_push(StrList *l, char *s)
{
    DXV_PUSH(l->items, l->len, l->cap, s);
}

static void nf_push(Node *n, const char *name, Node *value, const char *type_name)
{
    NameVal fv;
    fv.name = dx_strdup(name);
    fv.value = value;
    fv.type_name = type_name ? dx_strdup(type_name) : NULL;
    if (n->nfields == n->capfields) {
        n->capfields = n->capfields ? n->capfields * 2 : 8;
        n->fields = dx_realloc(n->fields, (size_t)n->capfields * sizeof *n->fields);
    }
    n->fields[n->nfields++] = fv;
}

/* ------------------------------------------------------------ 基础工具 */

static Token *peek(Parser *p, int n)
{
    int i = p->pos + n;
    return (i < p->n) ? &p->toks[i] : &p->toks[p->n - 1];
}

static Token *advance(Parser *p)
{
    Token *t = &p->toks[p->pos];
    if (t->kind != TK_EOF) p->pos++;
    return t;
}

static int at(Parser *p, int kind) { return peek(p, 0)->kind == kind; }

static int check(Parser *p, int kind)
{
    if (at(p, kind)) { advance(p); return 1; }
    return 0;
}

static Token *expect(Parser *p, int kind, const char *what)
{
    Token *t = peek(p, 0);
    if (t->kind != kind) {
        dx_throw("parser", t->line, t->col, "expected %s, got %s", what,
                 dx_py_repr_str(t->lexeme));
    }
    return advance(p);
}

static Node *loc(Node *node, Token *tok)
{
    node->line = tok->line;
    node->col = tok->col;
    return node;
}

static void raise_here(Parser *p, const char *msg)
{
    Token *t = peek(p, 0);
    dx_throw("parser", t->line, t->col, "%s", msg);
}

/* ------------------------------------------------------------ 前置声明 */

static Node *parse_statement(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_expression(Parser *p);
static Node *parse_primary(Parser *p);
static Node *parse_lvalue(Parser *p);

/* ------------------------------------------------------------ 语句 */

static Node *parse_program(Parser *p)
{
    Node *prog = node_new(N_PROGRAM, 0, 0);
    while (!at(p, TK_EOF)) nl_push(&prog->stmts, parse_statement(p));
    return prog;
}

static Node *parse_func(Parser *p, Token *start)
{
    Node *n = node_new(N_FUNC, start->line, start->col);
    advance(p);
    n->name = dx_strdup(expect(p, TK_IDENT, "function name")->lexeme);
    expect(p, TK_LPAREN, "'('");
    if (!at(p, TK_RPAREN)) {
        sl_push(&n->params, dx_strdup(expect(p, TK_IDENT, "parameter name")->lexeme));
        {
            char *pt = "";
            if (at(p, TK_COLON)) {
                advance(p);
                pt = expect(p, TK_IDENT, "parameter type")->lexeme;
            }
            sl_push(&n->param_types, dx_strdup(pt));
        }
        while (check(p, TK_COMMA)) {
            sl_push(&n->params,
                    dx_strdup(expect(p, TK_IDENT, "parameter name")->lexeme));
            {
                char *pt = "";
                if (at(p, TK_COLON)) {
                    advance(p);
                    pt = expect(p, TK_IDENT, "parameter type")->lexeme;
                }
                sl_push(&n->param_types, dx_strdup(pt));
            }
        }
    }
    expect(p, TK_RPAREN, "')'");
    n->sval = dx_strdup("");
    if (at(p, TK_ARROW)) {
        advance(p);
        n->sval = dx_strdup(expect(p, TK_IDENT, "return type")->lexeme);
    }
    {
        Node *blk = parse_block(p);
        n->stmts = blk->stmts;
    }
    return loc(n, start);
}

static Node *parse_statement(Parser *p)
{
    Token *start = peek(p, 0);
    int k = start->kind;

    if (k == TK_LET) {
        Node *n = node_new(N_LET, start->line, start->col);
        advance(p);
        n->name = dx_strdup(expect(p, TK_IDENT, "variable name")->lexeme);
        expect(p, TK_EQ, "'='");
        n->value = parse_expression(p);
        expect(p, TK_SEMI, "';'");
        return loc(n, start);
    }
    if (k == TK_PRINT) {
        Node *n = node_new(N_PRINT, start->line, start->col);
        advance(p);
        nl_push(&n->exprs, parse_expression(p));
        while (check(p, TK_COMMA)) nl_push(&n->exprs, parse_expression(p));
        expect(p, TK_SEMI, "';'");
        return loc(n, start);
    }
    if (k == TK_IF) {
        Node *n = node_new(N_IF, start->line, start->col);
        Node *blk;
        advance(p);
        n->value = parse_expression(p);          /* cond */
        blk = parse_block(p);
        n->stmts = blk->stmts;                   /* then */
        if (check(p, TK_ELSE)) {
            if (at(p, TK_IF)) {
                nl_push(&n->els, parse_statement(p));   /* else if */
            } else {
                Node *eb = parse_block(p);
                n->els = eb->stmts;
            }
        }
        return loc(n, start);
    }
    if (k == TK_WHILE) {
        Node *n = node_new(N_WHILE, start->line, start->col);
        Node *blk;
        advance(p);
        n->value = parse_expression(p);
        blk = parse_block(p);
        n->stmts = blk->stmts;
        return loc(n, start);
    }
    if (k == TK_FUNC) return parse_func(p, start);
    if (k == TK_RETURN) {
        Node *n = node_new(N_RETURN, start->line, start->col);
        advance(p);
        if (!at(p, TK_SEMI)) n->value = parse_expression(p);
        expect(p, TK_SEMI, "';'");
        return loc(n, start);
    }
    if (k == TK_TYPE) {
        Node *n = node_new(N_TYPEDEF, start->line, start->col);
        advance(p);
        n->name = dx_strdup(expect(p, TK_IDENT, "type name")->lexeme);
        expect(p, TK_LBRACE, "'{'");
        while (!at(p, TK_RBRACE)) {
            char *fname = expect(p, TK_IDENT, "field name")->lexeme;
            expect(p, TK_COLON, "':'");
            {
                char *ftype = expect(p, TK_IDENT, "field type")->lexeme;
                expect(p, TK_SEMI, "';'");
                nf_push(n, fname, NULL, ftype);
            }
        }
        expect(p, TK_RBRACE, "'}'");
        return loc(n, start);
    }
    if (k == TK_INCLUDE || k == TK_REFER) {
        Node *n = node_new(N_LIBREF, start->line, start->col);
        advance(p);
        n->name = dx_strdup((k == TK_INCLUDE) ? "include" : "refer");
        n->sval = dx_strdup(expect(p, TK_STRING, "library name/path string")->sval);
        expect(p, TK_SEMI, "';'");
        return loc(n, start);
    }
    if (k == TK_IDENT && (peek(p, 1)->kind == TK_EQ || peek(p, 1)->kind == TK_DOT)) {
        Node *lhs = parse_lvalue(p);
        Node *value;
        expect(p, TK_EQ, "'='");
        value = parse_expression(p);
        expect(p, TK_SEMI, "';'");
        if (lhs->kind == N_GETFIELD) {
            Node *n = node_new(N_SETFIELD, start->line, start->col);
            n->obj = lhs->obj;
            n->name = dx_strdup(lhs->name);
            n->value = value;
            return loc(n, start);
        } else {
            Node *n = node_new(N_ASSIGN, start->line, start->col);
            n->name = dx_strdup(lhs->name);
            n->value = value;
            return loc(n, start);
        }
    }
    {
        Node *n = node_new(N_EXPRSTMT, start->line, start->col);
        n->value = parse_expression(p);
        expect(p, TK_SEMI, "';'");
        return loc(n, start);
    }
}

static Node *parse_block(Parser *p)
{
    Node *blk;
    expect(p, TK_LBRACE, "'{'");
    blk = node_new(N_PROGRAM, 0, 0);
    while (!at(p, TK_RBRACE)) {
        if (at(p, TK_EOF)) raise_here(p, "expected '}' before end of file");
        nl_push(&blk->stmts, parse_statement(p));
    }
    advance(p);   /* '}' */
    return blk;
}

/* ------------------------------------------------------------ 表达式 */

static Node *binop(Node *left, Node *right, const char *op, Token *start)
{
    Node *n = node_new(N_BINOP, start->line, start->col);
    n->op = dx_strdup(op);
    n->left = left;
    n->right = right;
    return n;
}

static Node *parse_logical_or(Parser *p);
static Node *parse_logical_and(Parser *p);
static Node *parse_equality(Parser *p);
static Node *parse_comparison(Parser *p);
static Node *parse_term(Parser *p);
static Node *parse_factor(Parser *p);
static Node *parse_unary(Parser *p);

static Node *parse_logical_or(Parser *p)
{
    Token *start = peek(p, 0);
    Node *node = parse_logical_and(p);
    while (at(p, TK_OR)) {
        Node *right;
        advance(p);
        right = parse_logical_and(p);
        node = binop(node, right, "||", start);
    }
    return node;
}

static Node *parse_logical_and(Parser *p)
{
    Token *start = peek(p, 0);
    Node *node = parse_equality(p);
    while (at(p, TK_AND)) {
        Node *right;
        advance(p);
        right = parse_equality(p);
        node = binop(node, right, "&&", start);
    }
    return node;
}

static Node *parse_equality(Parser *p)
{
    Token *start = peek(p, 0);
    Node *node = parse_comparison(p);
    for (;;) {
        const char *op = NULL;
        Node *right;
        if (at(p, TK_EQEQ)) { advance(p); op = "=="; }
        else if (at(p, TK_NE)) { advance(p); op = "!="; }
        else break;
        right = parse_comparison(p);
        node = binop(node, right, op, start);
    }
    return node;
}

static Node *parse_comparison(Parser *p)
{
    Token *start = peek(p, 0);
    Node *node = parse_term(p);
    for (;;) {
        const char *op = NULL;
        Node *right;
        if (at(p, TK_LT)) { advance(p); op = "<"; }
        else if (at(p, TK_LE)) { advance(p); op = "<="; }
        else if (at(p, TK_GT)) { advance(p); op = ">"; }
        else if (at(p, TK_GE)) { advance(p); op = ">="; }
        else break;
        right = parse_term(p);
        node = binop(node, right, op, start);
    }
    return node;
}

static Node *parse_term(Parser *p)
{
    Token *start = peek(p, 0);
    Node *node = parse_factor(p);
    for (;;) {
        const char *op = NULL;
        Node *right;
        if (at(p, TK_PLUS)) { advance(p); op = "+"; }
        else if (at(p, TK_MINUS)) { advance(p); op = "-"; }
        else break;
        right = parse_factor(p);
        node = binop(node, right, op, start);
    }
    return node;
}

static Node *parse_factor(Parser *p)
{
    Token *start = peek(p, 0);
    Node *node = parse_unary(p);
    for (;;) {
        const char *op = NULL;
        Node *right;
        if (at(p, TK_STAR)) { advance(p); op = "*"; }
        else if (at(p, TK_SLASH)) { advance(p); op = "/"; }
        else if (at(p, TK_PERCENT)) { advance(p); op = "%"; }
        else break;
        right = parse_unary(p);
        node = binop(node, right, op, start);
    }
    return node;
}

static Node *parse_unary(Parser *p)
{
    Token *start = peek(p, 0);
    if (start->kind == TK_BANG) {
        Node *n = node_new(N_UNARYOP, start->line, start->col);
        advance(p);
        n->op = dx_strdup("!");
        n->value = parse_unary(p);
        return n;
    }
    if (start->kind == TK_MINUS) {
        Node *n = node_new(N_UNARYOP, start->line, start->col);
        advance(p);
        n->op = dx_strdup("-");
        n->value = parse_unary(p);
        return n;
    }
    return parse_primary(p);
}

static Node *parse_lvalue(Parser *p)
{
    Node *node = node_new(N_NAME, 0, 0);
    node->name = dx_strdup(expect(p, TK_IDENT, "name")->lexeme);
    while (check(p, TK_DOT)) {
        Node *g = node_new(N_GETFIELD, 0, 0);
        g->obj = node;
        g->name = dx_strdup(expect(p, TK_IDENT, "field name")->lexeme);
        node = g;
    }
    return node;
}

static Node *parse_primary(Parser *p)
{
    Token *start = peek(p, 0);
    Token *tok = start;

    if (tok->kind == TK_INT || tok->kind == TK_FLOAT || tok->kind == TK_STRING) {
        Node *n = node_new(N_LITERAL, start->line, start->col);
        advance(p);
        if (tok->kind == TK_INT) { n->lit_kind = LIT_INT; n->ival = tok->ival; }
        else if (tok->kind == TK_FLOAT) { n->lit_kind = LIT_FLOAT; n->fval = tok->fval; }
        else { n->lit_kind = LIT_STR; n->sval = dx_strdup(tok->sval); }
        return n;
    }
    if (tok->kind == TK_TRUE || tok->kind == TK_FALSE) {
        Node *n = node_new(N_LITERAL, start->line, start->col);
        advance(p);
        n->lit_kind = LIT_INT;
        n->ival = (tok->kind == TK_TRUE) ? 1 : 0;
        return n;
    }
    if (tok->kind == TK_IDENT) {
        advance(p);
        if (at(p, TK_LPAREN)) {
            Node *n = node_new(N_CALL, start->line, start->col);
            n->name = dx_strdup(tok->lexeme);
            advance(p);
            if (!at(p, TK_RPAREN)) {
                nl_push(&n->args, parse_expression(p));
                while (check(p, TK_COMMA)) nl_push(&n->args, parse_expression(p));
            }
            expect(p, TK_RPAREN, "')'");
            return n;
        }
        if (at(p, TK_LBRACE)) {
            Token *nxt = peek(p, 1);
            if (nxt->kind == TK_IDENT && peek(p, 2)->kind == TK_COLON) {
                Node *n = node_new(N_STRUCTLIT, start->line, start->col);
                n->name = dx_strdup(tok->lexeme);
                advance(p);
                while (!at(p, TK_RBRACE)) {
                    char *fname = expect(p, TK_IDENT, "field name")->lexeme;
                    Node *fval;
                    expect(p, TK_COLON, "':'");
                    fval = parse_expression(p);
                    nf_push(n, fname, fval, NULL);
                    if (!check(p, TK_COMMA)) break;
                }
                expect(p, TK_RBRACE, "'}'");
                return n;
            }
        }
        {
            Node *node = node_new(N_NAME, start->line, start->col);
            node->name = dx_strdup(tok->lexeme);
            while (at(p, TK_DOT)) {
                Node *g = node_new(N_GETFIELD, 0, 0);
                advance(p);
                g->obj = node;
                g->name = dx_strdup(expect(p, TK_IDENT, "field name")->lexeme);
                node = g;
            }
            return loc(node, start);
        }
    }
    if (tok->kind == TK_LPAREN) {
        Node *n;
        advance(p);
        n = parse_expression(p);
        expect(p, TK_RPAREN, "')'");
        return n;
    }
    raise_here(p, dx_aprintf("unexpected token %s in expression",
                             dx_py_repr_str(tok->lexeme)));
    return NULL;
}

static Node *parse_expression(Parser *p) { return parse_logical_or(p); }

Node *dx_parse(const Token *toks, int ntoks, const char *filename)
{
    Parser p;
    p.toks = (Token *)toks;
    p.n = ntoks;
    p.pos = 0;
    p.filename = filename;
    return parse_program(&p);
}
