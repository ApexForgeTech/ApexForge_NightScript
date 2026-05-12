#ifndef NIGHT_CODEGEN_H
#define NIGHT_CODEGEN_H

#include "../parser/ast.h"
#include "../common/arena.h"

typedef struct {
    char  *buf;
    int    len;
    int    cap;
    int    indent;
} COut;

/*
 * Generate C source from a parsed AST.
 * Returns 1 on success, 0 on error.
 * out->buf is heap-allocated — caller must free().
 */
int codegen_generate(Node *program, COut *out);
void cout_free(COut *out);

#endif /* NIGHT_CODEGEN_H */
