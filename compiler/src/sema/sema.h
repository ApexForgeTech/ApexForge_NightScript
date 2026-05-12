#ifndef NIGHT_SEMA_H
#define NIGHT_SEMA_H

#include "../parser/ast.h"

typedef struct {
    Node **functions;
    int    function_count;

    Node **extern_functions;
    int    extern_function_count;

    Node **structs;
    int    struct_count;

    Node **enums;
    int    enum_count;

    Node **unions;
    int    union_count;

    Node **impls;
    int    impl_count;
} SemanticModel;

/*
 * Analyze a parsed AST and populate `out` with top-level declarations.
 * Returns 1 on success, 0 on semantic error (message printed to stderr).
 */
int  sema_analyze(Node *program, const char *source_name, SemanticModel *out);
void sema_model_free(SemanticModel *model);

#endif /* NIGHT_SEMA_H */
