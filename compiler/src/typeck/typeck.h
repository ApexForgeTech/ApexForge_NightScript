#ifndef NIGHT_TYPECK_H
#define NIGHT_TYPECK_H

#include "../parser/ast.h"
#include "../sema/sema.h"

/*
 * Type-check a semantically valid AST.
 * Returns 1 on success, 0 on type error (message printed to stderr).
 */
int typeck_check(Node *program, const SemanticModel *sema, const char *source_name);

#endif /* NIGHT_TYPECK_H */
