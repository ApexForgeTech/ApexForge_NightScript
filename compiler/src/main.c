#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "common/arena.h"
#include "parser/ast.h"
#include "parser/ast_dump.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "sema/sema.h"
#include "typeck/typeck.h"
#include "codegen/codegen.h"

typedef enum {
    CMD_CODEGEN,
    CMD_AST,
    CMD_CHECK,
    CMD_BUILD,
    CMD_RUN,
} Command;

typedef struct {
    char *path;
    char *source;
    TokenList tokens;
    Node *ast;
    int include_in_package;
} SourceUnit;

typedef struct {
    SourceUnit *units;
    int unit_count;
    int unit_cap;
    Arena arena;
    Node *ast;
    SemanticModel sema;
    COut codegen;
    int arena_ready;
    int sema_ready;
    int codegen_ready;
} CompileState;

static char *dup_string(const char *text) {
    size_t len;
    char *copy;

    if (!text)
        return NULL;

    len = strlen(text);
    copy = malloc(len + 1);
    if (!copy)
        return NULL;

    memcpy(copy, text, len + 1);
    return copy;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static void print_usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  night <file.afns>\n"
            "  night codegen <file.afns>\n"
            "  night ast <file.afns>\n"
            "  night check <file.afns>\n"
            "  night build <file.afns> [-o output]\n"
            "  night run <file.afns> [-o output]\n");
}

static char *replace_extension(const char *path, const char *ext) {
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    size_t stem_len;
    char *out;
    size_t ext_len = strlen(ext);

    if (dot && (!slash || dot > slash)) {
        stem_len = (size_t)(dot - path);
    } else {
        stem_len = strlen(path);
    }

    out = malloc(stem_len + ext_len + 1);
    if (!out) return NULL;
    memcpy(out, path, stem_len);
    memcpy(out + stem_len, ext, ext_len + 1);
    return out;
}

static char *shell_quote(const char *s) {
    size_t len = 2;
    char *out;
    char *p;

    for (const char *it = s; *it; it++) {
        if (*it == '\'')
            len += 4;
        else
            len += 1;
    }

    out = malloc(len + 1);
    if (!out) return NULL;

    p = out;
    *p++ = '\'';
    for (const char *it = s; *it; it++) {
        if (*it == '\'') {
            memcpy(p, "'\\''", 4);
            p += 4;
        } else {
            *p++ = *it;
        }
    }
    *p++ = '\'';
    *p = '\0';
    return out;
}

static void compile_state_init(CompileState *state) {
    memset(state, 0, sizeof(*state));
}

static void source_unit_free(SourceUnit *unit) {
    if (!unit)
        return;

    token_list_free(&unit->tokens);
    free(unit->source);
    free(unit->path);
    memset(unit, 0, sizeof(*unit));
}

static void compile_state_free(CompileState *state) {
    for (int i = 0; i < state->unit_count; i++)
        source_unit_free(&state->units[i]);
    free(state->units);
    if (state->codegen_ready)
        cout_free(&state->codegen);
    if (state->sema_ready)
        sema_model_free(&state->sema);
    if (state->arena_ready)
        arena_free(&state->arena);
    memset(state, 0, sizeof(*state));
}

static int push_source_unit(CompileState *state, const char *path) {
    SourceUnit *new_units;

    if (state->unit_count == state->unit_cap) {
        int new_cap = state->unit_cap ? state->unit_cap * 2 : 8;
        new_units = realloc(state->units, (size_t)new_cap * sizeof(SourceUnit));
        if (!new_units)
            return 0;
        state->units = new_units;
        state->unit_cap = new_cap;
    }

    memset(&state->units[state->unit_count], 0, sizeof(SourceUnit));
    state->units[state->unit_count].path = dup_string(path);
    if (!state->units[state->unit_count].path)
        return 0;

    state->unit_count++;
    return 1;
}

static int has_afns_extension(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && !strcmp(dot, ".afns");
}

static char *path_dirname(const char *path) {
    const char *slash = strrchr(path, '/');
    size_t len;
    char *out;

    if (!slash)
        return dup_string(".");

    len = (size_t)(slash - path);
    if (len == 0)
        len = 1;

    out = malloc(len + 1);
    if (!out)
        return NULL;

    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}

static char *path_join(const char *dir, const char *name) {
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    int need_slash = dir_len > 0 && dir[dir_len - 1] != '/';
    char *out = malloc(dir_len + name_len + (size_t)need_slash + 1);

    if (!out)
        return NULL;

    memcpy(out, dir, dir_len);
    if (need_slash)
        out[dir_len++] = '/';
    memcpy(out + dir_len, name, name_len);
    out[dir_len + name_len] = '\0';
    return out;
}

static int compare_strings(const void *lhs, const void *rhs) {
    const char *const *a = lhs;
    const char *const *b = rhs;
    return strcmp(*a, *b);
}

static int collect_package_files(const char *entry_path, char ***out_paths, int *out_count) {
    char *dir_path = NULL;
    DIR *dir = NULL;
    struct dirent *ent;
    char **paths = NULL;
    int count = 0;
    int cap = 0;
    int ok = 0;

    dir_path = path_dirname(entry_path);
    if (!dir_path) {
        fprintf(stderr, "error: out of memory\n");
        goto cleanup;
    }

    dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "error: cannot open directory '%s': %s\n", dir_path, strerror(errno));
        goto cleanup;
    }

    while ((ent = readdir(dir)) != NULL) {
        struct stat st;
        char *path;
        char **new_paths;

        if (ent->d_name[0] == '.')
            continue;
        if (!has_afns_extension(ent->d_name))
            continue;

        path = path_join(dir_path, ent->d_name);
        if (!path) {
            fprintf(stderr, "error: out of memory\n");
            goto cleanup;
        }

        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(path);
            continue;
        }

        if (count == cap) {
            int new_cap = cap ? cap * 2 : 8;
            new_paths = realloc(paths, (size_t)new_cap * sizeof(char *));
            if (!new_paths) {
                free(path);
                fprintf(stderr, "error: out of memory\n");
                goto cleanup;
            }
            paths = new_paths;
            cap = new_cap;
        }

        paths[count++] = path;
    }

    qsort(paths, (size_t)count, sizeof(char *), compare_strings);
    *out_paths = paths;
    *out_count = count;
    paths = NULL;
    ok = 1;

cleanup:
    if (dir)
        closedir(dir);
    free(dir_path);
    if (!ok) {
        for (int i = 0; i < count; i++)
            free(paths[i]);
        free(paths);
    }
    return ok;
}

static void free_path_list(char **paths, int count) {
    for (int i = 0; i < count; i++)
        free(paths[i]);
    free(paths);
}

static int load_source_unit(CompileState *state, int unit_index) {
    SourceUnit *unit = &state->units[unit_index];

    unit->source = read_file(unit->path);
    if (!unit->source)
        return 0;

    if (!lexer_tokenize(unit->source, unit->path, &unit->tokens))
        return 0;

    unit->ast = parser_parse(&unit->tokens, unit->path, &state->arena);
    if (!unit->ast) {
        fprintf(stderr, "parse failed\n");
        return 0;
    }

    return 1;
}

static Node *merge_package_program(CompileState *state, int entry_unit_index) {
    Node *program;
    int import_count = 0;
    int decl_count = 0;
    int import_index = 0;
    int decl_index = 0;

    for (int i = 0; i < state->unit_count; i++) {
        SourceUnit *unit = &state->units[i];
        if (!unit->include_in_package)
            continue;

        import_count += unit->ast->as.program.imports.count;
        decl_count += unit->ast->as.program.decls.count;
    }

    program = arena_alloc(&state->arena, sizeof(Node));
    program->kind = NODE_PROGRAM;
    program->line = 1;
    program->col = 1;
    program->as.program.package = state->units[entry_unit_index].ast->as.program.package;

    if (import_count > 0)
        program->as.program.imports.items = arena_alloc(&state->arena, (size_t)import_count * sizeof(Node *));
    program->as.program.imports.count = import_count;

    if (decl_count > 0)
        program->as.program.decls.items = arena_alloc(&state->arena, (size_t)decl_count * sizeof(Node *));
    program->as.program.decls.count = decl_count;

    for (int i = 0; i < state->unit_count; i++) {
        SourceUnit *unit = &state->units[i];
        if (!unit->include_in_package)
            continue;

        for (int j = 0; j < unit->ast->as.program.imports.count; j++)
            program->as.program.imports.items[import_index++] = unit->ast->as.program.imports.items[j];
        for (int j = 0; j < unit->ast->as.program.decls.count; j++)
            program->as.program.decls.items[decl_index++] = unit->ast->as.program.decls.items[j];
    }

    return program;
}

static int compile_source(const char *path, CompileState *state) {
    char **package_paths = NULL;
    int package_path_count = 0;
    const char *entry_package = NULL;
    int entry_unit_index = 0;

    compile_state_init(state);

    arena_init(&state->arena);
    state->arena_ready = 1;

    if (!push_source_unit(state, path)) {
        fprintf(stderr, "error: out of memory\n");
        return 0;
    }
    if (!load_source_unit(state, 0))
        return 0;

    state->units[0].include_in_package = 1;
    state->ast = state->units[0].ast;

    if (state->units[0].ast->as.program.package)
        entry_package = state->units[0].ast->as.program.package->as.pkg.path;

    if (entry_package) {
        if (!collect_package_files(path, &package_paths, &package_path_count))
            return 0;

        for (int i = 0; i < package_path_count; i++) {
            SourceUnit *unit;
            const char *package_name;

            if (!strcmp(package_paths[i], path))
                continue;

            if (!push_source_unit(state, package_paths[i])) {
                fprintf(stderr, "error: out of memory\n");
                free_path_list(package_paths, package_path_count);
                return 0;
            }

            if (!load_source_unit(state, state->unit_count - 1)) {
                free_path_list(package_paths, package_path_count);
                return 0;
            }

            unit = &state->units[state->unit_count - 1];
            package_name = unit->ast->as.program.package ?
                           unit->ast->as.program.package->as.pkg.path :
                           NULL;
            if (package_name && !strcmp(package_name, entry_package))
                unit->include_in_package = 1;
        }

        free_path_list(package_paths, package_path_count);
        state->ast = merge_package_program(state, entry_unit_index);
    }

    if (!sema_analyze(state->ast, path, &state->sema))
        return 0;
    state->sema_ready = 1;

    if (!typeck_check(state->ast, &state->sema, path))
        return 0;

    if (!codegen_generate(state->ast, &state->codegen)) {
        fprintf(stderr, "codegen failed\n");
        return 0;
    }
    state->codegen_ready = 1;
    return 1;
}

static int write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "error: cannot write '%s': %s\n", path, strerror(errno));
        return 0;
    }
    if (fputs(text, f) == EOF) {
        fprintf(stderr, "error: failed writing '%s'\n", path);
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static int invoke_cc(const char *c_path, const char *out_path) {
    const char *cc = getenv("CC");
    char *q_cc;
    char *q_c;
    char *q_out;
    char *cmd;
    int ok = 0;
    int rc;

    if (!cc || !cc[0])
        cc = "gcc";

    q_cc = shell_quote(cc);
    q_c = shell_quote(c_path);
    q_out = shell_quote(out_path);
    if (!q_cc || !q_c || !q_out) {
        fprintf(stderr, "error: out of memory\n");
        goto cleanup;
    }

    cmd = malloc(strlen(q_cc) + strlen(q_c) + strlen(q_out) + 64);
    if (!cmd) {
        fprintf(stderr, "error: out of memory\n");
        goto cleanup;
    }

    snprintf(cmd, strlen(q_cc) + strlen(q_c) + strlen(q_out) + 64,
             "%s -std=c11 %s -o %s", q_cc, q_c, q_out);
    rc = system(cmd);
    free(cmd);
    ok = (rc == 0);
    if (!ok) {
        fprintf(stderr, "error: C compiler failed for '%s'\n", c_path);
    }

cleanup:
    free(q_cc);
    free(q_c);
    free(q_out);
    return ok;
}

static int build_binary(const char *src_path, const char *binary_path) {
    CompileState state;
    char *c_path = NULL;
    int ok = 0;

    if (!compile_source(src_path, &state))
        return 1;

    c_path = replace_extension(binary_path, ".generated.c");
    if (!c_path) {
        fprintf(stderr, "error: out of memory\n");
        goto cleanup;
    }

    if (!write_text_file(c_path, state.codegen.buf))
        goto cleanup;

    if (!invoke_cc(c_path, binary_path))
        goto cleanup;

    ok = 1;

cleanup:
    free(c_path);
    compile_state_free(&state);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    Command cmd = CMD_CODEGEN;
    const char *path = NULL;
    const char *output_path = NULL;
    int argi = 1;

    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (!strcmp(argv[argi], "codegen")) {
        cmd = CMD_CODEGEN;
        argi++;
    } else if (!strcmp(argv[argi], "ast")) {
        cmd = CMD_AST;
        argi++;
    } else if (!strcmp(argv[argi], "check")) {
        cmd = CMD_CHECK;
        argi++;
    } else if (!strcmp(argv[argi], "build")) {
        cmd = CMD_BUILD;
        argi++;
    } else if (!strcmp(argv[argi], "run")) {
        cmd = CMD_RUN;
        argi++;
    }

    if (argi >= argc) {
        print_usage();
        return 1;
    }

    path = argv[argi++];

    while (argi < argc) {
        if (!strcmp(argv[argi], "-o")) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "error: expected output path after -o\n");
                return 1;
            }
            output_path = argv[argi + 1];
            argi += 2;
            continue;
        }

        if (!strcmp(argv[argi], "--ast")) {
            cmd = CMD_AST;
            argi++;
            continue;
        }

        fprintf(stderr, "error: unknown argument '%s'\n", argv[argi]);
        print_usage();
        return 1;
    }

    if (cmd == CMD_BUILD || cmd == CMD_RUN) {
        char *default_out = NULL;
        char *quoted_out = NULL;
        char *run_cmd = NULL;
        int rc = 0;

        if (!output_path) {
            default_out = replace_extension(path, "");
            if (!default_out) {
                fprintf(stderr, "error: out of memory\n");
                return 1;
            }
            output_path = default_out;
        }

        rc = build_binary(path, output_path);
        if (rc != 0) {
            free(default_out);
            return rc;
        }

        if (cmd == CMD_RUN) {
            quoted_out = shell_quote(output_path);
            if (!quoted_out) {
                fprintf(stderr, "error: out of memory\n");
                free(default_out);
                return 1;
            }

            run_cmd = malloc(strlen(quoted_out) + 1);
            if (!run_cmd) {
                fprintf(stderr, "error: out of memory\n");
                free(quoted_out);
                free(default_out);
                return 1;
            }

            strcpy(run_cmd, quoted_out);
            rc = system(run_cmd);
            free(run_cmd);
            free(quoted_out);
            free(default_out);
            return rc == 0 ? 0 : 1;
        }

        free(default_out);
        return 0;
    }

    {
        CompileState state;
        int rc = 0;

        if (!compile_source(path, &state))
            return 1;

        if (cmd == CMD_AST) {
            ast_dump_node(stdout, state.ast);
        } else if (cmd == CMD_CHECK) {
            printf("OK\n");
        } else {
            printf("%s", state.codegen.buf);
        }

        compile_state_free(&state);
        return rc;
    }
}
