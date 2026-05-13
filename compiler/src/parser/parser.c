#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── parser state ──────────────────────────────────────────────────────── */

typedef struct {
    TokenList  *tl;
    int         pos;
    const char *src_name;
    Arena      *arena;
    int         had_error;
} Parser;

/* ── helpers ───────────────────────────────────────────────────────────── */

static Token *cur(Parser *p) {
    return &p->tl->tokens[p->pos];
}

static Token *prev(Parser *p) {
    return &p->tl->tokens[p->pos - 1];
}

static int check(Parser *p, TokenKind k) {
    return cur(p)->kind == k;
}

static int match(Parser *p, TokenKind k) {
    if (check(p, k)) { p->pos++; return 1; }
    return 0;
}

static Token *expect(Parser *p, TokenKind k, const char *msg) {
    if (check(p, k)) { p->pos++; return prev(p); }
    Token *t = cur(p);
    fprintf(stderr, "%s:%d:%d: error: %s (got '%s')\n",
            p->src_name, t->line, t->col, msg, token_kind_name(t->kind));
    p->had_error = 1;
    return t;
}

static Node *node_new(Parser *p, NodeKind kind, int line, int col) {
    Node *n = arena_alloc(p->arena, sizeof(Node));
    n->kind = kind;
    n->line = line;
    n->col  = col;
    return n;
}

static char *intern(Parser *p, Token *t) {
    return arena_strdup(p->arena, t->start, t->len);
}

static void nodelist_push(Parser *p, NodeList *list, Node *n) {
    if (list->count % 8 == 0) {
        int   new_cap = list->count + 8;
        Node **arr    = arena_alloc(p->arena, (size_t)new_cap * sizeof(Node *));
        if (list->items)
            memcpy(arr, list->items, (size_t)list->count * sizeof(Node *));
        list->items = arr;
    }
    list->items[list->count++] = n;
}

static int is_path_token(TokenKind kind, int allow_self) {
    if (kind == TOK_IDENT)
        return 1;
    if (allow_self && kind == TOK_SELF)
        return 1;

    switch (kind) {
        case TOK_KERNEL:
        case TOK_NATIVE:
        case TOK_UI:
        case TOK_ANDROID:
        case TOK_DRIVER:
        case TOK_APP:
        case TOK_MODULE:
            return 1;
        default:
            return 0;
    }
}

static Token *expect_path_token(Parser *p, const char *msg, int allow_self) {
    if (is_path_token(cur(p)->kind, allow_self)) {
        p->pos++;
        return prev(p);
    }
    return expect(p, TOK_IDENT, msg);
}

static void append_type_token(char *buf, int *blen, int cap, Token *tok) {
    int part = tok->len;

    if (*blen >= cap - 1 || part <= 0)
        return;

    if (part > cap - 1 - *blen)
        part = cap - 1 - *blen;

    memcpy(buf + *blen, tok->start, (size_t)part);
    *blen += part;
}

/* ── forward declarations ──────────────────────────────────────────────── */

static Node *parse_type(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_expr(Parser *p);
static Node *parse_decl(Parser *p);

/* ── type parsing ──────────────────────────────────────────────────────── */

static Node *parse_type(Parser *p) {
    Token *t    = cur(p);
    int is_null = match(p, TOK_QUESTION);

    if (match(p, TOK_STAR)) {
        int is_const = match(p, TOK_CONST);
        Node *inner  = parse_type(p);
        Node *n      = node_new(p, NODE_TYPE_POINTER, t->line, t->col);
        n->as.type_ptr.inner      = inner;
        n->as.type_ptr.is_const   = is_const;
        n->as.type_ptr.is_nullable = is_null;
        return n;
    }

    if (match(p, TOK_LBRACKET)) {
        int length = -1;
        if (check(p, TOK_INT)) {
            length = (int)cur(p)->int_val;
            p->pos++;
        }
        expect(p, TOK_RBRACKET, "expected ']' in array/slice type");
        Node *elem = parse_type(p);
        Node *n    = node_new(p, NODE_TYPE_ARRAY, t->line, t->col);
        n->as.type_array.elem   = elem;
        n->as.type_array.length = length;
        return n;
    }

    /* named type: may be dotted path like std.io */
    char buf[256];
    int  blen = 0;

    Token *id = expect_path_token(p, "expected type name", 1);
    blen = id->len < 255 ? id->len : 255;
    memcpy(buf, id->start, (size_t)blen);
    while (check(p, TOK_DOT)) {
        p->pos++;
        Token *id = expect_path_token(p, "expected identifier after '.'", 0);
        if (blen < 254) { buf[blen++] = '.'; }
        int part = id->len < (255 - blen) ? id->len : (255 - blen);
        memcpy(buf + blen, id->start, (size_t)part);
        blen += part;
    }

    if (check(p, TOK_LBRACKET)) {
        int depth = 0;
        do {
            Token *tok = cur(p);
            if (tok->kind == TOK_LBRACKET)
                depth++;
            else if (tok->kind == TOK_RBRACKET)
                depth--;
            append_type_token(buf, &blen, (int)sizeof(buf), tok);
            p->pos++;
            if (tok->kind == TOK_EOF)
                break;
        } while (depth > 0 && !check(p, TOK_EOF));
    }
    buf[blen] = '\0';

    Node *n = node_new(p, NODE_TYPE_NAMED, t->line, t->col);
    n->as.type_named.name = arena_strdup(p->arena, buf, blen);
    return n;
}

/* ── expression parsing ────────────────────────────────────────────────── */

static Node *parse_primary(Parser *p);
static Node *parse_postfix(Parser *p);
static Node *parse_unary(Parser *p);
static Node *parse_cast(Parser *p);
static Node *parse_factor(Parser *p);
static Node *parse_term(Parser *p);
static Node *parse_shift(Parser *p);
static Node *parse_comparison(Parser *p);
static Node *parse_equality(Parser *p);
static Node *parse_bitwise_and(Parser *p);
static Node *parse_bitwise_xor(Parser *p);
static Node *parse_bitwise_or(Parser *p);
static Node *parse_logical_and(Parser *p);
static Node *parse_logical_or(Parser *p);
static Node *parse_assign(Parser *p);

static Node *parse_primary(Parser *p) {
    Token *t = cur(p);

    if (match(p, TOK_INT)) {
        Node *n = node_new(p, NODE_LIT_INT, t->line, t->col);
        n->as.lit_int.value = prev(p)->int_val;
        return n;
    }
    if (match(p, TOK_FLOAT)) {
        Node *n = node_new(p, NODE_LIT_FLOAT, t->line, t->col);
        n->as.lit_float.value = prev(p)->float_val;
        return n;
    }
    if (match(p, TOK_STRING)) {
        Node *n = node_new(p, NODE_LIT_STRING, t->line, t->col);
        n->as.lit_str.value = arena_strdup(p->arena,
                                           prev(p)->str_val,
                                           (int)strlen(prev(p)->str_val));
        return n;
    }
    if (match(p, TOK_CHAR)) {
        Node *n = node_new(p, NODE_LIT_CHAR, t->line, t->col);
        n->as.lit_int.value = prev(p)->int_val;
        return n;
    }
    if (match(p, TOK_TRUE)) {
        Node *n = node_new(p, NODE_LIT_BOOL, t->line, t->col);
        n->as.lit_int.value = 1;
        return n;
    }
    if (match(p, TOK_FALSE)) {
        Node *n = node_new(p, NODE_LIT_BOOL, t->line, t->col);
        n->as.lit_int.value = 0;
        return n;
    }
    if (match(p, TOK_NULL)) {
        return node_new(p, NODE_LIT_NULL, t->line, t->col);
    }

    if (match(p, TOK_MATCH)) {
        Node *subject = parse_expr(p);
        expect(p, TOK_LBRACE, "expected '{' after match subject");

        int   cap      = 8;
        char **patterns = arena_alloc(p->arena, (size_t)cap * sizeof(char *));
        Node **values   = arena_alloc(p->arena, (size_t)cap * sizeof(Node *));
        int    count    = 0;

        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            char pat[128];
            int  plen = 0;
            if (match(p, TOK_UNDERSCORE)) {
                memcpy(pat, "_", 1); plen = 1;
            } else {
                Token *en = expect(p, TOK_IDENT, "expected enum name or '_' in match arm");
                if (match(p, TOK_DOT)) {
                    Token *vr = expect(p, TOK_IDENT, "expected variant name");
                    plen = snprintf(pat, sizeof(pat), "%.*s.%.*s",
                                    en->len, en->start, vr->len, vr->start);
                } else {
                    plen = en->len < (int)sizeof(pat) - 1 ? en->len : (int)sizeof(pat) - 1;
                    memcpy(pat, en->start, (size_t)plen);
                }
            }
            expect(p, TOK_FAT_ARROW, "expected '=>' in match arm");
            Node *val = parse_expr(p);
            match(p, TOK_COMMA);

            if (count == cap) {
                cap *= 2;
                char **np = arena_alloc(p->arena, (size_t)cap * sizeof(char *));
                Node **nv = arena_alloc(p->arena, (size_t)cap * sizeof(Node *));
                memcpy(np, patterns, (size_t)count * sizeof(char *));
                memcpy(nv, values,   (size_t)count * sizeof(Node *));
                patterns = np; values = nv;
            }
            patterns[count] = arena_strdup(p->arena, pat, plen);
            values[count]   = val;
            count++;
        }
        expect(p, TOK_RBRACE, "expected '}' after match arms");

        Node *n = node_new(p, NODE_MATCH, t->line, t->col);
        n->as.match.subject  = subject;
        n->as.match.patterns = patterns;
        n->as.match.values   = values;
        n->as.match.count    = count;
        return n;
    }

    if (match(p, TOK_LPAREN)) {
        Node *inner = parse_expr(p);
        expect(p, TOK_RPAREN, "expected ')'");
        Node *n = node_new(p, NODE_GROUP, t->line, t->col);
        n->as.group.expr = inner;
        return n;
    }

    if (match(p, TOK_IDENT) || match(p, TOK_SELF)) {
        Token *id = prev(p);
        /* struct literal: Name { field: val, ... } */
        if (check(p, TOK_LBRACE)) {
            /* peek ahead: { } or { ident : ... } */
            int looks_like = 0;
            if (p->pos + 1 < p->tl->count) {
                TokenKind nk1 = p->tl->tokens[p->pos + 1].kind;
                if (nk1 == TOK_RBRACE) {
                    looks_like = 1;
                } else if (nk1 == TOK_IDENT && p->pos + 2 < p->tl->count) {
                    TokenKind nk2 = p->tl->tokens[p->pos + 2].kind;
                    if (nk2 == TOK_COLON) looks_like = 1;
                }
            }
            if (looks_like) {
                p->pos++; /* consume { */
                int    cap    = 8;
                char **fnames = arena_alloc(p->arena, (size_t)cap * sizeof(char *));
                Node **fvals  = arena_alloc(p->arena, (size_t)cap * sizeof(Node *));
                int    count  = 0;

                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    Token *fn_tok = expect(p, TOK_IDENT, "expected field name");
                    expect(p, TOK_COLON, "expected ':' after field name");
                    Node *fv = parse_expr(p);
                    match(p, TOK_COMMA);

                    if (count == cap) {
                        cap *= 2;
                        char **nn = arena_alloc(p->arena, (size_t)cap * sizeof(char *));
                        Node **nv = arena_alloc(p->arena, (size_t)cap * sizeof(Node *));
                        memcpy(nn, fnames, (size_t)count * sizeof(char *));
                        memcpy(nv, fvals,  (size_t)count * sizeof(Node *));
                        fnames = nn; fvals = nv;
                    }
                    fnames[count] = intern(p, fn_tok);
                    fvals[count]  = fv;
                    count++;
                }
                expect(p, TOK_RBRACE, "expected '}' after struct literal");

                Node *n = node_new(p, NODE_STRUCT_LIT, id->line, id->col);
                n->as.struct_lit.type_name    = intern(p, id);
                n->as.struct_lit.field_names  = fnames;
                n->as.struct_lit.field_values = fvals;
                n->as.struct_lit.count        = count;
                return n;
            }
        }
        Node *n = node_new(p, NODE_IDENT, id->line, id->col);
        n->as.ident.name = intern(p, id);
        return n;
    }

    fprintf(stderr, "%s:%d:%d: error: unexpected token '%s' in expression\n",
            p->src_name, t->line, t->col, token_kind_name(t->kind));
    p->had_error = 1;
    p->pos++;
    return node_new(p, NODE_LIT_NULL, t->line, t->col);
}

static Node *parse_postfix(Parser *p) {
    Node *expr = parse_primary(p);
    for (;;) {
        if (match(p, TOK_LPAREN)) {
            Token *pt = prev(p);
            NodeList args = {0};
            if (!check(p, TOK_RPAREN)) {
                do {
                    nodelist_push(p, &args, parse_expr(p));
                } while (match(p, TOK_COMMA));
            }
            expect(p, TOK_RPAREN, "expected ')' after arguments");
            Node *n = node_new(p, NODE_CALL, pt->line, pt->col);
            n->as.call.callee = expr;
            n->as.call.args   = args;
            expr = n;
            continue;
        }
        if (match(p, TOK_DOT)) {
            Token *field = expect(p, TOK_IDENT, "expected field name after '.'");
            Node *n = node_new(p, NODE_FIELD, field->line, field->col);
            n->as.field.object = expr;
            n->as.field.field  = intern(p, field);
            expr = n;
            continue;
        }
        if (match(p, TOK_LBRACKET)) {
            Token *bt  = prev(p);
            Node  *idx = parse_expr(p);
            expect(p, TOK_RBRACKET, "expected ']' after index");
            Node *n = node_new(p, NODE_INDEX, bt->line, bt->col);
            n->as.index_expr.object = expr;
            n->as.index_expr.index  = idx;
            expr = n;
            continue;
        }
        if (match(p, TOK_QUESTION)) {
            Token *op = prev(p);
            Node *n = node_new(p, NODE_UNARY, op->line, op->col);
            n->as.unary.op = arena_strdup(p->arena, op->start, op->len);
            n->as.unary.operand = expr;
            expr = n;
            continue;
        }
        /* postfix ++ / -- : desugar to compound assign */
        if (match(p, TOK_PLUSPLUS) || match(p, TOK_MINUSMINUS)) {
            Token *op  = prev(p);
            const char *cop = (op->kind == TOK_PLUSPLUS) ? "+" : "-";
            /* build integer literal 1 */
            Node *one = node_new(p, NODE_LIT_INT, op->line, op->col);
            one->as.lit_int.value = 1;
            Node *n = node_new(p, NODE_ASSIGN, op->line, op->col);
            n->as.assign.target = expr;
            n->as.assign.value  = one;
            n->as.assign.op     = arena_strdup(p->arena, cop, 1);
            expr = n;
            continue;
        }
        break;
    }
    return expr;
}

static Node *parse_unary(Parser *p) {
    Token *t = cur(p);
    if (match(p, TOK_BANG) || match(p, TOK_MINUS) ||
        match(p, TOK_STAR) || match(p, TOK_AMP)   ||
        match(p, TOK_TILDE)) {
        Token *op = prev(p);
        Node *operand = parse_unary(p);
        Node *n = node_new(p, NODE_UNARY, op->line, op->col);
        n->as.unary.op      = arena_strdup(p->arena, op->start, op->len);
        n->as.unary.operand = operand;
        return n;
    }
    (void)t;
    return parse_postfix(p);
}

static Node *parse_cast(Parser *p) {
    Node *expr = parse_unary(p);
    while (match(p, TOK_AS)) {
        Token *as = prev(p);
        Node *type = parse_type(p);
        Node *n = node_new(p, NODE_CAST, as->line, as->col);
        n->as.cast.expr = expr;
        n->as.cast.type = type;
        expr = n;
    }
    return expr;
}

static Node *parse_binop(Parser *p, Node *(*next)(Parser *),
                          TokenKind *ops, int nops) {
    Node *left = next(p);
    for (;;) {
        int matched = 0;
        for (int i = 0; i < nops; i++) {
            if (match(p, ops[i])) { matched = 1; break; }
        }
        if (!matched) break;
        Token *op    = prev(p);
        Node  *right = next(p);
        Node  *n     = node_new(p, NODE_BINARY, op->line, op->col);
        n->as.binary.op    = arena_strdup(p->arena, op->start, op->len);
        n->as.binary.left  = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static Node *parse_factor(Parser *p) {
    TokenKind ops[] = {TOK_STAR, TOK_SLASH, TOK_PERCENT};
    return parse_binop(p, parse_cast, ops, 3);
}
static Node *parse_term(Parser *p) {
    TokenKind ops[] = {TOK_PLUS, TOK_MINUS};
    return parse_binop(p, parse_factor, ops, 2);
}
static Node *parse_shift(Parser *p) {
    TokenKind ops[] = {TOK_LSHIFT, TOK_RSHIFT};
    return parse_binop(p, parse_term, ops, 2);
}
static Node *parse_comparison(Parser *p) {
    TokenKind ops[] = {TOK_LT, TOK_GT, TOK_LE, TOK_GE};
    return parse_binop(p, parse_shift, ops, 4);
}
static Node *parse_equality(Parser *p) {
    TokenKind ops[] = {TOK_EQEQ, TOK_NE};
    return parse_binop(p, parse_comparison, ops, 2);
}
static Node *parse_bitwise_and(Parser *p) {
    TokenKind ops[] = {TOK_AMP};
    return parse_binop(p, parse_equality, ops, 1);
}
static Node *parse_bitwise_xor(Parser *p) {
    TokenKind ops[] = {TOK_CARET};
    return parse_binop(p, parse_bitwise_and, ops, 1);
}
static Node *parse_bitwise_or(Parser *p) {
    TokenKind ops[] = {TOK_PIPE};
    return parse_binop(p, parse_bitwise_xor, ops, 1);
}
static Node *parse_logical_and(Parser *p) {
    TokenKind ops[] = {TOK_ANDAND};
    return parse_binop(p, parse_bitwise_or, ops, 1);
}
static Node *parse_logical_or(Parser *p) {
    TokenKind ops[] = {TOK_OROR};
    return parse_binop(p, parse_logical_and, ops, 1);
}

static Node *parse_assign(Parser *p) {
    Node *left = parse_logical_or(p);

    /* compound assignment operators */
    const char *compound_op = NULL;
    if      (match(p, TOK_PLUS_EQ))    compound_op = "+";
    else if (match(p, TOK_MINUS_EQ))   compound_op = "-";
    else if (match(p, TOK_STAR_EQ))    compound_op = "*";
    else if (match(p, TOK_SLASH_EQ))   compound_op = "/";
    else if (match(p, TOK_PERCENT_EQ)) compound_op = "%";

    if (compound_op) {
        Token *eq = prev(p);
        Node *val = parse_assign(p);
        Node *n   = node_new(p, NODE_ASSIGN, eq->line, eq->col);
        n->as.assign.target = left;
        n->as.assign.value  = val;
        n->as.assign.op     = arena_strdup(p->arena, compound_op, (int)strlen(compound_op));
        return n;
    }

    if (match(p, TOK_EQ)) {
        Token *eq  = prev(p);
        Node *val  = parse_assign(p);
        Node *n    = node_new(p, NODE_ASSIGN, eq->line, eq->col);
        n->as.assign.target = left;
        n->as.assign.value  = val;
        n->as.assign.op     = NULL;
        return n;
    }
    return left;
}

static Node *parse_expr(Parser *p) {
    return parse_assign(p);
}

/* ── statement parsing ─────────────────────────────────────────────────── */

static Node *parse_block(Parser *p) {
    Token *brace = expect(p, TOK_LBRACE, "expected '{'");
    Node *n = node_new(p, NODE_BLOCK, brace->line, brace->col);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF))
        nodelist_push(p, &n->as.block.stmts, parse_stmt(p));
    expect(p, TOK_RBRACE, "expected '}'");
    return n;
}

static Node *parse_stmt(Parser *p) {
    Token *t = cur(p);

    if (match(p, TOK_LET)) {
        Token *name = expect(p, TOK_IDENT, "expected variable name");
        Node *type  = NULL;
        Node *value = NULL;
        if (match(p, TOK_COLON)) type  = parse_type(p);
        if (match(p, TOK_EQ))    value = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';' after let");
        Node *n = node_new(p, NODE_LET, t->line, t->col);
        n->as.let.name  = intern(p, name);
        n->as.let.type  = type;
        n->as.let.value = value;
        return n;
    }

    if (match(p, TOK_CONST)) {
        Token *name = expect(p, TOK_IDENT, "expected constant name");
        Node *type  = NULL;
        if (match(p, TOK_COLON)) type = parse_type(p);
        expect(p, TOK_EQ, "expected '=' in const");
        Node *value = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';' after const");
        Node *n = node_new(p, NODE_CONST, t->line, t->col);
        n->as.konst.name  = intern(p, name);
        n->as.konst.type  = type;
        n->as.konst.value = value;
        return n;
    }

    if (match(p, TOK_RETURN)) {
        Node *val = NULL;
        if (!check(p, TOK_SEMICOLON)) val = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';' after return");
        Node *n = node_new(p, NODE_RETURN, t->line, t->col);
        n->as.ret.value = val;
        return n;
    }

    if (match(p, TOK_IF)) {
        Node *cond      = parse_expr(p);
        Node *then_blk  = parse_block(p);
        Node *else_node = NULL;
        if (match(p, TOK_ELSE)) {
            if (match(p, TOK_IF)) {
                else_node = parse_stmt(p);
                /* wrap in NODE_IF already returned above */
            } else {
                else_node = parse_block(p);
            }
        }
        Node *n = node_new(p, NODE_IF, t->line, t->col);
        n->as.if_stmt.cond       = cond;
        n->as.if_stmt.then_block = then_blk;
        n->as.if_stmt.else_node  = else_node;
        return n;
    }

    if (match(p, TOK_WHILE)) {
        Node *cond = parse_expr(p);
        Node *body = parse_block(p);
        Node *n = node_new(p, NODE_WHILE, t->line, t->col);
        n->as.while_stmt.cond = cond;
        n->as.while_stmt.body = body;
        return n;
    }

    if (match(p, TOK_FOR)) {
        /* for [init] ; [cond] ; [post] { body }
           init is a let/const decl or expression (no trailing ';' — the ';' is the separator)
           all three parts are optional */
        Node *init = NULL;
        Node *cond = NULL;
        Node *post = NULL;

        if (!check(p, TOK_SEMICOLON)) {
            if (check(p, TOK_LET)) {
                p->pos++; /* consume 'let' */
                Token *vname = expect(p, TOK_IDENT, "expected variable name");
                Node *vtype  = NULL;
                Node *vval   = NULL;
                if (match(p, TOK_COLON)) vtype = parse_type(p);
                if (match(p, TOK_EQ))    vval  = parse_expr(p);
                init = node_new(p, NODE_LET, t->line, t->col);
                init->as.let.name  = intern(p, vname);
                init->as.let.type  = vtype;
                init->as.let.value = vval;
            } else {
                Node *expr = parse_expr(p);
                init = node_new(p, NODE_EXPR_STMT, expr->line, expr->col);
                init->as.expr_stmt.expr = expr;
            }
        }
        expect(p, TOK_SEMICOLON, "expected ';' after for init");

        if (!check(p, TOK_SEMICOLON))
            cond = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';' after for condition");

        if (!check(p, TOK_LBRACE))
            post = parse_expr(p);

        Node *body = parse_block(p);
        Node *n    = node_new(p, NODE_FOR, t->line, t->col);
        n->as.for_stmt.init = init;
        n->as.for_stmt.cond = cond;
        n->as.for_stmt.post = post;
        n->as.for_stmt.body = body;
        return n;
    }

    if (match(p, TOK_LOOP)) {
        Node *body = parse_block(p);
        Node *n    = node_new(p, NODE_LOOP, t->line, t->col);
        n->as.loop_stmt.body = body;
        return n;
    }

    if (match(p, TOK_UNSAFE)) {
        Node *body = parse_block(p);
        Node *n    = node_new(p, NODE_UNSAFE, t->line, t->col);
        n->as.loop_stmt.body = body;
        return n;
    }

    if (match(p, TOK_BREAK)) {
        expect(p, TOK_SEMICOLON, "expected ';' after break");
        return node_new(p, NODE_BREAK, t->line, t->col);
    }

    if (match(p, TOK_CONTINUE)) {
        expect(p, TOK_SEMICOLON, "expected ';' after continue");
        return node_new(p, NODE_CONTINUE, t->line, t->col);
    }

    if (match(p, TOK_DEFER)) {
        Node *expr = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';' after defer");
        Node *n = node_new(p, NODE_DEFER, t->line, t->col);
        n->as.defer_stmt.expr = expr;
        return n;
    }

    /* expression statement */
    Node *expr = parse_expr(p);
    expect(p, TOK_SEMICOLON, "expected ';' after expression");
    Node *n = node_new(p, NODE_EXPR_STMT, t->line, t->col);
    n->as.expr_stmt.expr = expr;
    return n;
}

/* ── parameter list ────────────────────────────────────────────────────── */

static NodeList parse_params(Parser *p) {
    NodeList params = {0};
    expect(p, TOK_LPAREN, "expected '('");
    if (!check(p, TOK_RPAREN)) {
        do {
            Token *name;
            if (check(p, TOK_SELF)) {
                name = cur(p); p->pos++;
            } else {
                name = expect(p, TOK_IDENT, "expected parameter name");
            }
            expect(p, TOK_COLON, "expected ':' after parameter name");
            Node *type = parse_type(p);
            Node *param = node_new(p, NODE_LET, name->line, name->col);
            param->as.let.name = intern(p, name);
            param->as.let.type = type;
            nodelist_push(p, &params, param);
        } while (match(p, TOK_COMMA));
    }
    expect(p, TOK_RPAREN, "expected ')' after parameters");
    return params;
}

/* ── declaration parsing ───────────────────────────────────────────────── */

static Node *parse_fn_decl(Parser *p, int is_public, char *owner_type) {
    Token *t    = prev(p); /* 'fn' already consumed */
    Token *name = expect(p, TOK_IDENT, "expected function name");
    NodeList params = parse_params(p);
    expect(p, TOK_ARROW, "expected '->' before return type");
    Node *ret  = parse_type(p);
    Node *body = parse_block(p);

    Node *n = node_new(p, NODE_FN_DECL, t->line, t->col);
    n->as.fn.name       = intern(p, name);
    n->as.fn.owner_type = owner_type;
    n->as.fn.params     = params;
    n->as.fn.ret_type   = ret;
    n->as.fn.body       = body;
    n->as.fn.is_public  = is_public;
    return n;
}

static Node *parse_decl(Parser *p) {
    Token *t      = cur(p);
    int is_public = match(p, TOK_PUB);

    if (match(p, TOK_IMPL)) {
        Token *target = expect(p, TOK_IDENT, "expected type name after impl");
        char *iface_name = NULL;
        if (match(p, TOK_COLON)) {
            Token *iface = expect(p, TOK_IDENT, "expected interface name after ':'");
            iface_name = intern(p, iface);
        }
        expect(p, TOK_LBRACE, "expected '{' after impl target");
        NodeList methods = {0};
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            int mpub = match(p, TOK_PUB);
            expect(p, TOK_FN, "expected 'fn' in impl block");
            char *owner = arena_strdup(p->arena, target->start, target->len);
            nodelist_push(p, &methods, parse_fn_decl(p, mpub, owner));
        }
        expect(p, TOK_RBRACE, "expected '}' after impl block");
        Node *n = node_new(p, NODE_IMPL_DECL, t->line, t->col);
        n->as.impl.target         = intern(p, target);
        n->as.impl.interface_name = iface_name;
        n->as.impl.methods        = methods;
        return n;
    }

    if (match(p, TOK_INTERFACE)) {
        Token *name = expect(p, TOK_IDENT, "expected interface name");
        expect(p, TOK_LBRACE, "expected '{' after interface name");
        NodeList methods = {0};
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            match(p, TOK_PUB);
            expect(p, TOK_FN, "expected 'fn' in interface body");
            Token *fn_tok = prev(p);
            Token *mname  = expect(p, TOK_IDENT, "expected method name");
            NodeList params = parse_params(p);
            expect(p, TOK_ARROW, "expected '->' in interface method");
            Node *ret = parse_type(p);
            expect(p, TOK_SEMICOLON, "expected ';' after interface method");
            Node *m = node_new(p, NODE_FN_DECL, fn_tok->line, fn_tok->col);
            m->as.fn.name       = intern(p, mname);
            m->as.fn.owner_type = NULL;
            m->as.fn.params     = params;
            m->as.fn.ret_type   = ret;
            m->as.fn.body       = NULL;
            m->as.fn.is_public  = 1;
            nodelist_push(p, &methods, m);
        }
        expect(p, TOK_RBRACE, "expected '}' after interface");
        Node *n = node_new(p, NODE_INTERFACE_DECL, t->line, t->col);
        n->as.interface_decl.name      = intern(p, name);
        n->as.interface_decl.methods   = methods;
        n->as.interface_decl.is_public = is_public;
        return n;
    }

    if (match(p, TOK_PACKED)) {
        /* packed struct */
        expect(p, TOK_STRUCT, "expected 'struct' after 'packed'");
        Token *name = expect(p, TOK_IDENT, "expected struct name");
        expect(p, TOK_LBRACE, "expected '{' after struct name");
        NodeList fields = {0};
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            Token *fn_tok = expect(p, TOK_IDENT, "expected field name");
            expect(p, TOK_COLON, "expected ':' after field name");
            Node *ftype = parse_type(p);
            expect(p, TOK_SEMICOLON, "expected ';' after field");
            Node *field = node_new(p, NODE_LET, fn_tok->line, fn_tok->col);
            field->as.let.name = intern(p, fn_tok);
            field->as.let.type = ftype;
            nodelist_push(p, &fields, field);
        }
        expect(p, TOK_RBRACE, "expected '}' after packed struct");
        Node *n = node_new(p, NODE_STRUCT_DECL, t->line, t->col);
        n->as.struct_decl.name      = intern(p, name);
        n->as.struct_decl.fields    = fields;
        n->as.struct_decl.is_public = is_public;
        n->as.struct_decl.is_packed = 1;
        return n;
    }

    if (match(p, TOK_STRUCT)) {
        Token *name = expect(p, TOK_IDENT, "expected struct name");
        expect(p, TOK_LBRACE, "expected '{' after struct name");
        NodeList fields = {0};
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            Token *fn_tok = expect(p, TOK_IDENT, "expected field name");
            expect(p, TOK_COLON, "expected ':' after field name");
            Node *ftype = parse_type(p);
            expect(p, TOK_SEMICOLON, "expected ';' after field");
            Node *field = node_new(p, NODE_LET, fn_tok->line, fn_tok->col);
            field->as.let.name = intern(p, fn_tok);
            field->as.let.type = ftype;
            nodelist_push(p, &fields, field);
        }
        expect(p, TOK_RBRACE, "expected '}' after struct");
        Node *n = node_new(p, NODE_STRUCT_DECL, t->line, t->col);
        n->as.struct_decl.name      = intern(p, name);
        n->as.struct_decl.fields    = fields;
        n->as.struct_decl.is_public = is_public;
        return n;
    }

    if (match(p, TOK_ENUM)) {
        Token *name = expect(p, TOK_IDENT, "expected enum name");
        expect(p, TOK_LBRACE, "expected '{' after enum name");
        int       cap      = 8;
        char    **variants = arena_alloc(p->arena, (size_t)cap * sizeof(char *));
        NodeList *vfields  = arena_alloc(p->arena, (size_t)cap * sizeof(NodeList));
        int       count    = 0;
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            Token *v = expect(p, TOK_IDENT, "expected enum variant");
            if (count == cap) {
                cap *= 2;
                char    **nv = arena_alloc(p->arena, (size_t)cap * sizeof(char *));
                NodeList *nf = arena_alloc(p->arena, (size_t)cap * sizeof(NodeList));
                memcpy(nv, variants, (size_t)count * sizeof(char *));
                memcpy(nf, vfields,  (size_t)count * sizeof(NodeList));
                variants = nv; vfields = nf;
            }
            variants[count] = intern(p, v);
            memset(&vfields[count], 0, sizeof(NodeList));
            /* optional data fields: Variant(field: type, ...) */
            if (match(p, TOK_LPAREN)) {
                while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                    Token *fn_tok = expect(p, TOK_IDENT, "expected field name");
                    expect(p, TOK_COLON, "expected ':' after field name");
                    Node *ftype = parse_type(p);
                    Node *field = node_new(p, NODE_LET, fn_tok->line, fn_tok->col);
                    field->as.let.name = intern(p, fn_tok);
                    field->as.let.type = ftype;
                    nodelist_push(p, &vfields[count], field);
                    if (!check(p, TOK_RPAREN)) match(p, TOK_COMMA);
                }
                expect(p, TOK_RPAREN, "expected ')' after variant fields");
            }
            expect(p, TOK_SEMICOLON, "expected ';' after variant");
            count++;
        }
        expect(p, TOK_RBRACE, "expected '}' after enum");
        Node *n = node_new(p, NODE_ENUM_DECL, t->line, t->col);
        n->as.enum_decl.name           = intern(p, name);
        n->as.enum_decl.variants       = variants;
        n->as.enum_decl.variant_fields = vfields;
        n->as.enum_decl.count          = count;
        n->as.enum_decl.is_public      = is_public;
        return n;
    }

    if (match(p, TOK_UNION)) {
        Token *name = expect(p, TOK_IDENT, "expected union name");
        expect(p, TOK_LBRACE, "expected '{' after union name");
        NodeList fields = {0};
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            Token *fn_tok = expect(p, TOK_IDENT, "expected field name");
            expect(p, TOK_COLON, "expected ':' after field name");
            Node *ftype = parse_type(p);
            expect(p, TOK_SEMICOLON, "expected ';' after field");
            Node *field = node_new(p, NODE_LET, fn_tok->line, fn_tok->col);
            field->as.let.name = intern(p, fn_tok);
            field->as.let.type = ftype;
            nodelist_push(p, &fields, field);
        }
        expect(p, TOK_RBRACE, "expected '}' after union");
        Node *n = node_new(p, NODE_UNION_DECL, t->line, t->col);
        n->as.union_decl.name      = intern(p, name);
        n->as.union_decl.fields    = fields;
        n->as.union_decl.is_public = is_public;
        return n;
    }

    if (match(p, TOK_EXTERN)) {
        char *conv = NULL;
        if (check(p, TOK_STRING)) {
            conv = arena_strdup(p->arena, cur(p)->str_val,
                                (int)strlen(cur(p)->str_val));
            p->pos++;
        }
        expect(p, TOK_FN, "expected 'fn' after extern");
        Token *name     = expect(p, TOK_IDENT, "expected function name");
        NodeList params = parse_params(p);
        expect(p, TOK_ARROW, "expected '->'");
        Node *ret = parse_type(p);
        expect(p, TOK_SEMICOLON, "expected ';' after extern declaration");

        Node *n = node_new(p, NODE_EXTERN_FN, t->line, t->col);
        n->as.extern_fn.name         = intern(p, name);
        n->as.extern_fn.calling_conv = conv;
        n->as.extern_fn.params       = params;
        n->as.extern_fn.ret_type     = ret;
        n->as.extern_fn.is_public    = is_public;
        return n;
    }

    if (match(p, TOK_FN)) {
        return parse_fn_decl(p, is_public, NULL);
    }

    fprintf(stderr, "%s:%d:%d: error: unexpected token '%s' at top level\n",
            p->src_name, t->line, t->col, token_kind_name(t->kind));
    p->had_error = 1;
    p->pos++;
    return node_new(p, NODE_LIT_NULL, t->line, t->col);
}

/* ── entry point ───────────────────────────────────────────────────────── */

Node *parser_parse(TokenList *tl, const char *source_name, Arena *arena) {
    Parser p = { .tl = tl, .pos = 0, .src_name = source_name,
                 .arena = arena, .had_error = 0 };

    Node *prog = node_new(&p, NODE_PROGRAM, 1, 1);

    /* optional package declaration */
    if (match(&p, TOK_PACKAGE)) {
        Token *t = prev(&p);
        char buf[256]; int blen = 0;
        Token *id = expect_path_token(&p, "expected package name", 0);
        blen = id->len < 255 ? id->len : 255;
        memcpy(buf, id->start, (size_t)blen);
        while (check(&p, TOK_DOT)) {
            p.pos++;
            Token *part = expect_path_token(&p, "expected identifier after '.'", 0);
            if (blen < 254) buf[blen++] = '.';
            int pl = part->len < (255 - blen) ? part->len : (255 - blen);
            memcpy(buf + blen, part->start, (size_t)pl); blen += pl;
        }
        buf[blen] = '\0';
        expect(&p, TOK_SEMICOLON, "expected ';' after package");
        Node *pkg = node_new(&p, NODE_PACKAGE, t->line, t->col);
        pkg->as.pkg.path      = arena_strdup(arena, buf, blen);
        prog->as.program.package = pkg;
    }

    /* import declarations */
    while (match(&p, TOK_IMPORT)) {
        Token *t = prev(&p);
        char buf[256]; int blen = 0;
        Token *id = expect_path_token(&p, "expected import path", 0);
        blen = id->len < 255 ? id->len : 255;
        memcpy(buf, id->start, (size_t)blen);
        while (check(&p, TOK_DOT)) {
            p.pos++;
            Token *part = expect_path_token(&p, "expected identifier after '.'", 0);
            if (blen < 254) buf[blen++] = '.';
            int pl = part->len < (255 - blen) ? part->len : (255 - blen);
            memcpy(buf + blen, part->start, (size_t)pl); blen += pl;
        }
        buf[blen] = '\0';
        expect(&p, TOK_SEMICOLON, "expected ';' after import");
        Node *imp = node_new(&p, NODE_IMPORT, t->line, t->col);
        imp->as.pkg.path = arena_strdup(arena, buf, blen);
        nodelist_push(&p, &prog->as.program.imports, imp);
    }

    /* top-level declarations */
    while (!check(&p, TOK_EOF))
        nodelist_push(&p, &prog->as.program.decls, parse_decl(&p));

    if (p.had_error) return NULL;
    return prog;
}
