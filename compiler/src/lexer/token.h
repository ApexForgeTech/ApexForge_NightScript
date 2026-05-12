#ifndef NIGHT_TOKEN_H
#define NIGHT_TOKEN_H

typedef enum {
    /* keywords */
    TOK_PACKAGE, TOK_IMPORT, TOK_PUB, TOK_FN, TOK_LET, TOK_CONST,
    TOK_RETURN, TOK_IF, TOK_ELSE, TOK_WHILE, TOK_LOOP,
    TOK_BREAK, TOK_CONTINUE,
    TOK_STRUCT, TOK_ENUM, TOK_UNION, TOK_INTERFACE, TOK_IMPL,
    TOK_UNSAFE, TOK_EXTERN, TOK_AS, TOK_MATCH,
    TOK_KERNEL, TOK_NATIVE, TOK_UI, TOK_ANDROID, TOK_DRIVER,
    TOK_APP, TOK_MODULE, TOK_DEFER, TOK_COMPTIME,
    TOK_TRUE, TOK_FALSE, TOK_NULL, TOK_SELF, TOK_UNDERSCORE,

    /* identifiers and literals */
    TOK_IDENT,
    TOK_INT,
    TOK_FLOAT,
    TOK_STRING,
    TOK_CHAR,

    /* symbols */
    TOK_LBRACE,    /* { */
    TOK_RBRACE,    /* } */
    TOK_LPAREN,    /* ( */
    TOK_RPAREN,    /* ) */
    TOK_LBRACKET,  /* [ */
    TOK_RBRACKET,  /* ] */
    TOK_SEMICOLON, /* ; */
    TOK_COLON,     /* : */
    TOK_COMMA,     /* , */
    TOK_DOT,       /* . */
    TOK_ARROW,     /* -> */
    TOK_FAT_ARROW, /* => */
    TOK_EQ,        /* = */
    TOK_EQEQ,      /* == */
    TOK_NE,        /* != */
    TOK_LT,        /* < */
    TOK_LE,        /* <= */
    TOK_GT,        /* > */
    TOK_GE,        /* >= */
    TOK_PLUS,      /* + */
    TOK_MINUS,     /* - */
    TOK_STAR,      /* * */
    TOK_SLASH,     /* / */
    TOK_PERCENT,   /* % */
    TOK_AMP,       /* & */
    TOK_ANDAND,    /* && */
    TOK_PIPE,      /* | */
    TOK_OROR,      /* || */
    TOK_CARET,     /* ^ */
    TOK_TILDE,     /* ~ */
    TOK_BANG,      /* ! */
    TOK_QUESTION,  /* ? */
    TOK_AT,        /* @ */

    /* compound assignment */
    TOK_PLUS_EQ,    /* += */
    TOK_MINUS_EQ,   /* -= */
    TOK_STAR_EQ,    /* *= */
    TOK_SLASH_EQ,   /* /= */
    TOK_PERCENT_EQ, /* %= */

    TOK_EOF,

    TOK_COUNT
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start;  /* pointer into source buffer */
    int         len;    /* byte length of lexeme */
    int         line;
    int         col;

    union {
        long long  int_val;
        double     float_val;
        char      *str_val;  /* heap-allocated, null-terminated */
    };
} Token;

const char *token_kind_name(TokenKind kind);

#endif /* NIGHT_TOKEN_H */
