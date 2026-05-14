#include "lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── keyword table ─────────────────────────────────────────────────────── */

typedef struct { const char *word; TokenKind kind; } Keyword;

static const Keyword KEYWORDS[] = {
    {"package",   TOK_PACKAGE},
    {"import",    TOK_IMPORT},
    {"pub",       TOK_PUB},
    {"fn",        TOK_FN},
    {"let",       TOK_LET},
    {"const",     TOK_CONST},
    {"return",    TOK_RETURN},
    {"if",        TOK_IF},
    {"else",      TOK_ELSE},
    {"while",     TOK_WHILE},
    {"loop",      TOK_LOOP},
    {"for",       TOK_FOR},
    {"break",     TOK_BREAK},
    {"continue",  TOK_CONTINUE},
    {"struct",    TOK_STRUCT},
    {"enum",      TOK_ENUM},
    {"union",     TOK_UNION},
    {"interface", TOK_INTERFACE},
    {"impl",      TOK_IMPL},
    {"unsafe",    TOK_UNSAFE},
    {"extern",    TOK_EXTERN},
    {"as",        TOK_AS},
    {"match",     TOK_MATCH},
    {"kernel",    TOK_KERNEL},
    {"native",    TOK_NATIVE},
    {"ui",        TOK_UI},
    {"android",   TOK_ANDROID},
    {"driver",    TOK_DRIVER},
    {"app",       TOK_APP},
    {"module",    TOK_MODULE},
    {"defer",     TOK_DEFER},
    {"comptime",  TOK_COMPTIME},
    {"packed",    TOK_PACKED},
    {"true",      TOK_TRUE},
    {"false",     TOK_FALSE},
    {"null",      TOK_NULL},
    {"Self",      TOK_SELF},
    {"_",         TOK_UNDERSCORE},
    {NULL,        TOK_EOF}
};

/* ── lexer state ───────────────────────────────────────────────────────── */

typedef struct {
    const char *src;
    const char *src_name;
    int         pos;
    int         line;
    int         col;
} Lexer;

static char peek(Lexer *l) {
    return l->src[l->pos];
}

static char peek_next(Lexer *l) {
    if (l->src[l->pos] == '\0') return '\0';
    return l->src[l->pos + 1];
}

static char advance(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') { l->line++; l->col = 1; }
    else            { l->col++; }
    return c;
}

static int is_at_end(Lexer *l) {
    return l->src[l->pos] == '\0';
}

/* ── token list helpers ────────────────────────────────────────────────── */

static void list_push(TokenList *tl, Token t) {
    if (tl->count == tl->capacity) {
        tl->capacity = tl->capacity ? tl->capacity * 2 : 64;
        tl->tokens   = realloc(tl->tokens, (size_t)tl->capacity * sizeof(Token));
    }
    tl->tokens[tl->count++] = t;
}

/* ── helpers ───────────────────────────────────────────────────────────── */

static void skip_whitespace_and_comments(Lexer *l) {
    for (;;) {
        while (!is_at_end(l) && isspace((unsigned char)peek(l)))
            advance(l);

        if (peek(l) == '/' && peek_next(l) == '/') {
            while (!is_at_end(l) && peek(l) != '\n')
                advance(l);
            continue;
        }

        if (peek(l) == '/' && peek_next(l) == '*') {
            advance(l); advance(l);
            int depth = 1;
            while (!is_at_end(l) && depth > 0) {
                if (peek(l) == '/' && peek_next(l) == '*') { depth++; advance(l); advance(l); }
                else if (peek(l) == '*' && peek_next(l) == '/') { depth--; advance(l); advance(l); }
                else advance(l);
            }
            continue;
        }

        break;
    }
}

static TokenKind ident_or_keyword(const char *start, int len) {
    for (int i = 0; KEYWORDS[i].word; i++) {
        if ((int)strlen(KEYWORDS[i].word) == len &&
            memcmp(KEYWORDS[i].word, start, (size_t)len) == 0)
            return KEYWORDS[i].kind;
    }
    return TOK_IDENT;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int lex_escape(Lexer *l, char *out) {
    char c = advance(l);
    switch (c) {
        case 'n':  *out = '\n'; return 1;
        case 'r':  *out = '\r'; return 1;
        case 't':  *out = '\t'; return 1;
        case '0':  *out = '\0'; return 1;
        case '\\': *out = '\\'; return 1;
        case '"':  *out = '"';  return 1;
        case '\'': *out = '\''; return 1;
        default:
            fprintf(stderr, "%s:%d:%d: error: unknown escape '\\%c'\n",
                    l->src_name, l->line, l->col, c);
            return 0;
    }
}

/* ── main tokenize ─────────────────────────────────────────────────────── */

int lexer_tokenize(const char *source, const char *source_name, TokenList *out) {
    Lexer l = { .src = source, .src_name = source_name, .pos = 0, .line = 1, .col = 1 };
    out->tokens   = NULL;
    out->count    = 0;
    out->capacity = 0;

    while (1) {
        skip_whitespace_and_comments(&l);

        if (is_at_end(&l)) break;

        int   start_line = l.line;
        int   start_col  = l.col;
        char  c          = peek(&l);

        Token t = {0};
        t.line  = start_line;
        t.col   = start_col;

        /* two-char tokens */
        char n = peek_next(&l);
        if (c == '-' && n == '>') { advance(&l); advance(&l); t.kind = TOK_ARROW;     t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '=' && n == '>') { advance(&l); advance(&l); t.kind = TOK_FAT_ARROW; t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '=' && n == '=') { advance(&l); advance(&l); t.kind = TOK_EQEQ;      t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '!' && n == '=') { advance(&l); advance(&l); t.kind = TOK_NE;        t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '<' && n == '=') { advance(&l); advance(&l); t.kind = TOK_LE;        t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '>' && n == '=') { advance(&l); advance(&l); t.kind = TOK_GE;        t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '&' && n == '&') { advance(&l); advance(&l); t.kind = TOK_ANDAND;    t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '|' && n == '|') { advance(&l); advance(&l); t.kind = TOK_OROR;      t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '+' && n == '+') { advance(&l); advance(&l); t.kind = TOK_PLUSPLUS;    t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '-' && n == '-') { advance(&l); advance(&l); t.kind = TOK_MINUSMINUS;  t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '+' && n == '=') { advance(&l); advance(&l); t.kind = TOK_PLUS_EQ;    t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '-' && n == '=') { advance(&l); advance(&l); t.kind = TOK_MINUS_EQ;   t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '*' && n == '=') { advance(&l); advance(&l); t.kind = TOK_STAR_EQ;    t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '/' && n == '=') { advance(&l); advance(&l); t.kind = TOK_SLASH_EQ;   t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '%' && n == '=') { advance(&l); advance(&l); t.kind = TOK_PERCENT_EQ; t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '<' && n == '<') { advance(&l); advance(&l); t.kind = TOK_LSHIFT;      t.start = l.src + l.pos - 2; t.len = 2; goto push; }
        if (c == '>' && n == '>') { advance(&l); advance(&l); t.kind = TOK_RSHIFT;      t.start = l.src + l.pos - 2; t.len = 2; goto push; }

        /* identifier or keyword */
        if (isalpha((unsigned char)c) || c == '_') {
            const char *start = l.src + l.pos;
            while (isalnum((unsigned char)peek(&l)) || peek(&l) == '_')
                advance(&l);
            int len    = (int)(l.src + l.pos - start);
            t.kind     = ident_or_keyword(start, len);
            t.start    = start;
            t.len      = len;
            if (t.kind == TOK_TRUE)  { t.int_val = 1; }
            if (t.kind == TOK_FALSE) { t.int_val = 0; }
            goto push;
        }

        /* integer or float */
        if (isdigit((unsigned char)c)) {
            const char *start = l.src + l.pos;
            int is_float = 0;

            if (c == '0' && (peek_next(&l) == 'x' || peek_next(&l) == 'X')) {
                advance(&l); advance(&l);
                long long val = 0;
                while (hex_digit(peek(&l)) >= 0 || peek(&l) == '_') {
                    char ch = advance(&l);
                    if (ch != '_') val = val * 16 + hex_digit(ch);
                }
                t.kind    = TOK_INT;
                t.start   = start;
                t.len     = (int)(l.src + l.pos - start);
                t.int_val = val;
                goto push;
            }

            long long ival = 0;
            while (isdigit((unsigned char)peek(&l)) || peek(&l) == '_') {
                char ch = advance(&l);
                if (ch != '_') ival = ival * 10 + (ch - '0');
            }
            if (peek(&l) == '.' && isdigit((unsigned char)peek_next(&l))) {
                is_float = 1;
                advance(&l);
                while (isdigit((unsigned char)peek(&l)) || peek(&l) == '_')
                    advance(&l);
            }
            if (peek(&l) == 'e' || peek(&l) == 'E') {
                is_float = 1;
                advance(&l);
                if (peek(&l) == '+' || peek(&l) == '-') advance(&l);
                while (isdigit((unsigned char)peek(&l))) advance(&l);
            }
            int len = (int)(l.src + l.pos - start);
            if (is_float) {
                char tmp[64];
                int  n2 = len < 63 ? len : 63;
                memcpy(tmp, start, (size_t)n2);
                tmp[n2]      = '\0';
                t.kind       = TOK_FLOAT;
                t.float_val  = atof(tmp);
            } else {
                t.kind    = TOK_INT;
                t.int_val = ival;
            }
            t.start = start;
            t.len   = len;
            goto push;
        }

        /* string literal */
        if (c == '"') {
            advance(&l);
            char buf[4096];
            int  blen = 0;
            while (!is_at_end(&l) && peek(&l) != '"') {
                if (peek(&l) == '\n') {
                    fprintf(stderr, "%s:%d:%d: error: unterminated string literal\n",
                            source_name, start_line, start_col);
                    return 0;
                }
                char ch;
                if (peek(&l) == '\\') {
                    advance(&l);
                    if (!lex_escape(&l, &ch)) return 0;
                } else {
                    ch = advance(&l);
                }
                if (blen < (int)sizeof(buf) - 1) buf[blen++] = ch;
            }
            if (is_at_end(&l)) {
                fprintf(stderr, "%s:%d:%d: error: unterminated string literal\n",
                        source_name, start_line, start_col);
                return 0;
            }
            advance(&l);
            buf[blen]   = '\0';
            t.kind      = TOK_STRING;
            t.start     = l.src + l.pos - blen - 2;
            t.len       = (int)(l.src + l.pos - t.start);
            t.str_val   = malloc((size_t)blen + 1);
            memcpy(t.str_val, buf, (size_t)blen + 1);
            goto push;
        }

        /* char literal */
        if (c == '\'') {
            advance(&l);
            if (is_at_end(&l)) {
                fprintf(stderr, "%s:%d:%d: error: unterminated character literal\n",
                        source_name, start_line, start_col);
                return 0;
            }
            char ch;
            if (peek(&l) == '\\') {
                advance(&l);
                if (!lex_escape(&l, &ch)) return 0;
            } else {
                ch = advance(&l);
            }
            if (peek(&l) != '\'') {
                fprintf(stderr, "%s:%d:%d: error: character literal must have exactly one character\n",
                        source_name, l.line, l.col);
                return 0;
            }
            advance(&l);
            t.kind    = TOK_CHAR;
            t.start   = l.src + l.pos - 3;
            t.len     = 3;
            t.int_val = (unsigned char)ch;
            goto push;
        }

        /* single-char tokens */
        if (c == '.' && peek_next(&l) == '.') {
            advance(&l);
            advance(&l);
            t.kind = TOK_DOTDOT;
            t.start = l.src + l.pos - 2;
            t.len = 2;
            goto push;
        }

        advance(&l);
        t.start = l.src + l.pos - 1;
        t.len   = 1;
        switch (c) {
            case '{': t.kind = TOK_LBRACE;    break;
            case '}': t.kind = TOK_RBRACE;    break;
            case '(': t.kind = TOK_LPAREN;    break;
            case ')': t.kind = TOK_RPAREN;    break;
            case '[': t.kind = TOK_LBRACKET;  break;
            case ']': t.kind = TOK_RBRACKET;  break;
            case ';': t.kind = TOK_SEMICOLON; break;
            case ':': t.kind = TOK_COLON;     break;
            case ',': t.kind = TOK_COMMA;     break;
            case '.': t.kind = TOK_DOT;       break;
            case '=': t.kind = TOK_EQ;        break;
            case '+': t.kind = TOK_PLUS;      break;
            case '-': t.kind = TOK_MINUS;     break;
            case '*': t.kind = TOK_STAR;      break;
            case '/': t.kind = TOK_SLASH;     break;
            case '%': t.kind = TOK_PERCENT;   break;
            case '&': t.kind = TOK_AMP;       break;
            case '|': t.kind = TOK_PIPE;      break;
            case '^': t.kind = TOK_CARET;     break;
            case '~': t.kind = TOK_TILDE;     break;
            case '!': t.kind = TOK_BANG;      break;
            case '<': t.kind = TOK_LT;        break;
            case '>': t.kind = TOK_GT;        break;
            case '?': t.kind = TOK_QUESTION;  break;
            case '@': t.kind = TOK_AT;        break;
            default:
                fprintf(stderr, "%s:%d:%d: error: unexpected character '%c'\n",
                        source_name, start_line, start_col, c);
                return 0;
        }

    push:
        list_push(out, t);
    }

    Token eof = {0};
    eof.kind  = TOK_EOF;
    eof.start = l.src + l.pos;
    eof.len   = 0;
    eof.line  = l.line;
    eof.col   = l.col;
    list_push(out, eof);

    return 1;
}

void token_list_free(TokenList *list) {
    for (int i = 0; i < list->count; i++) {
        if (list->tokens[i].kind == TOK_STRING)
            free(list->tokens[i].str_val);
    }
    free(list->tokens);
    list->tokens   = NULL;
    list->count    = 0;
    list->capacity = 0;
}
