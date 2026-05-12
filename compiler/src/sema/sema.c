#include "sema.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    Node       *node;
} NamedDecl;

typedef struct {
    const char *owner;
    const char *name;
    Node       *node;
} MethodDecl;

typedef struct LocalBinding LocalBinding;
typedef struct Scope Scope;

struct LocalBinding {
    const char   *name;
    Node         *node;
    LocalBinding *next;
};

struct Scope {
    Scope        *parent;
    LocalBinding *bindings;
};

typedef struct {
    const char    *source_name;
    int            had_error;
    NamedDecl     *functions;
    int            function_count;
    int            function_cap;
    NamedDecl     *extern_functions;
    int            extern_function_count;
    int            extern_function_cap;
    NamedDecl     *structs;
    int            struct_count;
    int            struct_cap;
    NamedDecl     *enums;
    int            enum_count;
    int            enum_cap;
    NamedDecl     *unions;
    int            union_count;
    int            union_cap;
    MethodDecl    *methods;
    int            method_count;
    int            method_cap;
    const char   **global_names;
    int            global_name_count;
    int            global_name_cap;
} Analyzer;

static void sema_reset_model(SemanticModel *model) {
    memset(model, 0, sizeof(*model));
}

void sema_model_free(SemanticModel *model) {
    if (!model) return;

    free(model->functions);
    free(model->extern_functions);
    free(model->structs);
    free(model->enums);
    free(model->unions);
    free(model->impls);
    sema_reset_model(model);
}

static void sema_error(Analyzer *a, int line, int col, const char *fmt, ...) {
    va_list ap;

    fprintf(stderr, "%s:%d:%d: error: ", a->source_name, line, col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);

    a->had_error = 1;
}

static int push_named_decl(NamedDecl **items, int *count, int *cap,
                           const char *name, Node *node) {
    if (*count == *cap) {
        int new_cap = *cap ? *cap * 2 : 8;
        NamedDecl *new_items = realloc(*items, (size_t)new_cap * sizeof(NamedDecl));
        if (!new_items) return 0;
        *items = new_items;
        *cap = new_cap;
    }

    (*items)[*count].name = name;
    (*items)[*count].node = node;
    (*count)++;
    return 1;
}

static int push_method_decl(MethodDecl **items, int *count, int *cap,
                            const char *owner, const char *name, Node *node) {
    if (*count == *cap) {
        int new_cap = *cap ? *cap * 2 : 8;
        MethodDecl *new_items = realloc(*items, (size_t)new_cap * sizeof(MethodDecl));
        if (!new_items) return 0;
        *items = new_items;
        *cap = new_cap;
    }

    (*items)[*count].owner = owner;
    (*items)[*count].name  = name;
    (*items)[*count].node  = node;
    (*count)++;
    return 1;
}

static NamedDecl *find_named_decl(NamedDecl *items, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (!strcmp(items[i].name, name))
            return &items[i];
    }
    return NULL;
}

static MethodDecl *find_method_decl(Analyzer *a, const char *owner, const char *name) {
    for (int i = 0; i < a->method_count; i++) {
        if (!strcmp(a->methods[i].owner, owner) && !strcmp(a->methods[i].name, name))
            return &a->methods[i];
    }
    return NULL;
}

static int is_builtin_type(const char *name) {
    static const char *BUILTINS[] = {
        "i8", "i16", "i32", "i64", "isize",
        "u8", "u16", "u32", "u64", "usize",
        "f32", "f64",
        "bool", "char", "void", "never",
        "str", "cstr",
        NULL
    };

    for (int i = 0; BUILTINS[i]; i++) {
        if (!strcmp(BUILTINS[i], name))
            return 1;
    }
    return 0;
}

static int is_known_type(Analyzer *a, const char *name) {
    return is_builtin_type(name) ||
           find_named_decl(a->structs, a->struct_count, name) ||
           find_named_decl(a->enums, a->enum_count, name) ||
           find_named_decl(a->unions, a->union_count, name);
}

static int register_global_name(Analyzer *a, const char *name, Node *node) {
    for (int i = 0; i < a->global_name_count; i++) {
        if (!strcmp(a->global_names[i], name)) {
            sema_error(a, node->line, node->col, "duplicate top-level symbol '%s'", name);
            return 0;
        }
    }

    if (a->global_name_count == a->global_name_cap) {
        int new_cap = a->global_name_cap ? a->global_name_cap * 2 : 16;
        const char **new_items = realloc(a->global_names, (size_t)new_cap * sizeof(const char *));
        if (!new_items) {
            sema_error(a, node->line, node->col, "out of memory");
            return 0;
        }
        a->global_names = new_items;
        a->global_name_cap = new_cap;
    }

    a->global_names[a->global_name_count++] = name;
    return 1;
}

static Scope *scope_new(Scope *parent) {
    Scope *scope = malloc(sizeof(Scope));
    if (!scope) return NULL;
    scope->parent = parent;
    scope->bindings = NULL;
    return scope;
}

static void scope_free(Scope *scope) {
    if (!scope) return;

    LocalBinding *binding = scope->bindings;
    while (binding) {
        LocalBinding *next = binding->next;
        free(binding);
        binding = next;
    }

    free(scope);
}

static int scope_define(Scope *scope, const char *name, Node *node) {
    for (LocalBinding *binding = scope->bindings; binding; binding = binding->next) {
        if (!strcmp(binding->name, name))
            return 0;
    }

    LocalBinding *binding = malloc(sizeof(LocalBinding));
    if (!binding) return 0;

    binding->name = name;
    binding->node = node;
    binding->next = scope->bindings;
    scope->bindings = binding;
    return 1;
}

static Node *scope_resolve(Scope *scope, const char *name) {
    for (Scope *cur = scope; cur; cur = cur->parent) {
        for (LocalBinding *binding = cur->bindings; binding; binding = binding->next) {
            if (!strcmp(binding->name, name))
                return binding->node;
        }
    }
    return NULL;
}

static int enum_has_variant(Analyzer *a, const char *enum_name, const char *variant_name) {
    NamedDecl *entry = find_named_decl(a->enums, a->enum_count, enum_name);
    if (!entry) return 0;

    for (int i = 0; i < entry->node->as.enum_decl.count; i++) {
        if (!strcmp(entry->node->as.enum_decl.variants[i], variant_name))
            return 1;
    }
    return 0;
}

static int composite_has_field(Node *decl, const char *field_name) {
    NodeList *fields = NULL;
    if (decl->kind == NODE_STRUCT_DECL) fields = &decl->as.struct_decl.fields;
    if (decl->kind == NODE_UNION_DECL)  fields = &decl->as.union_decl.fields;
    if (!fields) return 0;

    for (int i = 0; i < fields->count; i++) {
        if (!strcmp(fields->items[i]->as.let.name, field_name))
            return 1;
    }
    return 0;
}

static int is_identifier_like_name(const char *name) {
    if (!name || !name[0]) return 0;

    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '.')) {
            return 0;
        }
    }
    return 1;
}

static int validate_type(Analyzer *a, Node *type) {
    if (!type) return 1;

    switch (type->kind) {
        case NODE_TYPE_NAMED:
            if (!is_identifier_like_name(type->as.type_named.name) ||
                !is_known_type(a, type->as.type_named.name)) {
                sema_error(a, type->line, type->col,
                           "unknown type '%s'", type->as.type_named.name);
                return 0;
            }
            return 1;
        case NODE_TYPE_POINTER:
            return validate_type(a, type->as.type_ptr.inner);
        case NODE_TYPE_ARRAY:
            return validate_type(a, type->as.type_array.elem);
        default:
            sema_error(a, type->line, type->col, "unsupported type syntax");
            return 0;
    }
}

static int validate_receiver_type(Analyzer *a, const char *owner, Node *type, int line, int col) {
    if (!type) return 0;

    if (type->kind == NODE_TYPE_NAMED && !strcmp(type->as.type_named.name, owner))
        return 1;

    if (type->kind == NODE_TYPE_POINTER &&
        type->as.type_ptr.inner &&
        type->as.type_ptr.inner->kind == NODE_TYPE_NAMED &&
        !strcmp(type->as.type_ptr.inner->as.type_named.name, owner)) {
        return 1;
    }

    sema_error(a, line, col,
               "self parameter for '%s' method must be %s or *%s",
               owner, owner, owner);
    return 0;
}

static int register_function_like(Analyzer *a, Node *decl, int is_extern) {
    const char *name = is_extern ? decl->as.extern_fn.name : decl->as.fn.name;

    if (!register_global_name(a, name, decl))
        return 0;

    if (find_named_decl(a->functions, a->function_count, name) ||
        find_named_decl(a->extern_functions, a->extern_function_count, name)) {
        sema_error(a, decl->line, decl->col, "duplicate function '%s'", name);
        return 0;
    }

    if (!validate_type(a, is_extern ? decl->as.extern_fn.ret_type : decl->as.fn.ret_type))
        return 0;

    NodeList *params = is_extern ? &decl->as.extern_fn.params : &decl->as.fn.params;
    for (int i = 0; i < params->count; i++) {
        Node *param = params->items[i];

        for (int j = 0; j < i; j++) {
            if (!strcmp(params->items[j]->as.let.name, param->as.let.name)) {
                sema_error(a, param->line, param->col,
                           "duplicate parameter '%s'", param->as.let.name);
                return 0;
            }
        }

        if (!validate_type(a, param->as.let.type))
            return 0;
    }

    return is_extern
        ? push_named_decl(&a->extern_functions, &a->extern_function_count,
                          &a->extern_function_cap, name, decl)
        : push_named_decl(&a->functions, &a->function_count,
                          &a->function_cap, name, decl);
}

static int register_struct_decl(Analyzer *a, Node *decl) {
    if (!register_global_name(a, decl->as.struct_decl.name, decl))
        return 0;

    if (find_named_decl(a->structs, a->struct_count, decl->as.struct_decl.name)) {
        sema_error(a, decl->line, decl->col, "duplicate struct '%s'", decl->as.struct_decl.name);
        return 0;
    }

    return push_named_decl(&a->structs, &a->struct_count, &a->struct_cap,
                           decl->as.struct_decl.name, decl);
}

static int register_enum_decl(Analyzer *a, Node *decl) {
    if (!register_global_name(a, decl->as.enum_decl.name, decl))
        return 0;

    if (find_named_decl(a->enums, a->enum_count, decl->as.enum_decl.name)) {
        sema_error(a, decl->line, decl->col, "duplicate enum '%s'", decl->as.enum_decl.name);
        return 0;
    }

    return push_named_decl(&a->enums, &a->enum_count, &a->enum_cap,
                           decl->as.enum_decl.name, decl);
}

static int register_union_decl(Analyzer *a, Node *decl) {
    if (!register_global_name(a, decl->as.union_decl.name, decl))
        return 0;

    if (find_named_decl(a->unions, a->union_count, decl->as.union_decl.name)) {
        sema_error(a, decl->line, decl->col, "duplicate union '%s'", decl->as.union_decl.name);
        return 0;
    }

    return push_named_decl(&a->unions, &a->union_count, &a->union_cap,
                           decl->as.union_decl.name, decl);
}

static int register_impl_decl(Analyzer *a, Node *decl) {
    if (!find_named_decl(a->structs, a->struct_count, decl->as.impl.target)) {
        sema_error(a, decl->line, decl->col,
                   "unknown impl target '%s'", decl->as.impl.target);
        return 0;
    }

    for (int i = 0; i < decl->as.impl.methods.count; i++) {
        Node *method = decl->as.impl.methods.items[i];

        if (find_method_decl(a, decl->as.impl.target, method->as.fn.name)) {
            sema_error(a, method->line, method->col,
                       "duplicate method '%s.%s'",
                       decl->as.impl.target, method->as.fn.name);
            return 0;
        }

        if (!validate_type(a, method->as.fn.ret_type))
            return 0;

        for (int j = 0; j < method->as.fn.params.count; j++) {
            Node *param = method->as.fn.params.items[j];

            for (int k = 0; k < j; k++) {
                if (!strcmp(method->as.fn.params.items[k]->as.let.name, param->as.let.name)) {
                    sema_error(a, param->line, param->col,
                               "duplicate parameter '%s'", param->as.let.name);
                    return 0;
                }
            }

            if (!validate_type(a, param->as.let.type))
                return 0;

            if (j == 0 && !strcmp(param->as.let.name, "self") &&
                !validate_receiver_type(a, decl->as.impl.target, param->as.let.type,
                                        param->line, param->col)) {
                return 0;
            }
        }

        if (!push_method_decl(&a->methods, &a->method_count, &a->method_cap,
                              decl->as.impl.target, method->as.fn.name, method)) {
            sema_error(a, method->line, method->col, "out of memory");
            return 0;
        }
    }

    return 1;
}

static int validate_fields(Analyzer *a, NodeList *fields, const char *kind_name) {
    for (int i = 0; i < fields->count; i++) {
        Node *field = fields->items[i];
        for (int j = 0; j < i; j++) {
            if (!strcmp(fields->items[j]->as.let.name, field->as.let.name)) {
                sema_error(a, field->line, field->col,
                           "duplicate %s field '%s'", kind_name, field->as.let.name);
                return 0;
            }
        }

        if (!validate_type(a, field->as.let.type))
            return 0;
    }

    return 1;
}

static int validate_enum_variants(Analyzer *a, Node *decl) {
    for (int i = 0; i < decl->as.enum_decl.count; i++) {
        const char *variant = decl->as.enum_decl.variants[i];
        for (int j = 0; j < i; j++) {
            if (!strcmp(decl->as.enum_decl.variants[j], variant)) {
                sema_error(a, decl->line, decl->col,
                           "duplicate enum variant '%s.%s'",
                           decl->as.enum_decl.name, variant);
                return 0;
            }
        }
    }
    return 1;
}

static int analyze_expr(Analyzer *a, Scope *scope, Node *expr);

static int analyze_match_pattern(Analyzer *a, const char *pattern, int line, int col) {
    const char *dot;
    char enum_name[128];
    char variant_name[128];
    size_t enum_len;

    if (!strcmp(pattern, "_"))
        return 1;

    dot = strchr(pattern, '.');
    if (!dot) {
        sema_error(a, line, col, "invalid match pattern '%s'", pattern);
        return 0;
    }

    enum_len = (size_t)(dot - pattern);
    if (enum_len == 0 || enum_len >= sizeof(enum_name)) {
        sema_error(a, line, col, "invalid match pattern '%s'", pattern);
        return 0;
    }

    memcpy(enum_name, pattern, enum_len);
    enum_name[enum_len] = '\0';

    if (strlen(dot + 1) >= sizeof(variant_name)) {
        sema_error(a, line, col, "invalid match pattern '%s'", pattern);
        return 0;
    }

    strcpy(variant_name, dot + 1);

    if (!find_named_decl(a->enums, a->enum_count, enum_name)) {
        sema_error(a, line, col, "unknown enum '%s' in match pattern", enum_name);
        return 0;
    }

    if (!enum_has_variant(a, enum_name, variant_name)) {
        sema_error(a, line, col, "unknown enum variant '%s.%s'", enum_name, variant_name);
        return 0;
    }

    return 1;
}

static int analyze_expr(Analyzer *a, Scope *scope, Node *expr) {
    if (!expr) return 1;

    switch (expr->kind) {
        case NODE_LIT_INT:
        case NODE_LIT_FLOAT:
        case NODE_LIT_STRING:
        case NODE_LIT_BOOL:
        case NODE_LIT_NULL:
            return 1;

        case NODE_IDENT:
            if (!scope_resolve(scope, expr->as.ident.name)) {
                sema_error(a, expr->line, expr->col,
                           "unknown symbol '%s'", expr->as.ident.name);
                return 0;
            }
            return 1;

        case NODE_GROUP:
            return analyze_expr(a, scope, expr->as.group.expr);

        case NODE_UNARY:
            return analyze_expr(a, scope, expr->as.unary.operand);

        case NODE_BINARY:
            return analyze_expr(a, scope, expr->as.binary.left) &&
                   analyze_expr(a, scope, expr->as.binary.right);

        case NODE_ASSIGN:
            return analyze_expr(a, scope, expr->as.assign.target) &&
                   analyze_expr(a, scope, expr->as.assign.value);

        case NODE_INDEX:
            return analyze_expr(a, scope, expr->as.index_expr.object) &&
                   analyze_expr(a, scope, expr->as.index_expr.index);

        case NODE_CALL:
            if (!analyze_expr(a, scope, expr->as.call.callee))
                return 0;
            for (int i = 0; i < expr->as.call.args.count; i++) {
                if (!analyze_expr(a, scope, expr->as.call.args.items[i]))
                    return 0;
            }
            return 1;

        case NODE_CAST:
            return validate_type(a, expr->as.cast.type) &&
                   analyze_expr(a, scope, expr->as.cast.expr);

        case NODE_FIELD:
            if (expr->as.field.object->kind == NODE_IDENT) {
                const char *base = expr->as.field.object->as.ident.name;

                if (find_named_decl(a->enums, a->enum_count, base)) {
                    if (!enum_has_variant(a, base, expr->as.field.field) &&
                        !find_method_decl(a, base, expr->as.field.field)) {
                        sema_error(a, expr->line, expr->col,
                                   "unknown enum member '%s.%s'",
                                   base, expr->as.field.field);
                        return 0;
                    }
                    return 1;
                }

                if (is_known_type(a, base))
                    return 1;
            }
            return analyze_expr(a, scope, expr->as.field.object);

        case NODE_STRUCT_LIT: {
            NamedDecl *decl = find_named_decl(a->structs, a->struct_count, expr->as.struct_lit.type_name);
            if (!decl)
                decl = find_named_decl(a->unions, a->union_count, expr->as.struct_lit.type_name);

            if (!decl) {
                sema_error(a, expr->line, expr->col,
                           "unknown composite type '%s'", expr->as.struct_lit.type_name);
                return 0;
            }

            for (int i = 0; i < expr->as.struct_lit.count; i++) {
                for (int j = 0; j < i; j++) {
                    if (!strcmp(expr->as.struct_lit.field_names[j], expr->as.struct_lit.field_names[i])) {
                        sema_error(a, expr->line, expr->col,
                                   "duplicate struct literal field '%s'",
                                   expr->as.struct_lit.field_names[i]);
                        return 0;
                    }
                }

                if (!composite_has_field(decl->node, expr->as.struct_lit.field_names[i])) {
                    sema_error(a, expr->line, expr->col,
                               "unknown field '%s' for '%s'",
                               expr->as.struct_lit.field_names[i], expr->as.struct_lit.type_name);
                    return 0;
                }

                if (!analyze_expr(a, scope, expr->as.struct_lit.field_values[i]))
                    return 0;
            }
            return 1;
        }

        case NODE_MATCH:
            if (!analyze_expr(a, scope, expr->as.match.subject))
                return 0;
            for (int i = 0; i < expr->as.match.count; i++) {
                if (!analyze_match_pattern(a, expr->as.match.patterns[i], expr->line, expr->col) ||
                    !analyze_expr(a, scope, expr->as.match.values[i])) {
                    return 0;
                }
            }
            return 1;

        default:
            sema_error(a, expr->line, expr->col, "unsupported expression kind");
            return 0;
    }
}

static int analyze_block(Analyzer *a, Scope *scope, Node *block, int loop_depth);

static int analyze_stmt(Analyzer *a, Scope *scope, Node *stmt, int loop_depth) {
    if (!stmt) return 1;

    switch (stmt->kind) {
        case NODE_LET:
            if (stmt->as.let.type && !validate_type(a, stmt->as.let.type))
                return 0;
            if (stmt->as.let.value && !analyze_expr(a, scope, stmt->as.let.value))
                return 0;
            if (!scope_define(scope, stmt->as.let.name, stmt)) {
                sema_error(a, stmt->line, stmt->col,
                           "duplicate local '%s'", stmt->as.let.name);
                return 0;
            }
            return 1;

        case NODE_CONST:
            if (stmt->as.konst.type && !validate_type(a, stmt->as.konst.type))
                return 0;
            if (!analyze_expr(a, scope, stmt->as.konst.value))
                return 0;
            if (!scope_define(scope, stmt->as.konst.name, stmt)) {
                sema_error(a, stmt->line, stmt->col,
                           "duplicate local '%s'", stmt->as.konst.name);
                return 0;
            }
            return 1;

        case NODE_RETURN:
            return !stmt->as.ret.value || analyze_expr(a, scope, stmt->as.ret.value);

        case NODE_IF: {
            Scope *then_scope;
            Scope *else_scope;
            int ok = analyze_expr(a, scope, stmt->as.if_stmt.cond);
            if (!ok) return 0;

            then_scope = scope_new(scope);
            else_scope = scope_new(scope);
            if (!then_scope || !else_scope) {
                scope_free(then_scope);
                scope_free(else_scope);
                sema_error(a, stmt->line, stmt->col, "out of memory");
                return 0;
            }

            ok = analyze_block(a, then_scope, stmt->as.if_stmt.then_block, loop_depth);
            if (ok && stmt->as.if_stmt.else_node) {
                if (stmt->as.if_stmt.else_node->kind == NODE_BLOCK)
                    ok = analyze_block(a, else_scope, stmt->as.if_stmt.else_node, loop_depth);
                else
                    ok = analyze_stmt(a, else_scope, stmt->as.if_stmt.else_node, loop_depth);
            }

            scope_free(then_scope);
            scope_free(else_scope);
            return ok;
        }

        case NODE_WHILE: {
            Scope *body_scope;
            int ok = analyze_expr(a, scope, stmt->as.while_stmt.cond);
            if (!ok) return 0;

            body_scope = scope_new(scope);
            if (!body_scope) {
                sema_error(a, stmt->line, stmt->col, "out of memory");
                return 0;
            }

            ok = analyze_block(a, body_scope, stmt->as.while_stmt.body, loop_depth + 1);
            scope_free(body_scope);
            return ok;
        }

        case NODE_LOOP:
        case NODE_UNSAFE: {
            Scope *body_scope = scope_new(scope);
            int next_loop_depth = loop_depth + (stmt->kind == NODE_LOOP ? 1 : 0);
            int ok;

            if (!body_scope) {
                sema_error(a, stmt->line, stmt->col, "out of memory");
                return 0;
            }

            ok = analyze_block(a, body_scope, stmt->as.loop_stmt.body, next_loop_depth);
            scope_free(body_scope);
            return ok;
        }

        case NODE_FOR: {
            Scope *for_scope = scope_new(scope);
            int ok = 1;

            if (!for_scope) {
                sema_error(a, stmt->line, stmt->col, "out of memory");
                return 0;
            }

            if (stmt->as.for_stmt.init) {
                ok = analyze_stmt(a, for_scope, stmt->as.for_stmt.init, loop_depth);
            }
            if (ok && stmt->as.for_stmt.cond)
                ok = analyze_expr(a, for_scope, stmt->as.for_stmt.cond);
            if (ok && stmt->as.for_stmt.post)
                ok = analyze_expr(a, for_scope, stmt->as.for_stmt.post);
            if (ok) {
                Scope *body_scope = scope_new(for_scope);
                if (!body_scope) {
                    scope_free(for_scope);
                    return 0;
                }
                ok = analyze_block(a, body_scope, stmt->as.for_stmt.body, loop_depth + 1);
                scope_free(body_scope);
            }
            scope_free(for_scope);
            return ok;
        }

        case NODE_BREAK:
            if (loop_depth <= 0) {
                sema_error(a, stmt->line, stmt->col,
                           "'break' can only be used inside a loop");
                return 0;
            }
            return 1;

        case NODE_CONTINUE:
            if (loop_depth <= 0) {
                sema_error(a, stmt->line, stmt->col,
                           "'continue' can only be used inside a loop");
                return 0;
            }
            return 1;

        case NODE_DEFER:
            return analyze_expr(a, scope, stmt->as.defer_stmt.expr);

        case NODE_EXPR_STMT:
            return analyze_expr(a, scope, stmt->as.expr_stmt.expr);

        case NODE_BLOCK: {
            Scope *inner = scope_new(scope);
            int ok;

            if (!inner) {
                sema_error(a, stmt->line, stmt->col, "out of memory");
                return 0;
            }

            ok = analyze_block(a, inner, stmt, loop_depth);
            scope_free(inner);
            return ok;
        }

        default:
            sema_error(a, stmt->line, stmt->col, "unsupported statement kind");
            return 0;
    }
}

static int analyze_block(Analyzer *a, Scope *scope, Node *block, int loop_depth) {
    for (int i = 0; i < block->as.block.stmts.count; i++) {
        if (!analyze_stmt(a, scope, block->as.block.stmts.items[i], loop_depth))
            return 0;
    }
    return 1;
}

static int analyze_function_body(Analyzer *a, Node *fn_decl) {
    Scope *scope = scope_new(NULL);
    int ok = 1;

    if (!scope) {
        sema_error(a, fn_decl->line, fn_decl->col, "out of memory");
        return 0;
    }

    for (int i = 0; i < a->function_count; i++) {
        if (!scope_define(scope, a->functions[i].name, a->functions[i].node)) {
            ok = 0;
            break;
        }
    }

    for (int i = 0; ok && i < a->extern_function_count; i++) {
        if (!scope_define(scope, a->extern_functions[i].name, a->extern_functions[i].node)) {
            ok = 0;
            break;
        }
    }

    for (int i = 0; ok && i < fn_decl->as.fn.params.count; i++) {
        Node *param = fn_decl->as.fn.params.items[i];
        if (!scope_define(scope, param->as.let.name, param)) {
            sema_error(a, param->line, param->col,
                       "duplicate parameter '%s'", param->as.let.name);
            ok = 0;
        }
    }

    if (ok)
        ok = analyze_block(a, scope, fn_decl->as.fn.body, 0);

    scope_free(scope);
    return ok;
}

static Node **copy_nodes_from_named(NamedDecl *items, int count) {
    Node **nodes;

    if (count == 0)
        return NULL;

    nodes = malloc((size_t)count * sizeof(Node *));
    if (!nodes)
        return NULL;

    for (int i = 0; i < count; i++)
        nodes[i] = items[i].node;

    return nodes;
}

static Node **copy_impl_nodes(Node *program, int *out_count) {
    Node **nodes;
    int count = 0;

    for (int i = 0; i < program->as.program.decls.count; i++) {
        if (program->as.program.decls.items[i]->kind == NODE_IMPL_DECL)
            count++;
    }

    *out_count = count;
    if (count == 0)
        return NULL;

    nodes = malloc((size_t)count * sizeof(Node *));
    if (!nodes)
        return NULL;

    count = 0;
    for (int i = 0; i < program->as.program.decls.count; i++) {
        if (program->as.program.decls.items[i]->kind == NODE_IMPL_DECL)
            nodes[count++] = program->as.program.decls.items[i];
    }

    return nodes;
}

int sema_analyze(Node *program, const char *source_name, SemanticModel *out) {
    Analyzer a;

    memset(&a, 0, sizeof(a));
    a.source_name = source_name;
    sema_reset_model(out);

    if (!program || program->kind != NODE_PROGRAM) {
        fprintf(stderr, "%s:0:0: error: invalid AST root\n", source_name);
        return 0;
    }

    for (int i = 0; i < program->as.program.decls.count && !a.had_error; i++) {
        Node *decl = program->as.program.decls.items[i];

        switch (decl->kind) {
            case NODE_STRUCT_DECL:
                if (!register_struct_decl(&a, decl))
                    a.had_error = 1;
                break;
            case NODE_ENUM_DECL:
                if (!register_enum_decl(&a, decl))
                    a.had_error = 1;
                break;
            case NODE_UNION_DECL:
                if (!register_union_decl(&a, decl))
                    a.had_error = 1;
                break;
            default:
                break;
        }
    }

    for (int i = 0; i < program->as.program.decls.count && !a.had_error; i++) {
        Node *decl = program->as.program.decls.items[i];

        switch (decl->kind) {
            case NODE_STRUCT_DECL:
                if (!validate_fields(&a, &decl->as.struct_decl.fields, "struct"))
                    a.had_error = 1;
                break;
            case NODE_UNION_DECL:
                if (!validate_fields(&a, &decl->as.union_decl.fields, "union"))
                    a.had_error = 1;
                break;
            case NODE_ENUM_DECL:
                if (!validate_enum_variants(&a, decl))
                    a.had_error = 1;
                break;
            default:
                break;
        }
    }

    for (int i = 0; i < program->as.program.decls.count && !a.had_error; i++) {
        Node *decl = program->as.program.decls.items[i];

        switch (decl->kind) {
            case NODE_FN_DECL:
                if (!register_function_like(&a, decl, 0)) {
                    if (!a.had_error)
                        sema_error(&a, decl->line, decl->col, "out of memory");
                }
                break;
            case NODE_EXTERN_FN:
                if (!register_function_like(&a, decl, 1)) {
                    if (!a.had_error)
                        sema_error(&a, decl->line, decl->col, "out of memory");
                }
                break;
            case NODE_IMPL_DECL:
                if (!register_impl_decl(&a, decl))
                    a.had_error = 1;
                break;
            default:
                break;
        }
    }

    for (int i = 0; i < program->as.program.decls.count && !a.had_error; i++) {
        Node *decl = program->as.program.decls.items[i];

        if (decl->kind == NODE_FN_DECL) {
            if (!analyze_function_body(&a, decl))
                a.had_error = 1;
        } else if (decl->kind == NODE_IMPL_DECL) {
            for (int j = 0; j < decl->as.impl.methods.count && !a.had_error; j++) {
                if (!analyze_function_body(&a, decl->as.impl.methods.items[j]))
                    a.had_error = 1;
            }
        }
    }

    if (!a.had_error) {
        for (int i = 0; i < program->as.program.imports.count; i++) {
            if (!program->as.program.imports.items[i]->as.pkg.path[0]) {
                sema_error(&a, program->as.program.imports.items[i]->line,
                           program->as.program.imports.items[i]->col,
                           "import path cannot be empty");
            }
        }
    }

    if (a.had_error) {
        free(a.functions);
        free(a.extern_functions);
        free(a.structs);
        free(a.enums);
        free(a.unions);
        free(a.methods);
        free(a.global_names);
        return 0;
    }

    out->functions = copy_nodes_from_named(a.functions, a.function_count);
    out->extern_functions = copy_nodes_from_named(a.extern_functions, a.extern_function_count);
    out->structs = copy_nodes_from_named(a.structs, a.struct_count);
    out->enums = copy_nodes_from_named(a.enums, a.enum_count);
    out->unions = copy_nodes_from_named(a.unions, a.union_count);
    out->impls = copy_impl_nodes(program, &out->impl_count);
    out->function_count = a.function_count;
    out->extern_function_count = a.extern_function_count;
    out->struct_count = a.struct_count;
    out->enum_count = a.enum_count;
    out->union_count = a.union_count;

    if ((a.function_count && !out->functions) ||
        (a.extern_function_count && !out->extern_functions) ||
        (a.struct_count && !out->structs) ||
        (a.enum_count && !out->enums) ||
        (a.union_count && !out->unions) ||
        (out->impl_count && !out->impls)) {
        sema_model_free(out);
        free(a.functions);
        free(a.extern_functions);
        free(a.structs);
        free(a.enums);
        free(a.unions);
        free(a.methods);
        free(a.global_names);
        fprintf(stderr, "%s:0:0: error: out of memory\n", source_name);
        return 0;
    }

    free(a.functions);
    free(a.extern_functions);
    free(a.structs);
    free(a.enums);
    free(a.unions);
    free(a.methods);
    free(a.global_names);
    return 1;
}
