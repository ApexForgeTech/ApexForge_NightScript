#include "codegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ── output buffer ─────────────────────────────────────────────────────── */

static void emit(COut *o, const char *fmt, ...) {
    va_list ap;
    char tmp[4096];
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (o->len + n + 1 >= o->cap) {
        o->cap = (o->len + n + 1) * 2 + 1024;
        o->buf = realloc(o->buf, (size_t)o->cap);
    }
    memcpy(o->buf + o->len, tmp, (size_t)n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void emit_indent(COut *o) {
    for (int i = 0; i < o->indent * 4; i++)
        emit(o, " ");
}

static void emitln(COut *o, const char *fmt, ...) {
    emit_indent(o);
    va_list ap;
    char tmp[4096];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    emit(o, "%s\n", tmp);
}

/* ── type → C string ───────────────────────────────────────────────────── */

/*
 * C has inside-out type syntax for arrays/pointers.
 * We split type emission into a "left" part (before name) and
 * "right" part (after name, for array brackets).
 * For simple cases right is empty.
 */
static void emit_type_left(COut *o, Node *t);
static void emit_type_right(COut *o, Node *t);
static const char *header_for_extern(const char *name);
static void format_slice_type_name(Node *type, char *buf, size_t size);
static int has_ui_app(Node *prog);
static int has_kernel_app(Node *prog);

typedef struct SliceDef SliceDef;
typedef struct OptionResultDef OptionResultDef;

struct SliceDef {
    char     *name;
    Node     *type;
    SliceDef *next;
};

struct OptionResultDef {
    char            *name;
    int              is_result;
    OptionResultDef *next;
};

static void append_text(char *buf, size_t size, const char *text) {
    size_t len;
    size_t remaining;

    if (!buf || size == 0 || !text)
        return;

    len = strlen(buf);
    if (len >= size - 1)
        return;

    remaining = size - len - 1;
    strncat(buf, text, remaining);
}

static void append_sanitized(char *buf, size_t size, const char *text) {
    size_t len;

    if (!buf || size == 0 || !text)
        return;

    len = strlen(buf);
    while (*text && len + 1 < size) {
        char ch = *text++;
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9')) {
            buf[len++] = ch;
        } else {
            buf[len++] = '_';
        }
    }
    buf[len] = '\0';
}

static void append_number(char *buf, size_t size, int value) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%d", value);
    append_text(buf, size, tmp);
}

static char *dup_cstr(const char *text) {
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
            *right = dup_cstr(it + 1);
            return *left && *right;
        }
    }

    return 0;
}

static void format_option_result_type_name(const char *name, char *buf, size_t size) {
    size_t len;

    if (!buf || size == 0)
        return;

    buf[0] = '\0';
    append_text(buf, size, "NS_");
    append_sanitized(buf, size, name ? name : "invalid");
    len = strlen(buf);
    while (len > 0 && buf[len - 1] == '_')
        buf[--len] = '\0';
}

static int option_result_defs_contains(OptionResultDef *defs, const char *name) {
    for (OptionResultDef *cur = defs; cur; cur = cur->next) {
        if (!strcmp(cur->name, name))
            return 1;
    }
    return 0;
}

static void collect_option_result_named_text(OptionResultDef **defs, const char *name) {
    OptionResultDef *entry;
    char *inner = NULL;
    char *left = NULL;
    char *right = NULL;

    if (!name)
        return;

    if (type_name_has_form(name, "Option")) {
        if (option_result_defs_contains(*defs, name))
            goto recurse_option;

        entry = calloc(1, sizeof(OptionResultDef));
        if (!entry)
            return;
        entry->name = dup_cstr(name);
        if (!entry->name) {
            free(entry);
            return;
        }
        entry->is_result = 0;
        entry->next = *defs;
        *defs = entry;

recurse_option:
        inner = slice_range_dup(name + 7, name + strlen(name) - 1);
        if (inner) {
            collect_option_result_named_text(defs, inner);
            free(inner);
        }
        return;
    }

    if (!type_name_has_form(name, "Result"))
        return;

    if (!option_result_defs_contains(*defs, name)) {
        entry = calloc(1, sizeof(OptionResultDef));
        if (!entry)
            return;
        entry->name = dup_cstr(name);
        if (!entry->name) {
            free(entry);
            return;
        }
        entry->is_result = 1;
        entry->next = *defs;
        *defs = entry;
    }

    inner = slice_range_dup(name + 7, name + strlen(name) - 1);
    if (!inner)
        return;

    if (split_top_level_comma(inner, &left, &right)) {
        collect_option_result_named_text(defs, left);
        collect_option_result_named_text(defs, right);
    }

    free(inner);
    free(left);
    free(right);
}

static void append_type_mangle(Node *type, char *buf, size_t size) {
    if (!type) {
        append_text(buf, size, "void");
        return;
    }

    switch (type->kind) {
        case NODE_TYPE_NAMED:
            append_sanitized(buf, size, type->as.type_named.name);
            return;
        case NODE_TYPE_POINTER:
            append_text(buf, size, "ptr_");
            if (type->as.type_ptr.is_const)
                append_text(buf, size, "const_");
            if (type->as.type_ptr.is_nullable)
                append_text(buf, size, "nullable_");
            append_type_mangle(type->as.type_ptr.inner, buf, size);
            return;
        case NODE_TYPE_ARRAY:
            if (type->as.type_array.length < 0) {
                append_text(buf, size, "slice_");
            } else {
                append_text(buf, size, "arr_");
                append_number(buf, size, type->as.type_array.length);
                append_text(buf, size, "_");
            }
            append_type_mangle(type->as.type_array.elem, buf, size);
            return;
        default:
            append_text(buf, size, "type");
            return;
    }
}

static void format_slice_type_name(Node *type, char *buf, size_t size) {
    if (!buf || size == 0)
        return;

    buf[0] = '\0';
    append_text(buf, size, "NSlice_");
    if (!type || type->kind != NODE_TYPE_ARRAY || type->as.type_array.length >= 0) {
        append_text(buf, size, "invalid");
        return;
    }

    append_type_mangle(type->as.type_array.elem, buf, size);
}

static int slice_defs_contains(SliceDef *defs, const char *name) {
    for (SliceDef *cur = defs; cur; cur = cur->next) {
        if (!strcmp(cur->name, name))
            return 1;
    }
    return 0;
}

static void collect_slice_types_in_type(SliceDef **defs, Node *type) {
    char name[256];
    SliceDef *entry;

    if (!type)
        return;

    if (type->kind == NODE_TYPE_POINTER) {
        collect_slice_types_in_type(defs, type->as.type_ptr.inner);
        return;
    }

    if (type->kind != NODE_TYPE_ARRAY)
        return;

    collect_slice_types_in_type(defs, type->as.type_array.elem);

    if (type->as.type_array.length >= 0)
        return;

    format_slice_type_name(type, name, sizeof(name));
    if (slice_defs_contains(*defs, name))
        return;

    entry = calloc(1, sizeof(SliceDef));
    if (!entry)
        return;

    entry->name = dup_cstr(name);
    if (!entry->name) {
        free(entry);
        return;
    }

    entry->type = type;
    entry->next = *defs;
    *defs = entry;
}

static void collect_option_result_types_in_type(OptionResultDef **defs, Node *type) {
    if (!type)
        return;

    if (type->kind == NODE_TYPE_NAMED) {
        collect_option_result_named_text(defs, type->as.type_named.name);
        return;
    }

    if (type->kind == NODE_TYPE_POINTER) {
        collect_option_result_types_in_type(defs, type->as.type_ptr.inner);
        return;
    }

    if (type->kind == NODE_TYPE_ARRAY)
        collect_option_result_types_in_type(defs, type->as.type_array.elem);
}

static void collect_slice_types_in_node(SliceDef **defs, Node *node) {
    if (!node)
        return;

    switch (node->kind) {
        case NODE_TYPE_NAMED:
        case NODE_TYPE_POINTER:
        case NODE_TYPE_ARRAY:
            collect_slice_types_in_type(defs, node);
            return;
        case NODE_BINARY:
            collect_slice_types_in_node(defs, node->as.binary.left);
            collect_slice_types_in_node(defs, node->as.binary.right);
            return;
        case NODE_UNARY:
            collect_slice_types_in_node(defs, node->as.unary.operand);
            return;
        case NODE_CALL:
            collect_slice_types_in_node(defs, node->as.call.callee);
            for (int i = 0; i < node->as.call.args.count; i++)
                collect_slice_types_in_node(defs, node->as.call.args.items[i]);
            return;
        case NODE_FIELD:
            collect_slice_types_in_node(defs, node->as.field.object);
            return;
        case NODE_CAST:
            collect_slice_types_in_node(defs, node->as.cast.expr);
            collect_slice_types_in_node(defs, node->as.cast.type);
            return;
        case NODE_ASSIGN:
            collect_slice_types_in_node(defs, node->as.assign.target);
            collect_slice_types_in_node(defs, node->as.assign.value);
            return;
        case NODE_INDEX:
            collect_slice_types_in_node(defs, node->as.index_expr.object);
            collect_slice_types_in_node(defs, node->as.index_expr.index);
            return;
        case NODE_SLICE:
            collect_slice_types_in_node(defs, node->as.slice_expr.object);
            collect_slice_types_in_node(defs, node->as.slice_expr.start);
            collect_slice_types_in_node(defs, node->as.slice_expr.end);
            return;
        case NODE_DEFER:
            collect_slice_types_in_node(defs, node->as.defer_stmt.expr);
            return;
        case NODE_STRUCT_LIT:
            for (int i = 0; i < node->as.struct_lit.count; i++)
                collect_slice_types_in_node(defs, node->as.struct_lit.field_values[i]);
            return;
        case NODE_MATCH:
            collect_slice_types_in_node(defs, node->as.match.subject);
            for (int i = 0; i < node->as.match.count; i++)
                collect_slice_types_in_node(defs, node->as.match.values[i]);
            return;
        case NODE_GROUP:
            collect_slice_types_in_node(defs, node->as.group.expr);
            return;
        case NODE_BLOCK:
            for (int i = 0; i < node->as.block.stmts.count; i++)
                collect_slice_types_in_node(defs, node->as.block.stmts.items[i]);
            return;
        case NODE_LET:
            collect_slice_types_in_node(defs, node->as.let.type);
            collect_slice_types_in_node(defs, node->as.let.value);
            return;
        case NODE_CONST:
            collect_slice_types_in_node(defs, node->as.konst.type);
            collect_slice_types_in_node(defs, node->as.konst.value);
            return;
        case NODE_RETURN:
            collect_slice_types_in_node(defs, node->as.ret.value);
            return;
        case NODE_IF:
            collect_slice_types_in_node(defs, node->as.if_stmt.cond);
            collect_slice_types_in_node(defs, node->as.if_stmt.then_block);
            collect_slice_types_in_node(defs, node->as.if_stmt.else_node);
            return;
        case NODE_WHILE:
            collect_slice_types_in_node(defs, node->as.while_stmt.cond);
            collect_slice_types_in_node(defs, node->as.while_stmt.body);
            return;
        case NODE_LOOP:
        case NODE_UNSAFE:
            collect_slice_types_in_node(defs, node->as.loop_stmt.body);
            return;
        case NODE_FOR:
            collect_slice_types_in_node(defs, node->as.for_stmt.init);
            collect_slice_types_in_node(defs, node->as.for_stmt.cond);
            collect_slice_types_in_node(defs, node->as.for_stmt.post);
            collect_slice_types_in_node(defs, node->as.for_stmt.body);
            return;
        case NODE_EXPR_STMT:
            collect_slice_types_in_node(defs, node->as.expr_stmt.expr);
            return;
        case NODE_PROGRAM:
            collect_slice_types_in_node(defs, node->as.program.package);
            for (int i = 0; i < node->as.program.imports.count; i++)
                collect_slice_types_in_node(defs, node->as.program.imports.items[i]);
            for (int i = 0; i < node->as.program.decls.count; i++)
                collect_slice_types_in_node(defs, node->as.program.decls.items[i]);
            return;
        case NODE_FN_DECL:
            for (int i = 0; i < node->as.fn.params.count; i++)
                collect_slice_types_in_node(defs, node->as.fn.params.items[i]);
            collect_slice_types_in_node(defs, node->as.fn.ret_type);
            collect_slice_types_in_node(defs, node->as.fn.body);
            return;
        case NODE_EXTERN_FN:
            for (int i = 0; i < node->as.extern_fn.params.count; i++)
                collect_slice_types_in_node(defs, node->as.extern_fn.params.items[i]);
            collect_slice_types_in_node(defs, node->as.extern_fn.ret_type);
            return;
        case NODE_STRUCT_DECL:
            for (int i = 0; i < node->as.struct_decl.fields.count; i++)
                collect_slice_types_in_node(defs, node->as.struct_decl.fields.items[i]);
            return;
        case NODE_UNION_DECL:
            for (int i = 0; i < node->as.union_decl.fields.count; i++)
                collect_slice_types_in_node(defs, node->as.union_decl.fields.items[i]);
            return;
        case NODE_IMPL_DECL:
            for (int i = 0; i < node->as.impl.methods.count; i++)
                collect_slice_types_in_node(defs, node->as.impl.methods.items[i]);
            return;
        default:
            return;
    }
}

static void collect_option_result_types_in_node(OptionResultDef **defs, Node *node) {
    if (!node)
        return;

    switch (node->kind) {
        case NODE_TYPE_NAMED:
        case NODE_TYPE_POINTER:
        case NODE_TYPE_ARRAY:
            collect_option_result_types_in_type(defs, node);
            return;
        case NODE_BINARY:
            collect_option_result_types_in_node(defs, node->as.binary.left);
            collect_option_result_types_in_node(defs, node->as.binary.right);
            return;
        case NODE_UNARY:
            collect_option_result_types_in_node(defs, node->as.unary.operand);
            return;
        case NODE_CALL:
            collect_option_result_types_in_node(defs, node->as.call.callee);
            for (int i = 0; i < node->as.call.args.count; i++)
                collect_option_result_types_in_node(defs, node->as.call.args.items[i]);
            return;
        case NODE_FIELD:
            collect_option_result_types_in_node(defs, node->as.field.object);
            return;
        case NODE_CAST:
            collect_option_result_types_in_node(defs, node->as.cast.expr);
            collect_option_result_types_in_node(defs, node->as.cast.type);
            return;
        case NODE_ASSIGN:
            collect_option_result_types_in_node(defs, node->as.assign.target);
            collect_option_result_types_in_node(defs, node->as.assign.value);
            return;
        case NODE_INDEX:
            collect_option_result_types_in_node(defs, node->as.index_expr.object);
            collect_option_result_types_in_node(defs, node->as.index_expr.index);
            return;
        case NODE_SLICE:
            collect_option_result_types_in_node(defs, node->as.slice_expr.object);
            collect_option_result_types_in_node(defs, node->as.slice_expr.start);
            collect_option_result_types_in_node(defs, node->as.slice_expr.end);
            return;
        case NODE_DEFER:
            collect_option_result_types_in_node(defs, node->as.defer_stmt.expr);
            return;
        case NODE_STRUCT_LIT:
            for (int i = 0; i < node->as.struct_lit.count; i++)
                collect_option_result_types_in_node(defs, node->as.struct_lit.field_values[i]);
            return;
        case NODE_MATCH:
            collect_option_result_types_in_node(defs, node->as.match.subject);
            for (int i = 0; i < node->as.match.count; i++)
                collect_option_result_types_in_node(defs, node->as.match.values[i]);
            return;
        case NODE_GROUP:
            collect_option_result_types_in_node(defs, node->as.group.expr);
            return;
        case NODE_BLOCK:
            for (int i = 0; i < node->as.block.stmts.count; i++)
                collect_option_result_types_in_node(defs, node->as.block.stmts.items[i]);
            return;
        case NODE_LET:
            collect_option_result_types_in_node(defs, node->as.let.type);
            collect_option_result_types_in_node(defs, node->as.let.value);
            return;
        case NODE_CONST:
            collect_option_result_types_in_node(defs, node->as.konst.type);
            collect_option_result_types_in_node(defs, node->as.konst.value);
            return;
        case NODE_RETURN:
            collect_option_result_types_in_node(defs, node->as.ret.value);
            return;
        case NODE_IF:
            collect_option_result_types_in_node(defs, node->as.if_stmt.cond);
            collect_option_result_types_in_node(defs, node->as.if_stmt.then_block);
            collect_option_result_types_in_node(defs, node->as.if_stmt.else_node);
            return;
        case NODE_WHILE:
            collect_option_result_types_in_node(defs, node->as.while_stmt.cond);
            collect_option_result_types_in_node(defs, node->as.while_stmt.body);
            return;
        case NODE_LOOP:
        case NODE_UNSAFE:
            collect_option_result_types_in_node(defs, node->as.loop_stmt.body);
            return;
        case NODE_FOR:
            collect_option_result_types_in_node(defs, node->as.for_stmt.init);
            collect_option_result_types_in_node(defs, node->as.for_stmt.cond);
            collect_option_result_types_in_node(defs, node->as.for_stmt.post);
            collect_option_result_types_in_node(defs, node->as.for_stmt.body);
            return;
        case NODE_EXPR_STMT:
            collect_option_result_types_in_node(defs, node->as.expr_stmt.expr);
            return;
        case NODE_PROGRAM:
            collect_option_result_types_in_node(defs, node->as.program.package);
            for (int i = 0; i < node->as.program.imports.count; i++)
                collect_option_result_types_in_node(defs, node->as.program.imports.items[i]);
            for (int i = 0; i < node->as.program.decls.count; i++)
                collect_option_result_types_in_node(defs, node->as.program.decls.items[i]);
            return;
        case NODE_FN_DECL:
            for (int i = 0; i < node->as.fn.params.count; i++)
                collect_option_result_types_in_node(defs, node->as.fn.params.items[i]);
            collect_option_result_types_in_node(defs, node->as.fn.ret_type);
            collect_option_result_types_in_node(defs, node->as.fn.body);
            return;
        case NODE_EXTERN_FN:
            for (int i = 0; i < node->as.extern_fn.params.count; i++)
                collect_option_result_types_in_node(defs, node->as.extern_fn.params.items[i]);
            collect_option_result_types_in_node(defs, node->as.extern_fn.ret_type);
            return;
        case NODE_STRUCT_DECL:
            for (int i = 0; i < node->as.struct_decl.fields.count; i++)
                collect_option_result_types_in_node(defs, node->as.struct_decl.fields.items[i]);
            return;
        case NODE_UNION_DECL:
            for (int i = 0; i < node->as.union_decl.fields.count; i++)
                collect_option_result_types_in_node(defs, node->as.union_decl.fields.items[i]);
            return;
        case NODE_IMPL_DECL:
            for (int i = 0; i < node->as.impl.methods.count; i++)
                collect_option_result_types_in_node(defs, node->as.impl.methods.items[i]);
            return;
        default:
            return;
    }
}

static void free_slice_defs(SliceDef *defs) {
    while (defs) {
        SliceDef *next = defs->next;
        free(defs->name);
        free(defs);
        defs = next;
    }
}

static void free_option_result_defs(OptionResultDef *defs) {
    while (defs) {
        OptionResultDef *next = defs->next;
        free(defs->name);
        free(defs);
        defs = next;
    }
}

static void emit_type_left(COut *o, Node *t) {
    if (!t) { emit(o, "void"); return; }
    switch (t->kind) {
        case NODE_TYPE_NAMED: {
            const char *name = t->as.type_named.name;
            /* primitive type mapping */
            if      (!strcmp(name, "i8"))    emit(o, "int8_t");
            else if (!strcmp(name, "i16"))   emit(o, "int16_t");
            else if (!strcmp(name, "i32"))   emit(o, "int32_t");
            else if (!strcmp(name, "i64"))   emit(o, "int64_t");
            else if (!strcmp(name, "isize")) emit(o, "ptrdiff_t");
            else if (!strcmp(name, "u8"))    emit(o, "uint8_t");
            else if (!strcmp(name, "u16"))   emit(o, "uint16_t");
            else if (!strcmp(name, "u32"))   emit(o, "uint32_t");
            else if (!strcmp(name, "u64"))   emit(o, "uint64_t");
            else if (!strcmp(name, "usize")) emit(o, "size_t");
            else if (!strcmp(name, "f32"))   emit(o, "float");
            else if (!strcmp(name, "f64"))   emit(o, "double");
            else if (!strcmp(name, "bool"))  emit(o, "bool");
            else if (!strcmp(name, "char"))  emit(o, "char");
            else if (!strcmp(name, "void"))  emit(o, "void");
            else if (!strcmp(name, "never")) emit(o, "void");
            else if (!strcmp(name, "str"))   emit(o, "NStr");
            else if (!strcmp(name, "String")) emit(o, "NString");
            else if (!strcmp(name, "cstr"))  emit(o, "const char*");
            else if (type_name_has_form(name, "Option") || type_name_has_form(name, "Result")) {
                char c_name[256];
                format_option_result_type_name(name, c_name, sizeof(c_name));
                emit(o, "%s", c_name);
            } else {
                emit(o, "%s", name);
            }
            break;
        }
        case NODE_TYPE_POINTER:
            if (t->as.type_ptr.is_const)
                emit(o, "const ");
            emit_type_left(o, t->as.type_ptr.inner);
            emit(o, "*");
            break;
        case NODE_TYPE_ARRAY:
            if (t->as.type_array.length < 0) {
                char slice_name[256];
                format_slice_type_name(t, slice_name, sizeof(slice_name));
                emit(o, "%s", slice_name);
            } else {
                emit_type_left(o, t->as.type_array.elem);
            }
            break;
        default:
            emit(o, "void");
    }
}

static void emit_type_right(COut *o, Node *t) {
    if (!t) return;
    if (t->kind == NODE_TYPE_ARRAY && t->as.type_array.length >= 0)
        emit(o, "[%d]", t->as.type_array.length);
}

/* helper: emit full type + space + name (for declarations) */
static void emit_typed_name(COut *o, Node *type, const char *name) {
    emit_type_left(o, type);
    if (name) emit(o, " %s", name);
    emit_type_right(o, type);
}

static void make_c_name(char *out, int outsz, const char *owner, const char *name);

typedef struct CGBinding CGBinding;
typedef struct CGScope CGScope;
typedef struct CGTempType CGTempType;
typedef struct IntStack IntStack;

struct CGBinding {
    const char *name;
    Node       *type;
    CGBinding  *next;
};

struct CGScope {
    CGScope   *parent;
    CGBinding *bindings;
};

struct CGTempType {
    Node       node;
    CGTempType *next;
};

struct IntStack {
    int *items;
    int count;
    int cap;
};

typedef struct {
    Node       *program;
    CGScope    *scope;
    CGTempType *temps;
    Node       *current_return_type;
    int         temp_counter;
    Node      **defers;     /* function-scoped defer list */
    int         defer_count;
    int         defer_cap;
    IntStack    scope_defer_markers;
    IntStack    loop_defer_markers;
} CGContext;

static void emit_stmt(COut *o, CGContext *cg, Node *n);
static void emit_scope_defers(COut *o, CGContext *cg);
static int stmt_terminates_control_flow(Node *n);

static int intstack_push(IntStack *stack, int value) {
    int *new_items;

    if (stack->count == stack->cap) {
        int new_cap = stack->cap ? stack->cap * 2 : 8;
        new_items = realloc(stack->items, (size_t)new_cap * sizeof(int));
        if (!new_items)
            return 0;
        stack->items = new_items;
        stack->cap = new_cap;
    }

    stack->items[stack->count++] = value;
    return 1;
}

static int intstack_pop(IntStack *stack) {
    if (stack->count == 0)
        return 0;
    return stack->items[--stack->count];
}

static int intstack_top(const IntStack *stack) {
    if (stack->count == 0)
        return 0;
    return stack->items[stack->count - 1];
}

static void cg_push_scope(CGContext *cg) {
    CGScope *scope = malloc(sizeof(CGScope));
    scope->parent  = cg->scope;
    scope->bindings = NULL;
    cg->scope      = scope;
    intstack_push(&cg->scope_defer_markers, cg->defer_count);
}

static void cg_pop_scope(CGContext *cg) {
    if (!cg->scope) return;

    CGBinding *binding = cg->scope->bindings;
    while (binding) {
        CGBinding *next = binding->next;
        free(binding);
        binding = next;
    }

    CGScope *parent = cg->scope->parent;
    free(cg->scope);
    cg->scope = parent;
    if (cg->scope_defer_markers.count > 0)
        intstack_pop(&cg->scope_defer_markers);
}

static void cg_define(CGContext *cg, const char *name, Node *type) {
    if (!cg->scope || !name || !type) return;

    CGBinding *binding = malloc(sizeof(CGBinding));
    binding->name = name;
    binding->type = type;
    binding->next = cg->scope->bindings;
    cg->scope->bindings = binding;
}

static Node *cg_lookup(CGContext *cg, const char *name) {
    for (CGScope *scope = cg->scope; scope; scope = scope->parent) {
        for (CGBinding *binding = scope->bindings; binding; binding = binding->next) {
            if (!strcmp(binding->name, name))
                return binding->type;
        }
    }
    return NULL;
}

static Node *cg_temp_type_named(CGContext *cg, const char *name) {
    CGTempType *temp = calloc(1, sizeof(CGTempType));
    temp->node.kind = NODE_TYPE_NAMED;
    temp->node.as.type_named.name = (char *)name;
    temp->next = cg->temps;
    cg->temps = temp;
    return &temp->node;
}

static Node *cg_temp_type_pointer(CGContext *cg, Node *inner, int is_const) {
    CGTempType *temp = calloc(1, sizeof(CGTempType));
    temp->node.kind = NODE_TYPE_POINTER;
    temp->node.as.type_ptr.inner = inner;
    temp->node.as.type_ptr.is_const = is_const;
    temp->node.as.type_ptr.is_nullable = 0;
    temp->next = cg->temps;
    cg->temps = temp;
    return &temp->node;
}

static Node *cg_temp_type_array(CGContext *cg, Node *elem, int length) {
    CGTempType *temp = calloc(1, sizeof(CGTempType));
    temp->node.kind = NODE_TYPE_ARRAY;
    temp->node.as.type_array.elem = elem;
    temp->node.as.type_array.length = length;
    temp->next = cg->temps;
    cg->temps = temp;
    return &temp->node;
}

static void emit_expr(COut *o, CGContext *cg, Node *n);

static void cg_free_temps(CGContext *cg) {
    CGTempType *temp = cg->temps;
    while (temp) {
        CGTempType *next = temp->next;
        free(temp);
        temp = next;
    }
    cg->temps = NULL;
}

static void emit_defers_range(COut *o, CGContext *cg, int marker) {
    if (marker < 0)
        marker = 0;

    for (int i = cg->defer_count - 1; i >= marker; i--) {
        emit_indent(o);
        emit_expr(o, cg, cg->defers[i]);
        emit(o, ";\n");
    }
}

static void emit_scope_defers(COut *o, CGContext *cg) {
    int marker = intstack_top(&cg->scope_defer_markers);
    emit_defers_range(o, cg, marker);
    cg->defer_count = marker;
}

static const char *type_named_name(Node *type) {
    if (!type || type->kind != NODE_TYPE_NAMED)
        return NULL;
    return type->as.type_named.name;
}

static Node *type_pointer_inner(Node *type) {
    if (!type || type->kind != NODE_TYPE_POINTER)
        return NULL;
    return type->as.type_ptr.inner;
}

static Node *find_decl_named(Node *program, NodeKind kind, const char *name) {
    for (int i = 0; i < program->as.program.decls.count; i++) {
        Node *decl = program->as.program.decls.items[i];
        if (decl->kind != kind)
            continue;

        const char *decl_name = NULL;
        if (kind == NODE_STRUCT_DECL) decl_name = decl->as.struct_decl.name;
        if (kind == NODE_ENUM_DECL)   decl_name = decl->as.enum_decl.name;
        if (kind == NODE_UNION_DECL)  decl_name = decl->as.union_decl.name;
        if (decl_name && !strcmp(decl_name, name))
            return decl;
    }
    return NULL;
}

static int is_known_type_name(CGContext *cg, const char *name) {
    return !strcmp(name, "String") ||
           find_decl_named(cg->program, NODE_STRUCT_DECL, name) ||
           find_decl_named(cg->program, NODE_ENUM_DECL, name) ||
           find_decl_named(cg->program, NODE_UNION_DECL, name);
}

static int enum_is_data_carrying(Node *d) {
    for (int j = 0; j < d->as.enum_decl.count; j++) {
        if (d->as.enum_decl.variant_fields[j].count > 0)
            return 1;
    }
    return 0;
}

static int enum_has_variant(CGContext *cg, const char *enum_name, const char *variant) {
    Node *decl = find_decl_named(cg->program, NODE_ENUM_DECL, enum_name);
    if (!decl) return 0;

    for (int i = 0; i < decl->as.enum_decl.count; i++) {
        if (!strcmp(decl->as.enum_decl.variants[i], variant))
            return 1;
    }
    return 0;
}

static Node *find_field_type(CGContext *cg, const char *owner_name, const char *field_name) {
    Node *decl = find_decl_named(cg->program, NODE_STRUCT_DECL, owner_name);
    NodeList *fields = NULL;

    if (decl) {
        fields = &decl->as.struct_decl.fields;
    } else {
        decl = find_decl_named(cg->program, NODE_UNION_DECL, owner_name);
        if (decl)
            fields = &decl->as.union_decl.fields;
    }

    if (!fields) return NULL;

    for (int i = 0; i < fields->count; i++) {
        Node *field = fields->items[i];
        if (!strcmp(field->as.let.name, field_name))
            return field->as.let.type;
    }
    return NULL;
}

static Node *find_free_function(CGContext *cg, const char *name) {
    for (int i = 0; i < cg->program->as.program.decls.count; i++) {
        Node *decl = cg->program->as.program.decls.items[i];
        if (decl->kind == NODE_FN_DECL && !decl->as.fn.owner_type &&
            !strcmp(decl->as.fn.name, name))
            return decl;
        if (decl->kind == NODE_EXTERN_FN && !strcmp(decl->as.extern_fn.name, name))
            return decl;
    }
    return NULL;
}

static Node *find_method(CGContext *cg, const char *owner_name, const char *method_name) {
    for (int i = 0; i < cg->program->as.program.decls.count; i++) {
        Node *decl = cg->program->as.program.decls.items[i];
        if (decl->kind != NODE_IMPL_DECL || strcmp(decl->as.impl.target, owner_name))
            continue;

        for (int j = 0; j < decl->as.impl.methods.count; j++) {
            Node *method = decl->as.impl.methods.items[j];
            if (!strcmp(method->as.fn.name, method_name))
                return method;
        }
    }
    return NULL;
}

static Node *find_enum_variant_decl(CGContext *cg, const char *enum_name, const char *variant_name, NodeList **out_fields) {
    Node *decl = find_decl_named(cg->program, NODE_ENUM_DECL, enum_name);
    if (!decl)
        return NULL;

    for (int i = 0; i < decl->as.enum_decl.count; i++) {
        if (!strcmp(decl->as.enum_decl.variants[i], variant_name)) {
            if (out_fields)
                *out_fields = &decl->as.enum_decl.variant_fields[i];
            return decl;
        }
    }

    return NULL;
}

static int method_has_receiver(Node *method) {
    if (!method || method->kind != NODE_FN_DECL || method->as.fn.params.count == 0)
        return 0;

    const char *name = method->as.fn.params.items[0]->as.let.name;
    return !strcmp(name, "self") || !strcmp(name, "Self");
}

static int expr_is_addressable(Node *expr) {
    return expr &&
           (expr->kind == NODE_IDENT  ||
            expr->kind == NODE_FIELD  ||
            expr->kind == NODE_INDEX  ||
            expr->kind == NODE_SLICE  ||
            (expr->kind == NODE_UNARY && expr->as.unary.op && expr->as.unary.op[0] == '*'));
}

static Node *infer_expr_type(CGContext *cg, Node *expr);
static Node *cg_temp_type_from_name(CGContext *cg, const char *name);
static void emit_expr(COut *o, CGContext *cg, Node *n);
static void emit_expr_typed(COut *o, CGContext *cg, Node *n, Node *expected_type);

static char *option_inner_name_text(const char *name) {
    if (!type_name_has_form(name, "Option"))
        return NULL;
    return slice_range_dup(name + 7, name + strlen(name) - 1);
}

static int result_inner_name_texts(const char *name, char **ok_name, char **err_name) {
    char *inner;
    int ok;

    *ok_name = NULL;
    *err_name = NULL;
    if (!type_name_has_form(name, "Result"))
        return 0;

    inner = slice_range_dup(name + 7, name + strlen(name) - 1);
    if (!inner)
        return 0;
    ok = split_top_level_comma(inner, ok_name, err_name);
    free(inner);
    return ok;
}

static int get_match_binding_type_nodes(CGContext *cg, Node *subject_type, const char *pattern,
                                        Node ***out_types, int *out_count) {
    Node **items = NULL;
    int count = 0;

    *out_types = NULL;
    *out_count = 0;
    if (!subject_type || subject_type->kind != NODE_TYPE_NAMED)
        return 0;

    if (type_name_has_form(subject_type->as.type_named.name, "Option")) {
        if (!strcmp(pattern, "Some")) {
            char *inner_name = option_inner_name_text(subject_type->as.type_named.name);
            items = malloc(sizeof(Node *));
            if (!items)
                return 0;
            items[0] = cg_temp_type_from_name(cg, inner_name);
            count = 1;
        } else if (!strcmp(pattern, "None")) {
            count = 0;
        } else {
            return 0;
        }
    } else if (type_name_has_form(subject_type->as.type_named.name, "Result")) {
        char *ok_name = NULL;
        char *err_name = NULL;
        if (!result_inner_name_texts(subject_type->as.type_named.name, &ok_name, &err_name))
            return 0;
        items = malloc(sizeof(Node *));
        if (!items)
            return 0;
        if (!strcmp(pattern, "Ok")) {
            items[0] = cg_temp_type_from_name(cg, ok_name);
            count = 1;
        } else if (!strcmp(pattern, "Err")) {
            items[0] = cg_temp_type_from_name(cg, err_name);
            count = 1;
        } else {
            return 0;
        }
    } else {
        NodeList *fields = NULL;
        const char *dot = strchr(pattern, '.');
        Node *enum_decl;
        char enum_name[128];
        const char *variant_name;

        if (!dot)
            return 0;
        memcpy(enum_name, pattern, (size_t)(dot - pattern));
        enum_name[dot - pattern] = '\0';
        variant_name = dot + 1;
        enum_decl = find_enum_variant_decl(cg, enum_name, variant_name, &fields);
        if (!enum_decl)
            return 0;
        count = fields ? fields->count : 0;
        if (count > 0) {
            items = malloc((size_t)count * sizeof(Node *));
            if (!items)
                return 0;
            for (int i = 0; i < count; i++)
                items[i] = fields->items[i]->as.let.type;
        }
    }

    *out_types = items;
    *out_count = count;
    return 1;
}

static Node *cg_temp_type_from_name(CGContext *cg, const char *name) {
    return name ? cg_temp_type_named(cg, name) : NULL;
}

static int expr_is_try(Node *expr) {
    return expr && expr->kind == NODE_UNARY && expr->as.unary.op &&
           !strcmp(expr->as.unary.op, "?");
}

static int expr_is_try_constructor(Node *expr, const char *ctor_name) {
    return expr &&
           expr->kind == NODE_CALL &&
           expr->as.call.callee &&
           expr->as.call.callee->kind == NODE_IDENT &&
           !strcmp(expr->as.call.callee->as.ident.name, ctor_name) &&
           expr->as.call.args.count == 1 &&
           expr_is_try(expr->as.call.args.items[0]);
}

static void make_try_temp_name(CGContext *cg, char *buf, size_t size) {
    snprintf(buf, size, "__ns_try_%d", cg->temp_counter++);
}

static void emit_try_propagation_failure(COut *o, CGContext *cg, Node *operand_type, const char *temp_name) {
    const char *ret_name;
    char c_ret_name[256];

    if (!cg->current_return_type || cg->current_return_type->kind != NODE_TYPE_NAMED ||
        !operand_type || operand_type->kind != NODE_TYPE_NAMED)
        return;

    ret_name = cg->current_return_type->as.type_named.name;
    format_option_result_type_name(ret_name, c_ret_name, sizeof(c_ret_name));

    emit_defers_range(o, cg, 0);
    emit_indent(o);
    if (type_name_has_form(operand_type->as.type_named.name, "Option")) {
        emit(o, "return ((%s){ .is_some = false });\n", c_ret_name);
    } else {
        emit(o, "return ((%s){ .is_ok = false, .as.err = %s.as.err });\n",
             c_ret_name, temp_name);
    }
}

static void emit_try_unwrap_prefix(COut *o, CGContext *cg, Node *try_expr,
                                   Node **out_operand_type, char *temp_name, size_t temp_name_size) {
    Node *operand = try_expr->as.unary.operand;
    Node *operand_type = infer_expr_type(cg, operand);

    make_try_temp_name(cg, temp_name, temp_name_size);
    emit_indent(o);
    emit_typed_name(o, operand_type, temp_name);
    emit(o, " = ");
    emit_expr(o, cg, operand);
    emit(o, ";\n");
    emit_indent(o);
    if (operand_type && operand_type->kind == NODE_TYPE_NAMED &&
        type_name_has_form(operand_type->as.type_named.name, "Option")) {
        emit(o, "if (!%s.is_some) {\n", temp_name);
    } else {
        emit(o, "if (!%s.is_ok) {\n", temp_name);
    }
    o->indent++;
    emit_try_propagation_failure(o, cg, operand_type, temp_name);
    o->indent--;
    emit_indent(o);
    emit(o, "}\n");
    *out_operand_type = operand_type;
}

static Node *try_unwrapped_type(CGContext *cg, Node *operand_type) {
    char *inner_name = NULL;
    char *ok_name = NULL;
    char *err_name = NULL;
    Node *result = NULL;

    if (!operand_type || operand_type->kind != NODE_TYPE_NAMED)
        return NULL;

    if (type_name_has_form(operand_type->as.type_named.name, "Option")) {
        inner_name = option_inner_name_text(operand_type->as.type_named.name);
        result = cg_temp_type_from_name(cg, inner_name);
        return result;
    }

    if (type_name_has_form(operand_type->as.type_named.name, "Result")) {
        if (result_inner_name_texts(operand_type->as.type_named.name, &ok_name, &err_name))
            result = cg_temp_type_from_name(cg, ok_name);
        return result;
    }

    return NULL;
}

static void emit_try_expr(COut *o, CGContext *cg, Node *try_expr) {
    Node *operand = try_expr->as.unary.operand;
    Node *operand_type = infer_expr_type(cg, operand);
    Node *inner_type = try_unwrapped_type(cg, operand_type);
    char temp_name[64];

    make_try_temp_name(cg, temp_name, sizeof(temp_name));
    emit(o, "({ ");
    emit_typed_name(o, operand_type, temp_name);
    emit(o, " = ");
    emit_expr(o, cg, operand);
    emit(o, "; ");
    if (operand_type && operand_type->kind == NODE_TYPE_NAMED &&
        type_name_has_form(operand_type->as.type_named.name, "Option")) {
        emit(o, "if (!%s.is_some) { ", temp_name);
    } else {
        emit(o, "if (!%s.is_ok) { ", temp_name);
    }
    emit_try_propagation_failure(o, cg, operand_type, temp_name);
    emit(o, "} %s.%s; })",
         temp_name,
         operand_type && operand_type->kind == NODE_TYPE_NAMED &&
         type_name_has_form(operand_type->as.type_named.name, "Option")
             ? "value"
             : "as.ok");
    (void)inner_type;
}

static void emit_string_as_view(COut *o, CGContext *cg, Node *object) {
    Node *object_type = infer_expr_type(cg, object);

    emit(o, "((NStr){ .ptr = (const uint8_t*)");
    emit_expr(o, cg, object);
    if (object_type && object_type->kind == NODE_TYPE_POINTER)
        emit(o, "->ptr, .len = ");
    else
        emit(o, ".ptr, .len = ");
    emit_expr(o, cg, object);
    if (object_type && object_type->kind == NODE_TYPE_POINTER)
        emit(o, "->len })");
    else
        emit(o, ".len })");
}

static void emit_string_from_expr(COut *o, CGContext *cg, Node *arg) {
    char src_name[64];
    char out_name[64];

    make_try_temp_name(cg, src_name, sizeof(src_name));
    make_try_temp_name(cg, out_name, sizeof(out_name));

    emit(o, "({ ");
    emit(o, "NStr %s = ", src_name);
    emit_expr_typed(o, cg, arg, cg_temp_type_named(cg, "str"));
    emit(o, "; ");
    emit(o, "NString %s; ", out_name);
    emit(o, "%s.len = %s.len; ", out_name, src_name);
    emit(o, "%s.cap = %s.len; ", out_name, src_name);
    emit(o, "%s.ptr = (uint8_t*)malloc(%s.len + 1); ", out_name, src_name);
    emit(o, "if (%s.ptr) { memcpy(%s.ptr, %s.ptr, %s.len); %s.ptr[%s.len] = 0; } ",
         out_name, out_name, src_name, src_name, out_name, src_name);
    emit(o, "%s; })", out_name);
}

static void emit_string_free_expr(COut *o, CGContext *cg, Node *object) {
    Node *object_type = infer_expr_type(cg, object);
    char ptr_name[64];

    make_try_temp_name(cg, ptr_name, sizeof(ptr_name));
    emit(o, "({ NString *%s = ", ptr_name);
    if (object_type && object_type->kind == NODE_TYPE_POINTER) {
        emit_expr(o, cg, object);
    } else {
        emit(o, "&(");
        emit_expr(o, cg, object);
        emit(o, ")");
    }
    emit(o, "; if (%s->ptr) free(%s->ptr); %s->ptr = NULL; %s->len = 0; %s->cap = 0; })",
         ptr_name, ptr_name, ptr_name, ptr_name, ptr_name);
}

static void emit_slice_expr(COut *o, CGContext *cg, Node *slice_expr) {
    Node *object = slice_expr->as.slice_expr.object;
    Node *object_type = infer_expr_type(cg, object);
    char obj_name[64];
    char start_name[64];
    char end_name[64];

    make_try_temp_name(cg, obj_name, sizeof(obj_name));
    make_try_temp_name(cg, start_name, sizeof(start_name));
    make_try_temp_name(cg, end_name, sizeof(end_name));

    emit(o, "({ ");
    if (object_type && object_type->kind == NODE_TYPE_ARRAY) {
        Node *result_type = cg_temp_type_array(cg, object_type->as.type_array.elem, -1);
        char slice_name[256];

        format_slice_type_name(result_type, slice_name, sizeof(slice_name));
        if (object_type->as.type_array.length < 0) {
            emit_type_left(o, result_type);
            emit(o, " %s = ", obj_name);
            emit_expr(o, cg, object);
            emit(o, "; ");
            emit(o, "size_t %s = ", start_name);
            if (slice_expr->as.slice_expr.start)
                emit_expr(o, cg, slice_expr->as.slice_expr.start);
            else
                emit(o, "0");
            emit(o, "; ");
            emit(o, "size_t %s = ", end_name);
            if (slice_expr->as.slice_expr.end)
                emit_expr(o, cg, slice_expr->as.slice_expr.end);
            else
                emit(o, "%s.len", obj_name);
            emit(o, "; ");
            emit(o, "((%s){ .ptr = %s.ptr + %s, .len = %s - %s }); })",
                 slice_name, obj_name, start_name, end_name, start_name);
        } else {
            emit(o, "size_t %s = ", start_name);
            if (slice_expr->as.slice_expr.start)
                emit_expr(o, cg, slice_expr->as.slice_expr.start);
            else
                emit(o, "0");
            emit(o, "; ");
            emit(o, "size_t %s = ", end_name);
            if (slice_expr->as.slice_expr.end)
                emit_expr(o, cg, slice_expr->as.slice_expr.end);
            else
                emit(o, "%d", object_type->as.type_array.length);
            emit(o, "; ");
            emit(o, "((%s){ .ptr = ", slice_name);
            emit_expr(o, cg, object);
            emit(o, " + %s, .len = %s - %s }); })", start_name, end_name, start_name);
        }
        return;
    }

    if (object_type && object_type->kind == NODE_TYPE_NAMED &&
        !strcmp(object_type->as.type_named.name, "String")) {
        emit(o, "NString %s = ", obj_name);
        emit_expr(o, cg, object);
        emit(o, "; ");
        emit(o, "size_t %s = ", start_name);
        if (slice_expr->as.slice_expr.start)
            emit_expr(o, cg, slice_expr->as.slice_expr.start);
        else
            emit(o, "0");
        emit(o, "; ");
        emit(o, "size_t %s = ", end_name);
        if (slice_expr->as.slice_expr.end)
            emit_expr(o, cg, slice_expr->as.slice_expr.end);
        else
            emit(o, "%s.len", obj_name);
        emit(o, "; ");
        emit(o, "((NStr){ .ptr = (const uint8_t*)(%s.ptr + %s), .len = %s - %s }); })",
             obj_name, start_name, end_name, start_name);
        return;
    }

    emit(o, "NStr %s = ", obj_name);
    emit_expr_typed(o, cg, object, cg_temp_type_named(cg, "str"));
    emit(o, "; ");
    emit(o, "size_t %s = ", start_name);
    if (slice_expr->as.slice_expr.start)
        emit_expr(o, cg, slice_expr->as.slice_expr.start);
    else
        emit(o, "0");
    emit(o, "; ");
    emit(o, "size_t %s = ", end_name);
    if (slice_expr->as.slice_expr.end)
        emit_expr(o, cg, slice_expr->as.slice_expr.end);
    else
        emit(o, "%s.len", obj_name);
    emit(o, "; ");
    emit(o, "((NStr){ .ptr = %s.ptr + %s, .len = %s - %s }); })",
         obj_name, start_name, end_name, start_name);
}

static void emit_match_condition_name(COut *o, CGContext *cg, const char *subject_name,
                                      Node *subject_type, const char *pattern) {
    const char *dot = strchr(pattern, '.');

    (void)cg;
    (void)subject_type;

    if (!strcmp(pattern, "Some")) {
        emit(o, "%s.is_some", subject_name);
        return;
    }

    if (!strcmp(pattern, "None")) {
        emit(o, "!%s.is_some", subject_name);
        return;
    }

    if (!strcmp(pattern, "Ok")) {
        emit(o, "%s.is_ok", subject_name);
        return;
    }

    if (!strcmp(pattern, "Err")) {
        emit(o, "!%s.is_ok", subject_name);
        return;
    }

    if (!dot) {
        emit(o, "0");
        return;
    }

    {
        char enum_name[128];
        int elen = (int)(dot - pattern);
        Node *decl;
        int is_data;

        if (elen >= 127)
            elen = 127;
        memcpy(enum_name, pattern, (size_t)elen);
        enum_name[elen] = '\0';

        decl = find_decl_named(cg->program, NODE_ENUM_DECL, enum_name);
        is_data = decl && enum_is_data_carrying(decl);
        if (is_data)
            emit(o, "%s.tag == %s_%s", subject_name, enum_name, dot + 1);
        else
            emit(o, "%s == %s_%s", subject_name, enum_name, dot + 1);
    }
}

static Node *infer_match_arm_type(CGContext *cg, Node *match, int arm_index) {
    Node *subject_type;
    Node **binding_types = NULL;
    int binding_count = 0;
    int declared_binding_count;
    Node *result;

    if (!match || arm_index < 0 || arm_index >= match->as.match.count)
        return NULL;

    subject_type = infer_expr_type(cg, match->as.match.subject);
    if (!subject_type)
        return NULL;

    if (!get_match_binding_type_nodes(cg, subject_type, match->as.match.patterns[arm_index],
                                      &binding_types, &binding_count)) {
        return infer_expr_type(cg, match->as.match.values[arm_index]);
    }

    declared_binding_count = match->as.match.binding_counts[arm_index];
    if (declared_binding_count > 0) {
        cg_push_scope(cg);
        for (int i = 0; i < declared_binding_count; i++)
            cg_define(cg, match->as.match.binding_names[arm_index][i], binding_types[i]);
    }
    result = infer_expr_type(cg, match->as.match.values[arm_index]);
    if (declared_binding_count > 0)
        cg_pop_scope(cg);
    free(binding_types);
    return result;
}

static void emit_match_binding_value(COut *o, CGContext *cg, const char *subject_name,
                                     Node *subject_type, const char *pattern,
                                     int binding_index) {
    if (!subject_type || subject_type->kind != NODE_TYPE_NAMED)
        return;

    if (type_name_has_form(subject_type->as.type_named.name, "Option")) {
        emit(o, "%s.value", subject_name);
        return;
    }

    if (type_name_has_form(subject_type->as.type_named.name, "Result")) {
        if (!strcmp(pattern, "Ok"))
            emit(o, "%s.as.ok", subject_name);
        else
            emit(o, "%s.as.err", subject_name);
        return;
    }

    {
        const char *dot = strchr(pattern, '.');
        NodeList *fields = NULL;
        char enum_name[128];
        const char *variant_name;

        if (!dot)
            return;
        memcpy(enum_name, pattern, (size_t)(dot - pattern));
        enum_name[dot - pattern] = '\0';
        variant_name = dot + 1;
        if (!find_enum_variant_decl(cg, enum_name, variant_name, &fields) || !fields ||
            binding_index < 0 || binding_index >= fields->count) {
            return;
        }
        emit(o, "%s.as.%s.%s", subject_name, variant_name, fields->items[binding_index]->as.let.name);
    }
}

static void emit_match_expr(COut *o, CGContext *cg, Node *match, Node *expected_type) {
    Node *subject_type;
    Node *result_type;
    char subject_name[64];
    char result_name[64];

    if (match->as.match.count == 0) {
        emit(o, "0");
        return;
    }

    subject_type = infer_expr_type(cg, match->as.match.subject);
    result_type = expected_type ? expected_type : infer_match_arm_type(cg, match, 0);
    make_try_temp_name(cg, subject_name, sizeof(subject_name));
    make_try_temp_name(cg, result_name, sizeof(result_name));

    emit(o, "({\n");
    o->indent++;
    emit_indent(o);
    emit_typed_name(o, subject_type, subject_name);
    emit(o, " = ");
    emit_expr(o, cg, match->as.match.subject);
    emit(o, ";\n");
    emit_indent(o);
    emit_typed_name(o, result_type ? result_type : cg_temp_type_named(cg, "i32"), result_name);
    emit(o, " = {0};\n");

    for (int i = 0; i < match->as.match.count; i++) {
        const char *pattern = match->as.match.patterns[i];
        Node **binding_types = NULL;
        int binding_count = 0;
        int declared_binding_count = match->as.match.binding_counts[i];

        emit_indent(o);
        if (!strcmp(pattern, "_")) {
            if (i == 0) emit(o, "{\n");
            else emit(o, "else {\n");
        } else {
            if (i == 0) emit(o, "if (");
            else emit(o, "else if (");
            emit_match_condition_name(o, cg, subject_name, subject_type, pattern);
            emit(o, ") {\n");
        }

        o->indent++;
        if (get_match_binding_type_nodes(cg, subject_type, pattern, &binding_types, &binding_count) &&
            declared_binding_count > 0) {
            cg_push_scope(cg);
            for (int j = 0; j < declared_binding_count; j++) {
                emit_indent(o);
                emit_typed_name(o, binding_types[j], match->as.match.binding_names[i][j]);
                emit(o, " = ");
                emit_match_binding_value(o, cg, subject_name, subject_type, pattern, j);
                emit(o, ";\n");
                cg_define(cg, match->as.match.binding_names[i][j], binding_types[j]);
            }
        }
        emit_indent(o);
        emit(o, "%s = ", result_name);
        emit_expr_typed(o, cg, match->as.match.values[i], result_type);
        emit(o, ";\n");
        if (declared_binding_count > 0)
            cg_pop_scope(cg);
        free(binding_types);
        o->indent--;
        emit_indent(o);
        emit(o, "}\n");
    }

    emit_indent(o);
    emit(o, "%s;\n", result_name);
    o->indent--;
    emit_indent(o);
    emit(o, "})");
}

static int is_enum_variant_constructor_call(CGContext *cg, Node *call, Node **out_enum_decl, NodeList **out_fields) {
    Node *callee = call->as.call.callee;
    Node *object;

    if (!callee || callee->kind != NODE_FIELD)
        return 0;

    object = callee->as.field.object;
    if (!object || object->kind != NODE_IDENT)
        return 0;

    *out_enum_decl = find_enum_variant_decl(cg, object->as.ident.name, callee->as.field.field, out_fields);
    return *out_enum_decl != NULL;
}

static Node *resolve_call_return_type(CGContext *cg, Node *call) {
    Node *callee = call->as.call.callee;

    if (callee->kind == NODE_IDENT) {
        Node *fn = find_free_function(cg, callee->as.ident.name);
        if (!fn) return NULL;
        return fn->kind == NODE_EXTERN_FN ? fn->as.extern_fn.ret_type : fn->as.fn.ret_type;
    }

    if (callee->kind == NODE_FIELD) {
        Node *object = callee->as.field.object;

        if (object->kind == NODE_IDENT && is_known_type_name(cg, object->as.ident.name)) {
            Node *method = find_method(cg, object->as.ident.name, callee->as.field.field);
            if (method && !method_has_receiver(method))
                return method->as.fn.ret_type;
        }

        Node *object_type = infer_expr_type(cg, object);
        if (!object_type) return NULL;

        const char *owner_name = type_named_name(object_type);
        if (!owner_name && object_type->kind == NODE_TYPE_POINTER)
            owner_name = type_named_name(object_type->as.type_ptr.inner);

        if (!owner_name) return NULL;

        Node *method = find_method(cg, owner_name, callee->as.field.field);
        if (method && method_has_receiver(method))
            return method->as.fn.ret_type;
    }

    return NULL;
}

static Node *infer_expr_type(CGContext *cg, Node *expr) {
    if (!expr) return NULL;

    switch (expr->kind) {
        case NODE_LIT_INT:
            return cg_temp_type_named(cg, "i32");
        case NODE_LIT_CHAR:
            return cg_temp_type_named(cg, "char");
        case NODE_LIT_FLOAT:
            return cg_temp_type_named(cg, "f64");
        case NODE_LIT_STRING:
            return cg_temp_type_named(cg, "str");
        case NODE_LIT_BOOL:
            return cg_temp_type_named(cg, "bool");
        case NODE_IDENT: {
            Node *type = cg_lookup(cg, expr->as.ident.name);
            if (type) return type;
            if (is_known_type_name(cg, expr->as.ident.name))
                return cg_temp_type_named(cg, expr->as.ident.name);
            return NULL;
        }
        case NODE_GROUP:
            return infer_expr_type(cg, expr->as.group.expr);
        case NODE_UNARY: {
            if (!strcmp(expr->as.unary.op, "?")) {
                Node *operand_type = infer_expr_type(cg, expr->as.unary.operand);
                return try_unwrapped_type(cg, operand_type);
            }
            if (!strcmp(expr->as.unary.op, "&")) {
                Node *inner = infer_expr_type(cg, expr->as.unary.operand);
                return inner ? cg_temp_type_pointer(cg, inner, 0) : NULL;
            }
            if (!strcmp(expr->as.unary.op, "*")) {
                Node *inner = infer_expr_type(cg, expr->as.unary.operand);
                return inner ? type_pointer_inner(inner) : NULL;
            }
            return infer_expr_type(cg, expr->as.unary.operand);
        }
        case NODE_CAST:
            return expr->as.cast.type;
        case NODE_STRUCT_LIT:
            return cg_temp_type_named(cg, expr->as.struct_lit.type_name);
        case NODE_CALL:
            if (resolve_call_return_type(cg, expr) == NULL) {
                Node *enum_decl = NULL;
                NodeList *fields = NULL;
                Node *callee = expr->as.call.callee;

                if (callee && callee->kind == NODE_FIELD &&
                    callee->as.field.object &&
                    callee->as.field.object->kind == NODE_IDENT &&
                    !strcmp(callee->as.field.object->as.ident.name, "String") &&
                    !strcmp(callee->as.field.field, "from")) {
                    return cg_temp_type_named(cg, "String");
                }

                if (callee && callee->kind == NODE_FIELD) {
                    Node *object = callee->as.field.object;
                    Node *object_type = infer_expr_type(cg, object);

                    if (object_type && object_type->kind == NODE_TYPE_NAMED &&
                        !strcmp(object_type->as.type_named.name, "String")) {
                        if (!strcmp(callee->as.field.field, "as_str"))
                            return cg_temp_type_named(cg, "str");
                        if (!strcmp(callee->as.field.field, "free"))
                            return cg_temp_type_named(cg, "void");
                    }

                    if (object_type && object_type->kind == NODE_TYPE_POINTER &&
                        object_type->as.type_ptr.inner &&
                        object_type->as.type_ptr.inner->kind == NODE_TYPE_NAMED &&
                        !strcmp(object_type->as.type_ptr.inner->as.type_named.name, "String")) {
                        if (!strcmp(callee->as.field.field, "as_str"))
                            return cg_temp_type_named(cg, "str");
                        if (!strcmp(callee->as.field.field, "free"))
                            return cg_temp_type_named(cg, "void");
                    }
                }

                if (is_enum_variant_constructor_call(cg, expr, &enum_decl, &fields))
                    return cg_temp_type_named(cg, enum_decl->as.enum_decl.name);
            }
            return resolve_call_return_type(cg, expr);
        case NODE_FIELD: {
            Node *object = expr->as.field.object;
            if (object->kind == NODE_IDENT &&
                enum_has_variant(cg, object->as.ident.name, expr->as.field.field)) {
                return cg_temp_type_named(cg, object->as.ident.name);
            }

            Node *object_type = infer_expr_type(cg, object);
            if (!object_type) return NULL;

            if (object_type->kind == NODE_TYPE_ARRAY &&
                object_type->as.type_array.length < 0) {
                if (!strcmp(expr->as.field.field, "len"))
                    return cg_temp_type_named(cg, "usize");
                if (!strcmp(expr->as.field.field, "ptr"))
                    return cg_temp_type_pointer(cg, object_type->as.type_array.elem, 0);
                return NULL;
            }

            if (object_type->kind == NODE_TYPE_NAMED &&
                !strcmp(object_type->as.type_named.name, "str")) {
                if (!strcmp(expr->as.field.field, "len"))
                    return cg_temp_type_named(cg, "usize");
                if (!strcmp(expr->as.field.field, "ptr"))
                    return cg_temp_type_pointer(cg, cg_temp_type_named(cg, "u8"), 1);
                return NULL;
            }

            if (object_type->kind == NODE_TYPE_NAMED &&
                !strcmp(object_type->as.type_named.name, "String")) {
                if (!strcmp(expr->as.field.field, "len"))
                    return cg_temp_type_named(cg, "usize");
                if (!strcmp(expr->as.field.field, "cap"))
                    return cg_temp_type_named(cg, "usize");
                if (!strcmp(expr->as.field.field, "ptr"))
                    return cg_temp_type_pointer(cg, cg_temp_type_named(cg, "u8"), 0);
                return NULL;
            }

            const char *owner_name = type_named_name(object_type);
            if (!owner_name && object_type->kind == NODE_TYPE_POINTER)
                owner_name = type_named_name(object_type->as.type_ptr.inner);

            return owner_name ? find_field_type(cg, owner_name, expr->as.field.field) : NULL;
        }
        case NODE_MATCH:
            if (expr->as.match.count > 0)
                return infer_match_arm_type(cg, expr, 0);
            return NULL;
        case NODE_INDEX: {
            Node *obj_type = infer_expr_type(cg, expr->as.index_expr.object);
            if (!obj_type) return NULL;
            if (obj_type->kind == NODE_TYPE_ARRAY)
                return obj_type->as.type_array.elem;
            if (obj_type->kind == NODE_TYPE_POINTER)
                return obj_type->as.type_ptr.inner;
            if (obj_type->kind == NODE_TYPE_NAMED &&
                !strcmp(obj_type->as.type_named.name, "str")) {
                return cg_temp_type_named(cg, "u8");
            }
            if (obj_type->kind == NODE_TYPE_NAMED &&
                !strcmp(obj_type->as.type_named.name, "String")) {
                return cg_temp_type_named(cg, "u8");
            }
            return NULL;
        }
        case NODE_SLICE: {
            Node *obj_type = infer_expr_type(cg, expr->as.slice_expr.object);
            if (!obj_type)
                return NULL;
            if (obj_type->kind == NODE_TYPE_ARRAY)
                return cg_temp_type_array(cg, obj_type->as.type_array.elem, -1);
            if (obj_type->kind == NODE_TYPE_NAMED &&
                (!strcmp(obj_type->as.type_named.name, "str") ||
                 !strcmp(obj_type->as.type_named.name, "String"))) {
                return cg_temp_type_named(cg, "str");
            }
            return NULL;
        }
        default:
            return NULL;
    }
}

typedef enum {
    RECEIVER_NONE,
    RECEIVER_DIRECT,
    RECEIVER_ADDRESS_OF,
} ReceiverStrategy;

typedef struct {
    Node            *method;
    ReceiverStrategy receiver;
} MethodResolution;

static MethodResolution resolve_method_call(CGContext *cg, Node *call) {
    MethodResolution result = {0};
    Node *callee = call->as.call.callee;
    if (!callee || callee->kind != NODE_FIELD)
        return result;

    Node *object = callee->as.field.object;

    if (object->kind == NODE_IDENT && is_known_type_name(cg, object->as.ident.name)) {
        Node *method = find_method(cg, object->as.ident.name, callee->as.field.field);
        if (method) {
            result.method = method;
            /* receiver is either absent (static) or passed explicitly as first arg */
            result.receiver = RECEIVER_NONE;
        }
        return result;
    }

    Node *object_type = infer_expr_type(cg, object);
    if (!object_type) return result;

    const char *owner_name = type_named_name(object_type);
    if (owner_name) {
        Node *method = find_method(cg, owner_name, callee->as.field.field);
        if (method && method_has_receiver(method) && expr_is_addressable(object)) {
            result.method = method;
            result.receiver = RECEIVER_ADDRESS_OF;
        }
        return result;
    }

    if (object_type->kind == NODE_TYPE_POINTER) {
        const char *inner_name = type_named_name(object_type->as.type_ptr.inner);
        Node *method = inner_name ? find_method(cg, inner_name, callee->as.field.field) : NULL;
        if (method && method_has_receiver(method)) {
            result.method = method;
            result.receiver = RECEIVER_DIRECT;
        }
    }

    return result;
}

static void emit_expr(COut *o, CGContext *cg, Node *n) {
    emit_expr_typed(o, cg, n, NULL);
}

static int emit_contextual_constructor(COut *o, CGContext *cg, Node *n, Node *expected_type) {
    const char *type_name;
    char c_name[256];

    if (!n || !expected_type || expected_type->kind != NODE_TYPE_NAMED)
        return 0;

    type_name = expected_type->as.type_named.name;
    if (!type_name)
        return 0;

    format_option_result_type_name(type_name, c_name, sizeof(c_name));

    if (type_name_has_form(type_name, "Option")) {
        if (n->kind == NODE_IDENT && !strcmp(n->as.ident.name, "None")) {
            emit(o, "((%s){ .is_some = false })", c_name);
            return 1;
        }

        if (n->kind == NODE_CALL && n->as.call.callee &&
            n->as.call.callee->kind == NODE_IDENT &&
            !strcmp(n->as.call.callee->as.ident.name, "Some") &&
            n->as.call.args.count == 1) {
            emit(o, "((%s){ .is_some = true, .value = ", c_name);
            emit_expr(o, cg, n->as.call.args.items[0]);
            emit(o, " })");
            return 1;
        }
    }

    if (type_name_has_form(type_name, "Result") &&
        n->kind == NODE_CALL && n->as.call.callee &&
        n->as.call.callee->kind == NODE_IDENT &&
        n->as.call.args.count == 1) {
        const char *ctor = n->as.call.callee->as.ident.name;
        if (!strcmp(ctor, "Ok")) {
            emit(o, "((%s){ .is_ok = true, .as.ok = ", c_name);
            emit_expr(o, cg, n->as.call.args.items[0]);
            emit(o, " })");
            return 1;
        }
        if (!strcmp(ctor, "Err")) {
            emit(o, "((%s){ .is_ok = false, .as.err = ", c_name);
            emit_expr(o, cg, n->as.call.args.items[0]);
            emit(o, " })");
            return 1;
        }
    }

    return 0;
}

static void emit_expr_typed(COut *o, CGContext *cg, Node *n, Node *expected_type) {
    if (!n) return;
    if (emit_contextual_constructor(o, cg, n, expected_type))
        return;

    switch (n->kind) {
        case NODE_LIT_INT:
            emit(o, "%lld", n->as.lit_int.value);
            break;
        case NODE_LIT_CHAR:
            emit(o, "%lld", n->as.lit_int.value);
            break;
        case NODE_LIT_FLOAT:
            emit(o, "%g", n->as.lit_float.value);
            break;
        case NODE_LIT_STRING:
            if (expected_type && expected_type->kind == NODE_TYPE_NAMED &&
                !strcmp(expected_type->as.type_named.name, "str")) {
                emit(o, "((NStr){ .ptr = (const uint8_t*)\"");
                emit(o, "%s", n->as.lit_str.value);
                emit(o, "\", .len = %zu })", strlen(n->as.lit_str.value));
            } else {
                emit(o, "\"%s\"", n->as.lit_str.value);
            }
            break;
        case NODE_LIT_BOOL:
            emit(o, "%s", n->as.lit_int.value ? "true" : "false");
            break;
        case NODE_LIT_NULL:
            emit(o, "NULL");
            break;
        case NODE_IDENT:
            emit(o, "%s", n->as.ident.name);
            break;
        case NODE_BINARY:
            emit(o, "(");
            emit_expr(o, cg, n->as.binary.left);
            emit(o, " %s ", n->as.binary.op);
            emit_expr(o, cg, n->as.binary.right);
            emit(o, ")");
            break;
        case NODE_UNARY:
            if (!strcmp(n->as.unary.op, "?")) {
                emit_try_expr(o, cg, n);
                break;
            }
            /* bitwise NOT: ~x */
            if (!strcmp(n->as.unary.op, "~")) {
                emit(o, "(~");
                emit_expr(o, cg, n->as.unary.operand);
                emit(o, ")");
                break;
            }
            /* all other prefix unary ops */
            emit(o, "(%s", n->as.unary.op);
            emit_expr(o, cg, n->as.unary.operand);
            emit(o, ")");
            break;
        case NODE_GROUP:
            emit(o, "(");
            emit_expr(o, cg, n->as.group.expr);
            emit(o, ")");
            break;
        case NODE_CALL: {
            Node *enum_decl = NULL;
            NodeList *fields = NULL;
            MethodResolution method = resolve_method_call(cg, n);

            /* ── built-in I/O call emission ── */
            if (n->as.call.callee && n->as.call.callee->kind == NODE_IDENT) {
                const char *nm = n->as.call.callee->as.ident.name;
                Node *a0 = n->as.call.args.count > 0 ? n->as.call.args.items[0] : NULL;
#define _IO_ARG0  do { emit(o, "("); emit_expr(o, cg, a0); emit(o, ")"); } while(0)
                /* stdout — no-newline */
                if (!strcmp(nm, "print")) {
                    emit(o, "fputs("); emit_expr(o, cg, a0); emit(o, ", stdout)");
                    break;
                }
                /* stdout — with newline (reuse puts) */
                if (!strcmp(nm, "println")) {
                    emit(o, "puts"); _IO_ARG0;
                    break;
                }
                /* stderr — no-newline */
                if (!strcmp(nm, "eprint")) {
                    emit(o, "fputs("); emit_expr(o, cg, a0); emit(o, ", stderr)");
                    break;
                }
                /* stderr — with newline */
                if (!strcmp(nm, "eprintln")) {
                    emit(o, "(fputs("); emit_expr(o, cg, a0);
                    emit(o, ", stderr), fputc('\\n', stderr))");
                    break;
                }
                /* typed print */
                if (!strcmp(nm, "print_int"))   { emit(o, "ns_io_print_i32");  _IO_ARG0; break; }
                if (!strcmp(nm, "print_long"))  { emit(o, "ns_io_print_i64");  _IO_ARG0; break; }
                if (!strcmp(nm, "print_uint"))  { emit(o, "ns_io_print_u32");  _IO_ARG0; break; }
                if (!strcmp(nm, "print_ulong")) { emit(o, "ns_io_print_u64");  _IO_ARG0; break; }
                if (!strcmp(nm, "print_f32"))   { emit(o, "ns_io_print_f64");  _IO_ARG0; break; }
                if (!strcmp(nm, "print_f64"))   { emit(o, "ns_io_print_f64");  _IO_ARG0; break; }
                if (!strcmp(nm, "print_bool"))  { emit(o, "ns_io_print_bool"); _IO_ARG0; break; }
                if (!strcmp(nm, "print_char"))  { emit(o, "putchar");          _IO_ARG0; break; }
                /* flush stdout */
                if (!strcmp(nm, "flush")) {
                    emit(o, "fflush(stdout)");
                    break;
                }
                /* stdin — read line, no prompt (like Python input()) */
                if (!strcmp(nm, "input")) {
                    if (a0) {
                        /* input("prompt") → print prompt then read */
                        emit(o, "(fputs("); emit_expr(o, cg, a0);
                        emit(o, ", stdout), fflush(stdout), ns_io_readln())");
                    } else {
                        emit(o, "ns_io_readln()");
                    }
                    break;
                }
                /* typed read */
                if (!strcmp(nm, "read_int"))  { emit(o, "ns_io_read_i32()");  break; }
                if (!strcmp(nm, "read_long")) { emit(o, "ns_io_read_i64()");  break; }
                if (!strcmp(nm, "read_uint")) { emit(o, "ns_io_read_u32()");  break; }
                if (!strcmp(nm, "read_f64"))  { emit(o, "ns_io_read_f64()");  break; }
#undef _IO_ARG0
            }
            /* stream method calls: stdout.flush(), stderr.flush(),
               stdout.write(s), stderr.write(s), stdin.read_line(),
               stdin.read_int(), stdin.read_f64()                       */
            if (n->as.call.callee && n->as.call.callee->kind == NODE_FIELD) {
                Node *obj   = n->as.call.callee->as.field.object;
                const char *fld = n->as.call.callee->as.field.field;
                if (obj && obj->kind == NODE_IDENT) {
                    const char *on = obj->as.ident.name;
                    Node *a0 = n->as.call.args.count > 0 ? n->as.call.args.items[0] : NULL;
                    if (!strcmp(on, "stdout") && !strcmp(fld, "flush"))
                        { emit(o, "fflush(stdout)"); break; }
                    if (!strcmp(on, "stderr") && !strcmp(fld, "flush"))
                        { emit(o, "fflush(stderr)"); break; }
                    if (!strcmp(on, "stdout") && !strcmp(fld, "write") && a0)
                        { emit(o, "fputs("); emit_expr(o, cg, a0); emit(o, ", stdout)"); break; }
                    if (!strcmp(on, "stderr") && !strcmp(fld, "write") && a0)
                        { emit(o, "fputs("); emit_expr(o, cg, a0); emit(o, ", stderr)"); break; }
                    if (!strcmp(on, "stdin") && !strcmp(fld, "read_line"))
                        { emit(o, "ns_io_readln()"); break; }
                    if (!strcmp(on, "stdin") && !strcmp(fld, "read_int"))
                        { emit(o, "ns_io_read_i32()"); break; }
                    if (!strcmp(on, "stdin") && !strcmp(fld, "read_f64"))
                        { emit(o, "ns_io_read_f64()"); break; }
                }
            }

            if (n->as.call.callee &&
                n->as.call.callee->kind == NODE_FIELD &&
                n->as.call.callee->as.field.object &&
                n->as.call.callee->as.field.object->kind == NODE_IDENT &&
                !strcmp(n->as.call.callee->as.field.object->as.ident.name, "String") &&
                !strcmp(n->as.call.callee->as.field.field, "from") &&
                n->as.call.args.count == 1) {
                emit_string_from_expr(o, cg, n->as.call.args.items[0]);
                break;
            }
            if (n->as.call.callee && n->as.call.callee->kind == NODE_FIELD) {
                Node *object = n->as.call.callee->as.field.object;
                Node *object_type = infer_expr_type(cg, object);
                int is_string_receiver = object_type &&
                    ((object_type->kind == NODE_TYPE_NAMED &&
                      !strcmp(object_type->as.type_named.name, "String")) ||
                     (object_type->kind == NODE_TYPE_POINTER &&
                      object_type->as.type_ptr.inner &&
                      object_type->as.type_ptr.inner->kind == NODE_TYPE_NAMED &&
                      !strcmp(object_type->as.type_ptr.inner->as.type_named.name, "String")));

                if (is_string_receiver &&
                    !strcmp(n->as.call.callee->as.field.field, "as_str") &&
                    n->as.call.args.count == 0) {
                    emit_string_as_view(o, cg, object);
                    break;
                }

                if (is_string_receiver &&
                    !strcmp(n->as.call.callee->as.field.field, "free") &&
                    n->as.call.args.count == 0) {
                    emit_string_free_expr(o, cg, object);
                    break;
                }
            }
            if (is_enum_variant_constructor_call(cg, n, &enum_decl, &fields)) {
                const char *enum_name = enum_decl->as.enum_decl.name;
                const char *variant_name = n->as.call.callee->as.field.field;
                emit(o, "((%s){ .tag = %s_%s", enum_name, enum_name, variant_name);
                if (fields && fields->count > 0) {
                    emit(o, ", .as.%s = { ", variant_name);
                    for (int i = 0; i < fields->count; i++) {
                        if (i) emit(o, ", ");
                        emit(o, ".%s = ", fields->items[i]->as.let.name);
                        emit_expr(o, cg, n->as.call.args.items[i]);
                    }
                    emit(o, " }");
                }
                emit(o, " })");
                break;
            }
            if (method.method) {
                char cname[256];
                make_c_name(cname, sizeof(cname),
                            method.method->as.fn.owner_type,
                            method.method->as.fn.name);
                emit(o, "%s(", cname);
                if (method.receiver == RECEIVER_ADDRESS_OF) {
                    emit(o, "&(");
                    emit_expr(o, cg, n->as.call.callee->as.field.object);
                    emit(o, ")");
                } else if (method.receiver == RECEIVER_DIRECT) {
                    emit_expr(o, cg, n->as.call.callee->as.field.object);
                }
                for (int i = 0; i < n->as.call.args.count; i++) {
                    Node *param_type = NULL;
                    if (i || method.receiver != RECEIVER_NONE) emit(o, ", ");
                    if (method.method) {
                        int param_index = i + (method.receiver != RECEIVER_NONE ? 1 : 0);
                        if (param_index < method.method->as.fn.params.count)
                            param_type = method.method->as.fn.params.items[param_index]->as.let.type;
                    }
                    emit_expr_typed(o, cg, n->as.call.args.items[i], param_type);
                }
                emit(o, ")");
                break;
            }

            emit_expr(o, cg, n->as.call.callee);
            emit(o, "(");
            for (int i = 0; i < n->as.call.args.count; i++) {
                Node *fn = NULL;
                Node *param_type = NULL;
                if (i) emit(o, ", ");
                if (n->as.call.callee->kind == NODE_IDENT)
                    fn = find_free_function(cg, n->as.call.callee->as.ident.name);
                if (fn) {
                    NodeList *params = fn->kind == NODE_EXTERN_FN ? &fn->as.extern_fn.params : &fn->as.fn.params;
                    if (i < params->count)
                        param_type = params->items[i]->as.let.type;
                }
                emit_expr_typed(o, cg, n->as.call.args.items[i], param_type);
            }
            emit(o, ")");
            break;
        }
        case NODE_FIELD: {
            Node *obj = n->as.field.object;

            if (obj->kind == NODE_IDENT &&
                enum_has_variant(cg, obj->as.ident.name, n->as.field.field)) {
                Node *ed = find_decl_named(cg->program, NODE_ENUM_DECL, obj->as.ident.name);
                if (ed && enum_is_data_carrying(ed)) {
                    /* no-payload variant of a tagged-union enum → emit struct literal */
                    emit(o, "((%s){ .tag = %s_%s })",
                         obj->as.ident.name, obj->as.ident.name, n->as.field.field);
                } else {
                    emit(o, "%s_%s", obj->as.ident.name, n->as.field.field);
                }
                break;
            }

            Node *obj_type = infer_expr_type(cg, obj);
            emit_expr(o, cg, obj);
            if (obj_type && obj_type->kind == NODE_TYPE_POINTER)
                emit(o, "->%s", n->as.field.field);
            else
                emit(o, ".%s", n->as.field.field);
            break;
        }
        case NODE_CAST:
            emit(o, "((");
            emit_type_left(o, n->as.cast.type);
            emit(o, ")(");
            emit_expr(o, cg, n->as.cast.expr);
            emit(o, "))");
            break;
        case NODE_ASSIGN:
            emit_expr(o, cg, n->as.assign.target);
            if (n->as.assign.op)
                emit(o, " %s= ", n->as.assign.op);
            else
                emit(o, " = ");
            emit_expr(o, cg, n->as.assign.value);
            break;
        case NODE_INDEX: {
            Node *obj_type = infer_expr_type(cg, n->as.index_expr.object);
            if (obj_type && obj_type->kind == NODE_TYPE_ARRAY &&
                obj_type->as.type_array.length < 0) {
                /* slice: obj.ptr[i] */
                emit_expr(o, cg, n->as.index_expr.object);
                emit(o, ".ptr[");
                emit_expr(o, cg, n->as.index_expr.index);
                emit(o, "]");
            } else if (obj_type && obj_type->kind == NODE_TYPE_NAMED &&
                       !strcmp(obj_type->as.type_named.name, "str")) {
                emit_expr(o, cg, n->as.index_expr.object);
                emit(o, ".ptr[");
                emit_expr(o, cg, n->as.index_expr.index);
                emit(o, "]");
            } else if (obj_type && obj_type->kind == NODE_TYPE_NAMED &&
                       !strcmp(obj_type->as.type_named.name, "String")) {
                emit_expr(o, cg, n->as.index_expr.object);
                emit(o, ".ptr[");
                emit_expr(o, cg, n->as.index_expr.index);
                emit(o, "]");
            } else {
                emit_expr(o, cg, n->as.index_expr.object);
                emit(o, "[");
                emit_expr(o, cg, n->as.index_expr.index);
                emit(o, "]");
            }
            break;
        }
        case NODE_SLICE:
            emit_slice_expr(o, cg, n);
            break;
        case NODE_STRUCT_LIT: {
            emit(o, "(%s){", n->as.struct_lit.type_name);
            for (int i = 0; i < n->as.struct_lit.count; i++) {
                if (i) emit(o, ", ");
                emit(o, ".%s = ", n->as.struct_lit.field_names[i]);
                emit_expr(o, cg, n->as.struct_lit.field_values[i]);
            }
            emit(o, "}");
            break;
        }
        case NODE_MATCH: {
            emit_match_expr(o, cg, n, expected_type);
            break;
        }
        default:
            emit(o, "/*expr?*/0");
    }
}

/* ── statement emission ────────────────────────────────────────────────── */

static void emit_stmt(COut *o, CGContext *cg, Node *n);
static void emit_block(COut *o, CGContext *cg, Node *block);

static int stmt_terminates_control_flow(Node *n) {
    if (!n)
        return 0;

    switch (n->kind) {
        case NODE_RETURN:
        case NODE_BREAK:
        case NODE_CONTINUE:
            return 1;
        case NODE_BLOCK:
            for (int i = 0; i < n->as.block.stmts.count; i++) {
                if (stmt_terminates_control_flow(n->as.block.stmts.items[i]))
                    return 1;
            }
            return 0;
        case NODE_IF:
            if (!n->as.if_stmt.else_node)
                return 0;
            return stmt_terminates_control_flow(n->as.if_stmt.then_block) &&
                   stmt_terminates_control_flow(n->as.if_stmt.else_node);
        default:
            return 0;
    }
}

static void emit_block(COut *o, CGContext *cg, Node *block) {
    int terminated = 0;
    emit(o, "{\n");
    cg_push_scope(cg);
    o->indent++;
    for (int i = 0; i < block->as.block.stmts.count; i++) {
        emit_stmt(o, cg, block->as.block.stmts.items[i]);
        if (stmt_terminates_control_flow(block->as.block.stmts.items[i])) {
            terminated = 1;
            break;
        }
    }
    if (!terminated) {
        emit_scope_defers(o, cg);
    } else {
        cg->defer_count = intstack_top(&cg->scope_defer_markers);
    }
    o->indent--;
    cg_pop_scope(cg);
    emit_indent(o);
    emit(o, "}");
}

static void emit_stmt(COut *o, CGContext *cg, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case NODE_LET: {
            Node *type = n->as.let.type ? n->as.let.type : infer_expr_type(cg, n->as.let.value);
            if (n->as.let.value && expr_is_try(n->as.let.value)) {
                char temp_name[64];
                Node *operand_type = NULL;
                Node *inner_type;

                emit_try_unwrap_prefix(o, cg, n->as.let.value, &operand_type, temp_name, sizeof(temp_name));
                inner_type = try_unwrapped_type(cg, operand_type);
                emit_indent(o);
                emit_typed_name(o, type ? type : inner_type, n->as.let.name);
                emit(o, " = %s.%s;\n", temp_name,
                     operand_type && operand_type->kind == NODE_TYPE_NAMED &&
                     type_name_has_form(operand_type->as.type_named.name, "Option")
                         ? "value"
                         : "as.ok");
                cg_define(cg, n->as.let.name, type ? type : inner_type);
                break;
            }
            emit_indent(o);
            emit_typed_name(o, type ? type : cg_temp_type_named(cg, "void"), n->as.let.name);
            if (n->as.let.value) {
                emit(o, " = ");
                emit_expr_typed(o, cg, n->as.let.value, type);
            }
            emit(o, ";\n");
            cg_define(cg, n->as.let.name, type);
            break;
        }
        case NODE_CONST: {
            Node *type = n->as.konst.type ? n->as.konst.type : infer_expr_type(cg, n->as.konst.value);
            if (n->as.konst.value && expr_is_try(n->as.konst.value)) {
                char temp_name[64];
                Node *operand_type = NULL;
                Node *inner_type;

                emit_try_unwrap_prefix(o, cg, n->as.konst.value, &operand_type, temp_name, sizeof(temp_name));
                inner_type = try_unwrapped_type(cg, operand_type);
                emit_indent(o);
                emit(o, "const ");
                emit_typed_name(o, type ? type : inner_type, n->as.konst.name);
                emit(o, " = %s.%s;\n", temp_name,
                     operand_type && operand_type->kind == NODE_TYPE_NAMED &&
                     type_name_has_form(operand_type->as.type_named.name, "Option")
                         ? "value"
                         : "as.ok");
                cg_define(cg, n->as.konst.name, type ? type : inner_type);
                break;
            }
            emit_indent(o);
            emit(o, "const ");
            emit_typed_name(o, type ? type : cg_temp_type_named(cg, "void"), n->as.konst.name);
            emit(o, " = ");
            emit_expr_typed(o, cg, n->as.konst.value, type);
            emit(o, ";\n");
            cg_define(cg, n->as.konst.name, type);
            break;
        }
        case NODE_DEFER: {
            if (cg->defer_count == cg->defer_cap) {
                cg->defer_cap = cg->defer_cap ? cg->defer_cap * 2 : 4;
                cg->defers = realloc(cg->defers, (size_t)cg->defer_cap * sizeof(Node *));
            }
            cg->defers[cg->defer_count++] = n->as.defer_stmt.expr;
            break;
        }
        case NODE_RETURN: {
            if (n->as.ret.value && expr_is_try_constructor(n->as.ret.value, "Ok")) {
                char temp_name[64];
                Node *operand_type = NULL;
                char c_ret_name[256];

                emit_try_unwrap_prefix(o, cg, n->as.ret.value->as.call.args.items[0],
                                       &operand_type, temp_name, sizeof(temp_name));
                emit_defers_range(o, cg, 0);
                format_option_result_type_name(cg->current_return_type->as.type_named.name,
                                               c_ret_name, sizeof(c_ret_name));
                emit_indent(o);
                emit(o, "return ((%s){ .is_ok = true, .as.ok = %s.as.ok });\n",
                     c_ret_name, temp_name);
                break;
            }
            if (n->as.ret.value && expr_is_try_constructor(n->as.ret.value, "Some")) {
                char temp_name[64];
                Node *operand_type = NULL;
                char c_ret_name[256];

                emit_try_unwrap_prefix(o, cg, n->as.ret.value->as.call.args.items[0],
                                       &operand_type, temp_name, sizeof(temp_name));
                emit_defers_range(o, cg, 0);
                format_option_result_type_name(cg->current_return_type->as.type_named.name,
                                               c_ret_name, sizeof(c_ret_name));
                emit_indent(o);
                emit(o, "return ((%s){ .is_some = true, .value = %s.value });\n",
                     c_ret_name, temp_name);
                break;
            }
            emit_defers_range(o, cg, 0);
            emit_indent(o);
            if (n->as.ret.value) {
                emit(o, "return ");
                emit_expr_typed(o, cg, n->as.ret.value, cg->current_return_type);
                emit(o, ";\n");
            } else {
                emit(o, "return;\n");
            }
            break;
        }
        case NODE_IF: {
            emit_indent(o);
            emit(o, "if (");
            emit_expr(o, cg, n->as.if_stmt.cond);
            emit(o, ") ");
            emit_block(o, cg, n->as.if_stmt.then_block);
            if (n->as.if_stmt.else_node) {
                Node *el = n->as.if_stmt.else_node;
                if (el->kind == NODE_IF) {
                    emit(o, " else ");
                    /* inline else-if: no indent, no newline before */
                    o->indent--;          /* compensate for emit_indent inside */
                    emit_stmt(o, cg, el);
                    o->indent++;
                    return;
                } else {
                    emit(o, " else ");
                    emit_block(o, cg, el);
                }
            }
            emit(o, "\n");
            break;
        }
        case NODE_WHILE: {
            emit_indent(o);
            emit(o, "while (");
            emit_expr(o, cg, n->as.while_stmt.cond);
            emit(o, ") ");
            intstack_push(&cg->loop_defer_markers, cg->defer_count);
            emit_block(o, cg, n->as.while_stmt.body);
            intstack_pop(&cg->loop_defer_markers);
            emit(o, "\n");
            break;
        }
        case NODE_LOOP: {
            emit_indent(o);
            emit(o, "for (;;) ");
            intstack_push(&cg->loop_defer_markers, cg->defer_count);
            emit_block(o, cg, n->as.loop_stmt.body);
            intstack_pop(&cg->loop_defer_markers);
            emit(o, "\n");
            break;
        }
        case NODE_FOR: {
            emit_indent(o);
            emit(o, "for (");
            cg_push_scope(cg);
            /* init */
            if (n->as.for_stmt.init) {
                Node *init = n->as.for_stmt.init;
                if (init->kind == NODE_LET) {
                    Node *type = init->as.let.type
                                 ? init->as.let.type
                                 : infer_expr_type(cg, init->as.let.value);
                    emit_typed_name(o, type ? type : cg_temp_type_named(cg, "void"), init->as.let.name);
                    if (init->as.let.value) {
                        emit(o, " = ");
                        emit_expr(o, cg, init->as.let.value);
                    }
                    cg_define(cg, init->as.let.name, type);
                } else if (init->kind == NODE_EXPR_STMT) {
                    emit_expr(o, cg, init->as.expr_stmt.expr);
                }
            }
            emit(o, "; ");
            /* cond */
            if (n->as.for_stmt.cond)
                emit_expr(o, cg, n->as.for_stmt.cond);
            emit(o, "; ");
            /* post */
            if (n->as.for_stmt.post)
                emit_expr(o, cg, n->as.for_stmt.post);
            emit(o, ") ");
            intstack_push(&cg->loop_defer_markers, cg->defer_count);
            emit_block(o, cg, n->as.for_stmt.body);
            intstack_pop(&cg->loop_defer_markers);
            emit_scope_defers(o, cg);
            cg_pop_scope(cg);
            emit(o, "\n");
            break;
        }
        case NODE_UNSAFE: {
            emit_indent(o);
            emit_block(o, cg, n->as.loop_stmt.body);
            emit(o, "\n");
            break;
        }
        case NODE_BREAK: {
            emit_defers_range(o, cg, intstack_top(&cg->loop_defer_markers));
            emitln(o, "break;");
            break;
        }
        case NODE_CONTINUE: {
            emit_defers_range(o, cg, intstack_top(&cg->loop_defer_markers));
            emitln(o, "continue;");
            break;
        }
        case NODE_BLOCK: {
            emit_indent(o);
            emit_block(o, cg, n);
            emit(o, "\n");
            break;
        }
        case NODE_EXPR_STMT: {
            emit_indent(o);
            emit_expr(o, cg, n->as.expr_stmt.expr);
            emit(o, ";\n");
            break;
        }
        /* match as statement: emit if-else chain */
        case NODE_MATCH: {
            Node *subj = n->as.match.subject;
            for (int i = 0; i < n->as.match.count; i++) {
                const char *pat = n->as.match.patterns[i];
                emit_indent(o);
                if (!strcmp(pat, "_")) {
                    /* wildcard — else branch */
                    if (i > 0) emit(o, "} else {\n");
                    else        emit(o, "{\n");
                } else {
                    if (i == 0) emit(o, "if (");
                    else        emit(o, "} else if (");
                    /* find dot separator */
                    const char *dot = strchr(pat, '.');
                    if (dot) {
                        char enum_name[128], variant[128];
                        int elen = (int)(dot - pat);
                        strncpy(enum_name, pat, (size_t)elen);
                        enum_name[elen] = '\0';
                        strcpy(variant, dot + 1);
                        emit_expr(o, cg, subj);
                        emit(o, " == %s_%s) {\n", enum_name, variant);
                    } else {
                        emit_expr(o, cg, subj);
                        emit(o, " == %s) {\n", pat);
                    }
                }
                o->indent++;
                emit_indent(o);
                emit_expr(o, cg, n->as.match.values[i]);
                emit(o, ";\n");
                o->indent--;
            }
            if (n->as.match.count > 0) {
                emit_indent(o);
                emit(o, "}\n");
            }
            break;
        }
        default:
            break;
    }
}

/* ── function signature emission ───────────────────────────────────────── */

static void emit_fn_sig(COut *o, const char *c_name, NodeList *params, Node *ret_type) {
    emit_type_left(o, ret_type);
    emit(o, " %s(", c_name);
    if (params->count == 0) {
        emit(o, "void");
    } else {
        for (int i = 0; i < params->count; i++) {
            if (i) emit(o, ", ");
            Node *p = params->items[i];
            emit_typed_name(o, p->as.let.type, p->as.let.name);
        }
    }
    emit(o, ")");
}

/* build C name for a method: owner_type + "_" + name */
static void make_c_name(char *out, int outsz, const char *owner, const char *name) {
    if (owner)
        snprintf(out, (size_t)outsz, "%s_%s", owner, name);
    else
        snprintf(out, (size_t)outsz, "%s", name);
}

static const char *header_for_extern(const char *name) {
    if (!strcmp(name, "puts") ||
        !strcmp(name, "printf") ||
        !strcmp(name, "fprintf") ||
        !strcmp(name, "snprintf")) {
        return "stdio.h";
    }

    if (!strcmp(name, "malloc") ||
        !strcmp(name, "free") ||
        !strcmp(name, "realloc") ||
        !strcmp(name, "calloc") ||
        !strcmp(name, "exit")) {
        return "stdlib.h";
    }

    if (!strcmp(name, "memcpy") ||
        !strcmp(name, "memset") ||
        !strcmp(name, "memcmp") ||
        !strcmp(name, "strlen") ||
        !strcmp(name, "strcmp")) {
        return "string.h";
    }

    if (strncmp(name, "ns_io_", 6) == 0)
        return "stdio.h";

    return NULL;
}

/* ── top-level passes ──────────────────────────────────────────────────── */

static void emit_standard_headers(COut *o, Node *prog) {
    int need_stdbool = 1;
    int need_stddef = 1;
    int need_stdint = 1;
    int need_stdio = 1;  /* always needed — io runtime uses fputs/puts/scanf */
    int need_stdlib = 0;
    int need_string = 0;

    (void)need_stdbool;
    (void)need_stddef;
    (void)need_stdint;
    (void)need_stdlib;
    (void)need_string;

    for (int i = 0; i < prog->as.program.decls.count; i++) {
        Node *decl = prog->as.program.decls.items[i];
        if (decl->kind != NODE_EXTERN_FN)
            continue;

        {
            const char *header = header_for_extern(decl->as.extern_fn.name);
            if (!header) continue;
            if (!strcmp(header, "stdio.h")) need_stdio = 1;
            if (!strcmp(header, "stdlib.h")) need_stdlib = 1;
            if (!strcmp(header, "string.h")) need_string = 1;
        }
    }

    emit(o, "#include <stdbool.h>\n");
    emit(o, "#include <stddef.h>\n");
    emit(o, "#include <stdint.h>\n");
    if (need_stdio) emit(o, "#include <stdio.h>\n");
    emit(o, "#include <stdlib.h>\n");
    emit(o, "#include <string.h>\n");
    emit(o, "\n");
}

/* pass 1: struct / enum / union definitions */
static void emit_type_definitions(COut *o, Node *prog) {
    SliceDef *slice_defs = NULL;
    OptionResultDef *option_result_defs = NULL;
    int emitted_forward = 0;

    for (int i = 0; i < prog->as.program.decls.count; i++) {
        Node *d = prog->as.program.decls.items[i];

        if (d->kind == NODE_STRUCT_DECL) {
            emit(o, "typedef struct %s %s;\n", d->as.struct_decl.name, d->as.struct_decl.name);
            emitted_forward = 1;
        }

        if (d->kind == NODE_UNION_DECL) {
            emit(o, "typedef union %s %s;\n", d->as.union_decl.name, d->as.union_decl.name);
            emitted_forward = 1;
        }

        if (d->kind == NODE_ENUM_DECL && enum_is_data_carrying(d)) {
            emit(o, "typedef struct %s %s;\n", d->as.enum_decl.name, d->as.enum_decl.name);
            emitted_forward = 1;
        }
    }

    if (emitted_forward)
        emit(o, "\n");

    emit(o, "typedef struct NStr {\n");
    emit(o, "    const uint8_t *ptr;\n");
    emit(o, "    size_t len;\n");
    emit(o, "} NStr;\n\n");

    emit(o, "typedef struct NString {\n");
    emit(o, "    uint8_t *ptr;\n");
    emit(o, "    size_t len;\n");
    emit(o, "    size_t cap;\n");
    emit(o, "} NString;\n\n");

    for (int i = 0; i < prog->as.program.decls.count; i++) {
        Node *d = prog->as.program.decls.items[i];

        if (d->kind == NODE_ENUM_DECL) {
            if (enum_is_data_carrying(d)) {
                /* tagged union: emit tag enum + struct wrapper */
                emit(o, "typedef enum {\n");
                for (int j = 0; j < d->as.enum_decl.count; j++)
                    emit(o, "    %s_%s = %d,\n",
                         d->as.enum_decl.name, d->as.enum_decl.variants[j], j);
                emit(o, "} %s_Tag;\n\n", d->as.enum_decl.name);

                emit(o, "struct %s {\n", d->as.enum_decl.name);
                emit(o, "    %s_Tag tag;\n", d->as.enum_decl.name);
                emit(o, "    union {\n");
                for (int j = 0; j < d->as.enum_decl.count; j++) {
                    NodeList *fields = &d->as.enum_decl.variant_fields[j];
                    if (fields->count > 0) {
                        emit(o, "        struct {\n");
                        for (int k = 0; k < fields->count; k++) {
                            Node *f = fields->items[k];
                            emit(o, "            ");
                            emit_typed_name(o, f->as.let.type, f->as.let.name);
                            emit(o, ";\n");
                        }
                        emit(o, "        } %s;\n", d->as.enum_decl.variants[j]);
                    }
                }
                emit(o, "    } as;\n");
                emit(o, "};\n\n");
            } else {
                emit(o, "typedef enum {\n");
                for (int j = 0; j < d->as.enum_decl.count; j++)
                    emit(o, "    %s_%s = %d,\n",
                         d->as.enum_decl.name, d->as.enum_decl.variants[j], j);
                emit(o, "} %s;\n\n", d->as.enum_decl.name);
            }
        }
    }

    collect_slice_types_in_node(&slice_defs, prog);
    collect_option_result_types_in_node(&option_result_defs, prog);
    for (SliceDef *slice = slice_defs; slice; slice = slice->next) {
        Node ptr_type;

        memset(&ptr_type, 0, sizeof(ptr_type));
        ptr_type.kind = NODE_TYPE_POINTER;
        ptr_type.as.type_ptr.inner = slice->type->as.type_array.elem;
        ptr_type.as.type_ptr.is_const = 0;
        ptr_type.as.type_ptr.is_nullable = 0;

        emit(o, "typedef struct %s {\n", slice->name);
        emit(o, "    ");
        emit_typed_name(o, &ptr_type, "ptr");
        emit(o, ";\n");
        emit(o, "    size_t len;\n");
        emit(o, "} %s;\n\n", slice->name);
    }

    for (int i = 0; i < prog->as.program.decls.count; i++) {
        Node *d = prog->as.program.decls.items[i];

        if (d->kind == NODE_STRUCT_DECL) {
            emit(o, "struct %s {\n", d->as.struct_decl.name);
            for (int j = 0; j < d->as.struct_decl.fields.count; j++) {
                Node *f = d->as.struct_decl.fields.items[j];
                emit(o, "    ");
                emit_typed_name(o, f->as.let.type, f->as.let.name);
                emit(o, ";\n");
            }
            if (d->as.struct_decl.is_packed)
                emit(o, "} __attribute__((packed));\n\n");
            else
                emit(o, "};\n\n");
        }

        if (d->kind == NODE_UNION_DECL) {
            emit(o, "union %s {\n", d->as.union_decl.name);
            for (int j = 0; j < d->as.union_decl.fields.count; j++) {
                Node *f = d->as.union_decl.fields.items[j];
                emit(o, "    ");
                emit_typed_name(o, f->as.let.type, f->as.let.name);
                emit(o, ";\n");
            }
            emit(o, "};\n\n");
        }
    }

    for (OptionResultDef *def = option_result_defs; def; def = def->next) {
        char c_name[256];
        char *inner = NULL;
        char *left = NULL;
        char *right = NULL;
        Node left_type = {0};
        Node right_type = {0};

        format_option_result_type_name(def->name, c_name, sizeof(c_name));
        if (!def->is_result) {
            inner = slice_range_dup(def->name + 7, def->name + strlen(def->name) - 1);
            if (!inner)
                continue;
            left_type.kind = NODE_TYPE_NAMED;
            left_type.as.type_named.name = inner;
            emit(o, "typedef struct %s {\n", c_name);
            emit(o, "    bool is_some;\n");
            emit(o, "    ");
            emit_typed_name(o, &left_type, "value");
            emit(o, ";\n");
            emit(o, "} %s;\n\n", c_name);
            free(inner);
            continue;
        }

        inner = slice_range_dup(def->name + 7, def->name + strlen(def->name) - 1);
        if (!inner)
            continue;
        if (!split_top_level_comma(inner, &left, &right)) {
            free(inner);
            continue;
        }
        left_type.kind = NODE_TYPE_NAMED;
        left_type.as.type_named.name = left;
        right_type.kind = NODE_TYPE_NAMED;
        right_type.as.type_named.name = right;
        emit(o, "typedef struct %s {\n", c_name);
        emit(o, "    bool is_ok;\n");
        emit(o, "    union {\n");
        emit(o, "        ");
        emit_typed_name(o, &left_type, "ok");
        emit(o, ";\n");
        emit(o, "        ");
        emit_typed_name(o, &right_type, "err");
        emit(o, ";\n");
        emit(o, "    } as;\n");
        emit(o, "} %s;\n\n", c_name);
        free(inner);
        free(left);
        free(right);
    }

    free_slice_defs(slice_defs);
    free_option_result_defs(option_result_defs);
}

/* pass 2: function prototypes */
static void emit_prototypes(COut *o, Node *prog) {
    char cname[256];
    for (int i = 0; i < prog->as.program.decls.count; i++) {
        Node *d = prog->as.program.decls.items[i];

        /* top-level const — emit as static const or #define */
        if (d->kind == NODE_CONST) {
            COut tmp;
            tmp.buf = malloc(256); tmp.len = 0; tmp.cap = 256; tmp.indent = 0;
            if (tmp.buf) { tmp.buf[0] = '\0';
                CGContext cg0 = { .program = prog };
                emit_expr(&tmp, &cg0, d->as.konst.value);
                emit(o, "#define %s (%s)\n", d->as.konst.name, tmp.buf);
                free(tmp.buf);
            }
        }

        if (d->kind == NODE_EXTERN_FN) {
            if (header_for_extern(d->as.extern_fn.name))
                continue;
            make_c_name(cname, sizeof(cname), NULL, d->as.extern_fn.name);
            emit_fn_sig(o, cname, &d->as.extern_fn.params, d->as.extern_fn.ret_type);
            emit(o, ";\n");
        }

        if (d->kind == NODE_FN_DECL) {
            make_c_name(cname, sizeof(cname), d->as.fn.owner_type, d->as.fn.name);
            int is_ep = !d->as.fn.owner_type &&
                        !strcmp(d->as.fn.name, "main") &&
                        d->as.fn.params.count == 0 &&
                        !has_ui_app(prog) && !has_kernel_app(prog);
            if (is_ep)
                emit(o, "int main(void)");
            else
                emit_fn_sig(o, cname, &d->as.fn.params, d->as.fn.ret_type);
            emit(o, ";\n");
        }

        if (d->kind == NODE_IMPL_DECL) {
            for (int j = 0; j < d->as.impl.methods.count; j++) {
                Node *m = d->as.impl.methods.items[j];
                make_c_name(cname, sizeof(cname), d->as.impl.target, m->as.fn.name);
                emit_fn_sig(o, cname, &m->as.fn.params, m->as.fn.ret_type);
                emit(o, ";\n");
            }
        }
    }
}

/* pass 3: function definitions */
static void emit_definitions(COut *o, Node *prog) {
    char cname[256];
    CGContext cg = { .program = prog, .scope = NULL, .temps = NULL,
                     .defers = NULL, .defer_count = 0, .defer_cap = 0 };
    for (int i = 0; i < prog->as.program.decls.count; i++) {
        Node *d = prog->as.program.decls.items[i];

        if (d->kind == NODE_FN_DECL) {
            make_c_name(cname, sizeof(cname), d->as.fn.owner_type, d->as.fn.name);
            int is_entry = !d->as.fn.owner_type &&
                           !strcmp(d->as.fn.name, "main") &&
                           d->as.fn.params.count == 0 &&
                           !has_ui_app(prog) && !has_kernel_app(prog);
            emit(o, "\n");
            if (is_entry)
                emit(o, "int main(void)");
            else
                emit_fn_sig(o, cname, &d->as.fn.params, d->as.fn.ret_type);
            emit(o, "\n");
            cg.defer_count = 0; /* reset defers per function */
            cg.current_return_type = d->as.fn.ret_type;
            cg_push_scope(&cg);
            for (int j = 0; j < d->as.fn.params.count; j++) {
                Node *param = d->as.fn.params.items[j];
                cg_define(&cg, param->as.let.name, param->as.let.type);
            }
            if (is_entry) {
                /* emit body without the closing brace, then add return 0 */
                int terminated = 0;
                emit(o, "{\n");
                cg_push_scope(&cg);
                o->indent++;
                for (int j = 0; j < d->as.fn.body->as.block.stmts.count; j++) {
                    emit_stmt(o, &cg, d->as.fn.body->as.block.stmts.items[j]);
                    if (stmt_terminates_control_flow(d->as.fn.body->as.block.stmts.items[j])) {
                        terminated = 1; break;
                    }
                }
                if (!terminated) emit_scope_defers(o, &cg);
                emit_indent(o);
                emit(o, "return 0;\n");
                o->indent--;
                cg_pop_scope(&cg);
                emit_indent(o);
                emit(o, "}");
            } else {
                emit_block(o, &cg, d->as.fn.body);
                cg_pop_scope(&cg);
            }
            emit(o, "\n");
        }

        if (d->kind == NODE_IMPL_DECL) {
            for (int j = 0; j < d->as.impl.methods.count; j++) {
                Node *m = d->as.impl.methods.items[j];
                make_c_name(cname, sizeof(cname), d->as.impl.target, m->as.fn.name);
                emit(o, "\n");
                emit_fn_sig(o, cname, &m->as.fn.params, m->as.fn.ret_type);
                emit(o, "\n");
                cg.defer_count = 0;
                cg.current_return_type = m->as.fn.ret_type;
                cg_push_scope(&cg);
                for (int k = 0; k < m->as.fn.params.count; k++) {
                    Node *param = m->as.fn.params.items[k];
                    cg_define(&cg, param->as.let.name, param->as.let.type);
                }
                emit_block(o, &cg, m->as.fn.body);
                cg_pop_scope(&cg);
                emit(o, "\n");
            }
        }

        if (d->kind == NODE_KERNEL_APP) {
            for (int j = 0; j < d->as.kernel_app.fns.count; j++) {
                Node *fn = d->as.kernel_app.fns.items[j];
                make_c_name(cname, sizeof(cname), NULL, fn->as.fn.name);
                emit(o, "\n");
                emit_fn_sig(o, cname, &fn->as.fn.params, fn->as.fn.ret_type);
                emit(o, "\n");
                cg.defer_count = 0;
                cg.current_return_type = fn->as.fn.ret_type;
                cg_push_scope(&cg);
                for (int k = 0; k < fn->as.fn.params.count; k++) {
                    Node *param = fn->as.fn.params.items[k];
                    cg_define(&cg, param->as.let.name, param->as.let.type);
                }
                emit_block(o, &cg, fn->as.fn.body);
                cg_pop_scope(&cg);
                emit(o, "\n");
            }
        }
    }
    cg_free_temps(&cg);
    free(cg.defers);
    free(cg.scope_defer_markers.items);
    free(cg.loop_defer_markers.items);
}

/* ── UI code generation (v0.4) ─────────────────────────────────────────── */

static int has_ui_app(Node *prog) {
    for (int i = 0; i < prog->as.program.decls.count; i++)
        if (prog->as.program.decls.items[i]->kind == NODE_UI_APP)
            return 1;
    return 0;
}

/* ── UI helper accessors ─────────────────────────────────────────────────── */

static long long ui_prop_int(Node *elem, const char *name, long long def) {
    for (int i = 0; i < elem->as.ui_element.properties.count; i++) {
        Node *p = elem->as.ui_element.properties.items[i];
        if (!strcmp(p->as.ui_property.name, name) &&
            p->as.ui_property.value &&
            p->as.ui_property.value->kind == NODE_LIT_INT)
            return p->as.ui_property.value->as.lit_int.value;
    }
    return def;
}

static const char *ui_elem_text(Node *elem) {
    if (elem->as.ui_element.text && elem->as.ui_element.text[0])
        return elem->as.ui_element.text;
    for (int i = 0; i < elem->as.ui_element.properties.count; i++) {
        Node *p = elem->as.ui_element.properties.items[i];
        if (p->as.ui_property.value &&
            p->as.ui_property.value->kind == NODE_LIT_STRING &&
            (!strcmp(p->as.ui_property.name, "text") ||
             !strcmp(p->as.ui_property.name, "title") ||
             !strcmp(p->as.ui_property.name, "label") ||
             !strcmp(p->as.ui_property.name, "placeholder")))
            return p->as.ui_property.value->as.lit_str.value;
    }
    return "";
}

/* ── UIElemInfo: layout record per leaf element ─────────────────────────── */

typedef struct UIElemInfo {
    int   kind;        /* UIElemKind */
    int   x, y, w, h;
    const char *label;
    int   elem_idx;    /* index in source tree (matches handler name) */
    int   has_onclick;
    int   has_onkey;
    int   has_onchange;
} UIElemInfo;

/* ── layout pass ─────────────────────────────────────────────────────────── */

static void collect_ui_elems(Node *elem, int *x_cur, int *y_cur, int *idx,
                              UIElemInfo *elems, int *count, int max) {
    if (*count >= max) return;
    int ek = elem->as.ui_element.elem_kind;
    int is_container = (ek == UI_ELEM_WINDOW || ek == UI_ELEM_ROW  ||
                        ek == UI_ELEM_COLUMN || ek == UI_ELEM_PANEL ||
                        ek == UI_ELEM_CANVAS || ek == UI_ELEM_MENU);

    if (!is_container) {
        /* default dimensions by element type */
        int def_w = (ek == UI_ELEM_BUTTON) ? 160 :
                    (ek == UI_ELEM_LABEL)  ? 240 :
                    (ek == UI_ELEM_INPUT)  ? 260 : 160;
        int def_h = (ek == UI_ELEM_BUTTON) ? 40  :
                    (ek == UI_ELEM_LABEL)  ? 24  :
                    (ek == UI_ELEM_INPUT)  ? 36  : 40;

        UIElemInfo *info = &elems[*count];
        info->kind       = ek;
        info->x          = (int)ui_prop_int(elem, "x",      *x_cur);
        info->y          = (int)ui_prop_int(elem, "y",      *y_cur);
        info->w          = (int)ui_prop_int(elem, "width",  def_w);
        info->h          = (int)ui_prop_int(elem, "height", def_h);
        info->label      = ui_elem_text(elem);
        info->elem_idx   = *idx;
        info->has_onclick  = 0;
        info->has_onkey    = 0;
        info->has_onchange = 0;

        for (int i = 0; i < elem->as.ui_element.handlers.count; i++) {
            int hk = elem->as.ui_element.handlers.items[i]->as.ui_handler.handler_kind;
            if (hk == UI_HANDLER_CLICK)  info->has_onclick  = 1;
            if (hk == UI_HANDLER_KEY)    info->has_onkey    = 1;
            if (hk == UI_HANDLER_CHANGE) info->has_onchange = 1;
        }

        *y_cur += info->h + 10;
        (*count)++;
    }
    (*idx)++;

    if (elem->as.ui_element.children.count > 0) {
        if (ek == UI_ELEM_ROW) {
            /* horizontal flow */
            int child_x   = *x_cur + 8;
            int row_y     = *y_cur;
            int row_max_h = 0;
            for (int i = 0; i < elem->as.ui_element.children.count; i++) {
                int cy = row_y;
                collect_ui_elems(elem->as.ui_element.children.items[i],
                                 &child_x, &cy, idx, elems, count, max);
                int used_h = cy - row_y;
                if (used_h > row_max_h) row_max_h = used_h;
                child_x += 10; /* gap between row children */
            }
            *y_cur = row_y + row_max_h;
        } else {
            /* vertical flow: window, column, panel, canvas */
            int pad = (ek == UI_ELEM_PANEL || ek == UI_ELEM_COLUMN) ? 8 : 0;
            int cx = *x_cur + pad;
            for (int i = 0; i < elem->as.ui_element.children.count; i++)
                collect_ui_elems(elem->as.ui_element.children.items[i],
                                 &cx, y_cur, idx, elems, count, max);
        }
    }
}

/* ── handler emission ────────────────────────────────────────────────────── */

/* Handlers receive context via globals:
     ns_last_key      — SDL_Keycode of the last keydown
     ns_focused_idx   — index of the currently focused input element (-1 = none)
     ns_input_text(i) — macro to get the char* text of input element i       */

static void emit_ui_handlers(COut *o, Node *elem, Node *prog, int *counter) {
    for (int i = 0; i < elem->as.ui_element.handlers.count; i++) {
        CGContext cg;
        memset(&cg, 0, sizeof(cg));
        cg.program = prog;

        Node *h    = elem->as.ui_element.handlers.items[i];
        int   hk   = h->as.ui_handler.handler_kind;
        const char *hname = (hk == UI_HANDLER_CLICK)  ? "onclick"  :
                            (hk == UI_HANDLER_KEY)    ? "onkey"    : "onchange";

        emit(o, "static void ns_handler_%d_%s(void) ", *counter, hname);
        cg_push_scope(&cg);
        emit_block(o, &cg, h->as.ui_handler.body);
        cg_pop_scope(&cg);
        emit(o, "\n");

        cg_free_temps(&cg);
        free(cg.defers);
        free(cg.scope_defer_markers.items);
        free(cg.loop_defer_markers.items);
    }
    (*counter)++;

    for (int i = 0; i < elem->as.ui_element.children.count; i++)
        emit_ui_handlers(o, elem->as.ui_element.children.items[i], prog, counter);
}

/* ── std.io runtime ──────────────────────────────────────────────────────── */


static void emit_io_runtime(COut *o) {
    emit(o, "/* ── std.io runtime ──────────────────────────────────────── */\n");
    emit(o, "static char _ns_io_buf[4096];\n");
    emit(o, "static inline void      ns_io_print(const char *s)     { fputs(s, stdout); }\n");
    emit(o, "static inline void      ns_io_println(const char *s)   { fputs(s, stdout); fputc('\\n', stdout); }\n");
    emit(o, "static inline void      ns_io_eprint(const char *s)    { fputs(s, stderr); }\n");
    emit(o, "static inline void      ns_io_eprintln(const char *s)  { fputs(s, stderr); fputc('\\n', stderr); }\n");
    emit(o, "static inline void      ns_io_print_i32(int32_t n)     { printf(\"%%d\",   n); }\n");
    emit(o, "static inline void      ns_io_print_i64(int64_t n)     { printf(\"%%lld\", (long long)n); }\n");
    emit(o, "static inline void      ns_io_print_u32(uint32_t n)    { printf(\"%%u\",   n); }\n");
    emit(o, "static inline void      ns_io_print_u64(uint64_t n)    { printf(\"%%llu\", (unsigned long long)n); }\n");
    emit(o, "static inline void      ns_io_print_f64(double n)      { printf(\"%%g\",   n); }\n");
    emit(o, "static inline void      ns_io_print_bool(int b)        { fputs(b ? \"true\" : \"false\", stdout); }\n");
    emit(o, "static inline void      ns_io_flush(void)              { fflush(stdout); }\n");
    emit(o, "static inline const char *ns_io_readln(void) {\n");
    emit(o, "    if (!fgets(_ns_io_buf, (int)sizeof(_ns_io_buf), stdin))\n");
    emit(o, "        { _ns_io_buf[0] = '\\0'; return _ns_io_buf; }\n");
    emit(o, "    int _l = 0;\n");
    emit(o, "    while (_ns_io_buf[_l] && _ns_io_buf[_l] != '\\n' && _ns_io_buf[_l] != '\\r') _l++;\n");
    emit(o, "    _ns_io_buf[_l] = '\\0';\n");
    emit(o, "    return _ns_io_buf;\n");
    emit(o, "}\n");
    emit(o, "static inline int32_t   ns_io_read_i32(void)  { int32_t  n = 0; scanf(\"%%d\",  &n); return n; }\n");
    emit(o, "static inline int64_t   ns_io_read_i64(void)  { long long n = 0; scanf(\"%%lld\", &n); return (int64_t)n; }\n");
    emit(o, "static inline uint32_t  ns_io_read_u32(void)  { uint32_t n = 0; scanf(\"%%u\",  &n); return n; }\n");
    emit(o, "static inline double    ns_io_read_f64(void)  { double   n = 0; scanf(\"%%lf\", &n); return n; }\n");
    emit(o, "/* ──────────────────────────────────────────────────────────── */\n\n");
}

/* ── emit_ui_app: full SDL2 runtime with text + input ───────────────────── */

static void emit_ui_app(COut *o, Node *prog) {
    /* ── find app and window ── */
    Node *app = NULL;
    for (int i = 0; i < prog->as.program.decls.count; i++) {
        if (prog->as.program.decls.items[i]->kind == NODE_UI_APP) {
            app = prog->as.program.decls.items[i]; break;
        }
    }
    if (!app) return;

    Node *window = NULL;
    for (int i = 0; i < app->as.ui_app.children.count; i++) {
        Node *ch = app->as.ui_app.children.items[i];
        if (ch->kind == NODE_UI_ELEMENT &&
            ch->as.ui_element.elem_kind == UI_ELEM_WINDOW) {
            window = ch; break;
        }
    }

    const char *title = app->as.ui_app.name;
    int win_w = 800, win_h = 600;
    if (window) {
        const char *wt = ui_elem_text(window);
        if (wt && wt[0]) title = wt;
        win_w = (int)ui_prop_int(window, "width",  800);
        win_h = (int)ui_prop_int(window, "height", 600);
    }

    emit(o, "\n/* ════════════════════════════════════════════════════════\n");
    emit(o, "   NightScript SDL2 UI Runtime  (v0.4)\n");
    emit(o, "   Generated by the NightScript compiler — do not edit\n");
    emit(o, "   ════════════════════════════════════════════════════════ */\n\n");

    /* ── 8x8 embedded bitmap font (public-domain PC BIOS font, ASCII 32-127) ── */
    emit(o, "/* 8x8 bitmap font: each char = 8 bytes, rows top-to-bottom, MSB=left */\n");
    emit(o, "static const unsigned char ns_font8x8[96][8] = {\n");
    /* space (0x20) */ emit(o, "  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */\n");
    /* ! */  emit(o, "  {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},\n");
    /* \" */ emit(o, "  {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},\n");
    /* # */  emit(o, "  {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},\n");
    /* $ */  emit(o, "  {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},\n");
    /* % */  emit(o, "  {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},\n");
    /* & */  emit(o, "  {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},\n");
    /* ' */  emit(o, "  {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},\n");
    /* ( */  emit(o, "  {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},\n");
    /* ) */  emit(o, "  {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},\n");
    /* * */  emit(o, "  {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},\n");
    /* + */  emit(o, "  {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},\n");
    /* , */  emit(o, "  {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06},\n");
    /* - */  emit(o, "  {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},\n");
    /* . */  emit(o, "  {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},\n");
    /* / */  emit(o, "  {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},\n");
    /* 0 */  emit(o, "  {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},\n");
    /* 1 */  emit(o, "  {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},\n");
    /* 2 */  emit(o, "  {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},\n");
    /* 3 */  emit(o, "  {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},\n");
    /* 4 */  emit(o, "  {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},\n");
    /* 5 */  emit(o, "  {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},\n");
    /* 6 */  emit(o, "  {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},\n");
    /* 7 */  emit(o, "  {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},\n");
    /* 8 */  emit(o, "  {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},\n");
    /* 9 */  emit(o, "  {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},\n");
    /* : */  emit(o, "  {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00},\n");
    /* ; */  emit(o, "  {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},\n");
    /* < */  emit(o, "  {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},\n");
    /* = */  emit(o, "  {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},\n");
    /* > */  emit(o, "  {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},\n");
    /* ? */  emit(o, "  {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},\n");
    /* @ */  emit(o, "  {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},\n");
    /* A */  emit(o, "  {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},\n");
    /* B */  emit(o, "  {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},\n");
    /* C */  emit(o, "  {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},\n");
    /* D */  emit(o, "  {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},\n");
    /* E */  emit(o, "  {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},\n");
    /* F */  emit(o, "  {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},\n");
    /* G */  emit(o, "  {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},\n");
    /* H */  emit(o, "  {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},\n");
    /* I */  emit(o, "  {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},\n");
    /* J */  emit(o, "  {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},\n");
    /* K */  emit(o, "  {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},\n");
    /* L */  emit(o, "  {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},\n");
    /* M */  emit(o, "  {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},\n");
    /* N */  emit(o, "  {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},\n");
    /* O */  emit(o, "  {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},\n");
    /* P */  emit(o, "  {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},\n");
    /* Q */  emit(o, "  {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},\n");
    /* R */  emit(o, "  {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},\n");
    /* S */  emit(o, "  {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},\n");
    /* T */  emit(o, "  {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},\n");
    /* U */  emit(o, "  {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},\n");
    /* V */  emit(o, "  {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},\n");
    /* W */  emit(o, "  {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},\n");
    /* X */  emit(o, "  {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},\n");
    /* Y */  emit(o, "  {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},\n");
    /* Z */  emit(o, "  {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},\n");
    /* [ */  emit(o, "  {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},\n");
    /* \\ */ emit(o, "  {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},\n");
    /* ] */  emit(o, "  {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},\n");
    /* ^ */  emit(o, "  {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},\n");
    /* _ */  emit(o, "  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},\n");
    /* ` */  emit(o, "  {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},\n");
    /* a */  emit(o, "  {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},\n");
    /* b */  emit(o, "  {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},\n");
    /* c */  emit(o, "  {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},\n");
    /* d */  emit(o, "  {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00},\n");
    /* e */  emit(o, "  {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},\n");
    /* f */  emit(o, "  {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00},\n");
    /* g */  emit(o, "  {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},\n");
    /* h */  emit(o, "  {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},\n");
    /* i */  emit(o, "  {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},\n");
    /* j */  emit(o, "  {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},\n");
    /* k */  emit(o, "  {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},\n");
    /* l */  emit(o, "  {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},\n");
    /* m */  emit(o, "  {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},\n");
    /* n */  emit(o, "  {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},\n");
    /* o */  emit(o, "  {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},\n");
    /* p */  emit(o, "  {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},\n");
    /* q */  emit(o, "  {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},\n");
    /* r */  emit(o, "  {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},\n");
    /* s */  emit(o, "  {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},\n");
    /* t */  emit(o, "  {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},\n");
    /* u */  emit(o, "  {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},\n");
    /* v */  emit(o, "  {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},\n");
    /* w */  emit(o, "  {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},\n");
    /* x */  emit(o, "  {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},\n");
    /* y */  emit(o, "  {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},\n");
    /* z */  emit(o, "  {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},\n");
    /* { */  emit(o, "  {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},\n");
    /* | */  emit(o, "  {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},\n");
    /* } */  emit(o, "  {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},\n");
    /* ~ */  emit(o, "  {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},\n");
    /* del */ emit(o, "  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},\n");
    emit(o, "};\n\n");

    /* ── text rendering via pixel font ── */
    emit(o, "static void ns_draw_char(SDL_Renderer *r, int x, int y,\n");
    emit(o, "                         char c, Uint8 R, Uint8 G, Uint8 B) {\n");
    emit(o, "    if (c < 32 || c > 127) return;\n");
    emit(o, "    const unsigned char *glyph = ns_font8x8[(unsigned char)c - 32];\n");
    emit(o, "    SDL_SetRenderDrawColor(r, R, G, B, 255);\n");
    emit(o, "    for (int row = 0; row < 8; row++) {\n");
    emit(o, "        unsigned char bits = glyph[row];\n");
    emit(o, "        for (int col = 0; col < 8; col++) {\n");
    emit(o, "            if (bits & (0x80 >> col))\n");
    emit(o, "                SDL_RenderDrawPoint(r, x + col, y + row);\n");
    emit(o, "        }\n");
    emit(o, "    }\n");
    emit(o, "}\n\n");

    emit(o, "static void ns_draw_text(SDL_Renderer *r, int x, int y,\n");
    emit(o, "                          const char *s, Uint8 R, Uint8 G, Uint8 B) {\n");
    emit(o, "    for (; *s; s++, x += 9)\n");
    emit(o, "        ns_draw_char(r, x, y, *s, R, G, B);\n");
    emit(o, "}\n\n");

    emit(o, "static int ns_text_width(const char *s) {\n");
    emit(o, "    int n = 0; while (*s++) n++; return n * 9;\n");
    emit(o, "}\n\n");

    /* ── element types ── */
    emit(o, "#define NS_ELEM_LABEL  0\n");
    emit(o, "#define NS_ELEM_BUTTON 1\n");
    emit(o, "#define NS_ELEM_INPUT  2\n");
    emit(o, "#define NS_ELEM_OTHER  3\n\n");

    /* ── NSUIElem struct ── */
    emit(o, "typedef struct NSUIElem {\n");
    emit(o, "    int         kind;               /* NS_ELEM_* */\n");
    emit(o, "    int         x, y, w, h;\n");
    emit(o, "    const char *label;              /* static label or placeholder */\n");
    emit(o, "    char        input_buf[512];     /* editable text (input only) */\n");
    emit(o, "    int         input_len;          /* current text length */\n");
    emit(o, "    int         cursor;             /* caret position */\n");
    emit(o, "    int         focused;            /* 1 = keyboard focus */\n");
    emit(o, "    int         hovered;            /* 1 = mouse is over */\n");
    emit(o, "    void      (*on_click)(void);\n");
    emit(o, "    void      (*on_key)(void);      /* called with ns_last_key set */\n");
    emit(o, "    void      (*on_change)(void);   /* called with ns_changed_idx set */\n");
    emit(o, "} NSUIElem;\n\n");

    /* ── global state ── */
    emit(o, "static int          ns_focused_idx  = -1; /* index of focused element */\n");
    emit(o, "static int          ns_hovered_idx  = -1; /* index of hovered element */\n");
    emit(o, "static int          ns_last_key     =  0; /* SDL_Keycode of last keydown */\n");
    emit(o, "static int          ns_changed_idx  = -1; /* index of changed input */\n\n");

    /* ── accessor macros for user handlers ── */
    emit(o, "/* Accessible from onKey{} and onChange{} handler bodies */\n");
    emit(o, "#define ns_input_text(i)    (ns_ui_elems[i].input_buf)\n");
    emit(o, "#define ns_input_len(i)     (ns_ui_elems[i].input_len)\n");
    emit(o, "#define ns_key()            ((int)ns_last_key)\n");
    emit(o, "#define ns_changed_text()   (ns_ui_elems[ns_changed_idx].input_buf)\n\n");

    /* ── emit all handler functions ── */
    int hctr = 0;
    if (window)
        emit_ui_handlers(o, window, prog, &hctr);

    /* ── collect all leaf elements ── */
    UIElemInfo elems[256];
    int elem_count = 0;
    int x_cur = 16, y_cur = 16, idx = 0;
    if (window)
        collect_ui_elems(window, &x_cur, &y_cur, &idx, elems, &elem_count, 256);

    /* ── element table ── */
    emit(o, "static NSUIElem ns_ui_elems[] = {\n");
    for (int i = 0; i < elem_count; i++) {
        UIElemInfo *e = &elems[i];

        int kind_c = (e->kind == UI_ELEM_BUTTON) ? 1 :
                     (e->kind == UI_ELEM_LABEL)  ? 0 :
                     (e->kind == UI_ELEM_INPUT)  ? 2 : 3;

        char onclick_buf[80]   = "NULL";
        char onkey_buf[80]     = "NULL";
        char onchange_buf[80]  = "NULL";
        if (e->has_onclick)
            snprintf(onclick_buf,  sizeof(onclick_buf),  "ns_handler_%d_onclick",  e->elem_idx);
        if (e->has_onkey)
            snprintf(onkey_buf,    sizeof(onkey_buf),    "ns_handler_%d_onkey",    e->elem_idx);
        if (e->has_onchange)
            snprintf(onchange_buf, sizeof(onchange_buf), "ns_handler_%d_onchange", e->elem_idx);

        /* escape label for C string */
        char esc_label[256];
        const char *src = e->label ? e->label : "";
        int oi = 0;
        while (*src && oi < 250) {
            if (*src == '"' || *src == '\\') esc_label[oi++] = '\\';
            esc_label[oi++] = *src++;
        }
        esc_label[oi] = '\0';

        emit(o, "    { %d, %d,%d,%d,%d, \"%s\", {0}, 0, 0, 0, 0,\n",
             kind_c, e->x, e->y, e->w, e->h, esc_label);
        emit(o, "      %s, %s, %s },\n", onclick_buf, onkey_buf, onchange_buf);
    }
    emit(o, "};\n");
    emit(o, "#define NS_ELEM_COUNT ((int)(sizeof(ns_ui_elems)/sizeof(ns_ui_elems[0])))\n\n");

    /* ── rendering helpers ── */
    emit(o, "static void ns_render_label(SDL_Renderer *r, NSUIElem *e) {\n");
    emit(o, "    /* draw text centred vertically in the element bounds */\n");
    emit(o, "    int ty = e->y + (e->h - 8) / 2;\n");
    emit(o, "    ns_draw_text(r, e->x + 4, ty, e->label, 220, 220, 220);\n");
    emit(o, "}\n\n");

    emit(o, "static void ns_render_button(SDL_Renderer *r, NSUIElem *e) {\n");
    emit(o, "    /* background */\n");
    emit(o, "    Uint8 br = e->hovered ? 100 : 70;\n");
    emit(o, "    Uint8 bg = e->hovered ? 140 : 100;\n");
    emit(o, "    Uint8 bb = e->hovered ? 255 : 220;\n");
    emit(o, "    SDL_Rect rb = { e->x, e->y, e->w, e->h };\n");
    emit(o, "    SDL_SetRenderDrawColor(r, br, bg, bb, 255);\n");
    emit(o, "    SDL_RenderFillRect(r, &rb);\n");
    emit(o, "    /* border */\n");
    emit(o, "    SDL_SetRenderDrawColor(r, 40, 60, 160, 255);\n");
    emit(o, "    SDL_RenderDrawRect(r, &rb);\n");
    emit(o, "    /* label: white text, centred */\n");
    emit(o, "    int tw = ns_text_width(e->label);\n");
    emit(o, "    int tx = e->x + (e->w - tw) / 2;\n");
    emit(o, "    int ty = e->y + (e->h - 8) / 2;\n");
    emit(o, "    ns_draw_text(r, tx, ty, e->label, 255, 255, 255);\n");
    emit(o, "}\n\n");

    emit(o, "static void ns_render_input(SDL_Renderer *r, NSUIElem *e) {\n");
    emit(o, "    /* background */\n");
    emit(o, "    Uint8 abr = e->focused ? 28 : 22;\n");
    emit(o, "    SDL_Rect rb = { e->x, e->y, e->w, e->h };\n");
    emit(o, "    SDL_SetRenderDrawColor(r, abr, abr, abr + 12, 255);\n");
    emit(o, "    SDL_RenderFillRect(r, &rb);\n");
    emit(o, "    /* border: brighter when focused */\n");
    emit(o, "    if (e->focused)\n");
    emit(o, "        SDL_SetRenderDrawColor(r, 100, 140, 255, 255);\n");
    emit(o, "    else\n");
    emit(o, "        SDL_SetRenderDrawColor(r, 80, 80, 100, 255);\n");
    emit(o, "    SDL_RenderDrawRect(r, &rb);\n");
    emit(o, "    /* placeholder or typed text */\n");
    emit(o, "    int tx = e->x + 6;\n");
    emit(o, "    int ty = e->y + (e->h - 8) / 2;\n");
    emit(o, "    if (e->input_len > 0) {\n");
    emit(o, "        ns_draw_text(r, tx, ty, e->input_buf, 230, 230, 230);\n");
    emit(o, "    } else if (e->label && e->label[0]) {\n");
    emit(o, "        ns_draw_text(r, tx, ty, e->label, 100, 100, 120);\n");
    emit(o, "    }\n");
    emit(o, "    /* blinking cursor when focused */\n");
    emit(o, "    if (e->focused && (SDL_GetTicks() / 500) %% 2 == 0) {\n");
    emit(o, "        int cx = tx + e->cursor * 9;\n");
    emit(o, "        SDL_SetRenderDrawColor(r, 200, 200, 255, 255);\n");
    emit(o, "        SDL_RenderDrawLine(r, cx, ty, cx, ty + 8);\n");
    emit(o, "    }\n");
    emit(o, "}\n\n");

    /* ── input text editing helpers ── */
    emit(o, "static void ns_input_insert(NSUIElem *e, const char *text) {\n");
    emit(o, "    for (const char *p = text; *p; p++) {\n");
    emit(o, "        if (e->input_len >= 511) break;\n");
    emit(o, "        /* shift right from cursor */\n");
    emit(o, "        for (int i = e->input_len; i > e->cursor; i--)\n");
    emit(o, "            e->input_buf[i] = e->input_buf[i-1];\n");
    emit(o, "        e->input_buf[e->cursor] = *p;\n");
    emit(o, "        e->cursor++;\n");
    emit(o, "        e->input_len++;\n");
    emit(o, "        e->input_buf[e->input_len] = '\\0';\n");
    emit(o, "    }\n");
    emit(o, "}\n\n");

    emit(o, "static void ns_input_backspace(NSUIElem *e) {\n");
    emit(o, "    if (e->cursor <= 0 || e->input_len <= 0) return;\n");
    emit(o, "    for (int i = e->cursor - 1; i < e->input_len - 1; i++)\n");
    emit(o, "        e->input_buf[i] = e->input_buf[i+1];\n");
    emit(o, "    e->cursor--;\n");
    emit(o, "    e->input_len--;\n");
    emit(o, "    e->input_buf[e->input_len] = '\\0';\n");
    emit(o, "}\n\n");

    emit(o, "static void ns_input_delete(NSUIElem *e) {\n");
    emit(o, "    if (e->cursor >= e->input_len) return;\n");
    emit(o, "    for (int i = e->cursor; i < e->input_len - 1; i++)\n");
    emit(o, "        e->input_buf[i] = e->input_buf[i+1];\n");
    emit(o, "    e->input_len--;\n");
    emit(o, "    e->input_buf[e->input_len] = '\\0';\n");
    emit(o, "}\n\n");

    /* ── focus management ── */
    emit(o, "static void ns_set_focus(int new_idx) {\n");
    emit(o, "    if (ns_focused_idx >= 0 && ns_focused_idx < NS_ELEM_COUNT)\n");
    emit(o, "        ns_ui_elems[ns_focused_idx].focused = 0;\n");
    emit(o, "    ns_focused_idx = new_idx;\n");
    emit(o, "    if (new_idx >= 0 && new_idx < NS_ELEM_COUNT)\n");
    emit(o, "        ns_ui_elems[new_idx].focused = 1;\n");
    emit(o, "}\n\n");

    /* ── main() ── */
    emit(o, "int main(void) {\n");
    emit(o, "    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;\n");
    emit(o, "    SDL_Window *ns_win = SDL_CreateWindow(\"%s\",\n", title);
    emit(o, "        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,\n");
    emit(o, "        %d, %d, SDL_WINDOW_SHOWN);\n", win_w, win_h);
    emit(o, "    if (!ns_win) { SDL_Quit(); return 1; }\n");
    emit(o, "    SDL_Renderer *ns_rend = SDL_CreateRenderer(\n");
    emit(o, "        ns_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);\n");
    emit(o, "    if (!ns_rend) { SDL_DestroyWindow(ns_win); SDL_Quit(); return 1; }\n");
    emit(o, "    SDL_StartTextInput();\n\n");

    emit(o, "    int ns_running = 1;\n");
    emit(o, "    while (ns_running) {\n");

    /* event loop */
    emit(o, "        SDL_Event ns_ev;\n");
    emit(o, "        while (SDL_PollEvent(&ns_ev)) {\n");
    emit(o, "            switch (ns_ev.type) {\n");

    /* quit */
    emit(o, "            case SDL_QUIT:\n");
    emit(o, "                ns_running = 0;\n");
    emit(o, "                break;\n");

    /* mouse motion: hover detection */
    emit(o, "            case SDL_MOUSEMOTION: {\n");
    emit(o, "                int mx = ns_ev.motion.x, my = ns_ev.motion.y;\n");
    emit(o, "                ns_hovered_idx = -1;\n");
    emit(o, "                for (int i = 0; i < NS_ELEM_COUNT; i++) {\n");
    emit(o, "                    NSUIElem *e = &ns_ui_elems[i];\n");
    emit(o, "                    e->hovered = 0;\n");
    emit(o, "                    if (mx >= e->x && mx < e->x+e->w &&\n");
    emit(o, "                        my >= e->y && my < e->y+e->h) {\n");
    emit(o, "                        e->hovered = 1;\n");
    emit(o, "                        ns_hovered_idx = i;\n");
    emit(o, "                    }\n");
    emit(o, "                }\n");
    emit(o, "                break;\n");
    emit(o, "            }\n");

    /* mouse button down: click + focus */
    emit(o, "            case SDL_MOUSEBUTTONDOWN:\n");
    emit(o, "                if (ns_ev.button.button == SDL_BUTTON_LEFT) {\n");
    emit(o, "                    int mx = ns_ev.button.x, my = ns_ev.button.y;\n");
    emit(o, "                    int hit = -1;\n");
    emit(o, "                    for (int i = 0; i < NS_ELEM_COUNT; i++) {\n");
    emit(o, "                        NSUIElem *e = &ns_ui_elems[i];\n");
    emit(o, "                        if (mx >= e->x && mx < e->x+e->w &&\n");
    emit(o, "                            my >= e->y && my < e->y+e->h) {\n");
    emit(o, "                            hit = i;\n");
    emit(o, "                            if (e->kind == NS_ELEM_INPUT)\n");
    emit(o, "                                ns_set_focus(i);\n");
    emit(o, "                            if (e->on_click)\n");
    emit(o, "                                e->on_click();\n");
    emit(o, "                        }\n");
    emit(o, "                    }\n");
    emit(o, "                    /* click outside input clears focus */\n");
    emit(o, "                    if (hit < 0 || ns_ui_elems[hit].kind != NS_ELEM_INPUT)\n");
    emit(o, "                        ns_set_focus(-1);\n");
    emit(o, "                }\n");
    emit(o, "                break;\n");

    /* SDL_TEXTINPUT: printable text into focused input */
    emit(o, "            case SDL_TEXTINPUT:\n");
    emit(o, "                if (ns_focused_idx >= 0) {\n");
    emit(o, "                    NSUIElem *fe = &ns_ui_elems[ns_focused_idx];\n");
    emit(o, "                    ns_input_insert(fe, ns_ev.text.text);\n");
    emit(o, "                    ns_changed_idx = ns_focused_idx;\n");
    emit(o, "                    if (fe->on_change) fe->on_change();\n");
    emit(o, "                }\n");
    emit(o, "                break;\n");

    /* SDL_KEYDOWN: backspace/delete/arrows + onKey dispatch */
    emit(o, "            case SDL_KEYDOWN: {\n");
    emit(o, "                SDL_Keycode kc = ns_ev.key.keysym.sym;\n");
    emit(o, "                ns_last_key = (int)kc;\n");
    emit(o, "                if (ns_focused_idx >= 0) {\n");
    emit(o, "                    NSUIElem *fe = &ns_ui_elems[ns_focused_idx];\n");
    emit(o, "                    if (kc == SDLK_BACKSPACE) {\n");
    emit(o, "                        ns_input_backspace(fe);\n");
    emit(o, "                        ns_changed_idx = ns_focused_idx;\n");
    emit(o, "                        if (fe->on_change) fe->on_change();\n");
    emit(o, "                    } else if (kc == SDLK_DELETE) {\n");
    emit(o, "                        ns_input_delete(fe);\n");
    emit(o, "                        ns_changed_idx = ns_focused_idx;\n");
    emit(o, "                        if (fe->on_change) fe->on_change();\n");
    emit(o, "                    } else if (kc == SDLK_LEFT && fe->cursor > 0) {\n");
    emit(o, "                        fe->cursor--;\n");
    emit(o, "                    } else if (kc == SDLK_RIGHT && fe->cursor < fe->input_len) {\n");
    emit(o, "                        fe->cursor++;\n");
    emit(o, "                    } else if (kc == SDLK_HOME) {\n");
    emit(o, "                        fe->cursor = 0;\n");
    emit(o, "                    } else if (kc == SDLK_END) {\n");
    emit(o, "                        fe->cursor = fe->input_len;\n");
    emit(o, "                    }\n");
    emit(o, "                    /* fire onKey for focused input */\n");
    emit(o, "                    if (fe->on_key) fe->on_key();\n");
    emit(o, "                } else {\n");
    emit(o, "                    /* no focus: fire onKey on any element that has it */\n");
    emit(o, "                    for (int i = 0; i < NS_ELEM_COUNT; i++)\n");
    emit(o, "                        if (ns_ui_elems[i].on_key) ns_ui_elems[i].on_key();\n");
    emit(o, "                }\n");
    emit(o, "                break;\n");
    emit(o, "            }\n");

    /* tab: cycle focus between input elements */
    emit(o, "            default: break;\n");
    emit(o, "            } /* switch */\n");
    emit(o, "        } /* PollEvent */\n\n");

    /* render */
    emit(o, "        /* ── render ── */\n");
    emit(o, "        SDL_SetRenderDrawColor(ns_rend, 22, 22, 28, 255);\n");
    emit(o, "        SDL_RenderClear(ns_rend);\n\n");
    emit(o, "        for (int i = 0; i < NS_ELEM_COUNT; i++) {\n");
    emit(o, "            NSUIElem *e = &ns_ui_elems[i];\n");
    emit(o, "            switch (e->kind) {\n");
    emit(o, "            case NS_ELEM_LABEL:  ns_render_label(ns_rend, e);  break;\n");
    emit(o, "            case NS_ELEM_BUTTON: ns_render_button(ns_rend, e); break;\n");
    emit(o, "            case NS_ELEM_INPUT:  ns_render_input(ns_rend, e);  break;\n");
    emit(o, "            default: break;\n");
    emit(o, "            }\n");
    emit(o, "        }\n\n");
    emit(o, "        SDL_RenderPresent(ns_rend);\n");
    emit(o, "        SDL_Delay(16);\n");
    emit(o, "    } /* main loop */\n\n");
    emit(o, "    SDL_StopTextInput();\n");
    emit(o, "    SDL_DestroyRenderer(ns_rend);\n");
    emit(o, "    SDL_DestroyWindow(ns_win);\n");
    emit(o, "    SDL_Quit();\n");
    emit(o, "    return 0;\n");
    emit(o, "}\n");
}

/* ── kernel app code generation (v0.5) ─────────────────────────────────── */

static int has_kernel_app(Node *prog) {
    for (int i = 0; i < prog->as.program.decls.count; i++)
        if (prog->as.program.decls.items[i]->kind == NODE_KERNEL_APP)
            return 1;
    return 0;
}

static void emit_kernel_app(COut *o, Node *prog) {
    Node *app = NULL;
    for (int i = 0; i < prog->as.program.decls.count; i++) {
        if (prog->as.program.decls.items[i]->kind == NODE_KERNEL_APP) {
            app = prog->as.program.decls.items[i];
            break;
        }
    }
    if (!app) return;

    emit(o, "\n/* ── NightScript Kernel Runtime (v0.5) ────────────────── */\n");
    emit(o, "/* Multiboot2 header */\n");
    emit(o, "#define NS_MB2_MAGIC    0xe85250d6u\n");
    emit(o, "#define NS_MB2_ARCH_X86 0u\n");
    emit(o, "__attribute__((section(\".multiboot2\"), used))\n");
    emit(o, "static unsigned int ns_mb2_header[] = {\n");
    emit(o, "    NS_MB2_MAGIC, NS_MB2_ARCH_X86,\n");
    emit(o, "    24u,   /* header length */\n");
    emit(o, "    (unsigned int)(-(NS_MB2_MAGIC + NS_MB2_ARCH_X86 + 24u)),\n");
    emit(o, "    0u, 0u, 8u  /* end tag */\n");
    emit(o, "};\n\n");

    emit(o, "/* VGA text-mode helpers */\n");
    emit(o, "#define NS_VGA_BASE ((volatile unsigned short *)0xB8000)\n");
    emit(o, "static int ns_vga_row = 0, ns_vga_col = 0;\n");
    emit(o, "static void ns_vga_clear(void) {\n");
    emit(o, "    for (int i = 0; i < 80*25; i++)\n");
    emit(o, "        NS_VGA_BASE[i] = (unsigned short)(0x0F00 | ' ');\n");
    emit(o, "    ns_vga_row = ns_vga_col = 0;\n");
    emit(o, "}\n");
    emit(o, "static void ns_vga_putchar(char c) {\n");
    emit(o, "    if (c == '\\n') { ns_vga_col = 0; ns_vga_row++; return; }\n");
    emit(o, "    if (ns_vga_col >= 80) { ns_vga_col = 0; ns_vga_row++; }\n");
    emit(o, "    if (ns_vga_row >= 25) ns_vga_row = 0;\n");
    emit(o, "    NS_VGA_BASE[ns_vga_row * 80 + ns_vga_col] =\n");
    emit(o, "        (unsigned short)(0x0F00 | (unsigned char)c);\n");
    emit(o, "    ns_vga_col++;\n");
    emit(o, "}\n");
    emit(o, "static void ns_vga_print(const char *s) {\n");
    emit(o, "    while (*s) ns_vga_putchar(*s++);\n");
    emit(o, "}\n\n");

    emit(o, "/* Serial port (COM1 0x3F8) helpers */\n");
    emit(o, "#define NS_SERIAL_PORT 0x3F8\n");
    emit(o, "static void ns_outb(unsigned short port, unsigned char val) {\n");
    emit(o, "    __asm__ volatile(\"outb %%al, %%dx\" : : \"a\"(val), \"d\"(port));\n");
    emit(o, "}\n");
    emit(o, "static void ns_serial_init(void) {\n");
    emit(o, "    ns_outb(NS_SERIAL_PORT + 1, 0x00);\n");
    emit(o, "    ns_outb(NS_SERIAL_PORT + 3, 0x80);\n");
    emit(o, "    ns_outb(NS_SERIAL_PORT + 0, 0x03);\n");
    emit(o, "    ns_outb(NS_SERIAL_PORT + 1, 0x00);\n");
    emit(o, "    ns_outb(NS_SERIAL_PORT + 3, 0x03);\n");
    emit(o, "    ns_outb(NS_SERIAL_PORT + 2, 0xC7);\n");
    emit(o, "    ns_outb(NS_SERIAL_PORT + 4, 0x0B);\n");
    emit(o, "}\n");
    emit(o, "static void ns_serial_putchar(char c) {\n");
    emit(o, "    ns_outb(NS_SERIAL_PORT, (unsigned char)c);\n");
    emit(o, "}\n");
    emit(o, "static void ns_serial_print(const char *s) {\n");
    emit(o, "    while (*s) ns_serial_putchar(*s++);\n");
    emit(o, "}\n\n");

    emit(o, "/* Kernel entry point — called by bootloader */\n");
    emit(o, "__attribute__((noreturn))\n");
    emit(o, "void kernel_main(void) {\n");
    emit(o, "    ns_vga_clear();\n");
    emit(o, "    ns_serial_init();\n");
    emit(o, "    main();\n");
    emit(o, "    for (;;) __asm__ volatile(\"hlt\");\n");
    emit(o, "    __builtin_unreachable();\n");
    emit(o, "}\n");
}

/* ── main entry ────────────────────────────────────────────────────────── */

int codegen_generate(Node *program, COut *out) {
    out->buf    = malloc(4096);
    out->len    = 0;
    out->cap    = 4096;
    out->indent = 0;
    out->buf[0] = '\0';

    int is_kernel = has_kernel_app(program);
    if (!is_kernel)
        emit_standard_headers(out, program);
    else
        emit(out, "/* NightScript Kernel — freestanding (no libc) */\n\n");
    if (has_ui_app(program))
        emit(out, "#include <SDL2/SDL.h>\n\n");
    emit_type_definitions(out, program);
    if (!is_kernel)
        emit_io_runtime(out);
    emit_prototypes(out, program);
    emit_definitions(out, program);
    if (has_ui_app(program))
        emit_ui_app(out, program);
    if (is_kernel)
        emit_kernel_app(out, program);

    return 1;
}

void cout_free(COut *out) {
    free(out->buf);
    out->buf = NULL;
    out->len = out->cap = 0;
}
