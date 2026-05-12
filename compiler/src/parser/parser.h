#ifndef NIGHT_PARSER_H
#define NIGHT_PARSER_H

#include "../common/arena.h"
#include "ast.h"
#include "../lexer/lexer.h"

/*
 * Parse a token list produced by the lexer.
 * All AST nodes are allocated from `arena`.
 * Returns the root NODE_PROGRAM node, or NULL on parse error.
 */
Node *parser_parse(TokenList *tl, const char *source_name, Arena *arena);

#endif /* NIGHT_PARSER_H */
