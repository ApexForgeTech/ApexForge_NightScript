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

static int is_integer_kind(TypeKind kind) {
    return kind == TYPE_I8 || kind == TYPE_I16 || kind == TYPE_I32 || kind == TYPE_I64 ||
           kind == TYPE_ISIZE || kind == TYPE_U8 || kind == TYPE_U16 || kind == TYPE_U32 ||
           kind == TYPE_U64 || kind == TYPE_USIZE || kind == TYPE_INT_LITERAL;
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

    if (actual->kind == TYPE_INT_LITERAL && is_integer_kind(expected->kind))
        return 1;
    if (actual->kind == TYPE_FLOAT_LITERAL && is_float_kind(expected->kind))
        return 1;
    if (actual->kind == TYPE_STRING_LITERAL &&
        (expected->kind == TYPE_STR || expected->kind == TYPE_CSTR))
        return 1;
    if (actual->kind == TYPE_NULL &&
        expected->kind == TYPE_POINTER &&
        expected->is_nullable)
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
        Type *actual_type = check_expr(c, scope, args->items[i], unsafe_depth);
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
    Type *result_type = NULL;
    char expected_buf[128];
    char actual_buf[128];

    for (int i = 0; i < expr->as.match.count; i++) {
        const char *pattern = expr->as.match.patterns[i];
        Type *arm_type;

        if (strcmp(pattern, "_")) {
            const char *dot = strchr(pattern, '.');
            char enum_name[128];

            if (!dot) return &TYPE_ERROR_VALUE;
            memcpy(enum_name, pattern, (size_t)(dot - pattern));
            enum_name[dot - pattern] = '\0';

            if (!(subject_type->kind == TYPE_NAMED && !strcmp(subject_type->name, enum_name))) {
                format_type(subject_type, actual_buf, sizeof(actual_buf));
                typeck_error(c, expr->line, expr->col,
                             "match arm '%s' does not match subject type %s",
                             pattern, actual_buf);
                return &TYPE_ERROR_VALUE;
            }
        }

        arm_type = check_expr(c, scope, expr->as.match.values[i], unsafe_depth);
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

    return result_type ? result_type : &TYPE_ERROR_VALUE;
}

static Type *check_expr(Checker *c, Scope *scope, Node *expr, int unsafe_depth) {
    char expected_buf[128];
    char actual_buf[128];

    if (!expr) return &TYPE_VOID_VALUE;

    switch (expr->kind) {
        case NODE_LIT_INT:
            return &TYPE_INT_LIT_VALUE;
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

            if (binding) return binding->type;

            fn = find_named_node(c->sema->functions, c->sema->function_count,
                                 expr->as.ident.name, NODE_FN_DECL);
            if (!fn)
                fn = find_named_node(c->sema->extern_functions, c->sema->extern_function_count,
                                     expr->as.ident.name, NODE_EXTERN_FN);
            if (fn) return make_function_type(c, fn);

            return &TYPE_ERROR_VALUE;
        }

        case NODE_GROUP:
            return check_expr(c, scope, expr->as.group.expr, unsafe_depth);

        case NODE_UNARY: {
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

            if (obj_type->kind != TYPE_ERROR) {
                format_type(obj_type, actual_buf, sizeof(actual_buf));
                typeck_error(c, expr->line, expr->col,
                             "cannot index into type %s", actual_buf);
            }
            return &TYPE_ERROR_VALUE;
        }

        case NODE_CALL: {
            Node *callee = expr->as.call.callee;
            Type *callee_type;

            if (callee->kind == NODE_FIELD) {
                Node *object = callee->as.field.object;

                if (object->kind == NODE_IDENT) {
                    Node *method = find_method_decl(c->sema, object->as.ident.name, callee->as.field.field);
                    if (method) {
                        if (method_has_receiver(method)) {
                            typeck_error(c, callee->line, callee->col,
                                         "method '%s.%s' requires an instance",
                                         object->as.ident.name, callee->as.field.field);
                            return &TYPE_ERROR_VALUE;
                        }
                        if (!check_call_arguments(c, &method->as.fn.params, 0,
                                                  &expr->as.call.args, scope, unsafe_depth,
                                                  expr->line, expr->col)) {
                            return &TYPE_ERROR_VALUE;
                        }
                        return type_from_ast(c, method->as.fn.ret_type);
                    }
                }

                {
                    Type *object_type = check_expr(c, scope, object, unsafe_depth);
                    const char *owner = NULL;
                    Node *method;
                    Type *receiver_expected;

                    if (object_type->kind == TYPE_NAMED) owner = object_type->name;
                    if (object_type->kind == TYPE_POINTER && object_type->inner &&
                        object_type->inner->kind == TYPE_NAMED) {
                        owner = object_type->inner->name;
                    }

                    method = owner ? find_method_decl(c->sema, owner, callee->as.field.field) : NULL;
                    if (method) {
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
                        return type_from_ast(c, method->as.fn.ret_type);
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
                return type_from_ast(c, callee_type->decl->as.fn.ret_type);
            }

            if (!check_call_arguments(c, &callee_type->decl->as.extern_fn.params, 0,
                                      &expr->as.call.args, scope, unsafe_depth,
                                      expr->line, expr->col)) {
                return &TYPE_ERROR_VALUE;
            }
            return type_from_ast(c, callee_type->decl->as.extern_fn.ret_type);
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
                if (enum_decl && enum_has_variant(enum_decl, expr->as.field.field))
                    return make_named_type(c, object->as.ident.name);

                if (find_method_decl(c->sema, object->as.ident.name, expr->as.field.field))
                    return make_function_type(c, find_method_decl(c->sema, object->as.ident.name, expr->as.field.field));
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

                return type_from_ast(c, field_node->as.let.type);
            }
        }

        case NODE_STRUCT_LIT: {
            Node *decl = find_named_node(c->sema->structs, c->sema->struct_count,
                                         expr->as.struct_lit.type_name, NODE_STRUCT_DECL);
            if (!decl)
                decl = find_named_node(c->sema->unions, c->sema->union_count,
                                       expr->as.struct_lit.type_name, NODE_UNION_DECL);

            if (!decl)
                return &TYPE_ERROR_VALUE;

            for (int i = 0; i < expr->as.struct_lit.count; i++) {
                Node *field_node = NULL;
                Type *expected;
                Type *actual;

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
            return make_named_type(c, expr->as.struct_lit.type_name);
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
            declared = stmt->as.let.type ? type_from_ast(c, stmt->as.let.type) : NULL;
            actual = stmt->as.let.value ? check_expr(c, scope, stmt->as.let.value, unsafe_depth) : NULL;

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
            declared = stmt->as.konst.type ? type_from_ast(c, stmt->as.konst.type) : NULL;
            actual = check_expr(c, scope, stmt->as.konst.value, unsafe_depth);

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

            actual = check_expr(c, scope, stmt->as.ret.value, unsafe_depth);
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

        case NODE_DEFER:
            (void)check_expr(c, scope, stmt->as.defer_stmt.expr, unsafe_depth);
            return !c->had_error;

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

    if (!scope) {
        typeck_error(c, fn_decl->line, fn_decl->col, "out of memory");
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
        if (!scope_define(scope, param->as.let.name,
                          type_from_ast(c, param->as.let.type), 1)) {
            scope_free(scope);
            return 0;
        }
    }

    if (!check_block(c, scope, fn_decl->as.fn.body, return_type, 0)) {
        scope_free(scope);
        return 0;
    }

    scope_free(scope);
    return !c->had_error;
}

int typeck_check(Node *program, const SemanticModel *sema, const char *source_name) {
    Checker c;

    memset(&c, 0, sizeof(c));
    c.source_name = source_name;
    c.sema = sema;

    for (int i = 0; i < program->as.program.decls.count && !c.had_error; i++) {
        Node *decl = program->as.program.decls.items[i];
        if (decl->kind == NODE_FN_DECL) {
            if (!check_function(&c, decl))
                c.had_error = 1;
        } else if (decl->kind == NODE_IMPL_DECL) {
            for (int j = 0; j < decl->as.impl.methods.count && !c.had_error; j++) {
                if (!check_function(&c, decl->as.impl.methods.items[j]))
                    c.had_error = 1;
            }
        }
    }

    free_types(&c);
    return c.had_error ? 0 : 1;
}
