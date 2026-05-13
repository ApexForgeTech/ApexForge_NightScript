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
    CMD_FMT,
    CMD_INIT,
    CMD_CLEAN,
} Command;

typedef struct {
    char *root_dir;
    char *package_name;
    char *target_mode;
    char *target_arch;
    char *target_backend;
    char *entry;
    char *output;
} ProjectConfig;

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
            "  night run <file.afns> [-o output]\n"
            "  night fmt [file.afns|project-dir]\n"
            "  night init [dir]\n"
            "  night clean [file.afns|project-dir]\n");
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

static int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *path_basename_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    return dup_string(name);
}

static int mkdir_if_missing(const char *path) {
    if (mkdir(path, 0777) == 0)
        return 1;
    if (errno == EEXIST && path_is_dir(path))
        return 1;
    fprintf(stderr, "error: cannot create directory '%s': %s\n", path, strerror(errno));
    return 0;
}

static int ensure_parent_dir(const char *path) {
    char *dir = path_dirname(path);
    int ok;

    if (!dir) {
        fprintf(stderr, "error: out of memory\n");
        return 0;
    }

    if (!strcmp(dir, ".") || !strcmp(dir, "/")) {
        free(dir);
        return 1;
    }

    ok = mkdir_if_missing(dir);
    free(dir);
    return ok;
}

static void project_config_free(ProjectConfig *cfg) {
    if (!cfg)
        return;
    free(cfg->root_dir);
    free(cfg->package_name);
    free(cfg->target_mode);
    free(cfg->target_arch);
    free(cfg->target_backend);
    free(cfg->entry);
    free(cfg->output);
    memset(cfg, 0, sizeof(*cfg));
}

static char *trim_ascii(char *text) {
    char *start = text;
    char *end;

    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;

    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';

    return start;
}

static int parse_toml_string(const char *value, char **out) {
    const char *start;
    const char *end;
    size_t len;
    char *copy;

    if (!value || value[0] != '"')
        return 0;

    start = value + 1;
    end = strrchr(start, '"');
    if (!end)
        return 0;

    len = (size_t)(end - start);
    copy = malloc(len + 1);
    if (!copy)
        return 0;

    memcpy(copy, start, len);
    copy[len] = '\0';
    free(*out);
    *out = copy;
    return 1;
}

static int parse_project_config(const char *project_dir, ProjectConfig *cfg) {
    char *toml_path = NULL;
    char *text = NULL;
    char *cursor;
    char section[32] = "";
    int ok = 0;

    memset(cfg, 0, sizeof(*cfg));
    cfg->root_dir = dup_string(project_dir);
    if (!cfg->root_dir) {
        fprintf(stderr, "error: out of memory\n");
        goto cleanup;
    }

    toml_path = path_join(project_dir, "night.toml");
    if (!toml_path) {
        fprintf(stderr, "error: out of memory\n");
        goto cleanup;
    }

    text = read_file(toml_path);
    if (!text)
        goto cleanup;

    cursor = text;
    while (*cursor) {
        char *line = cursor;
        char *eq;
        char *key;
        char *value;
        char *next = strchr(cursor, '\n');

        if (next) {
            *next = '\0';
            cursor = next + 1;
        } else {
            cursor += strlen(cursor);
        }

        if (strchr(line, '#'))
            *strchr(line, '#') = '\0';
        line = trim_ascii(line);
        if (!line[0])
            continue;

        if (line[0] == '[') {
            char *close = strchr(line, ']');
            size_t len;
            if (!close) {
                fprintf(stderr, "error: malformed section header in '%s'\n", toml_path);
                goto cleanup;
            }
            len = (size_t)(close - line - 1);
            if (len >= sizeof(section))
                len = sizeof(section) - 1;
            memcpy(section, line + 1, len);
            section[len] = '\0';
            continue;
        }

        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        key = trim_ascii(line);
        value = trim_ascii(eq + 1);

        if (!strcmp(section, "package") && !strcmp(key, "name")) {
            if (!parse_toml_string(value, &cfg->package_name)) {
                fprintf(stderr, "error: invalid package.name in '%s'\n", toml_path);
                goto cleanup;
            }
        } else if (!strcmp(section, "target") && !strcmp(key, "mode")) {
            if (!parse_toml_string(value, &cfg->target_mode)) {
                fprintf(stderr, "error: invalid target.mode in '%s'\n", toml_path);
                goto cleanup;
            }
        } else if (!strcmp(section, "target") && !strcmp(key, "arch")) {
            if (!parse_toml_string(value, &cfg->target_arch)) {
                fprintf(stderr, "error: invalid target.arch in '%s'\n", toml_path);
                goto cleanup;
            }
        } else if (!strcmp(section, "target") && !strcmp(key, "backend")) {
            if (!parse_toml_string(value, &cfg->target_backend)) {
                fprintf(stderr, "error: invalid target.backend in '%s'\n", toml_path);
                goto cleanup;
            }
        } else if (!strcmp(section, "build") && !strcmp(key, "entry")) {
            if (!parse_toml_string(value, &cfg->entry)) {
                fprintf(stderr, "error: invalid build.entry in '%s'\n", toml_path);
                goto cleanup;
            }
        } else if (!strcmp(section, "build") && !strcmp(key, "output")) {
            if (!parse_toml_string(value, &cfg->output)) {
                fprintf(stderr, "error: invalid build.output in '%s'\n", toml_path);
                goto cleanup;
            }
        }
    }

    if (!cfg->entry)
        cfg->entry = dup_string("src/main.afns");
    if (!cfg->package_name)
        cfg->package_name = path_basename_dup(project_dir);
    if (!cfg->target_mode)
        cfg->target_mode = dup_string("native");
    if (!cfg->target_backend)
        cfg->target_backend = dup_string("c");
    if (!cfg->output && cfg->package_name)
        cfg->output = dup_string(cfg->package_name);

    if (!cfg->entry || !cfg->package_name || !cfg->target_mode ||
        !cfg->target_backend || !cfg->output) {
        fprintf(stderr, "error: out of memory\n");
        goto cleanup;
    }

    ok = 1;

cleanup:
    free(toml_path);
    free(text);
    if (!ok)
        project_config_free(cfg);
    return ok;
}

static char *resolve_project_entry(const ProjectConfig *cfg) {
    return path_join(cfg->root_dir, cfg->entry);
}

static char *resolve_project_output(const ProjectConfig *cfg) {
    return path_join(cfg->root_dir, cfg->output);
}

static int resolve_input_path(Command cmd, const char *input, char **resolved_path, char **resolved_output) {
    ProjectConfig cfg;
    const char *project_dir = input;
    char *entry = NULL;
    char *output = NULL;

    *resolved_path = NULL;
    *resolved_output = NULL;

    if (input && !path_is_dir(input)) {
        *resolved_path = dup_string(input);
        if (!*resolved_path) {
            fprintf(stderr, "error: out of memory\n");
            return 0;
        }
        return 1;
    }

    if (!project_dir)
        project_dir = ".";

    if (!parse_project_config(project_dir, &cfg))
        return 0;

    entry = resolve_project_entry(&cfg);
    if (!entry) {
        fprintf(stderr, "error: out of memory\n");
        project_config_free(&cfg);
        return 0;
    }

    if (cmd == CMD_BUILD || cmd == CMD_RUN || cmd == CMD_CLEAN) {
        output = resolve_project_output(&cfg);
        if (!output) {
            fprintf(stderr, "error: out of memory\n");
            free(entry);
            project_config_free(&cfg);
            return 0;
        }
    }

    *resolved_path = entry;
    *resolved_output = output;
    project_config_free(&cfg);
    return 1;
}

static int compare_strings(const void *lhs, const void *rhs) {
    const char *const *a = lhs;
    const char *const *b = rhs;
    return strcmp(*a, *b);
}

static int write_text_file(const char *path, const char *text);

static int write_if_changed(const char *path, const char *text) {
    char *existing = NULL;
    int ok = 1;

    if (path_exists(path)) {
        existing = read_file(path);
        if (existing && !strcmp(existing, text)) {
            free(existing);
            return 1;
        }
        free(existing);
    }

    ok = write_text_file(path, text);
    return ok;
}

static int format_source_file(const char *path) {
    char *text = read_file(path);
    char *out = NULL;
    size_t len;
    size_t cap;
    size_t out_len = 0;
    int ok = 0;

    if (!text)
        return 0;

    len = strlen(text);
    cap = len + 2;
    out = malloc(cap);
    if (!out) {
        fprintf(stderr, "error: out of memory\n");
        goto cleanup;
    }

    for (size_t i = 0; i < len;) {
        size_t start = i;
        size_t end;

        while (i < len && text[i] != '\n')
            i++;
        end = i;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
            end--;
        if (out_len + (end - start) + 2 >= cap) {
            size_t new_cap = cap * 2 + (end - start) + 2;
            char *new_out = realloc(out, new_cap);
            if (!new_out) {
                fprintf(stderr, "error: out of memory\n");
                goto cleanup;
            }
            out = new_out;
            cap = new_cap;
        }
        memcpy(out + out_len, text + start, end - start);
        out_len += end - start;
        out[out_len++] = '\n';
        if (i < len && text[i] == '\n')
            i++;
    }

    while (out_len > 1 && out[out_len - 1] == '\n' && out[out_len - 2] == '\n')
        out_len--;
    out[out_len] = '\0';

    ok = write_if_changed(path, out);

cleanup:
    free(text);
    free(out);
    return ok;
}

static int init_project(const char *target_dir) {
    char *src_dir = NULL;
    char *toml_path = NULL;
    char *main_path = NULL;
    char *pkg_name = NULL;
    char *toml = NULL;
    int ok = 0;
    size_t needed;

    if (!mkdir_if_missing(target_dir))
        return 0;

    src_dir = path_join(target_dir, "src");
    toml_path = path_join(target_dir, "night.toml");
    main_path = path_join(target_dir, "src/main.afns");
    pkg_name = path_basename_dup(target_dir);
    if (!src_dir || !toml_path || !main_path || !pkg_name) {
        fprintf(stderr, "error: out of memory\n");
        goto cleanup;
    }

    if (!mkdir_if_missing(src_dir))
        goto cleanup;

    needed = strlen(pkg_name) * 2 + 192;
    toml = malloc(needed);
    if (!toml) {
        fprintf(stderr, "error: out of memory\n");
        goto cleanup;
    }

    snprintf(toml, needed,
             "[package]\n"
             "name = \"%s\"\n"
             "\n"
             "[target]\n"
             "mode = \"native\"\n"
             "backend = \"c\"\n"
             "\n"
             "[build]\n"
             "entry = \"src/main.afns\"\n"
             "output = \"%s\"\n",
             pkg_name, pkg_name);

    if (!write_if_changed(toml_path, toml))
        goto cleanup;

    if (!write_if_changed(main_path,
                          "package main;\n\n"
                          "fn main() -> i32 {\n"
                          "    return 0;\n"
                          "}\n")) {
        goto cleanup;
    }

    ok = 1;

cleanup:
    free(src_dir);
    free(toml_path);
    free(main_path);
    free(pkg_name);
    free(toml);
    return ok;
}

static int remove_if_exists(const char *path) {
    if (remove(path) == 0)
        return 1;
    if (errno == ENOENT)
        return 1;
    fprintf(stderr, "error: cannot remove '%s': %s\n", path, strerror(errno));
    return 0;
}

static int clean_outputs(const char *src_path, const char *output_path) {
    char *generated = NULL;
    int ok = 0;

    if (output_path) {
        generated = replace_extension(output_path, ".generated.c");
        if (!generated) {
            fprintf(stderr, "error: out of memory\n");
            return 0;
        }
        if (!remove_if_exists(output_path))
            goto cleanup;
        if (!remove_if_exists(generated))
            goto cleanup;
        ok = 1;
        goto cleanup;
    }

    if (src_path) {
        char *default_out = replace_extension(src_path, "");
        if (!default_out) {
            fprintf(stderr, "error: out of memory\n");
            return 0;
        }
        generated = replace_extension(default_out, ".generated.c");
        if (!generated) {
            free(default_out);
            fprintf(stderr, "error: out of memory\n");
            return 0;
        }
        ok = remove_if_exists(default_out) && remove_if_exists(generated);
        free(default_out);
        goto cleanup;
    }

    ok = 1;

cleanup:
    free(generated);
    return ok;
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

    if (!ensure_parent_dir(binary_path) || !ensure_parent_dir(c_path))
        goto cleanup;

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
    char *resolved_path = NULL;
    char *resolved_output = NULL;
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
    } else if (!strcmp(argv[argi], "fmt")) {
        cmd = CMD_FMT;
        argi++;
    } else if (!strcmp(argv[argi], "init")) {
        cmd = CMD_INIT;
        argi++;
    } else if (!strcmp(argv[argi], "clean")) {
        cmd = CMD_CLEAN;
        argi++;
    }

    if (argi < argc)
        path = argv[argi++];

    if (cmd == CMD_INIT) {
        const char *target_dir = path ? path : ".";
        return init_project(target_dir) ? 0 : 1;
    }

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

    if (cmd == CMD_FMT) {
        if (!resolve_input_path(cmd, path, &resolved_path, &resolved_output))
            return 1;
        free(resolved_output);
        resolved_output = NULL;
        path = resolved_path;
        {
            int rc = format_source_file(path) ? 0 : 1;
            free(resolved_path);
            return rc;
        }
    }

    if (cmd == CMD_CLEAN) {
        if (!resolve_input_path(cmd, path, &resolved_path, &resolved_output))
            return 1;
        {
            int rc = clean_outputs(resolved_path, resolved_output) ? 0 : 1;
            free(resolved_path);
            free(resolved_output);
            return rc;
        }
    }

    if (!resolve_input_path(cmd, path, &resolved_path, &resolved_output))
        return 1;
    path = resolved_path;
    if (!output_path && resolved_output)
        output_path = resolved_output;

    if (cmd == CMD_BUILD || cmd == CMD_RUN) {
        char *default_out = NULL;
        char *quoted_out = NULL;
        char *run_cmd = NULL;
        int rc = 0;

        if (!output_path) {
            default_out = replace_extension(path, "");
            if (!default_out) {
                fprintf(stderr, "error: out of memory\n");
                free(resolved_path);
                free(resolved_output);
                return 1;
            }
            output_path = default_out;
        }

        rc = build_binary(path, output_path);
        if (rc != 0) {
            free(default_out);
            free(resolved_path);
            free(resolved_output);
            return rc;
        }

        if (cmd == CMD_RUN) {
            quoted_out = shell_quote(output_path);
            if (!quoted_out) {
                fprintf(stderr, "error: out of memory\n");
                free(default_out);
                free(resolved_path);
                free(resolved_output);
                return 1;
            }

            run_cmd = malloc(strlen(quoted_out) + 1);
            if (!run_cmd) {
                fprintf(stderr, "error: out of memory\n");
                free(quoted_out);
                free(default_out);
                free(resolved_path);
                free(resolved_output);
                return 1;
            }

            strcpy(run_cmd, quoted_out);
            rc = system(run_cmd);
            free(run_cmd);
            free(quoted_out);
            free(default_out);
            free(resolved_path);
            free(resolved_output);
            return rc == 0 ? 0 : 1;
        }

        free(default_out);
        free(resolved_path);
        free(resolved_output);
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
        free(resolved_path);
        free(resolved_output);
        return rc;
    }
}
