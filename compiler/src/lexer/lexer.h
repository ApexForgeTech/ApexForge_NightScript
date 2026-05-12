#ifndef NIGHT_LEXER_H
#define NIGHT_LEXER_H

#include "token.h"

typedef struct {
    Token  *tokens;
    int     count;
    int     capacity;
} TokenList;

/*
 * Tokenize `source` (null-terminated).
 * `source_name` is used only in error messages.
 * Returns 1 on success, 0 on lex error (message printed to stderr).
 */
int  lexer_tokenize(const char *source, const char *source_name, TokenList *out);
void token_list_free(TokenList *list);

#endif /* NIGHT_LEXER_H */
