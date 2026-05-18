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

/* ── v0.7 Generic & Interface state ────────────────────────────────────── */

/* Active type-param substitution context (set during monomorphization) */
static const char *g_sub_params[8];
static const char *g_sub_types[8];
static int         g_sub_count = 0;

/* Generic function registry (templates found in the program) */
static Node  *g_gfn_decls[64];
static char  *g_gfn_names[64];
static int    g_gfn_count = 0;

/* Generic struct registry */
static Node  *g_gst_decls[64];
static char  *g_gst_names[64];
static int    g_gst_count = 0;

typedef struct GenFnInst GenFnInst;
struct GenFnInst {
    Node        *decl;
    char         mangled[256];   /* e.g. "max_i32" */
    const char  *c_type;         /* C type string, e.g. "int32_t" */
    const char  *ns_type;        /* NightScript type name, e.g. "i32" */
    GenFnInst   *next;
};
static GenFnInst *g_gfn_insts = NULL;

typedef struct GenStInst GenStInst;
struct GenStInst {
    Node        *decl;
    char         ns_name[256];  /* "Vec[i32]" */
    char         mangled[256];  /* "Vec_i32" */
    const char  *c_types[8];
    const char  *ns_types[8];
    int          type_count;
    GenStInst   *next;
};
static GenStInst *g_gst_insts = NULL;

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

/* ── v0.7 helper functions ──────────────────────────────────────────────── */

/* Map NightScript type name to C type string */
static const char *ns_to_c(const char *ns) {
    if (!ns) return "void";
    if (!strcmp(ns,"i8"))    return "int8_t";
    if (!strcmp(ns,"i16"))   return "int16_t";
    if (!strcmp(ns,"i32"))   return "int32_t";
    if (!strcmp(ns,"i64"))   return "int64_t";
    if (!strcmp(ns,"isize")) return "ptrdiff_t";
    if (!strcmp(ns,"u8"))    return "uint8_t";
    if (!strcmp(ns,"u16"))   return "uint16_t";
    if (!strcmp(ns,"u32"))   return "uint32_t";
    if (!strcmp(ns,"u64"))   return "uint64_t";
    if (!strcmp(ns,"usize")) return "size_t";
    if (!strcmp(ns,"f32"))   return "float";
    if (!strcmp(ns,"f64"))   return "double";
    if (!strcmp(ns,"bool"))  return "bool";
    if (!strcmp(ns,"char"))  return "char";
    if (!strcmp(ns,"void"))  return "void";
    if (!strcmp(ns,"str"))   return "NStr";
    if (!strcmp(ns,"cstr"))  return "const char*";
    return ns; /* user-defined type: return as-is */
}

/* Check if name is a type param in current substitution context */
static const char *gen_subst_lookup(const char *name) {
    for (int i = 0; i < g_sub_count; i++)
        if (!strcmp(name, g_sub_params[i])) return g_sub_types[i];
    return NULL;
}

/* Build mangled name: base + "_" + sanitized(type_arg) */
static void gen_mangle(char *out, size_t sz, const char *base, const char *type_arg) {
    size_t len = 0;
    /* copy base */
    while (*base && len < sz-2) out[len++] = *base++;
    out[len] = '\0';
    /* append _ + sanitized type_arg */
    if (type_arg && len < sz-2) {
        out[len++] = '_'; out[len] = '\0';
        while (*type_arg && len < sz-2) {
            char c = *type_arg++;
            if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')) out[len++]=c;
            else out[len++]='_';
        }
        out[len] = '\0';
    }
    /* strip trailing underscores */
    while (len > 0 && out[len-1]=='_') out[--len]='\0';
}

/* Format generic type name to C identifier: "Vec[i32]" → "Vec_i32" */
static void format_generic_mangled(const char *ns_name, char *buf, size_t sz) {
    buf[0] = '\0';
    append_sanitized(buf, sz, ns_name);
    /* strip trailing underscores */
    size_t len = strlen(buf);
    while (len > 0 && buf[len-1]=='_') buf[--len]='\0';
}

/* Find or register a generic function instantiation */
static GenFnInst *gen_fn_find_or_add(Node *decl, const char *fn_name,
                                      const char *c_type, const char *ns_type) {
    char mangled[256];
    gen_mangle(mangled, sizeof(mangled), fn_name, c_type);
    for (GenFnInst *it = g_gfn_insts; it; it = it->next)
        if (!strcmp(it->mangled, mangled)) return it;
    GenFnInst *inst = calloc(1, sizeof(GenFnInst));
    if (!inst) return NULL;
    inst->decl    = decl;
    inst->c_type  = c_type;
    inst->ns_type = ns_type;
    strncpy(inst->mangled, mangled, sizeof(inst->mangled)-1);
    inst->next    = g_gfn_insts;
    g_gfn_insts   = inst;
    return inst;
}

/* Find or register a generic struct instantiation */
static GenStInst *gen_st_find_or_add(Node *decl, const char *ns_name,
                                      const char **c_types, const char **ns_types, int n) {
    char mangled[256];
    format_generic_mangled(ns_name, mangled, sizeof(mangled));
    for (GenStInst *it = g_gst_insts; it; it = it->next)
        if (!strcmp(it->ns_name, ns_name)) return it;
    GenStInst *inst = calloc(1, sizeof(GenStInst));
    if (!inst) return NULL;
    inst->decl = decl;
    inst->type_count = n < 8 ? n : 8;
    strncpy(inst->ns_name, ns_name, sizeof(inst->ns_name)-1);
    strncpy(inst->mangled, mangled, sizeof(inst->mangled)-1);
    for (int i = 0; i < inst->type_count; i++) {
        inst->c_types[i]  = c_types[i];
        inst->ns_types[i] = ns_types[i];
    }
    inst->next    = g_gst_insts;
    g_gst_insts   = inst;
    return inst;
}

/* Extract inner type string from "Name[inner]" or "Name[a, b]" */
static char *gen_extract_inner(const char *ns_name) {
    const char *lb = strchr(ns_name, '[');
    if (!lb) return NULL;
    const char *rb = strrchr(ns_name, ']');
    if (!rb || rb <= lb+1) return NULL;
    size_t len = (size_t)(rb - lb - 1);
    char *inner = malloc(len+1);
    if (!inner) return NULL;
    memcpy(inner, lb+1, len);
    inner[len] = '\0';
    return inner;
}

/* Pre-scan a type node for generic struct instantiations */
static void prescan_type(Node *type) {
    if (!type) return;
    switch (type->kind) {
    case NODE_TYPE_NAMED: {
        const char *name = type->as.type_named.name;
        for (int gi = 0; gi < g_gst_count; gi++) {
            if (type_name_has_form(name, g_gst_names[gi])) {
                char *inner = gen_extract_inner(name);
                if (inner) {
                    const char *c_t  = ns_to_c(inner);
                    const char *ns_t = inner;
                    gen_st_find_or_add(g_gst_decls[gi], name, &c_t, &ns_t, 1);
                    free(inner);
                }
                break;
            }
        }
        break;
    }
    case NODE_TYPE_POINTER: prescan_type(type->as.type_ptr.inner); break;
    case NODE_TYPE_ARRAY:   prescan_type(type->as.type_array.elem); break;
    default: break;
    }
}

/* Pre-scan an expression node for generic function calls */
static void prescan_expr(Node *n);
static void prescan_stmt(Node *n);

static void prescan_expr(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case NODE_CALL:
        /* Detect fn[TypeArg](args) → NODE_CALL{callee=INDEX{obj=IDENT,idx=type}} */
        if (n->as.call.callee && n->as.call.callee->kind == NODE_INDEX) {
            Node *idx = n->as.call.callee;
            Node *obj = idx->as.index_expr.object;
            Node *ta  = idx->as.index_expr.index;
            if (obj && obj->kind == NODE_IDENT && ta) {
                const char *fn_name = obj->as.ident.name;
                for (int i = 0; i < g_gfn_count; i++) {
                    if (!strcmp(g_gfn_names[i], fn_name)) {
                        const char *ns_t = NULL;
                        if (ta->kind == NODE_IDENT)       ns_t = ta->as.ident.name;
                        else if (ta->kind == NODE_TYPE_NAMED) ns_t = ta->as.type_named.name;
                        if (ns_t)
                            gen_fn_find_or_add(g_gfn_decls[i], fn_name, ns_to_c(ns_t), ns_t);
                        break;
                    }
                }
            }
        }
        prescan_expr(n->as.call.callee);
        for (int i = 0; i < n->as.call.args.count; i++)
            prescan_expr(n->as.call.args.items[i]);
        break;
    case NODE_BINARY:
        prescan_expr(n->as.binary.left);
        prescan_expr(n->as.binary.right);
        break;
    case NODE_UNARY:
        prescan_expr(n->as.unary.operand);
        break;
    case NODE_ASSIGN:
        prescan_expr(n->as.assign.target);
        prescan_expr(n->as.assign.value);
        break;
    case NODE_FIELD:
        prescan_expr(n->as.field.object);
        break;
    case NODE_INDEX:
        prescan_expr(n->as.index_expr.object);
        prescan_expr(n->as.index_expr.index);
        break;
    case NODE_CAST:
        prescan_expr(n->as.cast.expr);
        break;
    default: break;
    }
}

static void prescan_stmt(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case NODE_BLOCK:
        for (int i = 0; i < n->as.block.stmts.count; i++)
            prescan_stmt(n->as.block.stmts.items[i]);
        break;
    case NODE_RETURN:    prescan_expr(n->as.ret.value); break;
    case NODE_LET:
        prescan_type(n->as.let.type);
        prescan_expr(n->as.let.value);
        break;
    case NODE_EXPR_STMT: prescan_expr(n->as.expr_stmt.expr); break;
    case NODE_DEFER:     prescan_expr(n->as.defer_stmt.expr); break;
    case NODE_IF:
        prescan_expr(n->as.if_stmt.cond);
        prescan_stmt(n->as.if_stmt.then_block);
        prescan_stmt(n->as.if_stmt.else_node);
        break;
    case NODE_WHILE:
        prescan_expr(n->as.while_stmt.cond);
        prescan_stmt(n->as.while_stmt.body);
        break;
    case NODE_FOR:
        prescan_stmt(n->as.for_stmt.init);
        prescan_expr(n->as.for_stmt.cond);
        prescan_expr(n->as.for_stmt.post);
        prescan_stmt(n->as.for_stmt.body);
        break;
    case NODE_LOOP:
    case NODE_UNSAFE:
        prescan_stmt(n->as.loop_stmt.body);
        break;
    default: break;
    }
}

static void prescan_program(Node *prog) {
    for (int i = 0; i < prog->as.program.decls.count; i++) {
        Node *d = prog->as.program.decls.items[i];
        if (d->kind == NODE_FN_DECL && d->as.fn.type_params.count == 0) {
            /* scan param types and return type for generic struct usage */
            for (int j = 0; j < d->as.fn.params.count; j++)
                prescan_type(d->as.fn.params.items[j]->as.let.type);
            prescan_type(d->as.fn.ret_type);
            if (d->as.fn.body) prescan_stmt(d->as.fn.body);
        } else if (d->kind == NODE_IMPL_DECL) {
            for (int j = 0; j < d->as.impl.methods.count; j++) {
                Node *m = d->as.impl.methods.items[j];
                for (int k = 0; k < m->as.fn.params.count; k++)
                    prescan_type(m->as.fn.params.items[k]->as.let.type);
                prescan_type(m->as.fn.ret_type);
                if (m->as.fn.body) prescan_stmt(m->as.fn.body);
            }
        }
    }
}

/* ── end v0.7 helpers ───────────────────────────────────────────────────── */

static void append_type_mangle(Node *type, char *buf, size_t size) {
    if (!type) {
        append_text(buf, size, "void");
        return;
    }

    switch (type->kind) {
        case NODE_TYPE_NAMED: {
            const char *tn = type->as.type_named.name;
            /* Check active generic substitution */
            const char *sub = gen_subst_lookup(tn);
            if (sub) { append_sanitized(buf, size, sub); return; }
            append_sanitized(buf, size, tn);
            return;
        }
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
            /* 1. Check generic type-param substitution */
            const char *sub = gen_subst_lookup(name);
            if (sub) { emit(o, "%s", sub); break; }
            /* 2. Primitive type mapping */
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
                /* 3. Check generic struct instantiation: "Vec[i32]" → "Vec_i32" */
                int is_gen_struct = 0;
                for (int gi = 0; gi < g_gst_count; gi++) {
                    if (type_name_has_form(name, g_gst_names[gi])) {
                        char mangled[256];
                        format_generic_mangled(name, mangled, sizeof(mangled));
                        emit(o, "%s", mangled);
                        /* Collect instantiation for later emission */
                        char *inner = gen_extract_inner(name);
                        if (inner) {
                            const char *c_t  = ns_to_c(inner);
                            const char *ns_t = inner; /* use as-is for ns type */
                            gen_st_find_or_add(g_gst_decls[gi], name, &c_t, &ns_t, 1);
                            free(inner);
                        }
                        is_gen_struct = 1;
                        break;
                    }
                }
                if (!is_gen_struct) emit(o, "%s", name);
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
        if (kind == NODE_STRUCT_DECL)    decl_name = decl->as.struct_decl.name;
        if (kind == NODE_ENUM_DECL)      decl_name = decl->as.enum_decl.name;
        if (kind == NODE_UNION_DECL)     decl_name = decl->as.union_decl.name;
        if (kind == NODE_INTERFACE_DECL) decl_name = decl->as.interface_decl.name;
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
        case NODE_LIT_STRING: {
            /* emit string value with proper C escape sequences */
            const char *sv = n->as.lit_str.value;
            if (expected_type && expected_type->kind == NODE_TYPE_NAMED &&
                !strcmp(expected_type->as.type_named.name, "str")) {
                emit(o, "((NStr){ .ptr = (const uint8_t*)\"");
                for (const char *cp = sv; *cp; cp++) {
                    unsigned char ch = (unsigned char)*cp;
                    if      (ch == '\n') emit(o, "\\n");
                    else if (ch == '\r') emit(o, "\\r");
                    else if (ch == '\t') emit(o, "\\t");
                    else if (ch == '\\') emit(o, "\\\\");
                    else if (ch == '"')  emit(o, "\\\"");
                    else                 emit(o, "%c", ch);
                }
                emit(o, "\", .len = %zu })", strlen(sv));
            } else {
                emit(o, "\"");
                for (const char *cp = sv; *cp; cp++) {
                    unsigned char ch = (unsigned char)*cp;
                    if      (ch == '\n') emit(o, "\\n");
                    else if (ch == '\r') emit(o, "\\r");
                    else if (ch == '\t') emit(o, "\\t");
                    else if (ch == '\\') emit(o, "\\\\");
                    else if (ch == '"')  emit(o, "\\\"");
                    else                 emit(o, "%c", ch);
                }
                emit(o, "\"");
            }
            break;
        }
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
                /* inline assembly: asm("instruction") → __asm__ volatile("instruction") */
                if (!strcmp(nm, "asm") && a0 && a0->kind == NODE_LIT_STRING) {
                    emit(o, "__asm__ volatile(\"");
                    for (const char *cp = a0->as.lit_str.value; *cp; cp++) {
                        if (*cp == '"') emit(o, "\\\"");
                        else emit(o, "%c", *cp);
                    }
                    emit(o, "\")");
                    break;
                }
                /* port I/O built-ins */
                if (!strcmp(nm, "inb"))  { emit(o, "ns_inb");  _IO_ARG0; break; }
                if (!strcmp(nm, "inw"))  { emit(o, "ns_inw");  _IO_ARG0; break; }
                if (!strcmp(nm, "inl"))  { emit(o, "ns_inl");  _IO_ARG0; break; }
                if (!strcmp(nm, "io_wait")) { emit(o, "ns_io_wait()"); break; }
                if (!strcmp(nm, "outb") && n->as.call.args.count == 2) {
                    emit(o, "ns_outb("); emit_expr(o, cg, n->as.call.args.items[0]);
                    emit(o, ", "); emit_expr(o, cg, n->as.call.args.items[1]); emit(o, ")"); break;
                }
                if (!strcmp(nm, "outw") && n->as.call.args.count == 2) {
                    emit(o, "ns_outw("); emit_expr(o, cg, n->as.call.args.items[0]);
                    emit(o, ", "); emit_expr(o, cg, n->as.call.args.items[1]); emit(o, ")"); break;
                }
                if (!strcmp(nm, "outl") && n->as.call.args.count == 2) {
                    emit(o, "ns_outl("); emit_expr(o, cg, n->as.call.args.items[0]);
                    emit(o, ", "); emit_expr(o, cg, n->as.call.args.items[1]); emit(o, ")"); break;
                }
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

            /* Generic fn[T](args) → mangled_fn(args) */
            if (n->as.call.callee && n->as.call.callee->kind == NODE_INDEX) {
                Node *idx_n = n->as.call.callee;
                Node *fn_obj = idx_n->as.index_expr.object;
                Node *type_arg = idx_n->as.index_expr.index;
                if (fn_obj && fn_obj->kind == NODE_IDENT && type_arg) {
                    int found_gfn = 0;
                    for (int gi = 0; gi < g_gfn_count; gi++) {
                        if (!strcmp(g_gfn_names[gi], fn_obj->as.ident.name)) {
                            found_gfn = 1; break;
                        }
                    }
                    if (found_gfn) {
                        const char *ns_t = NULL;
                        if (type_arg->kind == NODE_IDENT)
                            ns_t = type_arg->as.ident.name;
                        else if (type_arg->kind == NODE_TYPE_NAMED)
                            ns_t = type_arg->as.type_named.name;
                        if (ns_t) {
                            char mangled[256];
                            gen_mangle(mangled, sizeof(mangled),
                                       fn_obj->as.ident.name, ns_to_c(ns_t));
                            emit(o, "%s(", mangled);
                            for (int ai = 0; ai < n->as.call.args.count; ai++) {
                                if (ai) emit(o, ", ");
                                emit_expr(o, cg, n->as.call.args.items[ai]);
                            }
                            emit(o, ")");
                            break;
                        }
                    }
                }
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
            const char *slt_name = n->as.struct_lit.type_name;
            int slt_generic = 0;
            for (int gi = 0; gi < g_gst_count; gi++) {
                if (type_name_has_form(slt_name, g_gst_names[gi])) {
                    char slt_mangled[256];
                    format_generic_mangled(slt_name, slt_mangled, sizeof(slt_mangled));
                    emit(o, "(%s){", slt_mangled);
                    slt_generic = 1;
                    break;
                }
            }
            if (!slt_generic)
                emit(o, "(%s){", slt_name);
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

    /* POSIX/XSI extensions — must come before any system header.
     * _XOPEN_SOURCE 700 enables POSIX.1-2008 + XSI (exposes realpath,
     * nanosleep, etc. even under -std=c11). */
    emit(o, "#ifndef _XOPEN_SOURCE\n#define _XOPEN_SOURCE 700\n#endif\n");
    emit(o, "#ifndef _POSIX_C_SOURCE\n#define _POSIX_C_SOURCE 200809L\n#endif\n");
    emit(o, "#include <stdbool.h>\n");
    emit(o, "#include <stddef.h>\n");
    emit(o, "#include <stdint.h>\n");
    if (need_stdio) emit(o, "#include <stdio.h>\n");
    emit(o, "#include <stdlib.h>\n");
    emit(o, "#include <string.h>\n");
    emit(o, "\n");
}

/* emit one monomorphized generic function body */
static void emit_gen_fn_inst(COut *o, CGContext *cg_parent, GenFnInst *inst) {
    Node *gd = inst->decl;
    if (!gd->as.fn.body) return;
    int tp_count = gd->as.fn.type_params.count;
    g_sub_count = tp_count < 8 ? tp_count : 8;
    for (int i = 0; i < g_sub_count && i < 1; i++) {
        g_sub_params[i] = gd->as.fn.type_params.items[i]->as.type_named.name;
        g_sub_types[i]  = inst->c_type;
    }
    emit(o, "\n");
    emit_fn_sig(o, inst->mangled, &gd->as.fn.params, gd->as.fn.ret_type);
    emit(o, "\n");
    CGContext cg;
    memset(&cg, 0, sizeof(cg));
    cg.program = cg_parent->program;
    cg.defer_count = 0;
    cg.current_return_type = gd->as.fn.ret_type;
    cg_push_scope(&cg);
    for (int j = 0; j < gd->as.fn.params.count; j++) {
        Node *param = gd->as.fn.params.items[j];
        cg_define(&cg, param->as.let.name, param->as.let.type);
    }
    emit_block(o, &cg, gd->as.fn.body);
    cg_pop_scope(&cg);
    cg_free_temps(&cg);
    free(cg.defers);
    free(cg.scope_defer_markers.items);
    free(cg.loop_defer_markers.items);
    emit(o, "\n");
    g_sub_count = 0;
}

/* pass 1: struct / enum / union definitions */
static void emit_type_definitions(COut *o, Node *prog) {
    SliceDef *slice_defs = NULL;
    OptionResultDef *option_result_defs = NULL;
    int emitted_forward = 0;

    for (int i = 0; i < prog->as.program.decls.count; i++) {
        Node *d = prog->as.program.decls.items[i];

        if (d->kind == NODE_STRUCT_DECL) {
            if (d->as.struct_decl.type_params.count > 0) continue; /* skip generic template */
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

        if (d->kind == NODE_INTERFACE_DECL) {
            emit(o, "typedef struct %s_vtable %s_vtable;\n",
                 d->as.interface_decl.name, d->as.interface_decl.name);
            emit(o, "typedef struct %s %s;\n",
                 d->as.interface_decl.name, d->as.interface_decl.name);
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
            if (d->as.struct_decl.type_params.count > 0) continue; /* skip generic template */
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

    /* interface vtable structs + fat pointer structs */
    for (int i = 0; i < prog->as.program.decls.count; i++) {
        Node *d = prog->as.program.decls.items[i];
        if (d->kind != NODE_INTERFACE_DECL) continue;
        const char *iname = d->as.interface_decl.name;
        emit(o, "struct %s_vtable {\n", iname);
        for (int j = 0; j < d->as.interface_decl.methods.count; j++) {
            Node *m = d->as.interface_decl.methods.items[j];
            emit(o, "    ");
            emit_type_left(o, m->as.fn.ret_type);
            emit(o, " (*%s)(void*", m->as.fn.name);
            for (int k = 1; k < m->as.fn.params.count; k++) {
                Node *p = m->as.fn.params.items[k];
                emit(o, ", ");
                emit_typed_name(o, p->as.let.type, p->as.let.name);
            }
            emit(o, ");\n");
        }
        emit(o, "};\n\n");
        emit(o, "struct %s {\n", iname);
        emit(o, "    void *data;\n");
        emit(o, "    const %s_vtable *vtbl;\n", iname);
        emit(o, "};\n\n");
    }

    /* generic struct instantiations */
    for (GenStInst *gi = g_gst_insts; gi; gi = gi->next) {
        Node *gd = gi->decl;
        int tp_count = gd->as.struct_decl.type_params.count;
        g_sub_count = gi->type_count < tp_count ? gi->type_count : tp_count;
        for (int i = 0; i < g_sub_count; i++) {
            g_sub_params[i] = gd->as.struct_decl.type_params.items[i]->as.type_named.name;
            g_sub_types[i]  = gi->c_types[i];
        }
        emit(o, "typedef struct %s %s;\n", gi->mangled, gi->mangled);
        emit(o, "struct %s {\n", gi->mangled);
        for (int j = 0; j < gd->as.struct_decl.fields.count; j++) {
            Node *f = gd->as.struct_decl.fields.items[j];
            emit(o, "    ");
            emit_typed_name(o, f->as.let.type, f->as.let.name);
            emit(o, ";\n");
        }
        if (gd->as.struct_decl.is_packed)
            emit(o, "} __attribute__((packed));\n\n");
        else
            emit(o, "};\n\n");
        g_sub_count = 0;
    }
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
            if (has_kernel_app(prog)) continue; /* kernel runtime defines these as static */
            if (header_for_extern(d->as.extern_fn.name))
                continue;
            make_c_name(cname, sizeof(cname), NULL, d->as.extern_fn.name);
            emit_fn_sig(o, cname, &d->as.extern_fn.params, d->as.extern_fn.ret_type);
            emit(o, ";\n");
        }

        if (d->kind == NODE_FN_DECL) {
            if (d->as.fn.type_params.count > 0) continue; /* skip generic template */
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
            /* coercion fn prototype for interface impl */
            if (d->as.impl.interface_name) {
                emit(o, "%s %s_as_%s(%s *self);\n",
                     d->as.impl.interface_name, d->as.impl.target,
                     d->as.impl.interface_name, d->as.impl.target);
            }
        }

        if (d->kind == NODE_COMPTIME) {
            for (int j = 0; j < d->as.comptime_block.decls.count; j++) {
                Node *cn = d->as.comptime_block.decls.items[j];
                if (cn->kind != NODE_CONST) continue;
                COut tmp;
                tmp.buf = malloc(256); tmp.len = 0; tmp.cap = 256; tmp.indent = 0;
                if (tmp.buf) {
                    tmp.buf[0] = '\0';
                    CGContext cg0 = { .program = prog };
                    emit_expr(&tmp, &cg0, cn->as.konst.value);
                    emit(o, "#define %s (%s)\n", cn->as.konst.name, tmp.buf);
                    free(tmp.buf);
                }
            }
        }
    }
    /* forward decls for generic fn instantiations */
    for (GenFnInst *gi = g_gfn_insts; gi; gi = gi->next) {
        Node *gd = gi->decl;
        int tp_count = gd->as.fn.type_params.count;
        g_sub_count = tp_count < 8 ? tp_count : 8;
        for (int i = 0; i < g_sub_count && i < 1; i++) {
            g_sub_params[i] = gd->as.fn.type_params.items[i]->as.type_named.name;
            g_sub_types[i]  = gi->c_type;
        }
        emit_fn_sig(o, gi->mangled, &gd->as.fn.params, gd->as.fn.ret_type);
        emit(o, ";\n");
        g_sub_count = 0;
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
            if (d->as.fn.type_params.count > 0) continue; /* skip generic template */
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
            /* interface impl: emit vtable instance + coercion fn */
            if (d->as.impl.interface_name) {
                Node *iface = find_decl_named(cg.program, NODE_INTERFACE_DECL,
                                              d->as.impl.interface_name);
                if (iface) {
                    const char *iname = d->as.impl.interface_name;
                    const char *tname = d->as.impl.target;
                    emit(o, "\nstatic const %s_vtable %s_%s_vtable = {\n", iname, tname, iname);
                    for (int j = 0; j < iface->as.interface_decl.methods.count; j++) {
                        Node *m = iface->as.interface_decl.methods.items[j];
                        /* cast to the actual vtable fn ptr type, matching the struct field */
                        emit(o, "    .%s = (", m->as.fn.name);
                        emit_type_left(o, m->as.fn.ret_type);
                        emit(o, " (*)(void*))%s_%s,\n", tname, m->as.fn.name);
                    }
                    emit(o, "};\n\n");
                    emit(o, "%s %s_as_%s(%s *self)\n", iname, tname, iname, tname);
                    emit(o, "{\n");
                    emit(o, "    return (%s){ .data = self, .vtbl = &%s_%s_vtable };\n",
                         iname, tname, iname);
                    emit(o, "}\n");
                }
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
    /* emit monomorphized generic fn bodies */
    for (GenFnInst *gi = g_gfn_insts; gi; gi = gi->next)
        emit_gen_fn_inst(o, &cg, gi);

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

/* ── std.path / std.time runtime (always emitted for native apps) ─────────── */
static void emit_stdlib_runtime(COut *o) {
    emit(o, "/* ════════════════════════════════════════════════════════════\n");
    emit(o, "   NightScript stdlib runtime — emitted by codegen (do not edit)\n");
    emit(o, "   ════════════════════════════════════════════════════════════ */\n\n");

    emit(o, "#include <time.h>\n");
    emit(o, "#include <math.h>\n");
    emit(o, "#include <sys/stat.h>\n");
    emit(o, "#ifdef _WIN32\n#include <direct.h>\n#else\n#include <unistd.h>\n#endif\n\n");

    /* ── std.time ─────────────────────────────────────────────────────────── */
    emit(o, "/* std.time */\n");
    emit(o, "static inline int64_t ns_time_now_secs(void) { return (int64_t)time((void*)0); }\n");
    emit(o, "static inline uint64_t ns_time_now_ms(void) {\n");
    emit(o, "#if defined(CLOCK_MONOTONIC)\n");
    emit(o, "    struct timespec _ts; clock_gettime(CLOCK_MONOTONIC,&_ts);\n");
    emit(o, "    return (uint64_t)_ts.tv_sec*1000ULL+(uint64_t)(_ts.tv_nsec/1000000);\n");
    emit(o, "#else\n    return (uint64_t)((double)clock()*1000.0/CLOCKS_PER_SEC);\n#endif\n}\n");
    emit(o, "static inline uint64_t ns_time_now_us(void) {\n");
    emit(o, "#if defined(CLOCK_MONOTONIC)\n");
    emit(o, "    struct timespec _ts; clock_gettime(CLOCK_MONOTONIC,&_ts);\n");
    emit(o, "    return (uint64_t)_ts.tv_sec*1000000ULL+(uint64_t)(_ts.tv_nsec/1000);\n");
    emit(o, "#else\n    return (uint64_t)((double)clock()*1000000.0/CLOCKS_PER_SEC);\n#endif\n}\n");
    emit(o, "static inline void ns_time_sleep_ms(uint32_t ms) {\n");
    emit(o, "#if defined(_POSIX_VERSION)\n");
    emit(o, "    struct timespec _ts; _ts.tv_sec=ms/1000; _ts.tv_nsec=(long)((ms%%1000)*1000000L);\n");
    emit(o, "    nanosleep(&_ts,(void*)0);\n");
    emit(o, "#else\n    (void)ms;\n#endif\n}\n");
    emit(o, "static inline void ns_time_sleep_us(uint64_t us) {\n");
    emit(o, "#if defined(_POSIX_VERSION)\n");
    emit(o, "    struct timespec _ts; _ts.tv_sec=(time_t)(us/1000000ULL);\n");
    emit(o, "    _ts.tv_nsec=(long)((us%%1000000ULL)*1000ULL);\n");
    emit(o, "    nanosleep(&_ts,(void*)0);\n");
    emit(o, "#else\n    (void)us;\n#endif\n}\n");
    emit(o, "static inline void ns_time_sleep_secs(uint32_t s) { ns_time_sleep_ms(s*1000u); }\n\n");

    /* ── core.math (float functions via libm) ─────────────────────────────── */
    emit(o, "/* core.math — float */\n");
    emit(o, "static inline double ns_math_sqrt(double x)         { return sqrt(x); }\n");
    emit(o, "static inline double ns_math_fabs(double x)         { return fabs(x); }\n");
    emit(o, "static inline double ns_math_floor(double x)        { return floor(x); }\n");
    emit(o, "static inline double ns_math_ceil(double x)         { return ceil(x); }\n");
    emit(o, "static inline double ns_math_round(double x)        { return round(x); }\n");
    emit(o, "static inline double ns_math_fmod(double x, double y){ return fmod(x,y); }\n");
    emit(o, "static inline double ns_math_sin(double x)          { return sin(x); }\n");
    emit(o, "static inline double ns_math_cos(double x)          { return cos(x); }\n");
    emit(o, "static inline double ns_math_tan(double x)          { return tan(x); }\n");
    emit(o, "static inline double ns_math_asin(double x)         { return asin(x); }\n");
    emit(o, "static inline double ns_math_acos(double x)         { return acos(x); }\n");
    emit(o, "static inline double ns_math_atan(double x)         { return atan(x); }\n");
    emit(o, "static inline double ns_math_atan2(double y,double x){ return atan2(y,x); }\n");
    emit(o, "static inline double ns_math_log(double x)          { return log(x); }\n");
    emit(o, "static inline double ns_math_log2(double x)         { return log2(x); }\n");
    emit(o, "static inline double ns_math_log10(double x)        { return log10(x); }\n");
    emit(o, "static inline double ns_math_exp(double x)          { return exp(x); }\n");
    emit(o, "static inline double ns_math_pow(double b,double e)  { return pow(b,e); }\n\n");

    /* ── core.convert (number → string) ──────────────────────────────────── */
    emit(o, "/* core.convert */\n");
    emit(o, "static char _ns_conv_buf[8][64];\n");
    emit(o, "static int  _ns_conv_idx = 0;\n");
    emit(o, "static inline char *_ns_conv_next(void) {\n");
    emit(o, "    char *b = _ns_conv_buf[_ns_conv_idx %% 8]; _ns_conv_idx++; return b;\n}\n");
    emit(o, "static inline const char *ns_conv_i32_to_str(int32_t n) {\n");
    emit(o, "    char *b=_ns_conv_next(); snprintf(b,64,\"%%d\",n); return b;\n}\n");
    emit(o, "static inline const char *ns_conv_i64_to_str(int64_t n) {\n");
    emit(o, "    char *b=_ns_conv_next(); snprintf(b,64,\"%%lld\",(long long)n); return b;\n}\n");
    emit(o, "static inline const char *ns_conv_u32_to_str(uint32_t n) {\n");
    emit(o, "    char *b=_ns_conv_next(); snprintf(b,64,\"%%u\",n); return b;\n}\n");
    emit(o, "static inline const char *ns_conv_u64_to_str(uint64_t n) {\n");
    emit(o, "    char *b=_ns_conv_next(); snprintf(b,64,\"%%llu\",(unsigned long long)n); return b;\n}\n");
    emit(o, "static inline const char *ns_conv_f64_to_str(double n) {\n");
    emit(o, "    char *b=_ns_conv_next(); snprintf(b,64,\"%%g\",n); return b;\n}\n");
    emit(o, "static inline const char *ns_conv_i32_to_hex(int32_t n) {\n");
    emit(o, "    char *b=_ns_conv_next(); snprintf(b,64,\"0x%%x\",(unsigned)n); return b;\n}\n");
    emit(o, "static inline const char *ns_conv_u32_to_hex(uint32_t n) {\n");
    emit(o, "    char *b=_ns_conv_next(); snprintf(b,64,\"0x%%x\",n); return b;\n}\n");
    emit(o, "static inline const char *ns_conv_bool_to_str(int b) {\n");
    emit(o, "    return b ? \"true\" : \"false\";\n}\n\n");

    /* ── std.path ─────────────────────────────────────────────────────────── */
    emit(o, "/* std.path */\n");
    emit(o, "static char _ns_path_buf[4096];\n");
    emit(o, "static char _ns_path_dir[4096];\n");
    emit(o, "static char _ns_path_abs[4096];\n");
    emit(o, "static inline const char *ns_path_join(const char *a, const char *b) {\n");
    emit(o, "    int la=(int)strlen(a), sep=(la>0&&a[la-1]!='/');\n");
    emit(o, "    snprintf(_ns_path_buf,sizeof(_ns_path_buf),\"%%s%%s%%s\",a,sep?\"/\":\"\",b);\n");
    emit(o, "    return _ns_path_buf;\n}\n");
    emit(o, "static inline const char *ns_path_basename(const char *p) {\n");
    emit(o, "    const char *s=strrchr(p,'/'); return s ? s+1 : p;\n}\n");
    emit(o, "static inline const char *ns_path_dirname(const char *p) {\n");
    emit(o, "    int n=(int)strlen(p);\n");
    emit(o, "    while(n>0&&p[n-1]!='/') n--;\n");
    emit(o, "    if(n==0){_ns_path_dir[0]='.';_ns_path_dir[1]='\\0';return _ns_path_dir;}\n");
    emit(o, "    snprintf(_ns_path_dir,sizeof(_ns_path_dir),\"%%.*s\",n-1,p); return _ns_path_dir;\n}\n");
    emit(o, "static inline const char *ns_path_extension(const char *p) {\n");
    emit(o, "    const char *d=strrchr(p,'.'); return d?d:\"\";\n}\n");
    emit(o, "static inline _Bool ns_path_exists(const char *p) {\n");
    emit(o, "    struct stat st; return stat(p,&st)==0;\n}\n");
    emit(o, "static inline _Bool ns_path_is_dir(const char *p) {\n");
    emit(o, "    struct stat st; return stat(p,&st)==0&&S_ISDIR(st.st_mode);\n}\n");
    emit(o, "static inline _Bool ns_path_is_file(const char *p) {\n");
    emit(o, "    struct stat st; return stat(p,&st)==0&&S_ISREG(st.st_mode);\n}\n");
    emit(o, "static inline _Bool ns_path_is_absolute(const char *p) {\n");
    emit(o, "    return p && p[0]=='/';\n}\n");
    emit(o, "static inline const char *ns_path_absolute(const char *p) {\n");
    emit(o, "#if defined(_POSIX_VERSION)\n");
    emit(o, "    if(realpath(p,_ns_path_abs)) return _ns_path_abs;\n");
    emit(o, "#endif\n    return p;\n}\n\n");

    /* ── std.env ──────────────────────────────────────────────────────────── */
    emit(o, "/* std.env */\n");
    emit(o, "static inline _Bool ns_env_exists(const char *name) {\n");
    emit(o, "    return getenv(name) != (void*)0;\n}\n");
    emit(o, "static inline const char *ns_env_get_or(const char *name, const char *def) {\n");
    emit(o, "    const char *v=getenv(name); return v ? v : def;\n}\n\n");

    /* ── std.process ──────────────────────────────────────────────────────── */
    emit(o, "/* std.process */\n");
    emit(o, "#include <signal.h>\n");
    emit(o, "#if defined(_POSIX_VERSION)\n");
    emit(o, "static inline int32_t ns_process_getpid(void)  { return (int32_t)getpid(); }\n");
    emit(o, "static inline int32_t ns_process_getppid(void) { return (int32_t)getppid(); }\n");
    emit(o, "#else\n");
    emit(o, "static inline int32_t ns_process_getpid(void)  { return 0; }\n");
    emit(o, "static inline int32_t ns_process_getppid(void) { return 0; }\n");
    emit(o, "#endif\n\n");

    /* ── std.fs extras ────────────────────────────────────────────────────── */
    emit(o, "/* std.fs extras */\n");
    emit(o, "static inline int64_t ns_fs_size(void *fp) {\n");
    emit(o, "    long cur=ftell((FILE*)fp); fseek((FILE*)fp,0,2);\n");
    emit(o, "    long sz=ftell((FILE*)fp); fseek((FILE*)fp,cur,0); return (int64_t)sz;\n}\n");
    emit(o, "static inline int32_t ns_fs_mkdir(const char *p) {\n");
    emit(o, "#ifdef _WIN32\n    return _mkdir(p);\n");
    emit(o, "#else\n    return mkdir(p,0755);\n#endif\n}\n");
    emit(o, "static inline int32_t ns_fs_rmdir(const char *p) {\n");
    emit(o, "#ifdef _WIN32\n    return _rmdir(p);\n");
    emit(o, "#else\n    return rmdir(p);\n#endif\n}\n\n");
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
    emit(o, "static inline void      ns_io_print_char(uint8_t c)    { fputc((int)c, stdout); }\n");
    emit(o, "static inline void      ns_io_eflush(void)             { fflush(stderr); }\n");
    emit(o, "static inline int32_t   ns_io_read_i32(void)  { int32_t  n = 0; scanf(\"%%d\",  &n); return n; }\n");
    emit(o, "static inline int64_t   ns_io_read_i64(void)  { long long n = 0; scanf(\"%%lld\", &n); return (int64_t)n; }\n");
    emit(o, "static inline uint32_t  ns_io_read_u32(void)  { uint32_t n = 0; scanf(\"%%u\",  &n); return n; }\n");
    emit(o, "static inline uint64_t  ns_io_read_u64(void)  { unsigned long long n = 0; scanf(\"%%llu\", &n); return (uint64_t)n; }\n");
    emit(o, "static inline double    ns_io_read_f64(void)  { double   n = 0; scanf(\"%%lf\", &n); return n; }\n");
    emit(o, "static inline int       ns_io_read_bool(void) { char b[8]={0}; scanf(\"%%7s\",b);\n");
    emit(o, "    return (b[0]=='t'||b[0]=='T'||b[0]=='1'||b[0]=='y'||b[0]=='Y'); }\n");
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
            app = prog->as.program.decls.items[i]; break;
        }
    }
    if (!app) return;

    emit(o, "\n/* ════════════════════════════════════════════════════════════\n");
    emit(o, "   NightScript Kernel Runtime — v0.6\n");
    emit(o, "   Generated by the NightScript compiler (ApexForge)\n");
    emit(o, "════════════════════════════════════════════════════════════ */\n\n");

    /* Freestanding typedefs are emitted in codegen_generate before prototypes */

    /* ── Multiboot2 boot header (with framebuffer request) ──────────────── */
    /* Header: 16 bytes + FB tag: 20 bytes + pad: 4 bytes + end tag: 8 bytes = 48 bytes */
    emit(o, "/* ── Multiboot2 (with framebuffer request) ─── */\n");
    emit(o, "#define NS_MB2_MAGIC  0xe85250d6u\n");
    emit(o, "#define NS_MB2_LEN    48u\n");
    emit(o, "#define NS_MB2_SUM    (uint32_t)(0u-(NS_MB2_MAGIC+0u+NS_MB2_LEN))\n");
    emit(o, "__attribute__((section(\".multiboot2\"),used))\n");
    emit(o, "static uint32_t ns_mb2_header[] = {\n");
    emit(o, "    NS_MB2_MAGIC, 0u, NS_MB2_LEN, NS_MB2_SUM,\n");
    emit(o, "    /* framebuffer request tag: type=5, flags=0, size=20 packed in first u32 */\n");
    emit(o, "    /* [type:u16=5][flags:u16=0] then size:u32=20, width:u32, height:u32, depth:u32 */\n");
    emit(o, "    5u | (0u<<16), 20u, 1024u, 768u, 32u,\n");
    emit(o, "    0u,            /* 4-byte pad so end tag is 8-byte aligned */\n");
    emit(o, "    /* end tag: type=0, flags=0, size=8 */\n");
    emit(o, "    0u, 8u\n");
    emit(o, "};\n\n");

    /* ── CPU ────────────────────────────────────────────────────────────── */
    emit(o, "/* ── CPU ─── */\n");
    emit(o, "static inline void ns_cpu_halt(void)  { __asm__ volatile(\"hlt\"); }\n");
    emit(o, "static inline void ns_cpu_cli(void)   { __asm__ volatile(\"cli\"); }\n");
    emit(o, "static inline void ns_cpu_sti(void)   { __asm__ volatile(\"sti\"); }\n");
    emit(o, "static inline void ns_cpu_nop(void)   { __asm__ volatile(\"nop\"); }\n");
    emit(o, "static inline void ns_cpu_pause(void) { __asm__ volatile(\"pause\"); }\n\n");

    /* ── Port I/O ────────────────────────────────────────────────────────── */
    emit(o, "/* ── Port I/O ─── */\n");
    emit(o, "static inline unsigned char  ns_inb(unsigned short p)\n");
    emit(o, "    { unsigned char r; __asm__ volatile(\"inb %%%%dx,%%%%al\":\"=a\"(r):\"d\"(p)); return r; }\n");
    emit(o, "static inline unsigned short ns_inw(unsigned short p)\n");
    emit(o, "    { unsigned short r; __asm__ volatile(\"inw %%%%dx,%%%%ax\":\"=a\"(r):\"d\"(p)); return r; }\n");
    emit(o, "static inline unsigned int   ns_inl(unsigned short p)\n");
    emit(o, "    { unsigned int r; __asm__ volatile(\"inl %%%%dx,%%%%eax\":\"=a\"(r):\"d\"(p)); return r; }\n");
    emit(o, "static inline void ns_outb(unsigned short p, unsigned char v)\n");
    emit(o, "    { __asm__ volatile(\"outb %%%%al,%%%%dx\"::\"a\"(v),\"d\"(p)); }\n");
    emit(o, "static inline void ns_outw(unsigned short p, unsigned short v)\n");
    emit(o, "    { __asm__ volatile(\"outw %%%%ax,%%%%dx\"::\"a\"(v),\"d\"(p)); }\n");
    emit(o, "static inline void ns_outl(unsigned short p, unsigned int v)\n");
    emit(o, "    { __asm__ volatile(\"outl %%%%eax,%%%%dx\"::\"a\"(v),\"d\"(p)); }\n");
    emit(o, "static inline void ns_io_wait(void) { ns_outb(0x80, 0); }\n\n");

    /* ── VGA text mode ──────────────────────────────────────────────────── */
    emit(o, "/* ── VGA text mode (80×25) ─── */\n");
    emit(o, "#define NS_VGA ((volatile unsigned short *)0xB8000)\n");
    emit(o, "static int ns_vga_row=0, ns_vga_col=0;\n");
    emit(o, "static unsigned char ns_vga_attr=0x0F; /* white on black */\n");
    emit(o, "static void ns_vga_set_color(unsigned char attr) { ns_vga_attr=attr; }\n");
    emit(o, "static void ns_vga_clear(void) {\n");
    emit(o, "    for(int i=0;i<80*25;i++) NS_VGA[i]=(unsigned short)(((unsigned short)ns_vga_attr<<8)|' ');\n");
    emit(o, "    ns_vga_row=ns_vga_col=0;\n}\n");
    emit(o, "static void ns_vga_scroll(void) {\n");
    emit(o, "    for(int i=0;i<80*24;i++) NS_VGA[i]=NS_VGA[i+80];\n");
    emit(o, "    for(int i=80*24;i<80*25;i++) NS_VGA[i]=(unsigned short)(((unsigned short)ns_vga_attr<<8)|' ');\n");
    emit(o, "    ns_vga_row=24;\n}\n");
    emit(o, "static void ns_vga_putchar(char c) {\n");
    emit(o, "    if(c=='\\n'||ns_vga_col>=80){ns_vga_col=0;ns_vga_row++;}\n");
    emit(o, "    if(c=='\\n') return;\n");
    emit(o, "    if(ns_vga_row>=25) ns_vga_scroll();\n");
    emit(o, "    NS_VGA[ns_vga_row*80+ns_vga_col]=(unsigned short)(((unsigned short)ns_vga_attr<<8)|(unsigned char)c);\n");
    emit(o, "    ns_vga_col++;\n}\n");
    emit(o, "static void ns_vga_print(const char *s){while(*s)ns_vga_putchar(*s++);}\n");
    emit(o, "static void ns_vga_println(const char *s){ns_vga_print(s);ns_vga_putchar('\\n');}\n");
    emit(o, "static void ns_vga_print_u32(unsigned int n) {\n");
    emit(o, "    if(!n){ns_vga_putchar('0');return;}\n");
    emit(o, "    char buf[12];int i=0;\n");
    emit(o, "    while(n){buf[i++]='0'+n%%10;n/=10;}\n");
    emit(o, "    while(i--) ns_vga_putchar(buf[i]);\n}\n");
    emit(o, "static void ns_vga_print_hex(unsigned int n) {\n");
    emit(o, "    const char *h=\"0123456789ABCDEF\";\n");
    emit(o, "    ns_vga_print(\"0x\");\n");
    emit(o, "    for(int s=28;s>=0;s-=4) ns_vga_putchar(h[(n>>s)&0xF]);\n}\n\n");

    /* ── Serial (COM1) ──────────────────────────────────────────────────── */
    emit(o, "/* ── Serial COM1 (0x3F8) ─── */\n");
    emit(o, "#define NS_COM1 0x3F8\n");
    emit(o, "static void ns_serial_init(void) {\n");
    emit(o, "    ns_outb(NS_COM1+1,0x00); ns_outb(NS_COM1+3,0x80);\n");
    emit(o, "    ns_outb(NS_COM1+0,0x03); ns_outb(NS_COM1+1,0x00);\n");
    emit(o, "    ns_outb(NS_COM1+3,0x03); ns_outb(NS_COM1+2,0xC7);\n");
    emit(o, "    ns_outb(NS_COM1+4,0x0B);\n}\n");
    emit(o, "static void ns_serial_putchar(char c) {\n");
    emit(o, "    while(!(ns_inb(NS_COM1+5)&0x20)); ns_outb(NS_COM1,(unsigned char)c);\n}\n");
    emit(o, "static void ns_serial_print(const char *s){while(*s)ns_serial_putchar(*s++);}\n");
    emit(o, "static void ns_serial_println(const char *s){ns_serial_print(s);ns_serial_putchar('\\n');}\n\n");

    /* ── GDT ────────────────────────────────────────────────────────────── */
    emit(o, "/* ── GDT (Global Descriptor Table) ─── */\n");
    emit(o, "typedef struct __attribute__((packed)) {\n");
    emit(o, "    unsigned short lim_low, base_low;\n");
    emit(o, "    unsigned char  base_mid, access, gran, base_high;\n");
    emit(o, "} NS_GdtEntry;\n");
    emit(o, "typedef struct __attribute__((packed)) {\n");
    emit(o, "    unsigned short size; unsigned int base;\n");
    emit(o, "} NS_GdtDescriptor;\n");
    emit(o, "static NS_GdtEntry      ns_gdt[5];\n");
    emit(o, "static NS_GdtDescriptor ns_gdt_ptr;\n");
    emit(o, "static void ns_gdt_set(int i,unsigned int base,unsigned int lim,\n");
    emit(o, "                       unsigned char access,unsigned char gran){\n");
    emit(o, "    ns_gdt[i].base_low =(unsigned short)(base&0xFFFF);\n");
    emit(o, "    ns_gdt[i].base_mid =(unsigned char)((base>>16)&0xFF);\n");
    emit(o, "    ns_gdt[i].base_high=(unsigned char)((base>>24)&0xFF);\n");
    emit(o, "    ns_gdt[i].lim_low  =(unsigned short)(lim&0xFFFF);\n");
    emit(o, "    ns_gdt[i].gran     =(unsigned char)(((lim>>16)&0x0F)|(gran&0xF0));\n");
    emit(o, "    ns_gdt[i].access   =access;\n}\n");
    emit(o, "static void ns_gdt_install(void) {\n");
    emit(o, "    ns_gdt_ptr.size=(unsigned short)(sizeof(ns_gdt)-1);\n");
    emit(o, "    ns_gdt_ptr.base=(unsigned int)(unsigned long)&ns_gdt;\n");
    emit(o, "    __asm__ volatile(\"lgdt (%0)\"::\"r\"(&ns_gdt_ptr):\"memory\");\n}\n");
    emit(o, "static void ns_gdt_init(void) {\n");
    emit(o, "    ns_gdt_set(0,0,0,0,0);             /* null */\n");
    emit(o, "    ns_gdt_set(1,0,0xFFFFFFFF,0x9A,0xCF); /* code ring0 */\n");
    emit(o, "    ns_gdt_set(2,0,0xFFFFFFFF,0x92,0xCF); /* data ring0 */\n");
    emit(o, "    ns_gdt_set(3,0,0xFFFFFFFF,0xFA,0xCF); /* code ring3 */\n");
    emit(o, "    ns_gdt_set(4,0,0xFFFFFFFF,0xF2,0xCF); /* data ring3 */\n");
    emit(o, "    ns_gdt_install();\n}\n\n");

    /* ── IDT + PIC ──────────────────────────────────────────────────────── */
    emit(o, "/* ── IDT + PIC (8259A) ─── */\n");
    emit(o, "typedef struct __attribute__((packed)) {\n");
    emit(o, "    unsigned short base_low,sel;\n");
    emit(o, "    unsigned char  zero,flags;\n");
    emit(o, "    unsigned short base_high;\n");
    emit(o, "} NS_IdtEntry;\n");
    emit(o, "typedef struct __attribute__((packed)) {\n");
    emit(o, "    unsigned short size; unsigned int base;\n");
    emit(o, "} NS_IdtDescriptor;\n");
    emit(o, "static NS_IdtEntry      ns_idt[256];\n");
    emit(o, "static NS_IdtDescriptor ns_idt_ptr;\n");
    emit(o, "typedef void (*NS_IsrFn)(void);\n");
    emit(o, "static NS_IsrFn ns_irq_handlers[16];\n");
    emit(o, "static void ns_idt_set(unsigned char n,unsigned int base,\n");
    emit(o, "                       unsigned short sel,unsigned char flags){\n");
    emit(o, "    ns_idt[n].base_low =(unsigned short)(base&0xFFFF);\n");
    emit(o, "    ns_idt[n].base_high=(unsigned short)((base>>16)&0xFFFF);\n");
    emit(o, "    ns_idt[n].sel=sel; ns_idt[n].zero=0; ns_idt[n].flags=flags;\n}\n");
    emit(o, "static void ns_idt_install(void) {\n");
    emit(o, "    ns_idt_ptr.size=(unsigned short)(sizeof(ns_idt)-1);\n");
    emit(o, "    ns_idt_ptr.base=(unsigned int)(unsigned long)&ns_idt;\n");
    emit(o, "    __asm__ volatile(\"lidt (%0)\"::\"r\"(&ns_idt_ptr):\"memory\");\n}\n");
    emit(o, "static void ns_pic_remap(unsigned char o1,unsigned char o2){\n");
    emit(o, "    unsigned char m1=ns_inb(0x21),m2=ns_inb(0xA1);\n");
    emit(o, "    ns_outb(0x20,0x11);ns_io_wait(); ns_outb(0xA0,0x11);ns_io_wait();\n");
    emit(o, "    ns_outb(0x21,o1); ns_io_wait(); ns_outb(0xA1,o2); ns_io_wait();\n");
    emit(o, "    ns_outb(0x21,0x04);ns_io_wait(); ns_outb(0xA1,0x02);ns_io_wait();\n");
    emit(o, "    ns_outb(0x21,0x01);ns_io_wait(); ns_outb(0xA1,0x01);ns_io_wait();\n");
    emit(o, "    ns_outb(0x21,m1); ns_outb(0xA1,m2);\n}\n");
    emit(o, "static void ns_pic_eoi(unsigned char irq){\n");
    emit(o, "    if(irq>=8) ns_outb(0xA0,0x20); ns_outb(0x20,0x20);\n}\n");
    emit(o, "static void ns_irq_register(unsigned char irq,NS_IsrFn fn){\n");
    emit(o, "    ns_irq_handlers[irq]=fn;\n");
    emit(o, "    ns_idt_set((unsigned char)(irq+0x20),(unsigned int)(unsigned long)fn,0x08,0x8E);\n}\n");
    emit(o, "static void ns_interrupts_init(void){\n");
    emit(o, "    ns_pic_remap(0x20,0x28);\n");
    emit(o, "    ns_idt_install();\n}\n\n");

    /* ── PIT timer ──────────────────────────────────────────────────────── */
    emit(o, "/* ── PIT Timer (IRQ0) ─── */\n");
    emit(o, "static volatile unsigned int ns_ticks=0;\n");
    emit(o, "static void ns_timer_tick(void){ns_ticks++;ns_pic_eoi(0);}\n");
    emit(o, "static void ns_timer_init(unsigned int hz){\n");
    emit(o, "    unsigned int div=1193180/hz;\n");
    emit(o, "    ns_outb(0x43,0x36);\n");
    emit(o, "    ns_outb(0x40,(unsigned char)(div&0xFF));\n");
    emit(o, "    ns_outb(0x40,(unsigned char)((div>>8)&0xFF));\n");
    emit(o, "    ns_irq_register(0,ns_timer_tick);\n}\n");
    emit(o, "static unsigned int ns_timer_ticks(void){return ns_ticks;}\n");
    emit(o, "static void ns_timer_wait(unsigned int ms){\n");
    emit(o, "    unsigned int end=ns_ticks+ms;\n");
    emit(o, "    while(ns_ticks<end) ns_cpu_halt();\n}\n\n");

    /* ── PS/2 Keyboard ──────────────────────────────────────────────────── */
    emit(o, "/* ── PS/2 Keyboard (IRQ1) ─── */\n");
    emit(o, "static volatile unsigned char ns_kb_last=0;\n");
    emit(o, "static void (*ns_kb_handler)(unsigned char)=(void*)0;\n");
    emit(o, "static const char ns_kb_ascii[128]={\n");
    emit(o, "  0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\\b','\\t',\n");
    emit(o, "  'q','w','e','r','t','y','u','i','o','p','[',']','\\n',0,\n");
    emit(o, "  'a','s','d','f','g','h','j','k','l',';','\\'',' ',0,'\\\\',\n");
    emit(o, "  'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0,\n");
    emit(o, "  0,0,0,0,0,0,0,0,0,0,0,0,0,'7','8','9','-',\n");
    emit(o, "  '4','5','6','+','1','2','3','0','.',0,0,0,0,0,0,0\n};\n");
    emit(o, "static void ns_kb_irq(void){\n");
    emit(o, "    ns_kb_last=ns_inb(0x60);\n");
    emit(o, "    if(ns_kb_handler) ns_kb_handler(ns_kb_last);\n");
    emit(o, "    ns_pic_eoi(1);\n}\n");
    emit(o, "static void ns_keyboard_init(void)  {ns_irq_register(1,ns_kb_irq);}\n");
    emit(o, "static void ns_keyboard_on_key(void(*fn)(unsigned char)){ns_kb_handler=fn;}\n");
    emit(o, "static char ns_keyboard_to_ascii(unsigned char sc){\n");
    emit(o, "    return (sc<128)?ns_kb_ascii[sc]:0;\n}\n\n");

    /* ── Physical Memory Manager ─────────────────────────────────────────── */
    emit(o, "/* ── Physical Memory Manager (bitmap) ─── */\n");
    emit(o, "#define NS_PAGE_SIZE 4096u\n");
    emit(o, "static unsigned char *ns_pmm_map=(unsigned char*)0;\n");
    emit(o, "static unsigned int ns_pmm_pages=0, ns_pmm_used=0;\n");
    emit(o, "static void ns_pmm_init(unsigned int mem_kb, unsigned int bitmap_base){\n");
    emit(o, "    ns_pmm_map=(unsigned char*)(unsigned long)bitmap_base;\n");
    emit(o, "    ns_pmm_pages=(mem_kb*1024)/NS_PAGE_SIZE;\n");
    emit(o, "    for(unsigned int i=0;i<(ns_pmm_pages/8)+1;i++) ns_pmm_map[i]=0;\n");
    emit(o, "    ns_pmm_used=0;\n}\n");
    emit(o, "static void *ns_pmm_alloc(void){\n");
    emit(o, "    for(unsigned int i=0;i<ns_pmm_pages;i++)\n");
    emit(o, "        if(!(ns_pmm_map[i/8]&(1u<<(i%%8)))){\n");
    emit(o, "            ns_pmm_map[i/8]|=(unsigned char)(1u<<(i%%8));\n");
    emit(o, "            ns_pmm_used++;\n");
    emit(o, "            return (void*)(unsigned long)(i*NS_PAGE_SIZE);\n        }\n");
    emit(o, "    return (void*)0;\n}\n");
    emit(o, "static void ns_pmm_free(void *p){\n");
    emit(o, "    unsigned int i=(unsigned int)(unsigned long)p/NS_PAGE_SIZE;\n");
    emit(o, "    if(ns_pmm_map[i/8]&(1u<<(i%%8))){\n");
    emit(o, "        ns_pmm_map[i/8]&=(unsigned char)~(1u<<(i%%8)); ns_pmm_used--;\n    }\n}\n");
    emit(o, "static unsigned int ns_pmm_free_count(void){return ns_pmm_pages-ns_pmm_used;}\n\n");

    /* ── Linear Framebuffer ──────────────────────────────────────────────── */
    emit(o, "/* ── Linear Framebuffer ─── */\n");
    emit(o, "static unsigned int *ns_fb=(unsigned int*)0;\n");
    emit(o, "static unsigned int ns_fb_w=0,ns_fb_h=0,ns_fb_pitch=0;\n");
    emit(o, "static void ns_fb_init(unsigned int *addr,unsigned int w,\n");
    emit(o, "                       unsigned int h,unsigned int pitch_bytes){\n");
    emit(o, "    ns_fb=addr; ns_fb_w=w; ns_fb_h=h; ns_fb_pitch=pitch_bytes/4;\n}\n");
    emit(o, "static void ns_fb_pixel(unsigned int x,unsigned int y,unsigned int c){\n");
    emit(o, "    if(x<ns_fb_w&&y<ns_fb_h) ns_fb[y*ns_fb_pitch+x]=c;\n}\n");
    emit(o, "static void ns_fb_fill(unsigned int x,unsigned int y,\n");
    emit(o, "                       unsigned int w,unsigned int h,unsigned int c){\n");
    emit(o, "    for(unsigned int dy=0;dy<h;dy++)\n");
    emit(o, "        for(unsigned int dx=0;dx<w;dx++)\n");
    emit(o, "            ns_fb_pixel(x+dx,y+dy,c);\n}\n");
    emit(o, "static void ns_fb_clear(unsigned int c){ns_fb_fill(0,0,ns_fb_w,ns_fb_h,c);}\n");
    /* 8×8 bitmap font for framebuffer */
    emit(o, "static const unsigned char ns_fb_font[96][8]={\n");
    emit(o, /* space */ "  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},\n");
    emit(o, /* !     */ "  {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},\n");
    emit(o, /* "     */ "  {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},\n");
    emit(o, /* #     */ "  {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},\n");
    emit(o, /* $     */ "  {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},\n");
    emit(o, /* %%    */ "  {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},\n");
    emit(o, /* &     */ "  {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},\n");
    emit(o, /* '     */ "  {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},\n");
    emit(o, /* (     */ "  {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},\n");
    emit(o, /* )     */ "  {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},\n");
    emit(o, /* *     */ "  {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},\n");
    emit(o, /* +     */ "  {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},\n");
    emit(o, /* ,     */ "  {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06},\n");
    emit(o, /* -     */ "  {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},\n");
    emit(o, /* .     */ "  {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},\n");
    emit(o, /* /     */ "  {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},\n");
    emit(o, /* 0     */ "  {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},\n");
    emit(o, /* 1     */ "  {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},\n");
    emit(o, /* 2     */ "  {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},\n");
    emit(o, /* 3     */ "  {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},\n");
    emit(o, /* 4     */ "  {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},\n");
    emit(o, /* 5     */ "  {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},\n");
    emit(o, /* 6     */ "  {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},\n");
    emit(o, /* 7     */ "  {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},\n");
    emit(o, /* 8     */ "  {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},\n");
    emit(o, /* 9     */ "  {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},\n");
    emit(o, /* :     */ "  {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00},\n");
    emit(o, /* ;     */ "  {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},\n");
    emit(o, /* <     */ "  {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},\n");
    emit(o, /* =     */ "  {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},\n");
    emit(o, /* >     */ "  {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},\n");
    emit(o, /* ?     */ "  {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},\n");
    emit(o, /* @     */ "  {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},\n");
    emit(o, /* A     */ "  {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},\n");
    emit(o, /* B     */ "  {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},\n");
    emit(o, /* C     */ "  {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},\n");
    emit(o, /* D     */ "  {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},\n");
    emit(o, /* E     */ "  {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},\n");
    emit(o, /* F     */ "  {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},\n");
    emit(o, /* G     */ "  {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},\n");
    emit(o, /* H     */ "  {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},\n");
    emit(o, /* I     */ "  {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},\n");
    emit(o, /* J     */ "  {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},\n");
    emit(o, /* K     */ "  {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},\n");
    emit(o, /* L     */ "  {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},\n");
    emit(o, /* M     */ "  {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},\n");
    emit(o, /* N     */ "  {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},\n");
    emit(o, /* O     */ "  {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},\n");
    emit(o, /* P     */ "  {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},\n");
    emit(o, /* Q     */ "  {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},\n");
    emit(o, /* R     */ "  {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},\n");
    emit(o, /* S     */ "  {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},\n");
    emit(o, /* T     */ "  {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},\n");
    emit(o, /* U     */ "  {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},\n");
    emit(o, /* V     */ "  {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},\n");
    emit(o, /* W     */ "  {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},\n");
    emit(o, /* X     */ "  {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},\n");
    emit(o, /* Y     */ "  {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},\n");
    emit(o, /* Z     */ "  {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},\n");
    emit(o, /* [     */ "  {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},\n");
    emit(o, /* \\    */ "  {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},\n");
    emit(o, /* ]     */ "  {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},\n");
    emit(o, /* ^     */ "  {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},\n");
    emit(o, /* _     */ "  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},\n");
    emit(o, /* `     */ "  {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},\n");
    emit(o, /* a     */ "  {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},\n");
    emit(o, /* b     */ "  {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},\n");
    emit(o, /* c     */ "  {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},\n");
    emit(o, /* d     */ "  {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00},\n");
    emit(o, /* e     */ "  {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},\n");
    emit(o, /* f     */ "  {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00},\n");
    emit(o, /* g     */ "  {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},\n");
    emit(o, /* h     */ "  {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},\n");
    emit(o, /* i     */ "  {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},\n");
    emit(o, /* j     */ "  {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},\n");
    emit(o, /* k     */ "  {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},\n");
    emit(o, /* l     */ "  {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},\n");
    emit(o, /* m     */ "  {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},\n");
    emit(o, /* n     */ "  {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},\n");
    emit(o, /* o     */ "  {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},\n");
    emit(o, /* p     */ "  {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},\n");
    emit(o, /* q     */ "  {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},\n");
    emit(o, /* r     */ "  {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},\n");
    emit(o, /* s     */ "  {0x00,0x00,0x1E,0x03,0x1E,0x30,0x1F,0x00},\n");
    emit(o, /* t     */ "  {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},\n");
    emit(o, /* u     */ "  {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},\n");
    emit(o, /* v     */ "  {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},\n");
    emit(o, /* w     */ "  {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},\n");
    emit(o, /* x     */ "  {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},\n");
    emit(o, /* y     */ "  {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},\n");
    emit(o, /* z     */ "  {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},\n");
    emit(o, /* {     */ "  {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},\n");
    emit(o, /* |     */ "  {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},\n");
    emit(o, /* }     */ "  {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},\n");
    emit(o, /* ~     */ "  {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},\n");
    emit(o, /* DEL   */ "  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}\n};\n");
    emit(o, "static void ns_fb_char(unsigned int x,unsigned int y,char ch,\n");
    emit(o, "                       unsigned int fg,unsigned int bg){\n");
    emit(o, "    if(ch<32||ch>127) ch=32;\n");
    emit(o, "    const unsigned char *g=ns_fb_font[(unsigned char)(ch-32)];\n");
    emit(o, "    for(int r=0;r<8;r++) for(int c=0;c<8;c++)\n");
    emit(o, "        ns_fb_pixel(x+c,y+r,(g[r]&(1<<(7-c)))?fg:bg);\n}\n");
    emit(o, "static void ns_fb_str(unsigned int x,unsigned int y,const char *s,\n");
    emit(o, "                      unsigned int fg,unsigned int bg){\n");
    emit(o, "    unsigned int cx=x;\n");
    emit(o, "    while(*s){ if(*s=='\\n'){cx=x;y+=10;} else{ns_fb_char(cx,y,*s,fg,bg);cx+=9;} s++; }\n}\n\n");

    /* ══════════════════════════════════════════════════════════════════════
       v0.6 — NightOS Runtime: Mouse · Window Manager · Terminal · Shell
       ══════════════════════════════════════════════════════════════════════ */

    /* ── IRQ unmask helper (must be defined before mouse driver) ─────────── */
    emit(o, "/* ── IRQ unmask ─── */\n");
    emit(o, "static inline void ns_unmask_irq(uint8_t irq) {\n");
    emit(o, "    uint16_t port=(uint16_t)(irq>=8?0xA1u:0x21u);\n");
    emit(o, "    uint8_t  bit =(uint8_t)(irq>=8?(uint8_t)(irq-8):irq);\n");
    emit(o, "    ns_outb(port,(uint8_t)(ns_inb(port)&(uint8_t)(~(1u<<bit))));\n}\n");
    emit(o, "static inline void ns_mask_irq(uint8_t irq) {\n");
    emit(o, "    uint16_t port=(uint16_t)(irq>=8?0xA1u:0x21u);\n");
    emit(o, "    uint8_t  bit =(uint8_t)(irq>=8?(uint8_t)(irq-8):irq);\n");
    emit(o, "    ns_outb(port,(uint8_t)(ns_inb(port)|(1u<<bit)));\n}\n\n");

    /* ── Multiboot2 info parser → auto-init framebuffer ─────────────────── */
    emit(o, "/* ── MB2 info parser ─── */\n");
    emit(o, "static void _ns_mb2_init_fb(uint32_t mb2_ptr) {\n");
    emit(o, "    if(!mb2_ptr) return;\n");
    emit(o, "    uint8_t *p  = (uint8_t*)(uintptr_t)mb2_ptr;\n");
    emit(o, "    uint32_t sz = *(uint32_t*)p; p += 8;\n");
    emit(o, "    uint8_t *end= (uint8_t*)(uintptr_t)mb2_ptr + sz;\n");
    emit(o, "    while(p + 8 <= end) {\n");
    emit(o, "        uint32_t type = *(uint32_t*)p;\n");
    emit(o, "        uint32_t tlen = *(uint32_t*)(p+4);\n");
    emit(o, "        if(type==0) break;\n");
    emit(o, "        if(type==8 && tlen>=24) {\n");
    emit(o, "            /* framebuffer tag */\n");
    emit(o, "            uint32_t lo    = *(uint32_t*)(p+ 8);\n");
    emit(o, "            uint32_t pitch = *(uint32_t*)(p+16);\n");
    emit(o, "            uint32_t width = *(uint32_t*)(p+20);\n");
    emit(o, "            uint32_t height= *(uint32_t*)(p+24);\n");
    emit(o, "            ns_fb_init((uint32_t*)(uintptr_t)lo, width, height, pitch);\n");
    emit(o, "            return;\n");
    emit(o, "        }\n");
    emit(o, "        p += (tlen+7u)&~7u;\n");
    emit(o, "    }\n}\n\n");

    /* ── PS/2 Mouse driver (IRQ 12) ─────────────────────────────────────── */
    emit(o, "/* ── v0.6: PS/2 Mouse driver ─── */\n");
    emit(o, "static volatile int32_t  _ns_mouse_x   = 400;\n");
    emit(o, "static volatile int32_t  _ns_mouse_y   = 300;\n");
    emit(o, "static volatile uint8_t  _ns_mouse_btn = 0;\n");
    emit(o, "static volatile uint8_t  _ns_mouse_cycle = 0;\n");
    emit(o, "static volatile uint8_t  _ns_mouse_pkt[3] = {0,0,0};\n\n");

    emit(o, "static void _ns_mouse_wait_in(void)  { uint32_t t=200000; while(--t&&(ns_inb(0x64)&0x02)); }\n");
    emit(o, "static void _ns_mouse_wait_out(void) { uint32_t t=200000; while(--t&&!(ns_inb(0x64)&0x01)); }\n");
    emit(o, "static void _ns_mouse_cmd(uint8_t v) {\n");
    emit(o, "    _ns_mouse_wait_in(); ns_outb(0x64,0xD4);\n");
    emit(o, "    _ns_mouse_wait_in(); ns_outb(0x60,v);\n}\n");
    emit(o, "static uint8_t _ns_mouse_read(void) { _ns_mouse_wait_out(); return ns_inb(0x60); }\n\n");

    emit(o, "static void ns_mouse_irq(void) {\n");
    emit(o, "    uint8_t status = ns_inb(0x64);\n");
    emit(o, "    if(!(status&0x01)){ ns_pic_eoi(12); return; }\n");
    emit(o, "    uint8_t data = ns_inb(0x60);\n");
    emit(o, "    switch(_ns_mouse_cycle){\n");
    emit(o, "      case 0: if(!(data&0x08)) break; _ns_mouse_pkt[0]=data; _ns_mouse_cycle=1; break;\n");
    emit(o, "      case 1: _ns_mouse_pkt[1]=data; _ns_mouse_cycle=2; break;\n");
    emit(o, "      case 2: _ns_mouse_pkt[2]=data; _ns_mouse_cycle=0;\n");
    emit(o, "        { int32_t dx=(int32_t)(int8_t)_ns_mouse_pkt[1];\n");
    emit(o, "          int32_t dy=-(int32_t)(int8_t)_ns_mouse_pkt[2];\n");
    emit(o, "          _ns_mouse_x+=dx; _ns_mouse_y+=dy;\n");
    emit(o, "          if(_ns_mouse_x<0) _ns_mouse_x=0;\n");
    emit(o, "          if(_ns_mouse_y<0) _ns_mouse_y=0;\n");
    emit(o, "          if(_ns_mouse_x>=(int32_t)ns_fb_w)  _ns_mouse_x=(int32_t)ns_fb_w-1;\n");
    emit(o, "          if(_ns_mouse_y>=(int32_t)ns_fb_h) _ns_mouse_y=(int32_t)ns_fb_h-1;\n");
    emit(o, "          _ns_mouse_btn=_ns_mouse_pkt[0]&0x07; }\n");
    emit(o, "        break;\n    }\n    ns_pic_eoi(12);\n}\n\n");

    emit(o, "static void ns_mouse_init(void) {\n");
    emit(o, "    _ns_mouse_wait_in(); ns_outb(0x64,0xA8); /* enable aux port */\n");
    emit(o, "    _ns_mouse_wait_in(); ns_outb(0x64,0x20);\n");
    emit(o, "    _ns_mouse_wait_out();\n");
    emit(o, "    uint8_t st=ns_inb(0x60)|0x02;\n");
    emit(o, "    _ns_mouse_wait_in(); ns_outb(0x64,0x60);\n");
    emit(o, "    _ns_mouse_wait_in(); ns_outb(0x60,st);\n");
    emit(o, "    _ns_mouse_cmd(0xF6); _ns_mouse_read(); /* default settings */\n");
    emit(o, "    _ns_mouse_cmd(0xF4); _ns_mouse_read(); /* enable streaming  */\n");
    emit(o, "    ns_irq_register(12,(void*)ns_mouse_irq);\n");
    emit(o, "    ns_unmask_irq(12);\n}\n\n");

    emit(o, "static inline int32_t ns_mouse_x(void)       { return _ns_mouse_x; }\n");
    emit(o, "static inline int32_t ns_mouse_y(void)       { return _ns_mouse_y; }\n");
    emit(o, "static inline uint8_t ns_mouse_buttons(void) { return _ns_mouse_btn; }\n");
    emit(o, "static inline uint8_t ns_mouse_left(void)    { return _ns_mouse_btn&0x01; }\n");
    emit(o, "static inline uint8_t ns_mouse_right(void)   { return (_ns_mouse_btn>>1)&0x01; }\n");
    emit(o, "static inline uint8_t ns_mouse_middle(void)  { return (_ns_mouse_btn>>2)&0x01; }\n");
    emit(o, "static int32_t ns_mouse_in_rect(int32_t x,int32_t y,int32_t w,int32_t h){\n");
    emit(o, "    return _ns_mouse_x>=x&&_ns_mouse_x<x+w&&_ns_mouse_y>=y&&_ns_mouse_y<y+h; }\n\n");

    /* ── Mouse cursor (12×20 arrow sprite) ──────────────────────────────── */
    emit(o, "/* ── Mouse cursor sprite ─── */\n");
    emit(o, "static const uint16_t _ns_cursor[20]={\n");
    emit(o, "    0x8000,0xC000,0xE000,0xF000,0xF800,0xFC00,\n");
    emit(o, "    0xFE00,0xFF00,0xF800,0xEC00,0xC600,0x0300,\n");
    emit(o, "    0x0380,0x01C0,0x00E0,0x0070,0x0038,0x001C,\n");
    emit(o, "    0x000E,0x0007};\n");
    emit(o, "static void ns_draw_cursor(int32_t x,int32_t y) {\n");
    emit(o, "    for(int r=0;r<20;r++) {\n");
    emit(o, "        uint16_t row=_ns_cursor[r];\n");
    emit(o, "        for(int c=0;c<12;c++) {\n");
    emit(o, "            if(!(row&(0x8000u>>(unsigned)c))) continue;\n");
    emit(o, "            uint32_t px=(uint32_t)(x+c), py=(uint32_t)(y+r);\n");
    emit(o, "            if(px<ns_fb_w&&py<ns_fb_h) ns_fb_pixel(px,py,0x00FFFFFF);\n");
    emit(o, "        }\n    }\n}\n\n");

    /* ── Window Manager ──────────────────────────────────────────────────── */
    emit(o, "/* ── v0.6: Window Manager ─── */\n");
    emit(o, "#define NS_WM_MAX   8\n");
    emit(o, "#define NS_WM_TB_H  20\n");   /* title-bar height */
    emit(o, "#define NS_WM_BD    2\n");    /* border width */
    emit(o, "#define NS_WM_CB_W  16\n");   /* close-button width */
    emit(o, "typedef struct {\n");
    emit(o, "    int32_t x,y,w,h;\n");
    emit(o, "    const char *title;\n");
    emit(o, "    uint8_t visible, focused, closing;\n");
    emit(o, "    int32_t drag_dx, drag_dy;\n");
    emit(o, "} NS_Win;\n");
    emit(o, "static NS_Win   _ns_wins[NS_WM_MAX];\n");
    emit(o, "static int32_t  _ns_win_count   = 0;\n");
    emit(o, "static int32_t  _ns_win_focused = -1;\n");
    emit(o, "static int32_t  _ns_win_dragging= -1;\n");
    emit(o, "static uint8_t  _ns_wm_prev_btn = 0;\n\n");

    /* color constants */
    emit(o, "#define NS_C_DESKTOP 0x00203050u\n");
    emit(o, "#define NS_C_TASKBAR 0x00102040u\n");
    emit(o, "#define NS_C_TB_FOC  0x001060C0u\n");
    emit(o, "#define NS_C_TB_UNF  0x00405060u\n");
    emit(o, "#define NS_C_BORDER  0x004090D0u\n");
    emit(o, "#define NS_C_BODY    0x00181820u\n");
    emit(o, "#define NS_C_CLOSE   0x00CC2222u\n");
    emit(o, "#define NS_C_TXT_W   0x00FFFFFFu\n");
    emit(o, "#define NS_C_TXT_G   0x0080C0FFu\n\n");

    /* button states + colors */
    emit(o, "#define NS_BTN_NORMAL  0u\n");
    emit(o, "#define NS_BTN_HOVER   1u\n");
    emit(o, "#define NS_BTN_PRESSED 2u\n");
    emit(o, "#define NS_C_BTN_NRM   0x00304060u\n");
    emit(o, "#define NS_C_BTN_HOV   0x004080C0u\n");
    emit(o, "#define NS_C_BTN_PRS   0x001030A0u\n");
    emit(o, "#define NS_C_BTN_BDR   0x006090D0u\n\n");

    emit(o, "static void ns_wm_init(void) {\n");
    emit(o, "    for(int i=0;i<NS_WM_MAX;i++) _ns_wins[i].visible=0;\n");
    emit(o, "    _ns_win_count=0; _ns_win_focused=-1; _ns_win_dragging=-1;\n}\n\n");

    /* draw_button: normal/hover/pressed states, centered text */
    emit(o, "static void ns_wm_draw_button(int32_t x,int32_t y,int32_t w,int32_t h,\n");
    emit(o, "                               const char *text,uint8_t state) {\n");
    emit(o, "    uint32_t bg=(state==NS_BTN_PRESSED)?NS_C_BTN_PRS:\n");
    emit(o, "               (state==NS_BTN_HOVER)  ?NS_C_BTN_HOV:NS_C_BTN_NRM;\n");
    emit(o, "    /* drop shadow */\n");
    emit(o, "    ns_fb_fill((uint32_t)(x+2),(uint32_t)(y+2),(uint32_t)w,(uint32_t)h,0x00080810u);\n");
    emit(o, "    /* border */\n");
    emit(o, "    ns_fb_fill((uint32_t)x,(uint32_t)y,(uint32_t)w,(uint32_t)h,NS_C_BTN_BDR);\n");
    emit(o, "    /* body */\n");
    emit(o, "    ns_fb_fill((uint32_t)(x+1),(uint32_t)(y+1),(uint32_t)(w-2),(uint32_t)(h-2),bg);\n");
    emit(o, "    /* centered text */\n");
    emit(o, "    int32_t tw=0; const char *s=text; while(*s++){tw+=9;}\n");
    emit(o, "    int32_t tx=x+(w-tw)/2, ty=y+(h-8)/2;\n");
    emit(o, "    if(state==NS_BTN_PRESSED){tx++;ty++;}\n");
    emit(o, "    ns_fb_str((uint32_t)tx,(uint32_t)ty,text,NS_C_TXT_W,bg);\n}\n\n");

    /* hit test for buttons */
    emit(o, "static int32_t ns_wm_button_hit(int32_t x,int32_t y,int32_t w,int32_t h,\n");
    emit(o, "                                  int32_t mx,int32_t my){\n");
    emit(o, "    return (mx>=x&&mx<x+w&&my>=y&&my<y+h)?1:0;\n}\n\n");

    /* button state based on mouse position + click */
    emit(o, "static uint8_t ns_wm_button_state(int32_t x,int32_t y,int32_t w,int32_t h){\n");
    emit(o, "    if(!ns_wm_button_hit(x,y,w,h,_ns_mouse_x,_ns_mouse_y)) return NS_BTN_NORMAL;\n");
    emit(o, "    return (_ns_mouse_btn&0x01)?NS_BTN_PRESSED:NS_BTN_HOVER;\n}\n\n");

    emit(o, "static int32_t ns_wm_create(const char *title,int32_t x,int32_t y,int32_t w,int32_t h) {\n");
    emit(o, "    if(_ns_win_count>=NS_WM_MAX) return -1;\n");
    emit(o, "    int32_t idx=_ns_win_count++;\n");
    emit(o, "    NS_Win *win=&_ns_wins[idx];\n");
    emit(o, "    win->x=x; win->y=y; win->w=w; win->h=h; win->title=title;\n");
    emit(o, "    win->visible=1; win->focused=0; win->closing=0;\n");
    emit(o, "    win->drag_dx=0; win->drag_dy=0;\n");
    emit(o, "    if(_ns_win_focused<0) _ns_win_focused=idx;\n");
    emit(o, "    return idx;\n}\n\n");

    emit(o, "static void _ns_wm_draw_win(int32_t idx) {\n");
    emit(o, "    NS_Win *w=&_ns_wins[idx];\n");
    emit(o, "    if(!w->visible) return;\n");
    emit(o, "    uint32_t tc=(idx==_ns_win_focused)?NS_C_TB_FOC:NS_C_TB_UNF;\n");
    emit(o, "    /* border */\n");
    emit(o, "    ns_fb_fill((uint32_t)(w->x-NS_WM_BD),(uint32_t)(w->y-NS_WM_TB_H-NS_WM_BD),\n");
    emit(o, "               (uint32_t)(w->w+NS_WM_BD*2),(uint32_t)(w->h+NS_WM_TB_H+NS_WM_BD*2),NS_C_BORDER);\n");
    emit(o, "    /* title bar */\n");
    emit(o, "    ns_fb_fill((uint32_t)w->x,(uint32_t)(w->y-NS_WM_TB_H),(uint32_t)w->w,(uint32_t)NS_WM_TB_H,tc);\n");
    emit(o, "    /* close button */\n");
    emit(o, "    ns_fb_fill((uint32_t)(w->x+w->w-NS_WM_CB_W-3),(uint32_t)(w->y-NS_WM_TB_H+2),\n");
    emit(o, "               (uint32_t)NS_WM_CB_W,(uint32_t)(NS_WM_TB_H-4),NS_C_CLOSE);\n");
    emit(o, "    ns_fb_str((uint32_t)(w->x+w->w-NS_WM_CB_W)  ,(uint32_t)(w->y-NS_WM_TB_H+4),\"X\",NS_C_TXT_W,NS_C_CLOSE);\n");
    emit(o, "    /* title text */\n");
    emit(o, "    ns_fb_str((uint32_t)(w->x+5),(uint32_t)(w->y-NS_WM_TB_H+4),w->title,NS_C_TXT_W,tc);\n");
    emit(o, "    /* body */\n");
    emit(o, "    ns_fb_fill((uint32_t)w->x,(uint32_t)w->y,(uint32_t)w->w,(uint32_t)w->h,NS_C_BODY);\n}\n\n");

    /* taskbar clock helper */
    emit(o, "static void _ns_taskbar_clock(void) {\n");
    emit(o, "    uint32_t secs=ns_ticks/100u;\n");   /* 100 Hz timer */
    emit(o, "    uint32_t s=secs%%60u,m=(secs/60u)%%60u,h=(secs/3600u)%%24u;\n");
    emit(o, "    char clk[9];\n");
    emit(o, "    clk[0]=(char)('0'+h/10u); clk[1]=(char)('0'+h%%10u); clk[2]=':';\n");
    emit(o, "    clk[3]=(char)('0'+m/10u); clk[4]=(char)('0'+m%%10u); clk[5]=':';\n");
    emit(o, "    clk[6]=(char)('0'+s/10u); clk[7]=(char)('0'+s%%10u); clk[8]=0;\n");
    emit(o, "    uint32_t cx=(ns_fb_w>100)?(ns_fb_w-84):0;\n");
    emit(o, "    ns_fb_fill(cx,ns_fb_h-24u,84u,24u,NS_C_TASKBAR);\n");
    emit(o, "    ns_fb_str(cx+6u,ns_fb_h-17u,clk,NS_C_TXT_W,NS_C_TASKBAR);\n}\n\n");

    emit(o, "static void ns_wm_render(void) {\n");
    emit(o, "    ns_fb_clear(NS_C_DESKTOP);\n");
    emit(o, "    /* taskbar */\n");
    emit(o, "    ns_fb_fill(0u,ns_fb_h-24u,ns_fb_w,24u,NS_C_TASKBAR);\n");
    emit(o, "    ns_fb_str(6u,ns_fb_h-17u,\"NightOS v0.6  \\xb7  ApexForge NightScript\",NS_C_TXT_G,NS_C_TASKBAR);\n");
    emit(o, "    /* window buttons in taskbar */\n");
    emit(o, "    for(int i=0;i<_ns_win_count;i++){\n");
    emit(o, "        if(!_ns_wins[i].visible) continue;\n");
    emit(o, "        uint32_t tc=(i==_ns_win_focused)?NS_C_TB_FOC:NS_C_TB_UNF;\n");
    emit(o, "        uint32_t bx=(uint32_t)(280+i*110);\n");
    emit(o, "        ns_fb_fill(bx,ns_fb_h-22u,106u,20u,tc);\n");
    emit(o, "        ns_fb_str(bx+4u,ns_fb_h-17u,_ns_wins[i].title,NS_C_TXT_W,tc);\n");
    emit(o, "    }\n");
    emit(o, "    _ns_taskbar_clock();\n");
    emit(o, "    /* draw unfocused windows first, then focused on top */\n");
    emit(o, "    for(int i=0;i<_ns_win_count;i++) if(i!=_ns_win_focused) _ns_wm_draw_win(i);\n");
    emit(o, "    if(_ns_win_focused>=0) _ns_wm_draw_win(_ns_win_focused);\n}\n\n");

    emit(o, "static void ns_wm_handle_mouse(int32_t mx,int32_t my,uint8_t btn) {\n");
    emit(o, "    uint8_t pressed  = btn & (uint8_t)(~_ns_wm_prev_btn);\n");
    emit(o, "    uint8_t released = _ns_wm_prev_btn & (uint8_t)(~btn);\n");
    emit(o, "    if(pressed&0x01) {\n");
    emit(o, "        for(int i=_ns_win_count-1;i>=0;i--) {\n");
    emit(o, "            NS_Win *w=&_ns_wins[i]; if(!w->visible) continue;\n");
    emit(o, "            int32_t tbx=w->x, tby=w->y-NS_WM_TB_H;\n");
    emit(o, "            if(mx>=tbx&&mx<tbx+w->w&&my>=tby&&my<tby+NS_WM_TB_H) {\n");
    emit(o, "                _ns_win_focused=i;\n");
    emit(o, "                /* close button? */\n");
    emit(o, "                if(mx>=w->x+w->w-NS_WM_CB_W-3&&mx<w->x+w->w-3) {\n");
    emit(o, "                    w->visible=0; w->closing=1;\n");
    emit(o, "                } else {\n");
    emit(o, "                    _ns_win_dragging=i;\n");
    emit(o, "                    w->drag_dx=mx-w->x; w->drag_dy=my-w->y;\n");
    emit(o, "                }\n");
    emit(o, "                break;\n");
    emit(o, "            }\n");
    emit(o, "            if(mx>=w->x&&mx<w->x+w->w&&my>=w->y&&my<w->y+w->h) {\n");
    emit(o, "                _ns_win_focused=i; break;\n");
    emit(o, "            }\n");
    emit(o, "        }\n    }\n");
    emit(o, "    if((btn&0x01)&&_ns_win_dragging>=0) {\n");
    emit(o, "        NS_Win *w=&_ns_wins[_ns_win_dragging];\n");
    emit(o, "        w->x=mx-w->drag_dx; w->y=my-w->drag_dy;\n");
    emit(o, "        if(w->x<0) w->x=0;\n");
    emit(o, "        if(w->y<NS_WM_TB_H) w->y=NS_WM_TB_H;\n");
    emit(o, "        if(w->x+w->w>(int32_t)ns_fb_w)  w->x=(int32_t)ns_fb_w-w->w;\n");
    emit(o, "        if(w->y+w->h>(int32_t)ns_fb_h-24) w->y=(int32_t)ns_fb_h-24-w->h;\n");
    emit(o, "    }\n");
    emit(o, "    if(released&0x01) _ns_win_dragging=-1;\n");
    emit(o, "    _ns_wm_prev_btn=btn;\n}\n\n");

    emit(o, "static inline NS_Win *ns_wm_get(int32_t idx) {\n");
    emit(o, "    return (idx>=0&&idx<_ns_win_count)?&_ns_wins[idx]:(void*)0;\n}\n");
    emit(o, "static inline uint8_t ns_win_visible(int32_t idx) { NS_Win *w=ns_wm_get(idx); return w&&w->visible; }\n");
    emit(o, "static inline uint8_t ns_win_closing(int32_t idx) { NS_Win *w=ns_wm_get(idx); return w&&w->closing; }\n");
    emit(o, "static inline int32_t ns_win_x(int32_t idx) { NS_Win *w=ns_wm_get(idx); return w?w->x:0; }\n");
    emit(o, "static inline int32_t ns_win_y(int32_t idx) { NS_Win *w=ns_wm_get(idx); return w?w->y:0; }\n");
    emit(o, "static inline int32_t ns_win_w(int32_t idx) { NS_Win *w=ns_wm_get(idx); return w?w->w:0; }\n");
    emit(o, "static inline int32_t ns_win_h(int32_t idx) { NS_Win *w=ns_wm_get(idx); return w?w->h:0; }\n\n");

    /* ── Terminal emulator (with scrollback buffer) ─────────────────────── */
    emit(o, "/* ── v0.6: Terminal emulator (scrollback) ─── */\n");
    emit(o, "#define NS_TERM_COLS   64\n");
    emit(o, "#define NS_TERM_ROWS   22\n");
    emit(o, "#define NS_TERM_SCROLL 200\n");  /* scrollback history lines */
    emit(o, "#define NS_TERM_MAX     4\n\n");

    emit(o, "typedef struct {\n");
    emit(o, "    char     buf[NS_TERM_SCROLL * NS_TERM_COLS]; /* ring buffer */\n");
    emit(o, "    int32_t  total;    /* total rows ever written */\n");
    emit(o, "    int32_t  cx;       /* cursor column in current row */\n");
    emit(o, "    int32_t  view_off; /* lines scrolled above bottom (0=follow) */\n");
    emit(o, "    int32_t  win_idx;\n");
    emit(o, "    uint32_t fg, bg;\n");
    emit(o, "    uint8_t  active;\n");
    emit(o, "} NS_Term;\n");
    emit(o, "static NS_Term _ns_terms[NS_TERM_MAX];\n\n");

    emit(o, "static void ns_term_init(int32_t tidx,int32_t widx,uint32_t fg,uint32_t bg) {\n");
    emit(o, "    NS_Term *t=&_ns_terms[tidx];\n");
    emit(o, "    for(int i=0;i<NS_TERM_SCROLL*NS_TERM_COLS;i++) t->buf[i]=' ';\n");
    emit(o, "    t->total=0; t->cx=0; t->view_off=0;\n");
    emit(o, "    t->win_idx=widx; t->fg=fg; t->bg=bg; t->active=1;\n}\n\n");

    emit(o, "static void ns_term_putch(int32_t tidx,uint8_t c) {\n");
    emit(o, "    NS_Term *t=&_ns_terms[tidx]; if(!t->active) return;\n");
    emit(o, "    int32_t head=t->total%%NS_TERM_SCROLL;\n");
    emit(o, "    if(c=='\\n'||c=='\\r') {\n");
    emit(o, "        t->cx=0; t->total++;\n");
    emit(o, "        int32_t nh=t->total%%NS_TERM_SCROLL;\n");
    emit(o, "        for(int i=0;i<NS_TERM_COLS;i++) t->buf[nh*NS_TERM_COLS+i]=' ';\n");
    emit(o, "        t->view_off=0; return;\n");
    emit(o, "    }\n");
    emit(o, "    if(c=='\\b') {\n");
    emit(o, "        if(t->cx>0){t->cx--; t->buf[head*NS_TERM_COLS+t->cx]=' ';} return;\n");
    emit(o, "    }\n");
    emit(o, "    if(c<32||c>126) c='?';\n");
    emit(o, "    if(t->cx>=NS_TERM_COLS) {\n");
    emit(o, "        t->cx=0; t->total++;\n");
    emit(o, "        int32_t nh=t->total%%NS_TERM_SCROLL;\n");
    emit(o, "        for(int i=0;i<NS_TERM_COLS;i++) t->buf[nh*NS_TERM_COLS+i]=' ';\n");
    emit(o, "        head=nh;\n");
    emit(o, "    }\n");
    emit(o, "    t->buf[head*NS_TERM_COLS+t->cx]=(char)c;\n");
    emit(o, "    t->cx++;\n}\n\n");

    emit(o, "static void ns_term_puts(int32_t tidx,const char *s){\n");
    emit(o, "    while(*s) ns_term_putch(tidx,(uint8_t)*s++);\n}\n\n");

    emit(o, "static void ns_term_scroll_up(int32_t tidx,int32_t n) {\n");
    emit(o, "    NS_Term *t=&_ns_terms[tidx]; if(!t->active) return;\n");
    emit(o, "    int32_t avail=t->total-(NS_TERM_ROWS-1);\n");
    emit(o, "    if(avail>NS_TERM_SCROLL-NS_TERM_ROWS) avail=NS_TERM_SCROLL-NS_TERM_ROWS;\n");
    emit(o, "    if(avail<0) avail=0;\n");
    emit(o, "    t->view_off+=n; if(t->view_off>avail) t->view_off=avail;\n}\n\n");

    emit(o, "static void ns_term_scroll_down(int32_t tidx,int32_t n) {\n");
    emit(o, "    NS_Term *t=&_ns_terms[tidx]; if(!t->active) return;\n");
    emit(o, "    t->view_off-=n; if(t->view_off<0) t->view_off=0;\n}\n\n");

    emit(o, "static void ns_term_render(int32_t tidx) {\n");
    emit(o, "    NS_Term *t=&_ns_terms[tidx];\n");
    emit(o, "    if(!t->active||t->win_idx<0) return;\n");
    emit(o, "    NS_Win *w=ns_wm_get(t->win_idx); if(!w||!w->visible) return;\n");
    emit(o, "    /* clamp view_off */\n");
    emit(o, "    int32_t avail=t->total-(NS_TERM_ROWS-1);\n");
    emit(o, "    if(avail>NS_TERM_SCROLL-NS_TERM_ROWS) avail=NS_TERM_SCROLL-NS_TERM_ROWS;\n");
    emit(o, "    if(avail<0) avail=0;\n");
    emit(o, "    if(t->view_off>avail) t->view_off=avail;\n");
    emit(o, "    if(t->view_off<0)     t->view_off=0;\n");
    emit(o, "    static char row_buf[NS_TERM_COLS+1];\n");
    emit(o, "    for(int r=0;r<NS_TERM_ROWS;r++) {\n");
    emit(o, "        /* abs_row = bottom of visible area is (total-view_off), top is that-ROWS+1 */\n");
    emit(o, "        int32_t abs=t->total-t->view_off-(NS_TERM_ROWS-1-r);\n");
    emit(o, "        if(abs<0||abs>t->total) {\n");
    emit(o, "            for(int c=0;c<NS_TERM_COLS;c++) row_buf[c]=' ';\n");
    emit(o, "        } else {\n");
    emit(o, "            int32_t bi=((abs%%NS_TERM_SCROLL)+NS_TERM_SCROLL)%%NS_TERM_SCROLL;\n");
    emit(o, "            char *row=&t->buf[bi*NS_TERM_COLS];\n");
    emit(o, "            for(int c=0;c<NS_TERM_COLS;c++) row_buf[c]=row[c];\n");
    emit(o, "        }\n");
    emit(o, "        row_buf[NS_TERM_COLS]=0;\n");
    emit(o, "        ns_fb_str((uint32_t)(w->x+4),(uint32_t)(w->y+2+r*10),row_buf,t->fg,t->bg);\n");
    emit(o, "    }\n");
    emit(o, "    /* cursor bar (only when at bottom) */\n");
    emit(o, "    if(t->view_off==0)\n");
    emit(o, "        ns_fb_fill((uint32_t)(w->x+4+t->cx*9),(uint32_t)(w->y+2+(NS_TERM_ROWS-1)*10+8),8u,2u,t->fg);\n");
    emit(o, "    /* scrollback indicator */\n");
    emit(o, "    if(t->view_off>0) {\n");
    emit(o, "        ns_fb_str((uint32_t)(w->x+w->w-54u),(uint32_t)(w->y+4),\"[SCROLL]\",NS_C_TXT_G,NS_C_BODY);\n");
    emit(o, "    }\n}\n\n");

    /* ── Basic Shell ─────────────────────────────────────────────────────── */
    emit(o, "/* ── v0.6: Basic shell ─── */\n");
    emit(o, "#define NS_SHELL_BUF 256\n");
    emit(o, "static char    _ns_shell_line[NS_SHELL_BUF];\n");
    emit(o, "static int32_t _ns_shell_len  = 0;\n");
    emit(o, "static int32_t _ns_shell_term = -1;\n\n");

    /* helper: streq */
    emit(o, "static int _ns_streq(const char *a,const char *b) {\n");
    emit(o, "    while(*a&&*b&&*a==*b){a++;b++;} return *a==*b;\n}\n");
    emit(o, "static const char *_ns_skip_sp(const char *s) { while(*s==' ')s++; return s; }\n\n");

    /* helper: print uint32 to terminal */
    emit(o, "static void _ns_term_putu(int32_t t,uint32_t n){\n");
    emit(o, "    char tmp[12]; int i=11; tmp[i]=0;\n");
    emit(o, "    if(n==0){tmp[--i]='0';} else{while(n>0){tmp[--i]=(char)('0'+n%%10);n/=10;}}\n");
    emit(o, "    ns_term_puts(t,tmp+i);\n}\n\n");

    emit(o, "static void ns_shell_init(int32_t term_idx);\n\n");
    emit(o, "static void ns_shell_exec(const char *cmd) {\n");
    emit(o, "    int32_t t=_ns_shell_term; if(t<0) return;\n");
    emit(o, "    cmd=_ns_skip_sp(cmd);\n");
    emit(o, "    if(*cmd==0) { /* empty line */ }\n");
    emit(o, "    else if(_ns_streq(cmd,\"help\")) {\n");
    emit(o, "        ns_term_puts(t,\"Commands:\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  help      show this list\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  clear     clear screen\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  version   show version\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  halt      halt system\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  reboot    reboot system\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  mem       memory info\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  uptime    show uptime\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  ls        list files\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  ps        list processes\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  color <n> change color (0-7)\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  echo <text>\\r\\n\");\n");
    emit(o, "    } else if(_ns_streq(cmd,\"clear\")) {\n");
    emit(o, "        int32_t wi=_ns_terms[t].win_idx;\n");
    emit(o, "        uint32_t fg=_ns_terms[t].fg, bg=_ns_terms[t].bg;\n");
    emit(o, "        ns_term_init(t,wi,fg,bg); ns_shell_init(t); return;\n");
    emit(o, "    } else if(_ns_streq(cmd,\"version\")) {\n");
    emit(o, "        ns_term_puts(t,\"NightOS v0.6  (ApexForge NightScript Kernel)\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"Compiler: ApexForge NightScript v0.6\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"Arch: x86 (32-bit protected mode)\\r\\n\");\n");
    emit(o, "    } else if(_ns_streq(cmd,\"halt\")) {\n");
    emit(o, "        ns_term_puts(t,\"System halted. Safe to power off.\\r\\n\");\n");
    emit(o, "        ns_cpu_cli(); for(;;) ns_cpu_halt();\n");
    emit(o, "    } else if(_ns_streq(cmd,\"reboot\")) {\n");
    emit(o, "        ns_term_puts(t,\"Rebooting...\\r\\n\");\n");
    emit(o, "        ns_cpu_cli();\n");
    emit(o, "        _ns_mouse_wait_in(); ns_outb(0x64u,0xFEu);\n");
    emit(o, "        for(;;) ns_cpu_halt();\n");
    emit(o, "    } else if(_ns_streq(cmd,\"mem\")) {\n");
    emit(o, "        uint32_t fp=ns_pmm_free_count();\n");
    emit(o, "        ns_term_puts(t,\"Free pages : \"); _ns_term_putu(t,fp);\n");
    emit(o, "        ns_term_puts(t,\"\\r\\nFree KB    : \"); _ns_term_putu(t,fp*4u);\n");
    emit(o, "        ns_term_puts(t,\"\\r\\nFB size    : \"); _ns_term_putu(t,ns_fb_w*ns_fb_h*4u/1024u);\n");
    emit(o, "        ns_term_puts(t,\" KB\\r\\n\");\n");
    emit(o, "    } else if(_ns_streq(cmd,\"uptime\")) {\n");
    emit(o, "        uint32_t secs=ns_ticks/100u;\n");
    emit(o, "        ns_term_puts(t,\"Uptime: \"); _ns_term_putu(t,secs);\n");
    emit(o, "        ns_term_puts(t,\"s  (ticks: \"); _ns_term_putu(t,ns_ticks);\n");
    emit(o, "        ns_term_puts(t,\")\\r\\n\");\n");
    emit(o, "    } else if(_ns_streq(cmd,\"ls\")) {\n");
    emit(o, "        ns_term_puts(t,\"bin/   lib/   etc/   home/  tmp/   dev/\\r\\n\");\n");
    emit(o, "    } else if(_ns_streq(cmd,\"ps\")) {\n");
    emit(o, "        ns_term_puts(t,\"PID  STAT  NAME\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  1  R     kernel\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  2  S     wm\\r\\n\");\n");
    emit(o, "        ns_term_puts(t,\"  3  R     shell\\r\\n\");\n");
    emit(o, "    } else if(cmd[0]=='c'&&cmd[1]=='o'&&cmd[2]=='l'&&cmd[3]=='o'&&cmd[4]=='r') {\n");
    emit(o, "        const char *arg=_ns_skip_sp(cmd+5);\n");
    emit(o, "        uint32_t palette[8]={\n");
    emit(o, "            0x00FFFFFFu,0x0080FF80u,0x0080C0FFu,0x00FFD080u,\n");
    emit(o, "            0x00FF8080u,0x00C080FFu,0x0080FFFFu,0x00C0C0C0u};\n");
    emit(o, "        uint32_t n=0;\n");
    emit(o, "        while(*arg>='0'&&*arg<='9') n=n*10u+(uint32_t)(*arg++-'0');\n");
    emit(o, "        if(n>7u) n=7u;\n");
    emit(o, "        _ns_terms[t].fg=palette[n];\n");
    emit(o, "        ns_term_puts(t,\"Color set.\\r\\n\");\n");
    emit(o, "    } else if(cmd[0]=='e'&&cmd[1]=='c'&&cmd[2]=='h'&&cmd[3]=='o'&&(cmd[4]==' '||cmd[4]==0)) {\n");
    emit(o, "        ns_term_puts(t,_ns_skip_sp(cmd+4)); ns_term_puts(t,\"\\r\\n\");\n");
    emit(o, "    } else {\n");
    emit(o, "        ns_term_puts(t,\"Unknown command: '\"); ns_term_puts(t,cmd);\n");
    emit(o, "        ns_term_puts(t,\"'  (type 'help')\\r\\n\");\n");
    emit(o, "    }\n");
    emit(o, "    ns_term_puts(t,\"> \");\n}\n\n");

    emit(o, "static void ns_shell_init(int32_t term_idx) {\n");
    emit(o, "    _ns_shell_term=term_idx; _ns_shell_len=0;\n");
    emit(o, "    ns_term_puts(term_idx,\"NightOS v0.6 — NightScript Shell\\r\\n\");\n");
    emit(o, "    ns_term_puts(term_idx,\"Type 'help' for commands.\\r\\n> \");\n}\n\n");

    emit(o, "static void ns_shell_on_key(uint8_t scancode) {\n");
    emit(o, "    if(_ns_shell_term<0) return;\n");
    emit(o, "    uint8_t c=ns_keyboard_to_ascii(scancode);\n");
    emit(o, "    if(!c) return;\n");
    emit(o, "    if(c=='\\n'||c=='\\r') {\n");
    emit(o, "        ns_term_putch(_ns_shell_term,'\\n');\n");
    emit(o, "        _ns_shell_line[_ns_shell_len]=0;\n");
    emit(o, "        ns_shell_exec(_ns_shell_line);\n");
    emit(o, "        _ns_shell_len=0;\n");
    emit(o, "    } else if(c=='\\b') {\n");
    emit(o, "        if(_ns_shell_len>0) { _ns_shell_len--; ns_term_putch(_ns_shell_term,'\\b'); }\n");
    emit(o, "    } else if(c>=32&&c<127&&_ns_shell_len<NS_SHELL_BUF-1) {\n");
    emit(o, "        _ns_shell_line[_ns_shell_len++]=c;\n");
    emit(o, "        ns_term_putch(_ns_shell_term,c);\n");
    emit(o, "    }\n}\n\n");

    /* ── OS-level init and render ────────────────────────────────────────── */
    emit(o, "/* ── v0.6: OS event loop helpers ─── */\n");
    emit(o, "static uint8_t _ns_os_last_scan = 0;\n");
    emit(o, "static uint8_t _ns_os_extended  = 0;\n\n");  /* E0 prefix tracking */

    emit(o, "static void ns_os_poll(void) {\n");
    emit(o, "    if(!(ns_inb(0x64)&0x01)) return;\n");
    emit(o, "    uint8_t sc=ns_inb(0x60);\n");
    emit(o, "    if(sc==0xE0) { _ns_os_extended=1; return; }\n");
    emit(o, "    uint8_t ext=_ns_os_extended; _ns_os_extended=0;\n");
    emit(o, "    if(sc&0x80) { _ns_os_last_scan=0; return; } /* key up */\n");
    emit(o, "    if(sc==_ns_os_last_scan) return;\n");
    emit(o, "    _ns_os_last_scan=sc;\n");
    emit(o, "    /* extended keys: Page Up=0x49, Page Down=0x51 → terminal scrollback */\n");
    emit(o, "    if(ext&&sc==0x49) { /* Page Up */\n");
    emit(o, "        if(_ns_shell_term>=0) ns_term_scroll_up(_ns_shell_term,4);\n");
    emit(o, "        return;\n");
    emit(o, "    }\n");
    emit(o, "    if(ext&&sc==0x51) { /* Page Down */\n");
    emit(o, "        if(_ns_shell_term>=0) ns_term_scroll_down(_ns_shell_term,4);\n");
    emit(o, "        return;\n");
    emit(o, "    }\n");
    emit(o, "    ns_shell_on_key(sc);\n}\n\n");

    emit(o, "static void ns_os_render(void) {\n");
    emit(o, "    ns_wm_render();\n");
    emit(o, "    /* render all active terminals */\n");
    emit(o, "    for(int i=0;i<NS_TERM_MAX;i++) if(_ns_terms[i].active) ns_term_render(i);\n");
    emit(o, "    /* draw mouse cursor on top */\n");
    emit(o, "    ns_draw_cursor(_ns_mouse_x,_ns_mouse_y);\n}\n\n");

    emit(o, "static void ns_os_init(void) {\n");
    emit(o, "    ns_wm_init();\n");
    emit(o, "    for(int i=0;i<NS_TERM_MAX;i++) _ns_terms[i].active=0;\n");
    emit(o, "    _ns_shell_term=-1; _ns_shell_len=0;\n}\n\n");

    /* ── Kernel entry point ──────────────────────────────────────────────── */
    emit(o, "/* ── Kernel entry ─── */\n");
    emit(o, "void main(void);\n"); /* defined below in user code */
    emit(o, "__attribute__((noreturn))\n");
    emit(o, "void kernel_main(void) {\n");
    emit(o, "    /* capture Multiboot2 registers before any C prologue clobbers them */\n");
    emit(o, "    uint32_t _mb2_magic=0, _mb2_info=0;\n");
    emit(o, "    __asm__ volatile(\"\" : \"=a\"(_mb2_magic),\"=b\"(_mb2_info));\n");
    emit(o, "\n");
    emit(o, "    ns_cpu_cli();\n");
    emit(o, "    ns_gdt_init();\n");
    emit(o, "    ns_interrupts_init();\n");
    emit(o, "    ns_serial_init();\n");
    emit(o, "    ns_vga_clear();\n");
    emit(o, "    ns_vga_print(\"NightOS v0.6 — booting...\\n\");\n");
    emit(o, "\n");
    emit(o, "    /* init framebuffer from Multiboot2 info (magic = 0x36d76289) */\n");
    emit(o, "    if(_mb2_magic==0x36d76289u) {\n");
    emit(o, "        _ns_mb2_init_fb(_mb2_info);\n");
    emit(o, "    }\n");
    emit(o, "    /* fallback: QEMU/Bochs VESA framebuffer at 0xFD000000 (1024x768x32) */\n");
    emit(o, "    if(!ns_fb) {\n");
    emit(o, "        ns_fb_init((uint32_t*)0xFD000000u, 1024u, 768u, 1024u*4u);\n");
    emit(o, "    }\n");
    emit(o, "\n");
    emit(o, "    /* init timer (100 Hz), keyboard, mouse */\n");
    emit(o, "    ns_timer_init(100u);\n");
    emit(o, "    ns_keyboard_init();\n");
    emit(o, "    ns_pmm_init(65536u, 0x400000u); /* assume 64 MB, bitmap at 4 MB */\n");
    emit(o, "\n");
    emit(o, "    /* init WM + OS subsystems */\n");
    emit(o, "    ns_os_init();\n");
    emit(o, "    ns_mouse_init();\n");
    emit(o, "\n");
    emit(o, "    ns_cpu_sti();\n");
    emit(o, "    ns_serial_println(\"NightOS v0.6 kernel_main: calling main()\");\n");
    emit(o, "\n");
    emit(o, "    main();\n");
    emit(o, "    for(;;) ns_cpu_halt();\n");
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
    else {
        emit(out, "/* NightScript Kernel — freestanding (no libc) */\n\n");
        emit(out, "/* ── Freestanding types ─── */\n");
        emit(out, "typedef unsigned char      uint8_t;\n");
        emit(out, "typedef unsigned short     uint16_t;\n");
        emit(out, "typedef unsigned int       uint32_t;\n");
        emit(out, "typedef unsigned long long uint64_t;\n");
        emit(out, "typedef signed char        int8_t;\n");
        emit(out, "typedef signed short       int16_t;\n");
        emit(out, "typedef signed int         int32_t;\n");
        emit(out, "typedef signed long long   int64_t;\n");
        emit(out, "typedef unsigned long      uintptr_t;\n");
        emit(out, "typedef unsigned long      size_t;\n");
        emit(out, "#define NULL ((void*)0)\n\n");
    }
    if (has_ui_app(program))
        emit(out, "#include <SDL2/SDL.h>\n\n");

    /* ── v0.7: build generic registries + prescan ── */
    g_sub_count = 0;
    g_gfn_count = 0;
    g_gst_count = 0;
    for (GenFnInst *gi = g_gfn_insts; gi; ) { GenFnInst *nx = gi->next; free(gi); gi = nx; }
    g_gfn_insts = NULL;
    for (GenStInst *gi = g_gst_insts; gi; ) { GenStInst *nx = gi->next; free(gi); gi = nx; }
    g_gst_insts = NULL;
    for (int i = 0; i < program->as.program.decls.count; i++) {
        Node *d = program->as.program.decls.items[i];
        if (d->kind == NODE_FN_DECL && d->as.fn.type_params.count > 0 &&
                g_gfn_count < 64) {
            g_gfn_decls[g_gfn_count] = d;
            g_gfn_names[g_gfn_count] = d->as.fn.name;
            g_gfn_count++;
        }
        if (d->kind == NODE_STRUCT_DECL && d->as.struct_decl.type_params.count > 0 &&
                g_gst_count < 64) {
            g_gst_decls[g_gst_count] = d;
            g_gst_names[g_gst_count] = d->as.struct_decl.name;
            g_gst_count++;
        }
    }
    prescan_program(program);
    /* ── end v0.7 setup ── */

    emit_type_definitions(out, program);
    if (!is_kernel) {
        emit_stdlib_runtime(out);
        emit_io_runtime(out);
    }
    emit_prototypes(out, program);
    if (is_kernel)
        emit_kernel_app(out, program);  /* runtime must precede user fn bodies */
    emit_definitions(out, program);
    if (has_ui_app(program))
        emit_ui_app(out, program);

    return 1;
}

void cout_free(COut *out) {
    free(out->buf);
    out->buf = NULL;
    out->len = out->cap = 0;
}
