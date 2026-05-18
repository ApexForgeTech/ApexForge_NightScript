#include "typeck.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TYPE_ERROR,
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_CHAR,
    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,
    TYPE_ISIZE,
    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,
    TYPE_USIZE,
    TYPE_F32,
    TYPE_F64,
    TYPE_STR,
    TYPE_CSTR,
    TYPE_NAMED,
    TYPE_POINTER,
    TYPE_ARRAY,
    TYPE_FUNCTION,
    TYPE_INT_LITERAL,
    TYPE_FLOAT_LITERAL,
    TYPE_STRING_LITERAL,
    TYPE_NULL,
} TypeKind;

typedef struct Type Type;
typedef struct TypeAlloc TypeAlloc;
typedef struct Binding Binding;
typedef struct Scope Scope;

struct Type {
    TypeKind kind;
    const char *name;
    Type *inner;
    Node *decl;
    int is_const;
    int is_nullable;
    int array_len;
};

struct TypeAlloc {
    Type value;
    TypeAlloc *next;
};

struct Binding {
    const char *name;
    Type *type;
    int mutable;
    Binding *next;
};

struct Scope {
    Scope *parent;
    Binding *bindings;
};

typedef struct {
    const char *source_name;
    const SemanticModel *sema;
    Node *program;               /* root program node — for global const lookup */
    Type *current_return_type;
    const char *current_package;
    int had_error;
    TypeAlloc *allocs;
} Checker;

static Type TYPE_ERROR_VALUE = { TYPE_ERROR, "<error>", NULL, NULL, 0, 0, -1 };
static Type TYPE_VOID_VALUE  = { TYPE_VOID, "void", NULL, NULL, 0, 0, -1 };
static Type TYPE_BOOL_VALUE  = { TYPE_BOOL, "bool", NULL, NULL, 0, 0, -1 };
static Type TYPE_CHAR_VALUE  = { TYPE_CHAR, "char", NULL, NULL, 0, 0, -1 };
static Type TYPE_I8_VALUE    = { TYPE_I8, "i8", NULL, NULL, 0, 0, -1 };
static Type TYPE_I16_VALUE   = { TYPE_I16, "i16", NULL, NULL, 0, 0, -1 };
static Type TYPE_I32_VALUE   = { TYPE_I32, "i32", NULL, NULL, 0, 0, -1 };
static Type TYPE_I64_VALUE   = { TYPE_I64, "i64", NULL, NULL, 0, 0, -1 };
static Type TYPE_ISIZE_VALUE = { TYPE_ISIZE, "isize", NULL, NULL, 0, 0, -1 };
static Type TYPE_U8_VALUE    = { TYPE_U8, "u8", NULL, NULL, 0, 0, -1 };
static Type TYPE_U16_VALUE   = { TYPE_U16, "u16", NULL, NULL, 0, 0, -1 };
static Type TYPE_U32_VALUE   = { TYPE_U32, "u32", NULL, NULL, 0, 0, -1 };
static Type TYPE_U64_VALUE   = { TYPE_U64, "u64", NULL, NULL, 0, 0, -1 };
static Type TYPE_USIZE_VALUE = { TYPE_USIZE, "usize", NULL, NULL, 0, 0, -1 };
static Type TYPE_F32_VALUE   = { TYPE_F32, "f32", NULL, NULL, 0, 0, -1 };
static Type TYPE_F64_VALUE   = { TYPE_F64, "f64", NULL, NULL, 0, 0, -1 };
static Type TYPE_STR_VALUE   = { TYPE_STR, "str", NULL, NULL, 0, 0, -1 };
static Type TYPE_CSTR_VALUE  = { TYPE_CSTR, "cstr", NULL, NULL, 0, 0, -1 };
static Type TYPE_INT_LIT_VALUE = { TYPE_INT_LITERAL, "<int literal>", NULL, NULL, 0, 0, -1 };
static Type TYPE_FLOAT_LIT_VALUE = { TYPE_FLOAT_LITERAL, "<float literal>", NULL, NULL, 0, 0, -1 };
static Type TYPE_STRING_LIT_VALUE = { TYPE_STRING_LITERAL, "<string literal>", NULL, NULL, 0, 0, -1 };
static Type TYPE_NULL_VALUE  = { TYPE_NULL, "null", NULL, NULL, 0, 0, -1 };

static void typeck_error(Checker *c, int line, int col, const char *fmt, ...) {
    va_list ap;

    fprintf(stderr, "%s:%d:%d: error: ", c->source_name, line, col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    c->had_error = 1;
}

static Type *alloc_type(Checker *c) {
    TypeAlloc *node = calloc(1, sizeof(TypeAlloc));
    if (!node) return NULL;
    node->next = c->allocs;
    c->allocs = node;
    return &node->value;
}

static void free_types(Checker *c) {
    TypeAlloc *node = c->allocs;
    while (node) {
        TypeAlloc *next = node->next;
        free(node);
        node = next;
    }
    c->allocs = NULL;
}

static Type *make_named_type(Checker *c, const char *name) {
    Type *t = alloc_type(c);
    if (!t) return &TYPE_ERROR_VALUE;
    t->kind = TYPE_NAMED;
    t->name = name;
    t->array_len = -1;
    return t;
}

static Type *make_pointer_type(Checker *c, Type *inner, int is_const, int is_nullable) {
    Type *t = alloc_type(c);
    if (!t) return &TYPE_ERROR_VALUE;
    t->kind = TYPE_POINTER;
    t->name = "*";
    t->inner = inner;
    t->is_const = is_const;
    t->is_nullable = is_nullable;
    t->array_len = -1;
    return t;
}

static Type *make_array_type(Checker *c, Type *inner, int length) {
    Type *t = alloc_type(c);
    if (!t) return &TYPE_ERROR_VALUE;
    t->kind = TYPE_ARRAY;
    t->name = "[]";
    t->inner = inner;
    t->array_len = length;
    return t;
}

static Type *make_function_type(Checker *c, Node *decl) {
    Type *t = alloc_type(c);
    if (!t) return &TYPE_ERROR_VALUE;
    t->kind = TYPE_FUNCTION;
    t->name = "<function>";
    t->decl = decl;
    t->array_len = -1;
    return t;
}

static Scope *scope_new(Scope *parent) {
    Scope *scope = malloc(sizeof(Scope));
    if (!scope) return NULL;
    scope->parent = parent;
    scope->bindings = NULL;
    return scope;
}

static void scope_free(Scope *scope) {
    Binding *binding;

    if (!scope) return;

    binding = scope->bindings;
    while (binding) {
        Binding *next = binding->next;
        free(binding);
        binding = next;
    }

    free(scope);
}

static int scope_define(Scope *scope, const char *name, Type *type, int mutable) {
    Binding *binding;

    for (binding = scope->bindings; binding; binding = binding->next) {
        if (!strcmp(binding->name, name))
            return 0;
    }

    binding = malloc(sizeof(Binding));
    if (!binding) return 0;

    binding->name = name;
    binding->type = type;
    binding->mutable = mutable;
    binding->next = scope->bindings;
    scope->bindings = binding;
    return 1;
}

static Binding *scope_resolve(Scope *scope, const char *name) {
    for (Scope *cur = scope; cur; cur = cur->parent) {
        for (Binding *binding = cur->bindings; binding; binding = binding->next) {
            if (!strcmp(binding->name, name))
                return binding;
        }
    }
    return NULL;
}

static Node *find_named_node(Node **items, int count, const char *name, NodeKind kind) {
    for (int i = 0; i < count; i++) {
        Node *node = items[i];
        const char *candidate = NULL;

        if (node->kind != kind)
            continue;

        if (kind == NODE_FN_DECL) candidate = node->as.fn.name;
        if (kind == NODE_EXTERN_FN) candidate = node->as.extern_fn.name;
        if (kind == NODE_STRUCT_DECL) candidate = node->as.struct_decl.name;
        if (kind == NODE_ENUM_DECL) candidate = node->as.enum_decl.name;
        if (kind == NODE_UNION_DECL) candidate = node->as.union_decl.name;
        if (candidate && !strcmp(candidate, name))
            return node;
    }
    return NULL;
}

static Node *find_method_decl(const SemanticModel *sema, const char *owner, const char *method) {
    for (int i = 0; i < sema->impl_count; i++) {
        Node *impl = sema->impls[i];
        if (strcmp(impl->as.impl.target, owner))
            continue;

        for (int j = 0; j < impl->as.impl.methods.count; j++) {
            Node *candidate = impl->as.impl.methods.items[j];
            if (!strcmp(candidate->as.fn.name, method))
                return candidate;
        }
    }
    return NULL;
}

static const char *node_package_name(Node *node) {
    if (!node)
        return NULL;

    switch (node->kind) {
        case NODE_FN_DECL:
            return node->as.fn.package_name;
        case NODE_EXTERN_FN:
            return node->as.extern_fn.package_name;
        case NODE_STRUCT_DECL:
            return node->as.struct_decl.package_name;
        case NODE_ENUM_DECL:
            return node->as.enum_decl.package_name;
        case NODE_UNION_DECL:
            return node->as.union_decl.package_name;
        case NODE_INTERFACE_DECL:
            return node->as.interface_decl.package_name;
        default:
            return NULL;
    }
}

static int node_is_public(Node *node) {
    if (!node)
        return 0;

    switch (node->kind) {
        case NODE_FN_DECL:
            return node->as.fn.is_public;
        case NODE_EXTERN_FN:
            return node->as.extern_fn.is_public;
        case NODE_STRUCT_DECL:
            return node->as.struct_decl.is_public;
        case NODE_ENUM_DECL:
            return node->as.enum_decl.is_public;
        case NODE_UNION_DECL:
            return node->as.union_decl.is_public;
        case NODE_INTERFACE_DECL:
            return node->as.interface_decl.is_public;
        default:
            return 0;
    }
}

static int same_package_name(const char *a, const char *b) {
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    return !strcmp(a, b);
}

static int decl_visible_from(Checker *c, Node *node) {
    if (!node)
        return 0;
    if (node_is_public(node))
        return 1;
    return same_package_name(c->current_package, node_package_name(node));
}

static int require_decl_visible(Checker *c, Node *node, int line, int col, const char *name) {
    if (decl_visible_from(c, node))
        return 1;

    typeck_error(c, line, col,
                 "symbol '%s' is private to package '%s'",
                 name,
                 node_package_name(node) ? node_package_name(node) : "<unknown>");
    return 0;
}

static int is_integer_kind(TypeKind kind) {
    return kind == TYPE_CHAR || kind == TYPE_I8 || kind == TYPE_I16 || kind == TYPE_I32 || kind == TYPE_I64 ||
           kind == TYPE_ISIZE || kind == TYPE_U8 || kind == TYPE_U16 || kind == TYPE_U32 ||
           kind == TYPE_U64 || kind == TYPE_USIZE || kind == TYPE_INT_LITERAL;
}

static int ensure_type_visible(Checker *c, Node *type_node, int line, int col) {
    Node *decl;

    if (!type_node)
        return 1;

    switch (type_node->kind) {
        case NODE_TYPE_POINTER:
            return ensure_type_visible(c, type_node->as.type_ptr.inner, line, col);
        case NODE_TYPE_ARRAY:
            return ensure_type_visible(c, type_node->as.type_array.elem, line, col);
        case NODE_TYPE_NAMED:
            decl = find_named_node(c->sema->structs, c->sema->struct_count,
                                   type_node->as.type_named.name, NODE_STRUCT_DECL);
            if (!decl)
                decl = find_named_node(c->sema->enums, c->sema->enum_count,
                                       type_node->as.type_named.name, NODE_ENUM_DECL);
            if (!decl)
                decl = find_named_node(c->sema->unions, c->sema->union_count,
                                       type_node->as.type_named.name, NODE_UNION_DECL);
            if (!decl)
                return 1;
            return require_decl_visible(c, decl, line, col, type_node->as.type_named.name);
        default:
            return 1;
    }
}

static int ensure_resolved_type_visible(Checker *c, Type *type, int line, int col) {
    Node *decl;

    if (!type)
        return 1;

    switch (type->kind) {
        case TYPE_FUNCTION:
            return require_decl_visible(c, type->decl, line, col,
                                        type->decl && type->decl->kind == NODE_EXTERN_FN
                                            ? type->decl->as.extern_fn.name
                                            : (type->decl ? type->decl->as.fn.name : "<function>"));
        case TYPE_POINTER:
            return ensure_resolved_type_visible(c, type->inner, line, col);
        case TYPE_ARRAY:
            return ensure_resolved_type_visible(c, type->inner, line, col);
        case TYPE_NAMED:
            if (!strcmp(type->name, "String"))
                return 1;
            decl = find_named_node(c->sema->structs, c->sema->struct_count,
                                   type->name, NODE_STRUCT_DECL);
            if (!decl)
                decl = find_named_node(c->sema->enums, c->sema->enum_count,
                                       type->name, NODE_ENUM_DECL);
            if (!decl)
                decl = find_named_node(c->sema->unions, c->sema->union_count,
                                       type->name, NODE_UNION_DECL);
            if (!decl)
                return 1;
            return require_decl_visible(c, decl, line, col, type->name);
        default:
            return 1;
    }
}

static int is_float_kind(TypeKind kind) {
    return kind == TYPE_F32 || kind == TYPE_F64 || kind == TYPE_FLOAT_LITERAL;
}

static int is_numeric_type(Type *type) {
    return type && (is_integer_kind(type->kind) || is_float_kind(type->kind));
}

static int is_addressable(Node *expr) {
    return expr &&
           (expr->kind == NODE_IDENT  ||
            expr->kind == NODE_FIELD  ||
            expr->kind == NODE_INDEX  ||
            expr->kind == NODE_SLICE  ||
            (expr->kind == NODE_UNARY && expr->as.unary.op && expr->as.unary.op[0] == '*'));
}

static const char *builtin_name(TypeKind kind) {
    switch (kind) {
        case TYPE_VOID: return "void";
        case TYPE_BOOL: return "bool";
        case TYPE_CHAR: return "char";
        case TYPE_I8: return "i8";
        case TYPE_I16: return "i16";
        case TYPE_I32: return "i32";
        case TYPE_I64: return "i64";
        case TYPE_ISIZE: return "isize";
        case TYPE_U8: return "u8";
        case TYPE_U16: return "u16";
        case TYPE_U32: return "u32";
        case TYPE_U64: return "u64";
        case TYPE_USIZE: return "usize";
        case TYPE_F32: return "f32";
        case TYPE_F64: return "f64";
        case TYPE_STR: return "str";
        case TYPE_CSTR: return "cstr";
        case TYPE_INT_LITERAL: return "<int literal>";
        case TYPE_FLOAT_LITERAL: return "<float literal>";
        case TYPE_STRING_LITERAL: return "<string literal>";
        case TYPE_NULL: return "null";
        default: return NULL;
    }
}

static void format_type(Type *type, char *buf, size_t size) {
    if (!type) {
        snprintf(buf, size, "<unknown>");
        return;
    }

    switch (type->kind) {
        case TYPE_NAMED:
            snprintf(buf, size, "%s", type->name);
            return;
        case TYPE_POINTER: {
            char inner[128];
            format_type(type->inner, inner, sizeof(inner));
            if (type->is_const)
                snprintf(buf, size, "%s*const", inner);
            else
                snprintf(buf, size, "%s*", inner);
            return;
        }
        case TYPE_ARRAY: {
            char inner[128];
            format_type(type->inner, inner, sizeof(inner));
            if (type->array_len >= 0)
                snprintf(buf, size, "[%d]%s", type->array_len, inner);
            else
                snprintf(buf, size, "[]%s", inner);
            return;
        }
        case TYPE_FUNCTION:
            snprintf(buf, size, "<function>");
            return;
        default: {
            const char *name = builtin_name(type->kind);
            snprintf(buf, size, "%s", name ? name : "<type>");
            return;
        }
    }
}

static int type_equals(Type *a, Type *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->kind != b->kind) return 0;

    switch (a->kind) {
        case TYPE_NAMED:
            return !strcmp(a->name, b->name);
        case TYPE_POINTER:
            return a->is_const == b->is_const &&
                   a->is_nullable == b->is_nullable &&
                   type_equals(a->inner, b->inner);
        case TYPE_ARRAY:
            return a->array_len == b->array_len &&
                   type_equals(a->inner, b->inner);
        case TYPE_FUNCTION:
            return a->decl == b->decl;
        default:
            return 1;
    }
}

static int is_assignable(Type *expected, Type *actual) {
    if (!expected || !actual) return 0;
    if (expected->kind == TYPE_ERROR || actual->kind == TYPE_ERROR) return 1;
    if (type_equals(expected, actual)) return 1;

    if (actual->kind == TYPE_INT_LITERAL &&
        (is_integer_kind(expected->kind) || expected->kind == TYPE_CHAR))
        return 1;
    if (actual->kind == TYPE_FLOAT_LITERAL && is_float_kind(expected->kind))
        return 1;
    if (actual->kind == TYPE_STRING_LITERAL &&
        (expected->kind == TYPE_STR || expected->kind == TYPE_CSTR))
        return 1;
    /* null is assignable to any pointer type — raw pointers need null initialization */
    if (actual->kind == TYPE_NULL && expected->kind == TYPE_POINTER)
        return 1;

    return 0;
}

static int is_castable(Type *source, Type *target) {
    if (!source || !target) return 0;
    if (source->kind == TYPE_ERROR || target->kind == TYPE_ERROR) return 1;
    if (type_equals(source, target)) return 1;

    if (is_numeric_type(source) && is_numeric_type(target))
        return 1;

    if (source->kind == TYPE_POINTER && target->kind == TYPE_POINTER)
        return 1;

    if ((source->kind == TYPE_POINTER && is_integer_kind(target->kind)) ||
        (target->kind == TYPE_POINTER && is_integer_kind(source->kind)))
        return 1;

    if ((source->kind == TYPE_NULL && target->kind == TYPE_POINTER) ||
        (target->kind == TYPE_NULL && source->kind == TYPE_POINTER))
        return 1;

    return 0;
}

static Type *widen_literal(Type *type) {
    if (!type) return &TYPE_ERROR_VALUE;
    if (type->kind == TYPE_INT_LITERAL) return &TYPE_I32_VALUE;
    if (type->kind == TYPE_FLOAT_LITERAL) return &TYPE_F64_VALUE;
    if (type->kind == TYPE_STRING_LITERAL) return &TYPE_STR_VALUE;
    return type;
}

static Type *type_from_ast(Checker *c, Node *type_node) {
    if (!type_node) return &TYPE_VOID_VALUE;

    switch (type_node->kind) {
        case NODE_TYPE_NAMED: {
            const char *name = type_node->as.type_named.name;
            if (!strcmp(name, "void")) return &TYPE_VOID_VALUE;
            if (!strcmp(name, "bool")) return &TYPE_BOOL_VALUE;
            if (!strcmp(name, "char")) return &TYPE_CHAR_VALUE;
            if (!strcmp(name, "i8")) return &TYPE_I8_VALUE;
            if (!strcmp(name, "i16")) return &TYPE_I16_VALUE;
            if (!strcmp(name, "i32")) return &TYPE_I32_VALUE;
            if (!strcmp(name, "i64")) return &TYPE_I64_VALUE;
            if (!strcmp(name, "isize")) return &TYPE_ISIZE_VALUE;
            if (!strcmp(name, "u8")) return &TYPE_U8_VALUE;
            if (!strcmp(name, "u16")) return &TYPE_U16_VALUE;
            if (!strcmp(name, "u32")) return &TYPE_U32_VALUE;
            if (!strcmp(name, "u64")) return &TYPE_U64_VALUE;
            if (!strcmp(name, "usize")) return &TYPE_USIZE_VALUE;
            if (!strcmp(name, "f32")) return &TYPE_F32_VALUE;
            if (!strcmp(name, "f64")) return &TYPE_F64_VALUE;
            if (!strcmp(name, "str")) return &TYPE_STR_VALUE;
            if (!strcmp(name, "cstr")) return &TYPE_CSTR_VALUE;
            return make_named_type(c, name);
        }
        case NODE_TYPE_POINTER:
            return make_pointer_type(c,
                                     type_from_ast(c, type_node->as.type_ptr.inner),
                                     type_node->as.type_ptr.is_const,
                                     type_node->as.type_ptr.is_nullable);
        case NODE_TYPE_ARRAY:
            return make_array_type(c,
                                   type_from_ast(c, type_node->as.type_array.elem),
                                   type_node->as.type_array.length);
        default:
            return &TYPE_ERROR_VALUE;
    }
}

static Type *check_expr(Checker *c, Scope *scope, Node *expr, int unsafe_depth);

static int type_name_has_form(const char *name, const char *prefix) {
    size_t prefix_len;
    size_t len;

    if (!name || !prefix)
        return 0;

    prefix_len = strlen(prefix);
    len = strlen(name);
    return len > prefix_len + 1 &&
           !strncmp(name, prefix, prefix_len) &&
           name[prefix_len] == '[' &&
           name[len - 1] == ']';
}

static char *slice_range_dup(const char *start, const char *end) {
    size_t len;
    char *copy;

    if (!start || !end || end < start)
        return NULL;

    len = (size_t)(end - start);
    copy = malloc(len + 1);
    if (!copy)
        return NULL;

    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static char *dup_text(const char *text) {
    return text ? slice_range_dup(text, text + strlen(text)) : NULL;
}

static int split_top_level_comma(const char *text, char **left, char **right) {
    int depth = 0;

    *left = NULL;
    *right = NULL;

    for (const char *it = text; *it; it++) {
        if (*it == '[')
            depth++;
        else if (*it == ']')
            depth--;
        else if (*it == ',' && depth == 0) {
            *left = slice_range_dup(text, it);
            *right = dup_text(it + 1);
            return *left && *right;
        }
    }

    return 0;
}

static int is_option_type(Type *type) {
    return type && type->kind == TYPE_NAMED && type_name_has_form(type->name, "Option");
}

static int is_result_type(Type *type) {
    return type && type->kind == TYPE_NAMED && type_name_has_form(type->name, "Result");
}

static char *option_inner_name(Type *type) {
    if (!is_option_type(type))
        return NULL;
    return slice_range_dup(type->name + 7, type->name + strlen(type->name) - 1);
}

static int result_inner_names(Type *type, char **ok_name, char **err_name) {
    char *inner;
    int ok;

    *ok_name = NULL;
    *err_name = NULL;
    if (!is_result_type(type))
        return 0;

    inner = slice_range_dup(type->name + 7, type->name + strlen(type->name) - 1);
    if (!inner)
        return 0;
    ok = split_top_level_comma(inner, ok_name, err_name);
    free(inner);
    return ok;
}

static Type *type_from_name_text(Checker *c, const char *name) {
    Node fake = {0};
    fake.kind = NODE_TYPE_NAMED;
    fake.as.type_named.name = (char *)name;
    return type_from_ast(c, &fake);
}

static int is_option_constructor_expr(Node *expr) {
    if (!expr)
        return 0;
    if (expr->kind == NODE_IDENT && !strcmp(expr->as.ident.name, "None"))
        return 1;
    return expr->kind == NODE_CALL &&
           expr->as.call.callee &&
           expr->as.call.callee->kind == NODE_IDENT &&
           !strcmp(expr->as.call.callee->as.ident.name, "Some");
}

static int is_result_constructor_expr(Node *expr) {
    if (!expr || expr->kind != NODE_CALL || !expr->as.call.callee ||
        expr->as.call.callee->kind != NODE_IDENT)
        return 0;
    return !strcmp(expr->as.call.callee->as.ident.name, "Ok") ||
           !strcmp(expr->as.call.callee->as.ident.name, "Err");
}

static int check_contextual_constructor(Checker *c, Scope *scope, Type *expected, Node *expr,
                                        int unsafe_depth, Type **out_type) {
    char expected_buf[128];
    char actual_buf[128];

    if (is_option_type(expected) && is_option_constructor_expr(expr)) {
        if (expr->kind == NODE_IDENT) {
            *out_type = expected;
            return 1;
        }

        if (expr->as.call.args.count != 1) {
            typeck_error(c, expr->line, expr->col, "Some expects 1 argument, got %d",
                         expr->as.call.args.count);
            *out_type = &TYPE_ERROR_VALUE;
            return 1;
        }

        {
            char *inner_name = option_inner_name(expected);
            Type *inner_type = inner_name ? type_from_name_text(c, inner_name) : &TYPE_ERROR_VALUE;
            Type *actual_type = check_expr(c, scope, expr->as.call.args.items[0], unsafe_depth);
            if (!is_assignable(inner_type, actual_type)) {
                format_type(inner_type, expected_buf, sizeof(expected_buf));
                format_type(actual_type, actual_buf, sizeof(actual_buf));
                typeck_error(c, expr->line, expr->col,
                             "Some value type mismatch: expected %s, got %s",
                             expected_buf, actual_buf);
                *out_type = &TYPE_ERROR_VALUE;
                return 1;
            }
        }

        *out_type = expected;
        return 1;
    }

    if (is_result_type(expected) && is_result_constructor_expr(expr)) {
        char *ok_name = NULL;
        char *err_name = NULL;
        const char *ctor_name = expr->as.call.callee->as.ident.name;
        Type *inner_type;
        Type *actual_type;

        if (expr->as.call.args.count != 1) {
            typeck_error(c, expr->line, expr->col, "%s expects 1 argument, got %d",
                         ctor_name, expr->as.call.args.count);
            *out_type = &TYPE_ERROR_VALUE;
            return 1;
        }

        if (!result_inner_names(expected, &ok_name, &err_name)) {
            *out_type = &TYPE_ERROR_VALUE;
            return 1;
        }

        inner_type = !strcmp(ctor_name, "Ok") ? type_from_name_text(c, ok_name)
                                               : type_from_name_text(c, err_name);
        actual_type = check_expr(c, scope, expr->as.call.args.items[0], unsafe_depth);

        if (!is_assignable(inner_type, actual_type)) {
            format_type(inner_type, expected_buf, sizeof(expected_buf));
            format_type(actual_type, actual_buf, sizeof(actual_buf));
            typeck_error(c, expr->line, expr->col,
                         "%s value type mismatch: expected %s, got %s",
                         ctor_name, expected_buf, actual_buf);
            *out_type = &TYPE_ERROR_VALUE;
            return 1;
        }

        *out_type = expected;
        return 1;
    }

    return 0;
}

static NodeList *enum_variant_fields(Node *decl, const char *variant);

static int get_match_binding_types(Checker *c, Type *subject_type, const char *pattern,
                                   Type ***out_types, int *out_count) {
    Type **items = NULL;
    int count = 0;

    *out_types = NULL;
    *out_count = 0;

    if (is_option_type(subject_type)) {
        if (!strcmp(pattern, "Some")) {
            char *inner_name = option_inner_name(subject_type);
            Type *inner_type = inner_name ? type_from_name_text(c, inner_name) : &TYPE_ERROR_VALUE;
            items = malloc(sizeof(Type *));
            if (!items)
                return 0;
            items[0] = inner_type;
            count = 1;
        } else if (!strcmp(pattern, "None")) {
            count = 0;
        } else {
            return 0;
        }
    } else if (is_result_type(subject_type)) {
        char *ok_name = NULL;
        char *err_name = NULL;
        if (!result_inner_names(subject_type, &ok_name, &err_name))
            return 0;
        if (!strcmp(pattern, "Ok")) {
            items = malloc(sizeof(Type *));
            if (!items)
                return 0;
            items[0] = type_from_name_text(c, ok_name);
            count = 1;
        } else if (!strcmp(pattern, "Err")) {
            items = malloc(sizeof(Type *));
            if (!items)
                return 0;
            items[0] = type_from_name_text(c, err_name);
            count = 1;
        } else {
            return 0;
        }
    } else {
        const char *dot = strchr(pattern, '.');
        char enum_name[128];
        const char *variant_name;
        Node *enum_decl;
        NodeList *fields;

        if (!dot)
            return 0;
        memcpy(enum_name, pattern, (size_t)(dot - pattern));
        enum_name[dot - pattern] = '\0';
        variant_name = dot + 1;
        enum_decl = find_named_node(c->sema->enums, c->sema->enum_count, enum_name, NODE_ENUM_DECL);
        if (!enum_decl)
            return 0;
        fields = enum_variant_fields(enum_decl, variant_name);
        count = fields ? fields->count : 0;
        if (count > 0) {
            items = malloc((size_t)count * sizeof(Type *));
            if (!items)
                return 0;
            for (int i = 0; i < count; i++)
                items[i] = type_from_ast(c, fields->items[i]->as.let.type);
        }
    }

    *out_types = items;
    *out_count = count;
    return 1;
}

static Type *check_try_expr(Checker *c, Scope *scope, Node *expr, int unsafe_depth) {
    Type *operand;
    char actual_buf[128];

    if (!c->current_return_type) {
        typeck_error(c, expr->line, expr->col,
                     "operator ? can only be used inside a function");
        return &TYPE_ERROR_VALUE;
    }

    operand = check_expr(c, scope, expr->as.unary.operand, unsafe_depth);

    if (is_option_type(operand)) {
        char *inner_name;
        Type *inner_type;

        if (!is_option_type(c->current_return_type)) {
            format_type(c->current_return_type, actual_buf, sizeof(actual_buf));
            typeck_error(c, expr->line, expr->col,
                         "operator ? on Option requires function return type Option[...], got %s",
                         actual_buf);
            return &TYPE_ERROR_VALUE;
        }

        inner_name = option_inner_name(operand);
        inner_type = inner_name ? type_from_name_text(c, inner_name) : &TYPE_ERROR_VALUE;
        return inner_type;
    }

    if (is_result_type(operand)) {
        char *ok_name = NULL;
        char *err_name = NULL;
        char *ret_ok_name = NULL;
        char *ret_err_name = NULL;
        Type *ok_type;
        Type *err_type;
        Type *ret_err_type;

        if (!is_result_type(c->current_return_type)) {
            format_type(c->current_return_type, actual_buf, sizeof(actual_buf));
            typeck_error(c, expr->line, expr->col,
                         "operator ? on Result requires function return type Result[..., ...], got %s",
                         actual_buf);
            return &TYPE_ERROR_VALUE;
        }

        if (!result_inner_names(operand, &ok_name, &err_name) ||
            !result_inner_names(c->current_return_type, &ret_ok_name, &ret_err_name)) {
            return &TYPE_ERROR_VALUE;
        }

        ok_type = type_from_name_text(c, ok_name);
        err_type = type_from_name_text(c, err_name);
        ret_err_type = type_from_name_text(c, ret_err_name);

        if (!(is_assignable(ret_err_type, err_type) && is_assignable(err_type, ret_err_type))) {
            char expected_buf[128];
            format_type(ret_err_type, expected_buf, sizeof(expected_buf));
            format_type(err_type, actual_buf, sizeof(actual_buf));
            typeck_error(c, expr->line, expr->col,
                         "operator ? error type mismatch: expected %s, got %s",
                         expected_buf, actual_buf);
            return &TYPE_ERROR_VALUE;
        }

        return ok_type;
    }

    format_type(operand, actual_buf, sizeof(actual_buf));
    typeck_error(c, expr->line, expr->col,
                 "operator ? requires Option or Result, got %s", actual_buf);
    return &TYPE_ERROR_VALUE;
}

static int composite_has_field(Node *decl, const char *field, Node **field_node) {
    NodeList *fields = NULL;

    if (decl->kind == NODE_STRUCT_DECL) fields = &decl->as.struct_decl.fields;
    if (decl->kind == NODE_UNION_DECL)  fields = &decl->as.union_decl.fields;
    if (!fields) return 0;

    for (int i = 0; i < fields->count; i++) {
        if (!strcmp(fields->items[i]->as.let.name, field)) {
            if (field_node) *field_node = fields->items[i];
            return 1;
        }
    }
    return 0;
}

static int enum_has_variant(Node *decl, const char *variant) {
    if (!decl || decl->kind != NODE_ENUM_DECL) return 0;

    for (int i = 0; i < decl->as.enum_decl.count; i++) {
        if (!strcmp(decl->as.enum_decl.variants[i], variant))
            return 1;
    }
    return 0;
}

static int enum_variant_index(Node *decl, const char *variant) {
    if (!decl || decl->kind != NODE_ENUM_DECL) return -1;

    for (int i = 0; i < decl->as.enum_decl.count; i++) {
        if (!strcmp(decl->as.enum_decl.variants[i], variant))
            return i;
    }
    return -1;
}

static NodeList *enum_variant_fields(Node *decl, const char *variant) {
    int index = enum_variant_index(decl, variant);
    if (index < 0)
        return NULL;
    return &decl->as.enum_decl.variant_fields[index];
}

static Type *check_expr(Checker *c, Scope *scope, Node *expr, int unsafe_depth);

static int method_has_receiver(Node *method) {
    return method->as.fn.params.count > 0 &&
           !strcmp(method->as.fn.params.items[0]->as.let.name, "self");
}

static int check_call_arguments(Checker *c, NodeList *params, int start_index,
                                NodeList *args, Scope *scope, int unsafe_depth,
                                int line, int col) {
    int expected = params->count - start_index;
    char expected_buf[128];
    char actual_buf[128];

    if (args->count != expected) {
        typeck_error(c, line, col, "expected %d arguments, got %d", expected, args->count);
        return 0;
    }

    for (int i = 0; i < expected; i++) {
        Type *expected_type = type_from_ast(c, params->items[i + start_index]->as.let.type);
        Type *actual_type = NULL;

        if (!check_contextual_constructor(c, scope, expected_type, args->items[i], unsafe_depth, &actual_type))
            actual_type = check_expr(c, scope, args->items[i], unsafe_depth);
        if (!is_assignable(expected_type, actual_type)) {
            format_type(expected_type, expected_buf, sizeof(expected_buf));
            format_type(actual_type, actual_buf, sizeof(actual_buf));
            typeck_error(c, args->items[i]->line, args->items[i]->col,
                         "argument type mismatch: expected %s, got %s",
                         expected_buf, actual_buf);
            return 0;
        }
    }

    return 1;
}

static int check_receiver(Checker *c, Type *expected, Type *actual, Node *receiver_expr) {
    char expected_buf[128];
    char actual_buf[128];

    if (is_assignable(expected, actual))
        return 1;

    if (expected->kind == TYPE_POINTER &&
        actual->kind == TYPE_NAMED &&
        type_equals(expected->inner, actual)) {
        if (!is_addressable(receiver_expr)) {
            typeck_error(c, receiver_expr->line, receiver_expr->col,
                         "method call requires an addressable receiver");
            return 0;
        }
        return 1;
    }

    format_type(expected, expected_buf, sizeof(expected_buf));
    format_type(actual, actual_buf, sizeof(actual_buf));
    typeck_error(c, receiver_expr->line, receiver_expr->col,
                 "receiver type mismatch: expected %s, got %s",
                 expected_buf, actual_buf);
    return 0;
}

static Type *check_match_expr(Checker *c, Scope *scope, Node *expr, int unsafe_depth) {
    Type *subject_type = check_expr(c, scope, expr->as.match.subject, unsafe_depth);
    Node *enum_decl = NULL;
    Type *result_type = NULL;
    char expected_buf[128];
    char actual_buf[128];
    int saw_wildcard = 0;
    const char *seen_variants[256];
    int seen_count = 0;
    int subject_is_option = is_option_type(subject_type);
    int subject_is_result = is_result_type(subject_type);

    if (!(subject_type->kind == TYPE_NAMED)) {
        format_type(subject_type, actual_buf, sizeof(actual_buf));
        typeck_error(c, expr->line, expr->col,
                     "match subject must be an enum, Option, or Result, got %s", actual_buf);
        return &TYPE_ERROR_VALUE;
    }

    if (!subject_is_option && !subject_is_result) {
        enum_decl = find_named_node(c->sema->enums, c->sema->enum_count,
                                    subject_type->name, NODE_ENUM_DECL);
    }
    if (!subject_is_option && !subject_is_result && !enum_decl) {
        format_type(subject_type, actual_buf, sizeof(actual_buf));
        typeck_error(c, expr->line, expr->col,
                     "match subject must be an enum, Option, or Result, got %s", actual_buf);
        return &TYPE_ERROR_VALUE;
    }

    for (int i = 0; i < expr->as.match.count; i++) {
        const char *pattern = expr->as.match.patterns[i];
        Type *arm_type;
        Scope *arm_scope = NULL;
        Type **binding_types = NULL;
        int binding_type_count = 0;
        int declared_binding_count = expr->as.match.binding_counts[i];

        if (!strcmp(pattern, "_")) {
            if (saw_wildcard) {
                typeck_error(c, expr->line, expr->col,
                             "duplicate wildcard match arm");
                return &TYPE_ERROR_VALUE;
            }
            saw_wildcard = 1;
        } else {
            const char *dot = strchr(pattern, '.');
            char enum_name[128];
            const char *variant_name;

            if (subject_is_option || subject_is_result) {
                variant_name = pattern;

                if (subject_is_option &&
                    strcmp(variant_name, "Some") &&
                    strcmp(variant_name, "None")) {
                    typeck_error(c, expr->line, expr->col,
                                 "match arm '%s' does not match subject type %s",
                                 pattern, subject_type->name);
                    return &TYPE_ERROR_VALUE;
                }

                if (subject_is_result &&
                    strcmp(variant_name, "Ok") &&
                    strcmp(variant_name, "Err")) {
                    typeck_error(c, expr->line, expr->col,
                                 "match arm '%s' does not match subject type %s",
                                 pattern, subject_type->name);
                    return &TYPE_ERROR_VALUE;
                }
            } else {
                if (!dot) return &TYPE_ERROR_VALUE;
                memcpy(enum_name, pattern, (size_t)(dot - pattern));
                enum_name[dot - pattern] = '\0';
                variant_name = dot + 1;

                if (!(subject_type->kind == TYPE_NAMED && !strcmp(subject_type->name, enum_name))) {
                    format_type(subject_type, actual_buf, sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "match arm '%s' does not match subject type %s",
                                 pattern, actual_buf);
                    return &TYPE_ERROR_VALUE;
                }
            }

            for (int j = 0; j < seen_count; j++) {
                if (!strcmp(seen_variants[j], variant_name)) {
                    if (subject_is_option || subject_is_result) {
                        typeck_error(c, expr->line, expr->col,
                                     "duplicate match arm '%s'",
                                     variant_name);
                    } else {
                        typeck_error(c, expr->line, expr->col,
                                     "duplicate match arm '%s.%s'",
                                     enum_name, variant_name);
                    }
                    return &TYPE_ERROR_VALUE;
                }
            }

            if (seen_count < (int)(sizeof(seen_variants) / sizeof(seen_variants[0])))
                seen_variants[seen_count++] = variant_name;
        }

        if (!strcmp(pattern, "_")) {
            if (expr->as.match.binding_counts[i] != 0) {
                typeck_error(c, expr->line, expr->col,
                             "wildcard match arm cannot declare bindings");
                return &TYPE_ERROR_VALUE;
            }
        } else if (!get_match_binding_types(c, subject_type, pattern, &binding_types, &binding_type_count)) {
            typeck_error(c, expr->line, expr->col,
                         "invalid match pattern '%s'", pattern);
            return &TYPE_ERROR_VALUE;
        }

        if (declared_binding_count != 0 &&
            binding_type_count != declared_binding_count) {
            typeck_error(c, expr->line, expr->col,
                         "match arm '%s' expects %d bindings, got %d",
                         pattern, binding_type_count, declared_binding_count);
            free(binding_types);
            return &TYPE_ERROR_VALUE;
        }

        arm_scope = scope_new(scope);
        if (!arm_scope) {
            free(binding_types);
            return &TYPE_ERROR_VALUE;
        }

        for (int j = 0; j < declared_binding_count; j++) {
            if (!scope_define(arm_scope, expr->as.match.binding_names[i][j], binding_types[j], 0)) {
                typeck_error(c, expr->line, expr->col,
                             "duplicate match binding '%s'",
                             expr->as.match.binding_names[i][j]);
                free(binding_types);
                scope_free(arm_scope);
                return &TYPE_ERROR_VALUE;
            }
        }

        arm_type = check_expr(c, arm_scope, expr->as.match.values[i], unsafe_depth);
        free(binding_types);
        scope_free(arm_scope);
        if (!result_type) {
            result_type = arm_type;
        } else if (!(is_assignable(result_type, arm_type) || is_assignable(arm_type, result_type))) {
            format_type(result_type, expected_buf, sizeof(expected_buf));
            format_type(arm_type, actual_buf, sizeof(actual_buf));
            typeck_error(c, expr->as.match.values[i]->line, expr->as.match.values[i]->col,
                         "match arm type mismatch: expected %s, got %s",
                         expected_buf, actual_buf);
            return &TYPE_ERROR_VALUE;
        }
    }

    if (!saw_wildcard && enum_decl) {
        for (int i = 0; i < enum_decl->as.enum_decl.count; i++) {
            const char *variant_name = enum_decl->as.enum_decl.variants[i];
            int seen = 0;
            for (int j = 0; j < seen_count; j++) {
                if (!strcmp(seen_variants[j], variant_name)) {
                    seen = 1;
                    break;
                }
            }
            if (!seen) {
                typeck_error(c, expr->line, expr->col,
                             "non-exhaustive match for enum '%s': missing %s.%s",
                             subject_type->name, subject_type->name, variant_name);
                return &TYPE_ERROR_VALUE;
            }
        }
    }

    if (!saw_wildcard && subject_is_option) {
        int has_some = 0;
        int has_none = 0;
        for (int i = 0; i < seen_count; i++) {
            if (!strcmp(seen_variants[i], "Some"))
                has_some = 1;
            if (!strcmp(seen_variants[i], "None"))
                has_none = 1;
        }
        if (!has_some) {
            typeck_error(c, expr->line, expr->col,
                         "non-exhaustive match for Option: missing Some");
            return &TYPE_ERROR_VALUE;
        }
        if (!has_none) {
            typeck_error(c, expr->line, expr->col,
                         "non-exhaustive match for Option: missing None");
            return &TYPE_ERROR_VALUE;
        }
    }

    if (!saw_wildcard && subject_is_result) {
        int has_ok = 0;
        int has_err = 0;
        for (int i = 0; i < seen_count; i++) {
            if (!strcmp(seen_variants[i], "Ok"))
                has_ok = 1;
            if (!strcmp(seen_variants[i], "Err"))
                has_err = 1;
        }
        if (!has_ok) {
            typeck_error(c, expr->line, expr->col,
                         "non-exhaustive match for Result: missing Ok");
            return &TYPE_ERROR_VALUE;
        }
        if (!has_err) {
            typeck_error(c, expr->line, expr->col,
                         "non-exhaustive match for Result: missing Err");
            return &TYPE_ERROR_VALUE;
        }
    }

    return result_type ? result_type : &TYPE_ERROR_VALUE;
}

static Type *check_expr(Checker *c, Scope *scope, Node *expr, int unsafe_depth) {
    char expected_buf[128];
    char actual_buf[128];

    if (!expr) return &TYPE_VOID_VALUE;

    switch (expr->kind) {
        case NODE_LIT_INT:
            return &TYPE_INT_LIT_VALUE;
        case NODE_LIT_CHAR:
            return &TYPE_CHAR_VALUE;
        case NODE_LIT_FLOAT:
            return &TYPE_FLOAT_LIT_VALUE;
        case NODE_LIT_STRING:
            return &TYPE_STRING_LIT_VALUE;
        case NODE_LIT_BOOL:
            return &TYPE_BOOL_VALUE;
        case NODE_LIT_NULL:
            return &TYPE_NULL_VALUE;

        case NODE_IDENT: {
            Binding *binding = scope_resolve(scope, expr->as.ident.name);
            Node *fn;
            const char *iname = expr->as.ident.name;

            if (binding) {
                if (!ensure_resolved_type_visible(c, binding->type, expr->line, expr->col))
                    return &TYPE_ERROR_VALUE;
                return binding->type;
            }

            if (!strcmp(iname, "None")) {
                typeck_error(c, expr->line, expr->col,
                             "cannot infer Option type for None without context");
                return &TYPE_ERROR_VALUE;
            }

            /* built-in I/O stream objects */
            if (!strcmp(iname, "stdout") || !strcmp(iname, "stderr"))
                return &TYPE_VOID_VALUE;   /* stream handle — used only for .flush()/.write() */
            if (!strcmp(iname, "stdin"))
                return &TYPE_VOID_VALUE;

            fn = find_named_node(c->sema->functions, c->sema->function_count,
                                 iname, NODE_FN_DECL);
            if (!fn)
                fn = find_named_node(c->sema->extern_functions, c->sema->extern_function_count,
                                     iname, NODE_EXTERN_FN);
            if (fn) {
                if (!require_decl_visible(c, fn, expr->line, expr->col, iname))
                    return &TYPE_ERROR_VALUE;
                return make_function_type(c, fn);
            }

            /* check top-level const declarations */
            if (c->program) {
                for (int gi = 0; gi < c->program->as.program.decls.count; gi++) {
                    Node *gd = c->program->as.program.decls.items[gi];
                    if (gd->kind == NODE_CONST &&
                        !strcmp(gd->as.konst.name, iname)) {
                        if (gd->as.konst.type)
                            return type_from_ast(c, gd->as.konst.type);
                        return check_expr(c, NULL, gd->as.konst.value, unsafe_depth);
                    }
                }
            }

            return &TYPE_ERROR_VALUE;
        }

        case NODE_GROUP:
            return check_expr(c, scope, expr->as.group.expr, unsafe_depth);

        case NODE_UNARY: {
            if (!strcmp(expr->as.unary.op, "?"))
                return check_try_expr(c, scope, expr, unsafe_depth);

            Type *operand = check_expr(c, scope, expr->as.unary.operand, unsafe_depth);

            if (!strcmp(expr->as.unary.op, "!")) {
                if (!is_assignable(&TYPE_BOOL_VALUE, operand)) {
                    format_type(operand, actual_buf, sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "operator ! requires bool, got %s", actual_buf);
                    return &TYPE_ERROR_VALUE;
                }
                return &TYPE_BOOL_VALUE;
            }

            if (!strcmp(expr->as.unary.op, "-")) {
                if (!is_numeric_type(operand)) {
                    format_type(operand, actual_buf, sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "operator - requires numeric type, got %s", actual_buf);
                    return &TYPE_ERROR_VALUE;
                }
                return operand;
            }

            if (!strcmp(expr->as.unary.op, "&")) {
                if (!is_addressable(expr->as.unary.operand)) {
                    typeck_error(c, expr->line, expr->col,
                                 "operator & requires an addressable expression");
                    return &TYPE_ERROR_VALUE;
                }
                return make_pointer_type(c, widen_literal(operand), 0, 0);
            }

            if (!strcmp(expr->as.unary.op, "*")) {
                if (operand->kind != TYPE_POINTER) {
                    format_type(operand, actual_buf, sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "operator * requires pointer type, got %s", actual_buf);
                    return &TYPE_ERROR_VALUE;
                }
                return operand->inner;
            }

            if (!strcmp(expr->as.unary.op, "~")) {
                if (!is_integer_kind(operand->kind)) {
                    format_type(operand, actual_buf, sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "operator ~ requires integer type, got %s", actual_buf);
                    return &TYPE_ERROR_VALUE;
                }
                return operand;
            }

            return &TYPE_ERROR_VALUE;
        }

        case NODE_BINARY: {
            Type *left = check_expr(c, scope, expr->as.binary.left, unsafe_depth);
            Type *right = check_expr(c, scope, expr->as.binary.right, unsafe_depth);
            const char *op = expr->as.binary.op;

            if (!strcmp(op, "+") || !strcmp(op, "-") ||
                !strcmp(op, "*") || !strcmp(op, "/") || !strcmp(op, "%")) {
                if (!is_numeric_type(left) || !is_numeric_type(right)) {
                    format_type(left, expected_buf, sizeof(expected_buf));
                    format_type(right, actual_buf, sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "operator %s requires numeric operands, got %s and %s",
                                 op, expected_buf, actual_buf);
                    return &TYPE_ERROR_VALUE;
                }
                if (left->kind == TYPE_FLOAT_LITERAL || right->kind == TYPE_FLOAT_LITERAL ||
                    left->kind == TYPE_F32 || left->kind == TYPE_F64 ||
                    right->kind == TYPE_F32 || right->kind == TYPE_F64) {
                    return &TYPE_F64_VALUE;
                }
                return &TYPE_INT_LIT_VALUE;
            }

            if (!strcmp(op, "==") || !strcmp(op, "!=")) {
                if (!(is_assignable(left, right) || is_assignable(right, left))) {
                    format_type(left, expected_buf, sizeof(expected_buf));
                    format_type(right, actual_buf, sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "cannot compare %s and %s", expected_buf, actual_buf);
                    return &TYPE_ERROR_VALUE;
                }
                return &TYPE_BOOL_VALUE;
            }

            if (!strcmp(op, "<") || !strcmp(op, ">") ||
                !strcmp(op, "<=") || !strcmp(op, ">=")) {
                if (!is_numeric_type(left) || !is_numeric_type(right)) {
                    format_type(left, expected_buf, sizeof(expected_buf));
                    format_type(right, actual_buf, sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "comparison requires numeric operands, got %s and %s",
                                 expected_buf, actual_buf);
                    return &TYPE_ERROR_VALUE;
                }
                return &TYPE_BOOL_VALUE;
            }

            if (!strcmp(op, "&&") || !strcmp(op, "||")) {
                if (!is_assignable(&TYPE_BOOL_VALUE, left) || !is_assignable(&TYPE_BOOL_VALUE, right)) {
                    format_type(left, expected_buf, sizeof(expected_buf));
                    format_type(right, actual_buf, sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "logical operator %s requires bool operands, got %s and %s",
                                 op, expected_buf, actual_buf);
                    return &TYPE_ERROR_VALUE;
                }
                return &TYPE_BOOL_VALUE;
            }

            if (!strcmp(op, "&") || !strcmp(op, "|") || !strcmp(op, "^") ||
                !strcmp(op, "<<") || !strcmp(op, ">>")) {
                if (!is_integer_kind(left->kind) || !is_integer_kind(right->kind)) {
                    format_type(left, expected_buf, sizeof(expected_buf));
                    format_type(right, actual_buf, sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "bitwise operator %s requires integer operands, got %s and %s",
                                 op, expected_buf, actual_buf);
                    return &TYPE_ERROR_VALUE;
                }
                return (left->kind == TYPE_INT_LITERAL) ? right : left;
            }

            return &TYPE_ERROR_VALUE;
        }

        case NODE_ASSIGN: {
            Type *target_type;
            Type *value_type;
            Binding *binding;

            if (!is_addressable(expr->as.assign.target)) {
                typeck_error(c, expr->line, expr->col,
                             "left side of assignment is not assignable");
                return &TYPE_ERROR_VALUE;
            }

            if (expr->as.assign.target->kind == NODE_IDENT) {
                binding = scope_resolve(scope, expr->as.assign.target->as.ident.name);
                if (binding && !binding->mutable) {
                    typeck_error(c, expr->line, expr->col,
                                 "cannot assign to immutable binding '%s'",
                                 expr->as.assign.target->as.ident.name);
                    return &TYPE_ERROR_VALUE;
                }
            }

            target_type = check_expr(c, scope, expr->as.assign.target, unsafe_depth);
            value_type  = check_expr(c, scope, expr->as.assign.value,  unsafe_depth);

            if (expr->as.assign.op) {
                /* compound assignment: both sides must be numeric */
                if (!is_numeric_type(target_type) || !is_numeric_type(value_type)) {
                    format_type(target_type, expected_buf, sizeof(expected_buf));
                    format_type(value_type,  actual_buf,   sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "compound assignment requires numeric types, got %s and %s",
                                 expected_buf, actual_buf);
                    return &TYPE_ERROR_VALUE;
                }
                return target_type;
            }

            if (!is_assignable(target_type, value_type)) {
                format_type(target_type, expected_buf, sizeof(expected_buf));
                format_type(value_type,  actual_buf,   sizeof(actual_buf));
                typeck_error(c, expr->line, expr->col,
                             "cannot assign %s to %s", actual_buf, expected_buf);
                return &TYPE_ERROR_VALUE;
            }
            return target_type;
        }

        case NODE_INDEX: {
            Type *obj_type   = check_expr(c, scope, expr->as.index_expr.object, unsafe_depth);
            Type *index_type = check_expr(c, scope, expr->as.index_expr.index,  unsafe_depth);

            if (index_type->kind != TYPE_ERROR && !is_integer_kind(index_type->kind)) {
                format_type(index_type, actual_buf, sizeof(actual_buf));
                typeck_error(c, expr->line, expr->col,
                             "index must be an integer, got %s", actual_buf);
                return &TYPE_ERROR_VALUE;
            }

            if (obj_type->kind == TYPE_ARRAY)
                return obj_type->inner ? obj_type->inner : &TYPE_ERROR_VALUE;
            if (obj_type->kind == TYPE_POINTER)
                return obj_type->inner ? obj_type->inner : &TYPE_ERROR_VALUE;
            if (obj_type->kind == TYPE_STR)
                return &TYPE_U8_VALUE;
            if (obj_type->kind == TYPE_NAMED && !strcmp(obj_type->name, "String"))
                return &TYPE_U8_VALUE;

            if (obj_type->kind != TYPE_ERROR) {
                format_type(obj_type, actual_buf, sizeof(actual_buf));
                typeck_error(c, expr->line, expr->col,
                             "cannot index into type %s", actual_buf);
            }
            return &TYPE_ERROR_VALUE;
        }

        case NODE_SLICE: {
            Type *obj_type = check_expr(c, scope, expr->as.slice_expr.object, unsafe_depth);
            Type *start_type = expr->as.slice_expr.start
                ? check_expr(c, scope, expr->as.slice_expr.start, unsafe_depth)
                : NULL;
            Type *end_type = expr->as.slice_expr.end
                ? check_expr(c, scope, expr->as.slice_expr.end, unsafe_depth)
                : NULL;

            if (start_type && start_type->kind != TYPE_ERROR &&
                !is_integer_kind(start_type->kind)) {
                format_type(start_type, actual_buf, sizeof(actual_buf));
                typeck_error(c, expr->line, expr->col,
                             "slice start must be an integer, got %s", actual_buf);
                return &TYPE_ERROR_VALUE;
            }

            if (end_type && end_type->kind != TYPE_ERROR &&
                !is_integer_kind(end_type->kind)) {
                format_type(end_type, actual_buf, sizeof(actual_buf));
                typeck_error(c, expr->line, expr->col,
                             "slice end must be an integer, got %s", actual_buf);
                return &TYPE_ERROR_VALUE;
            }

            if (obj_type->kind == TYPE_STR)
                return &TYPE_STR_VALUE;
            if (obj_type->kind == TYPE_NAMED && !strcmp(obj_type->name, "String"))
                return &TYPE_STR_VALUE;
            if (obj_type->kind == TYPE_ARRAY)
                return make_array_type(c, obj_type->inner, -1);

            if (obj_type->kind != TYPE_ERROR) {
                format_type(obj_type, actual_buf, sizeof(actual_buf));
                typeck_error(c, expr->line, expr->col,
                             "cannot slice type %s", actual_buf);
            }
            return &TYPE_ERROR_VALUE;
        }

        case NODE_CALL: {
            Node *callee = expr->as.call.callee;
            Type *callee_type;

            if (callee->kind == NODE_IDENT &&
                (!strcmp(callee->as.ident.name, "Some") ||
                 !strcmp(callee->as.ident.name, "Ok") ||
                 !strcmp(callee->as.ident.name, "Err"))) {
                typeck_error(c, expr->line, expr->col,
                             "cannot infer container type for %s without context",
                             callee->as.ident.name);
                return &TYPE_ERROR_VALUE;
            }

            /* ── built-in I/O calls ── */
            if (callee->kind == NODE_IDENT) {
                const char *nm = callee->as.ident.name;
                /* void-returning output functions */
                if (!strcmp(nm, "print")     || !strcmp(nm, "println")  ||
                    !strcmp(nm, "eprint")    || !strcmp(nm, "eprintln") ||
                    !strcmp(nm, "print_int") || !strcmp(nm, "print_long")||
                    !strcmp(nm, "print_uint")|| !strcmp(nm, "print_ulong")||
                    !strcmp(nm, "print_f32") || !strcmp(nm, "print_f64") ||
                    !strcmp(nm, "print_bool")|| !strcmp(nm, "print_char")||
                    !strcmp(nm, "flush")) {
                    for (int i = 0; i < expr->as.call.args.count; i++)
                        check_expr(c, scope, expr->as.call.args.items[i], unsafe_depth);
                    return &TYPE_VOID_VALUE;
                }
                /* cstr-returning input functions */
                if (!strcmp(nm, "input"))
                    return &TYPE_CSTR_VALUE;
                /* typed read functions */
                if (!strcmp(nm, "read_int")  || !strcmp(nm, "read_long"))
                    return &TYPE_I32_VALUE;
                if (!strcmp(nm, "read_uint"))
                    return &TYPE_U32_VALUE;
                if (!strcmp(nm, "read_f64"))
                    return &TYPE_F64_VALUE;
                /* inline assembly — asm("instruction") → void */
                if (!strcmp(nm, "asm"))
                    return &TYPE_VOID_VALUE;
                /* port I/O built-ins */
                if (!strcmp(nm, "outb") || !strcmp(nm, "outw") ||
                    !strcmp(nm, "outl") || !strcmp(nm, "io_wait"))
                    return &TYPE_VOID_VALUE;
                if (!strcmp(nm, "inb"))  return &TYPE_U8_VALUE;
                if (!strcmp(nm, "inw"))  return &TYPE_U16_VALUE;
                if (!strcmp(nm, "inl"))  return &TYPE_U32_VALUE;
            }
            /* stream method calls: stdout.flush(), stderr.flush(), stdin.read_line() */
            if (callee->kind == NODE_FIELD) {
                Node *obj = callee->as.field.object;
                const char *field = callee->as.field.field;
                if (obj->kind == NODE_IDENT) {
                    const char *oname = obj->as.ident.name;
                    if ((!strcmp(oname, "stdout") || !strcmp(oname, "stderr")) &&
                        !strcmp(field, "flush"))
                        return &TYPE_VOID_VALUE;
                    if ((!strcmp(oname, "stdout") || !strcmp(oname, "stderr")) &&
                        !strcmp(field, "write")) {
                        for (int i = 0; i < expr->as.call.args.count; i++)
                            check_expr(c, scope, expr->as.call.args.items[i], unsafe_depth);
                        return &TYPE_VOID_VALUE;
                    }
                    if (!strcmp(oname, "stdin") &&
                        (!strcmp(field, "read_line") || !strcmp(field, "read_int") ||
                         !strcmp(field, "read_f64"))) {
                        if (!strcmp(field, "read_line")) return &TYPE_CSTR_VALUE;
                        if (!strcmp(field, "read_int"))  return &TYPE_I32_VALUE;
                        if (!strcmp(field, "read_f64"))  return &TYPE_F64_VALUE;
                    }
                }
            }

            if (callee->kind == NODE_FIELD) {
                Node *object = callee->as.field.object;

                if (object->kind == NODE_IDENT) {
                    if (!strcmp(object->as.ident.name, "String") &&
                        !strcmp(callee->as.field.field, "from")) {
                        Type *arg_type;

                        if (expr->as.call.args.count != 1) {
                            typeck_error(c, expr->line, expr->col,
                                         "String.from expects 1 argument, got %d",
                                         expr->as.call.args.count);
                            return &TYPE_ERROR_VALUE;
                        }

                        arg_type = check_expr(c, scope, expr->as.call.args.items[0], unsafe_depth);
                        if (!(is_assignable(&TYPE_STR_VALUE, arg_type) ||
                              is_assignable(&TYPE_CSTR_VALUE, arg_type))) {
                            format_type(arg_type, actual_buf, sizeof(actual_buf));
                            typeck_error(c, expr->as.call.args.items[0]->line,
                                         expr->as.call.args.items[0]->col,
                                         "String.from expects str or cstr, got %s",
                                         actual_buf);
                            return &TYPE_ERROR_VALUE;
                        }

                        return make_named_type(c, "String");
                    }

                    Node *enum_decl = find_named_node(c->sema->enums, c->sema->enum_count,
                                                      object->as.ident.name, NODE_ENUM_DECL);
                    if (enum_decl && enum_has_variant(enum_decl, callee->as.field.field)) {
                        if (!require_decl_visible(c, enum_decl, expr->line, expr->col,
                                                  object->as.ident.name))
                            return &TYPE_ERROR_VALUE;
                        NodeList *fields = enum_variant_fields(enum_decl, callee->as.field.field);
                        int expected = fields ? fields->count : 0;

                        if (expr->as.call.args.count != expected) {
                            typeck_error(c, expr->line, expr->col,
                                         "enum constructor '%s.%s' expects %d arguments, got %d",
                                         object->as.ident.name, callee->as.field.field,
                                         expected, expr->as.call.args.count);
                            return &TYPE_ERROR_VALUE;
                        }

                        for (int i = 0; i < expected; i++) {
                            Type *expected_type = type_from_ast(c, fields->items[i]->as.let.type);
                            Type *actual_type = check_expr(c, scope, expr->as.call.args.items[i], unsafe_depth);
                            if (!is_assignable(expected_type, actual_type)) {
                                format_type(expected_type, expected_buf, sizeof(expected_buf));
                                format_type(actual_type, actual_buf, sizeof(actual_buf));
                                typeck_error(c, expr->as.call.args.items[i]->line,
                                             expr->as.call.args.items[i]->col,
                                             "enum constructor argument mismatch: expected %s, got %s",
                                             expected_buf, actual_buf);
                                return &TYPE_ERROR_VALUE;
                            }
                        }

                        return make_named_type(c, object->as.ident.name);
                    }
                }

                if (object->kind == NODE_IDENT) {
                    Node *method = find_method_decl(c->sema, object->as.ident.name, callee->as.field.field);
                    if (method) {
                        if (!require_decl_visible(c, method, expr->line, expr->col, callee->as.field.field))
                            return &TYPE_ERROR_VALUE;
                        /* Type.method(args...) — receiver passed explicitly as first arg */
                        if (!check_call_arguments(c, &method->as.fn.params, 0,
                                                  &expr->as.call.args, scope, unsafe_depth,
                                                  expr->line, expr->col)) {
                            return &TYPE_ERROR_VALUE;
                        }
                        {
                            Type *ret_type = type_from_ast(c, method->as.fn.ret_type);
                            if (!ensure_resolved_type_visible(c, ret_type, expr->line, expr->col))
                                return &TYPE_ERROR_VALUE;
                            return ret_type;
                        }
                    }
                }

                {
                    Type *object_type = check_expr(c, scope, object, unsafe_depth);
                    const char *owner = NULL;
                    Node *method;
                    Type *receiver_expected;

                    if (object_type->kind == TYPE_NAMED &&
                        !strcmp(object_type->name, "String")) {
                        if (!strcmp(callee->as.field.field, "free")) {
                            if (expr->as.call.args.count != 0) {
                                typeck_error(c, expr->line, expr->col,
                                             "String.free expects 0 arguments, got %d",
                                             expr->as.call.args.count);
                                return &TYPE_ERROR_VALUE;
                            }
                            if (!is_addressable(object)) {
                                typeck_error(c, object->line, object->col,
                                             "String.free requires an addressable receiver");
                                return &TYPE_ERROR_VALUE;
                            }
                            return &TYPE_VOID_VALUE;
                        }

                        if (!strcmp(callee->as.field.field, "as_str")) {
                            if (expr->as.call.args.count != 0) {
                                typeck_error(c, expr->line, expr->col,
                                             "String.as_str expects 0 arguments, got %d",
                                             expr->as.call.args.count);
                                return &TYPE_ERROR_VALUE;
                            }
                            return &TYPE_STR_VALUE;
                        }
                    }

                    if (object_type->kind == TYPE_POINTER && object_type->inner &&
                        object_type->inner->kind == TYPE_NAMED &&
                        !strcmp(object_type->inner->name, "String")) {
                        if (!strcmp(callee->as.field.field, "free")) {
                            if (expr->as.call.args.count != 0) {
                                typeck_error(c, expr->line, expr->col,
                                             "String.free expects 0 arguments, got %d",
                                             expr->as.call.args.count);
                                return &TYPE_ERROR_VALUE;
                            }
                            return &TYPE_VOID_VALUE;
                        }

                        if (!strcmp(callee->as.field.field, "as_str")) {
                            if (expr->as.call.args.count != 0) {
                                typeck_error(c, expr->line, expr->col,
                                             "String.as_str expects 0 arguments, got %d",
                                             expr->as.call.args.count);
                                return &TYPE_ERROR_VALUE;
                            }
                            return &TYPE_STR_VALUE;
                        }
                    }

                    if (object_type->kind == TYPE_NAMED) owner = object_type->name;
                    if (object_type->kind == TYPE_POINTER && object_type->inner &&
                        object_type->inner->kind == TYPE_NAMED) {
                        owner = object_type->inner->name;
                    }

                    method = owner ? find_method_decl(c->sema, owner, callee->as.field.field) : NULL;
                    if (method) {
                        if (!require_decl_visible(c, method, expr->line, expr->col, callee->as.field.field))
                            return &TYPE_ERROR_VALUE;
                        if (!method_has_receiver(method)) {
                            typeck_error(c, callee->line, callee->col,
                                         "associated function '%s.%s' must be called on the type",
                                         owner, callee->as.field.field);
                            return &TYPE_ERROR_VALUE;
                        }

                        receiver_expected = type_from_ast(c, method->as.fn.params.items[0]->as.let.type);
                        if (!check_receiver(c, receiver_expected, object_type, object))
                            return &TYPE_ERROR_VALUE;
                        if (!check_call_arguments(c, &method->as.fn.params, 1,
                                                  &expr->as.call.args, scope, unsafe_depth,
                                                  expr->line, expr->col)) {
                            return &TYPE_ERROR_VALUE;
                        }
                        {
                            Type *ret_type = type_from_ast(c, method->as.fn.ret_type);
                            if (!ensure_resolved_type_visible(c, ret_type, expr->line, expr->col))
                                return &TYPE_ERROR_VALUE;
                            return ret_type;
                        }
                    }
                }
            }

            /* interface coercion fn: Target_as_Interface(ptr) */
            if (callee->kind == NODE_IDENT && c->program) {
                const char *cn = callee->as.ident.name;
                for (int _ci = 0; _ci < c->program->as.program.decls.count; _ci++) {
                    Node *_cd = c->program->as.program.decls.items[_ci];
                    if (_cd->kind == NODE_IMPL_DECL && _cd->as.impl.interface_name) {
                        char _coerce[256];
                        snprintf(_coerce, sizeof(_coerce), "%s_as_%s",
                                 _cd->as.impl.target, _cd->as.impl.interface_name);
                        if (!strcmp(cn, _coerce)) {
                            for (int _ai = 0; _ai < expr->as.call.args.count; _ai++)
                                check_expr(c, scope, expr->as.call.args.items[_ai], unsafe_depth);
                            return make_named_type(c, _cd->as.impl.interface_name);
                        }
                    }
                }
            }
            /* generic fn[TypeArg](args) call — substitute return type and check args */
            if (callee->kind == NODE_INDEX && callee->as.index_expr.object &&
                callee->as.index_expr.object->kind == NODE_IDENT) {
                const char *gfn_name = callee->as.index_expr.object->as.ident.name;
                for (int _gi = 0; _gi < c->program->as.program.decls.count; _gi++) {
                    Node *_gd = c->program->as.program.decls.items[_gi];
                    if (_gd->kind == NODE_FN_DECL &&
                        _gd->as.fn.type_params.count > 0 &&
                        !strcmp(_gd->as.fn.name, gfn_name)) {
                        for (int _ai = 0; _ai < expr->as.call.args.count; _ai++)
                            check_expr(c, scope, expr->as.call.args.items[_ai], unsafe_depth);
                        /* resolve return type: if it's a type param, substitute the type arg */
                        Node *ret_node = _gd->as.fn.ret_type;
                        Node *ta = callee->as.index_expr.index;
                        if (ret_node && ret_node->kind == NODE_TYPE_NAMED &&
                            _gd->as.fn.type_params.count >= 1 &&
                            !strcmp(ret_node->as.type_named.name,
                                    _gd->as.fn.type_params.items[0]->as.type_named.name) &&
                            ta && (ta->kind == NODE_IDENT || ta->kind == NODE_TYPE_NAMED)) {
                            /* substitute: build a temporary named type node using the type arg */
                            const char *ns_t = (ta->kind == NODE_IDENT)
                                               ? ta->as.ident.name : ta->as.type_named.name;
                            Node tmp_type;
                            memset(&tmp_type, 0, sizeof(tmp_type));
                            tmp_type.kind = NODE_TYPE_NAMED;
                            tmp_type.as.type_named.name = (char *)ns_t;
                            return type_from_ast(c, &tmp_type);
                        }
                        return &TYPE_VOID_VALUE;
                    }
                }
            }
            callee_type = check_expr(c, scope, callee, unsafe_depth);
            if (callee_type->kind != TYPE_FUNCTION) {
                format_type(callee_type, actual_buf, sizeof(actual_buf));
                typeck_error(c, expr->line, expr->col,
                             "cannot call non-function value of type %s", actual_buf);
                return &TYPE_ERROR_VALUE;
            }

            if (callee_type->decl->kind == NODE_FN_DECL) {
                if (!check_call_arguments(c, &callee_type->decl->as.fn.params, 0,
                                          &expr->as.call.args, scope, unsafe_depth,
                                          expr->line, expr->col)) {
                    return &TYPE_ERROR_VALUE;
                }
                {
                    Type *ret_type = type_from_ast(c, callee_type->decl->as.fn.ret_type);
                    if (!ensure_resolved_type_visible(c, ret_type, expr->line, expr->col))
                        return &TYPE_ERROR_VALUE;
                    return ret_type;
                }
            }

            if (!check_call_arguments(c, &callee_type->decl->as.extern_fn.params, 0,
                                      &expr->as.call.args, scope, unsafe_depth,
                                      expr->line, expr->col)) {
                return &TYPE_ERROR_VALUE;
            }
            {
                Type *ret_type = type_from_ast(c, callee_type->decl->as.extern_fn.ret_type);
                if (!ensure_resolved_type_visible(c, ret_type, expr->line, expr->col))
                    return &TYPE_ERROR_VALUE;
                return ret_type;
            }
        }

        case NODE_CAST: {
            Type *source = check_expr(c, scope, expr->as.cast.expr, unsafe_depth);
            Type *target = type_from_ast(c, expr->as.cast.type);

            if (!is_castable(source, target)) {
                format_type(source, actual_buf, sizeof(actual_buf));
                format_type(target, expected_buf, sizeof(expected_buf));
                typeck_error(c, expr->line, expr->col,
                             "cannot cast %s to %s", actual_buf, expected_buf);
                return &TYPE_ERROR_VALUE;
            }
            return target;
        }

        case NODE_FIELD: {
            Node *object = expr->as.field.object;

            if (object->kind == NODE_IDENT) {
                Node *enum_decl = find_named_node(c->sema->enums, c->sema->enum_count,
                                                  object->as.ident.name, NODE_ENUM_DECL);
                if (enum_decl && enum_has_variant(enum_decl, expr->as.field.field)) {
                    if (!require_decl_visible(c, enum_decl, expr->line, expr->col, object->as.ident.name))
                        return &TYPE_ERROR_VALUE;
                    return make_named_type(c, object->as.ident.name);
                }

                if (find_method_decl(c->sema, object->as.ident.name, expr->as.field.field)) {
                    Node *method = find_method_decl(c->sema, object->as.ident.name, expr->as.field.field);
                    if (!require_decl_visible(c, method, expr->line, expr->col, expr->as.field.field))
                        return &TYPE_ERROR_VALUE;
                    return make_function_type(c, method);
                }
            }

            {
                Type *object_type = check_expr(c, scope, object, unsafe_depth);
                const char *owner = NULL;
                Node *decl = NULL;
                Node *field_node = NULL;

                if (object_type->kind == TYPE_ARRAY && object_type->array_len < 0) {
                    if (!strcmp(expr->as.field.field, "len"))
                        return &TYPE_USIZE_VALUE;
                    if (!strcmp(expr->as.field.field, "ptr"))
                        return make_pointer_type(c, object_type->inner, 0, 0);

                    typeck_error(c, expr->line, expr->col,
                                 "unknown field '%s' for slice type",
                                 expr->as.field.field);
                    return &TYPE_ERROR_VALUE;
                }

                if (object_type->kind == TYPE_STR) {
                    if (!strcmp(expr->as.field.field, "len"))
                        return &TYPE_USIZE_VALUE;
                    if (!strcmp(expr->as.field.field, "ptr"))
                        return make_pointer_type(c, &TYPE_U8_VALUE, 1, 0);

                    typeck_error(c, expr->line, expr->col,
                                 "unknown field '%s' for str",
                                 expr->as.field.field);
                    return &TYPE_ERROR_VALUE;
                }

                if (object_type->kind == TYPE_NAMED && !strcmp(object_type->name, "String")) {
                    if (!strcmp(expr->as.field.field, "len"))
                        return &TYPE_USIZE_VALUE;
                    if (!strcmp(expr->as.field.field, "cap"))
                        return &TYPE_USIZE_VALUE;
                    if (!strcmp(expr->as.field.field, "ptr"))
                        return make_pointer_type(c, &TYPE_U8_VALUE, 0, 0);

                    typeck_error(c, expr->line, expr->col,
                                 "unknown field '%s' for String",
                                 expr->as.field.field);
                    return &TYPE_ERROR_VALUE;
                }

                if (object_type->kind == TYPE_NAMED) owner = object_type->name;
                if (object_type->kind == TYPE_POINTER && object_type->inner &&
                    object_type->inner->kind == TYPE_NAMED) owner = object_type->inner->name;

                if (!owner) {
                    format_type(object_type, actual_buf, sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "cannot access field '%s' on %s",
                                 expr->as.field.field, actual_buf);
                    return &TYPE_ERROR_VALUE;
                }

                decl = find_named_node(c->sema->structs, c->sema->struct_count, owner, NODE_STRUCT_DECL);
                if (!decl)
                    decl = find_named_node(c->sema->unions, c->sema->union_count, owner, NODE_UNION_DECL);
                /* generic struct type like "Pair[i32]": look up by base name */
                if (!decl) {
                    const char *lb = strchr(owner, '[');
                    if (lb) {
                        char base_owner[256];
                        size_t blen = (size_t)(lb - owner);
                        if (blen < sizeof(base_owner)) {
                            memcpy(base_owner, owner, blen);
                            base_owner[blen] = '\0';
                            decl = find_named_node(c->sema->structs, c->sema->struct_count,
                                                   base_owner, NODE_STRUCT_DECL);
                        }
                    }
                }

                if (!decl) {
                    format_type(object_type, actual_buf, sizeof(actual_buf));
                    typeck_error(c, expr->line, expr->col,
                                 "cannot access field '%s' on %s",
                                 expr->as.field.field, actual_buf);
                    return &TYPE_ERROR_VALUE;
                }

                if (decl->kind == NODE_UNION_DECL && unsafe_depth <= 0) {
                    typeck_error(c, expr->line, expr->col,
                                 "union field access requires unsafe block");
                    return &TYPE_ERROR_VALUE;
                }

                if (!composite_has_field(decl, expr->as.field.field, &field_node)) {
                    typeck_error(c, expr->line, expr->col,
                                 "unknown field '%s' for %s",
                                 expr->as.field.field, owner);
                    return &TYPE_ERROR_VALUE;
                }

                {
                    Node *ft_node = field_node->as.let.type;
                    /* For generic struct fields (type param like T), substitute with concrete type */
                    if (ft_node && ft_node->kind == NODE_TYPE_NAMED && decl &&
                        decl->kind == NODE_STRUCT_DECL &&
                        decl->as.struct_decl.type_params.count > 0) {
                        const char *lb = strchr(owner, '[');
                        if (lb && owner[strlen(owner)-1] == ']') {
                            /* extract type arg */
                            size_t inner_len = strlen(owner) - (size_t)(lb - owner) - 2;
                            char inner_buf[256];
                            if (inner_len < sizeof(inner_buf)) {
                                memcpy(inner_buf, lb + 1, inner_len);
                                inner_buf[inner_len] = '\0';
                                /* Check if field type IS the type param */
                                for (int _tp = 0; _tp < decl->as.struct_decl.type_params.count; _tp++) {
                                    const char *tp_name = decl->as.struct_decl.type_params.items[_tp]->as.type_named.name;
                                    if (!strcmp(ft_node->as.type_named.name, tp_name)) {
                                        Node tmp_ft;
                                        memset(&tmp_ft, 0, sizeof(tmp_ft));
                                        tmp_ft.kind = NODE_TYPE_NAMED;
                                        tmp_ft.as.type_named.name = inner_buf;
                                        return type_from_ast(c, &tmp_ft);
                                    }
                                }
                            }
                        }
                    }
                    Type *field_type = type_from_ast(c, ft_node);
                    if (!ensure_resolved_type_visible(c, field_type, expr->line, expr->col))
                        return &TYPE_ERROR_VALUE;
                    return field_type;
                }
            }
        }

        case NODE_STRUCT_LIT: {
            const char *sl_tname = expr->as.struct_lit.type_name;
            Node *decl = find_named_node(c->sema->structs, c->sema->struct_count,
                                         sl_tname, NODE_STRUCT_DECL);
            if (!decl)
                decl = find_named_node(c->sema->unions, c->sema->union_count,
                                       sl_tname, NODE_UNION_DECL);
            /* generic struct instantiation: find by base name */
            int is_generic_sl = 0;
            if (!decl) {
                const char *lb = strchr(sl_tname, '[');
                if (lb && sl_tname[strlen(sl_tname)-1] == ']') {
                    size_t blen = (size_t)(lb - sl_tname);
                    char base[256];
                    if (blen < sizeof(base)) {
                        memcpy(base, sl_tname, blen); base[blen] = '\0';
                        decl = find_named_node(c->sema->structs, c->sema->struct_count,
                                               base, NODE_STRUCT_DECL);
                        if (decl) is_generic_sl = 1;
                    }
                }
            }

            if (!decl)
                return &TYPE_ERROR_VALUE;

            /* for generic struct literals, just check field values exist and are valid */
            for (int i = 0; i < expr->as.struct_lit.count; i++) {
                if (is_generic_sl) {
                    /* just check the value expression without field type checking */
                    check_expr(c, scope, expr->as.struct_lit.field_values[i], unsafe_depth);
                } else {
                    Node *field_node = NULL;
                    Type *expected;
                    Type *actual;

                    if (!require_decl_visible(c, decl, expr->line, expr->col, sl_tname))
                        return &TYPE_ERROR_VALUE;
                    if (!composite_has_field(decl, expr->as.struct_lit.field_names[i], &field_node))
                        return &TYPE_ERROR_VALUE;

                    expected = type_from_ast(c, field_node->as.let.type);
                    actual = check_expr(c, scope, expr->as.struct_lit.field_values[i], unsafe_depth);
                    if (!is_assignable(expected, actual)) {
                        format_type(expected, expected_buf, sizeof(expected_buf));
                        format_type(actual, actual_buf, sizeof(actual_buf));
                        typeck_error(c, expr->as.struct_lit.field_values[i]->line,
                                     expr->as.struct_lit.field_values[i]->col,
                                     "cannot initialize field '%s' with %s; expected %s",
                                     expr->as.struct_lit.field_names[i], actual_buf, expected_buf);
                        return &TYPE_ERROR_VALUE;
                    }
                }
            }
            return make_named_type(c, sl_tname);
        }

        case NODE_MATCH:
            return check_match_expr(c, scope, expr, unsafe_depth);

        default:
            return &TYPE_ERROR_VALUE;
    }
}

static int check_stmt(Checker *c, Scope *scope, Node *stmt, Type *expected_return, int unsafe_depth);

static int check_block(Checker *c, Scope *scope, Node *block, Type *expected_return, int unsafe_depth) {
    for (int i = 0; i < block->as.block.stmts.count; i++) {
        if (!check_stmt(c, scope, block->as.block.stmts.items[i], expected_return, unsafe_depth))
            return 0;
    }
    return 1;
}

static int check_stmt(Checker *c, Scope *scope, Node *stmt, Type *expected_return, int unsafe_depth) {
    Type *declared;
    Type *actual;
    Scope *inner;
    char expected_buf[128];
    char actual_buf[128];

    switch (stmt->kind) {
        case NODE_LET:
            if (stmt->as.let.type &&
                !ensure_type_visible(c, stmt->as.let.type, stmt->line, stmt->col))
                return 0;
            declared = stmt->as.let.type ? type_from_ast(c, stmt->as.let.type) : NULL;
            actual = NULL;
            if (stmt->as.let.value) {
                if (!declared || !check_contextual_constructor(c, scope, declared,
                                                               stmt->as.let.value, unsafe_depth,
                                                               &actual)) {
                    actual = check_expr(c, scope, stmt->as.let.value, unsafe_depth);
                }
            }

            if (!declared && !actual) {
                typeck_error(c, stmt->line, stmt->col,
                             "cannot infer type for '%s' without initializer",
                             stmt->as.let.name);
                return 0;
            }

            if (!declared)
                declared = widen_literal(actual);

            if (declared->kind == TYPE_NULL) {
                typeck_error(c, stmt->line, stmt->col,
                             "cannot infer type for '%s' from null",
                             stmt->as.let.name);
                return 0;
            }

            if (actual && !is_assignable(declared, actual)) {
                format_type(declared, expected_buf, sizeof(expected_buf));
                format_type(actual, actual_buf, sizeof(actual_buf));
                typeck_error(c, stmt->line, stmt->col,
                             "cannot assign %s to %s", actual_buf, expected_buf);
                return 0;
            }

            if (!scope_define(scope, stmt->as.let.name, declared, 1)) {
                typeck_error(c, stmt->line, stmt->col, "out of memory");
                return 0;
            }
            return 1;

        case NODE_CONST:
            if (stmt->as.konst.type &&
                !ensure_type_visible(c, stmt->as.konst.type, stmt->line, stmt->col))
                return 0;
            declared = stmt->as.konst.type ? type_from_ast(c, stmt->as.konst.type) : NULL;
            actual = NULL;
            if (!declared || !check_contextual_constructor(c, scope, declared,
                                                           stmt->as.konst.value, unsafe_depth,
                                                           &actual)) {
                actual = check_expr(c, scope, stmt->as.konst.value, unsafe_depth);
            }

            if (!declared)
                declared = widen_literal(actual);

            if (declared->kind == TYPE_NULL) {
                typeck_error(c, stmt->line, stmt->col,
                             "cannot infer const type from null");
                return 0;
            }

            if (!is_assignable(declared, actual)) {
                format_type(declared, expected_buf, sizeof(expected_buf));
                format_type(actual, actual_buf, sizeof(actual_buf));
                typeck_error(c, stmt->line, stmt->col,
                             "cannot assign %s to %s", actual_buf, expected_buf);
                return 0;
            }

            if (!scope_define(scope, stmt->as.konst.name, declared, 0)) {
                typeck_error(c, stmt->line, stmt->col, "out of memory");
                return 0;
            }
            return 1;

        case NODE_RETURN:
            if (expected_return->kind == TYPE_VOID) {
                if (stmt->as.ret.value) {
                    typeck_error(c, stmt->line, stmt->col,
                                 "void function cannot return a value");
                    return 0;
                }
                return 1;
            }

            if (!stmt->as.ret.value) {
                format_type(expected_return, expected_buf, sizeof(expected_buf));
                typeck_error(c, stmt->line, stmt->col,
                             "expected return value of type %s", expected_buf);
                return 0;
            }

            actual = NULL;
            if (!check_contextual_constructor(c, scope, expected_return,
                                              stmt->as.ret.value, unsafe_depth,
                                              &actual)) {
                actual = check_expr(c, scope, stmt->as.ret.value, unsafe_depth);
            }
            if (!is_assignable(expected_return, actual)) {
                format_type(expected_return, expected_buf, sizeof(expected_buf));
                format_type(actual, actual_buf, sizeof(actual_buf));
                typeck_error(c, stmt->line, stmt->col,
                             "return type mismatch: expected %s, got %s",
                             expected_buf, actual_buf);
                return 0;
            }
            return 1;

        case NODE_IF:
            actual = check_expr(c, scope, stmt->as.if_stmt.cond, unsafe_depth);
            if (!is_assignable(&TYPE_BOOL_VALUE, actual)) {
                format_type(actual, actual_buf, sizeof(actual_buf));
                typeck_error(c, stmt->line, stmt->col,
                             "if condition must be bool, got %s", actual_buf);
                return 0;
            }
            inner = scope_new(scope);
            if (!inner) return 0;
            if (!check_block(c, inner, stmt->as.if_stmt.then_block, expected_return, unsafe_depth)) {
                scope_free(inner);
                return 0;
            }
            scope_free(inner);
            if (stmt->as.if_stmt.else_node) {
                inner = scope_new(scope);
                if (!inner) return 0;
                if (stmt->as.if_stmt.else_node->kind == NODE_BLOCK) {
                    if (!check_block(c, inner, stmt->as.if_stmt.else_node, expected_return, unsafe_depth)) {
                        scope_free(inner);
                        return 0;
                    }
                } else {
                    if (!check_stmt(c, inner, stmt->as.if_stmt.else_node, expected_return, unsafe_depth)) {
                        scope_free(inner);
                        return 0;
                    }
                }
                scope_free(inner);
            }
            return 1;

        case NODE_WHILE:
            actual = check_expr(c, scope, stmt->as.while_stmt.cond, unsafe_depth);
            if (!is_assignable(&TYPE_BOOL_VALUE, actual)) {
                format_type(actual, actual_buf, sizeof(actual_buf));
                typeck_error(c, stmt->line, stmt->col,
                             "while condition must be bool, got %s", actual_buf);
                return 0;
            }
            inner = scope_new(scope);
            if (!inner) return 0;
            if (!check_block(c, inner, stmt->as.while_stmt.body, expected_return, unsafe_depth)) {
                scope_free(inner);
                return 0;
            }
            scope_free(inner);
            return 1;

        case NODE_LOOP:
            inner = scope_new(scope);
            if (!inner) return 0;
            if (!check_block(c, inner, stmt->as.loop_stmt.body, expected_return, unsafe_depth)) {
                scope_free(inner);
                return 0;
            }
            scope_free(inner);
            return 1;

        case NODE_FOR: {
            Scope *for_scope = scope_new(scope);
            if (!for_scope) return 0;

            if (stmt->as.for_stmt.init) {
                if (!check_stmt(c, for_scope, stmt->as.for_stmt.init, expected_return, unsafe_depth)) {
                    scope_free(for_scope);
                    return 0;
                }
            }
            if (stmt->as.for_stmt.cond) {
                actual = check_expr(c, for_scope, stmt->as.for_stmt.cond, unsafe_depth);
                if (!is_assignable(&TYPE_BOOL_VALUE, actual)) {
                    format_type(actual, actual_buf, sizeof(actual_buf));
                    typeck_error(c, stmt->line, stmt->col,
                                 "for condition must be bool, got %s", actual_buf);
                    scope_free(for_scope);
                    return 0;
                }
            }
            if (stmt->as.for_stmt.post) {
                Type *post_t = check_expr(c, for_scope, stmt->as.for_stmt.post, unsafe_depth);
                if (post_t->kind == TYPE_ERROR) {
                    scope_free(for_scope);
                    return 0;
                }
            }

            Scope *body_scope = scope_new(for_scope);
            if (!body_scope) { scope_free(for_scope); return 0; }
            if (!check_block(c, body_scope, stmt->as.for_stmt.body, expected_return, unsafe_depth)) {
                scope_free(body_scope);
                scope_free(for_scope);
                return 0;
            }
            scope_free(body_scope);
            scope_free(for_scope);
            return 1;
        }

        case NODE_UNSAFE:
            inner = scope_new(scope);
            if (!inner) return 0;
            if (!check_block(c, inner, stmt->as.loop_stmt.body, expected_return, unsafe_depth + 1)) {
                scope_free(inner);
                return 0;
            }
            scope_free(inner);
            return 1;

        case NODE_BREAK:
        case NODE_CONTINUE:
            return 1;

        case NODE_DEFER: {
            Type *defer_t = check_expr(c, scope, stmt->as.defer_stmt.expr, unsafe_depth);
            return defer_t->kind != TYPE_ERROR;
        }

        case NODE_EXPR_STMT:
            (void)check_expr(c, scope, stmt->as.expr_stmt.expr, unsafe_depth);
            return !c->had_error;

        case NODE_BLOCK:
            inner = scope_new(scope);
            if (!inner) return 0;
            if (!check_block(c, inner, stmt, expected_return, unsafe_depth)) {
                scope_free(inner);
                return 0;
            }
            scope_free(inner);
            return 1;

        default:
            typeck_error(c, stmt->line, stmt->col, "unsupported statement kind");
            return 0;
    }
}

static int check_function(Checker *c, Node *fn_decl) {
    Scope *scope = scope_new(NULL);
    Type *return_type = type_from_ast(c, fn_decl->as.fn.ret_type);
    Type *prev_return_type = c->current_return_type;
    const char *prev_package = c->current_package;

    if (!scope) {
        typeck_error(c, fn_decl->line, fn_decl->col, "out of memory");
        return 0;
    }

    c->current_package = fn_decl->as.fn.package_name;

    if (!ensure_type_visible(c, fn_decl->as.fn.ret_type, fn_decl->line, fn_decl->col)) {
        c->current_package = prev_package;
        scope_free(scope);
        return 0;
    }

    for (int i = 0; i < c->sema->function_count; i++) {
        if (!scope_define(scope, c->sema->functions[i]->as.fn.name,
                          make_function_type(c, c->sema->functions[i]), 0)) {
            scope_free(scope);
            return 0;
        }
    }

    for (int i = 0; i < c->sema->extern_function_count; i++) {
        if (!scope_define(scope, c->sema->extern_functions[i]->as.extern_fn.name,
                          make_function_type(c, c->sema->extern_functions[i]), 0)) {
            scope_free(scope);
            return 0;
        }
    }

    for (int i = 0; i < fn_decl->as.fn.params.count; i++) {
        Node *param = fn_decl->as.fn.params.items[i];
        if (!ensure_type_visible(c, param->as.let.type, param->line, param->col)) {
            c->current_package = prev_package;
            scope_free(scope);
            return 0;
        }
        if (!scope_define(scope, param->as.let.name,
                          type_from_ast(c, param->as.let.type), 1)) {
            c->current_package = prev_package;
            scope_free(scope);
            return 0;
        }
    }

    c->current_return_type = return_type;
    if (!check_block(c, scope, fn_decl->as.fn.body, return_type, 0)) {
        c->current_return_type = prev_return_type;
        c->current_package = prev_package;
        scope_free(scope);
        return 0;
    }

    c->current_return_type = prev_return_type;
    c->current_package = prev_package;
    scope_free(scope);
    return !c->had_error;
}

int typeck_check(Node *program, const SemanticModel *sema, const char *source_name) {
    Checker c;

    memset(&c, 0, sizeof(c));
    c.source_name = source_name;
    c.sema        = sema;
    c.program     = program;

    for (int i = 0; i < program->as.program.decls.count && !c.had_error; i++) {
        Node *decl = program->as.program.decls.items[i];
        if (decl->kind == NODE_FN_DECL) {
            /* skip type-checking generic templates — done at monomorphization */
            if (decl->as.fn.type_params.count > 0) continue;
            if (!check_function(&c, decl))
                c.had_error = 1;
        } else if (decl->kind == NODE_IMPL_DECL) {
            for (int j = 0; j < decl->as.impl.methods.count && !c.had_error; j++) {
                if (!check_function(&c, decl->as.impl.methods.items[j]))
                    c.had_error = 1;
            }
        } else if (decl->kind == NODE_KERNEL_APP) {
            for (int j = 0; j < decl->as.kernel_app.fns.count && !c.had_error; j++) {
                if (!check_function(&c, decl->as.kernel_app.fns.items[j]))
                    c.had_error = 1;
            }
        }
    }

    free_types(&c);
    return c.had_error ? 0 : 1;
}
