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
            else if (!strcmp(name, "str"))   emit(o, "const char*");
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
    return find_decl_named(cg->program, NODE_STRUCT_DECL, name) ||
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
            (expr->kind == NODE_UNARY && expr->as.unary.op && expr->as.unary.op[0] == '*'));
}

static Node *infer_expr_type(CGContext *cg, Node *expr);
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

static void emit_match_condition(COut *o, CGContext *cg, Node *subject, const char *pattern) {
    const char *dot = strchr(pattern, '.');

    if (!strcmp(pattern, "Some")) {
        emit_expr(o, cg, subject);
        emit(o, ".is_some");
        return;
    }

    if (!strcmp(pattern, "None")) {
        emit(o, "!(");
        emit_expr(o, cg, subject);
        emit(o, ".is_some)");
        return;
    }

    if (!strcmp(pattern, "Ok")) {
        emit_expr(o, cg, subject);
        emit(o, ".is_ok");
        return;
    }

    if (!strcmp(pattern, "Err")) {
        emit(o, "!(");
        emit_expr(o, cg, subject);
        emit(o, ".is_ok)");
        return;
    }

    if (!dot) {
        emit(o, "0");
        return;
    }

    char enum_name[128];
    int elen = (int)(dot - pattern);
    if (elen >= 127) elen = 127;
    memcpy(enum_name, pattern, (size_t)elen);
    enum_name[elen] = '\0';

    Node *decl = find_decl_named(cg->program, NODE_ENUM_DECL, enum_name);
    int is_data = decl && enum_is_data_carrying(decl);

    emit_expr(o, cg, subject);
    if (is_data)
        emit(o, ".tag == %s_%s", enum_name, dot + 1);
    else
        emit(o, " == %s_%s", enum_name, dot + 1);
}

static void emit_match_expr(COut *o, CGContext *cg, Node *match) {
    int emitted_fallback = 0;

    if (match->as.match.count == 0) {
        emit(o, "0");
        return;
    }

    emit(o, "(");
    for (int i = 0; i < match->as.match.count; i++) {
        const char *pattern = match->as.match.patterns[i];

        if (!strcmp(pattern, "_")) {
            emit_expr(o, cg, match->as.match.values[i]);
            emitted_fallback = 1;
            break;
        }

        emit_match_condition(o, cg, match->as.match.subject, pattern);
        emit(o, " ? ");
        emit_expr(o, cg, match->as.match.values[i]);
        emit(o, " : ");
    }

    if (!emitted_fallback)
        emit(o, "0");

    emit(o, ")");
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

            const char *owner_name = type_named_name(object_type);
            if (!owner_name && object_type->kind == NODE_TYPE_POINTER)
                owner_name = type_named_name(object_type->as.type_ptr.inner);

            return owner_name ? find_field_type(cg, owner_name, expr->as.field.field) : NULL;
        }
        case NODE_MATCH:
            if (expr->as.match.count > 0)
                return infer_expr_type(cg, expr->as.match.values[0]);
            return NULL;
        case NODE_INDEX: {
            Node *obj_type = infer_expr_type(cg, expr->as.index_expr.object);
            if (!obj_type) return NULL;
            if (obj_type->kind == NODE_TYPE_ARRAY)
                return obj_type->as.type_array.elem;
            if (obj_type->kind == NODE_TYPE_POINTER)
                return obj_type->as.type_ptr.inner;
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
            emit(o, "\"%s\"", n->as.lit_str.value);
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
                emit(o, "/*try?*/0");
                break;
            }
            /* dereference: *ptr → (*ptr) */
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
                emit(o, "%s_%s", obj->as.ident.name, n->as.field.field);
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
            } else {
                emit_expr(o, cg, n->as.index_expr.object);
                emit(o, "[");
                emit_expr(o, cg, n->as.index_expr.index);
                emit(o, "]");
            }
            break;
        }
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
            emit_match_expr(o, cg, n);
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

    return NULL;
}

/* ── top-level passes ──────────────────────────────────────────────────── */

static void emit_standard_headers(COut *o, Node *prog) {
    int need_stdbool = 1;
    int need_stddef = 1;
    int need_stdint = 1;
    int need_stdio = 0;
    int need_stdlib = 0;
    int need_string = 0;

    (void)need_stdbool;
    (void)need_stddef;
    (void)need_stdint;

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
    if (need_stdlib) emit(o, "#include <stdlib.h>\n");
    if (need_string) emit(o, "#include <string.h>\n");
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

        if (d->kind == NODE_EXTERN_FN) {
            if (header_for_extern(d->as.extern_fn.name))
                continue;
            make_c_name(cname, sizeof(cname), NULL, d->as.extern_fn.name);
            emit_fn_sig(o, cname, &d->as.extern_fn.params, d->as.extern_fn.ret_type);
            emit(o, ";\n");
        }

        if (d->kind == NODE_FN_DECL) {
            make_c_name(cname, sizeof(cname), d->as.fn.owner_type, d->as.fn.name);
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
            emit(o, "\n");
            emit_fn_sig(o, cname, &d->as.fn.params, d->as.fn.ret_type);
            emit(o, "\n");
            cg.defer_count = 0; /* reset defers per function */
            cg.current_return_type = d->as.fn.ret_type;
            cg_push_scope(&cg);
            for (int j = 0; j < d->as.fn.params.count; j++) {
                Node *param = d->as.fn.params.items[j];
                cg_define(&cg, param->as.let.name, param->as.let.type);
            }
            emit_block(o, &cg, d->as.fn.body);
            cg_pop_scope(&cg);
            emit(o, "\n");
        }

        if (d->kind == NODE_IMPL_DECL) {
            for (int j = 0; j < d->as.impl.methods.count; j++) {
                Node *m = d->as.impl.methods.items[j];
                make_c_name(cname, sizeof(cname), d->as.impl.target, m->as.fn.name);
                emit(o, "\n");
                emit_fn_sig(o, cname, &m->as.fn.params, m->as.fn.ret_type);
                emit(o, "\n");
                cg.defer_count = 0; /* reset defers per function */
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
    }
    cg_free_temps(&cg);
    free(cg.defers);
    free(cg.scope_defer_markers.items);
    free(cg.loop_defer_markers.items);
}

/* ── main entry ────────────────────────────────────────────────────────── */

int codegen_generate(Node *program, COut *out) {
    out->buf    = malloc(4096);
    out->len    = 0;
    out->cap    = 4096;
    out->indent = 0;
    out->buf[0] = '\0';

    emit_standard_headers(out, program);
    emit_type_definitions(out, program);
    emit_prototypes(out, program);
    emit_definitions(out, program);

    return 1;
}

void cout_free(COut *out) {
    free(out->buf);
    out->buf = NULL;
    out->len = out->cap = 0;
}
