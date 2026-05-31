#ifndef NIGHT_LLVMGEN_H
#define NIGHT_LLVMGEN_H

#ifdef NIGHT_LLVM_BACKEND

#include "../parser/ast.h"

/* Options for the LLVM backend */
typedef struct {
    const char *target_triple;  /* NULL = native default */
    int         opt_level;      /* 0=none 1=less 2=default 3=aggressive */
    int         emit_ir;        /* 1: write .ll text instead of binary */
} LLVMGenOptions;

/*
 * Generate a native binary from a parsed AST using the LLVM backend.
 * output_path: path to write the output binary.
 * Returns 1 on success, 0 on error (message printed to stderr).
 */
int llvmgen_generate(Node *program, const char *output_path,
                     const LLVMGenOptions *opts);

/*
 * Emit LLVM IR text as a heap-allocated C string.
 * Caller must free() the returned string.
 * Returns NULL on error.
 */
char *llvmgen_emit_ir(Node *program, const LLVMGenOptions *opts);

#endif /* NIGHT_LLVM_BACKEND */
#endif /* NIGHT_LLVMGEN_H */
