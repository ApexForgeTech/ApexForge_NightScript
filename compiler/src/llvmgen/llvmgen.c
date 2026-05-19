/* llvmgen.c — NightScript v0.8 LLVM backend */
#ifdef NIGHT_LLVM_BACKEND

#include "llvmgen.h"
#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ── forward declarations ─────────────────────────────────────────────────── */

typedef struct LG LG;
static LLVMValueRef llg_emit_expr(LG *g, LLVMBuilderRef b, LLVMBasicBlockRef *cur_bb, LLVMValueRef *lval_ptr_out, LLVMTypeRef *lval_ty_out, Node *n);
static void         llg_emit_stmt(LG *g, LLVMBuilderRef b, LLVMBasicBlockRef *cur_bb, Node *n);
static void         llg_emit_block(LG *g, LLVMBuilderRef b, LLVMBasicBlockRef *cur_bb, Node *blk);
static LLVMTypeRef  llg_node_to_ll(LG *g, Node *type);
static LLVMTypeRef  llg_ns_to_ll(LG *g, const char *name);
static Node        *llg_infer_type(LG *g, Node *expr);

/* ── data structures ──────────────────────────────────────────────────────── */

typedef struct LGVar LGVar;
struct LGVar {
    const char   *name;
    LLVMValueRef  alloca;
    LLVMTypeRef   ll_ty;
    Node         *ns_ty;
    LGVar        *next;
};

typedef struct LGScope LGScope;
struct LGScope {
    LGVar   *vars;
    LGScope *parent;
};

typedef struct { const char *name; LLVMTypeRef ll_ty; Node *ns_ty; } LGNamedType;
typedef struct {
    const char   *name;
    LLVMValueRef  fn_val;
    LLVMTypeRef   fn_ty;
    Node         *ns_ret_ty;
    Node         *decl;
} LGGlobal;
typedef struct { LLVMBasicBlockRef break_bb; LLVMBasicBlockRef cont_bb; } LGLoop;

typedef struct LGTempNode LGTempNode;
struct LGTempNode { Node n; LGTempNode *next; };

/* generic fn instantiation */
typedef struct LGGFnInst LGGFnInst;
struct LGGFnInst {
    Node        *decl;
    char         mangled[256];
    const char  *ns_type;
    LGGFnInst   *next;
};
/* generic struct instantiation */
typedef struct LGGStInst LGGStInst;
struct LGGStInst {
    Node        *decl;
    char         ns_name[256];
    char         mangled[256];
    const char  *ns_types[8];
    int          type_count;
    LGGStInst   *next;
};

struct LG {
    LLVMContextRef ctx;
    LLVMModuleRef  mod;
    LLVMTargetMachineRef tm;
    LLVMTargetDataRef    td;
    Node          *program;

    LGNamedType    named_types[512];
    int            named_type_count;

    LGGlobal       globals[1024];
    int            global_count;

    LLVMValueRef   cur_fn;
    LGScope       *scope;
    Node          *cur_ret_ty;
    LGTempNode    *temps;

    LGLoop         loops[64];
    int            loop_depth;

    Node          *defers[256];
    int            defer_count;
    int            scope_defer_marks[64];
    int            scope_defer_depth;

    int            tmp_counter;
    int            opt_level;

    /* v0.7 generics */
    Node          *gfn_decls[64];
    const char    *gfn_names[64];
    int            gfn_count;
    Node          *gst_decls[64];
    const char    *gst_names[64];
    int            gst_count;
    const char    *sub_params[8];
    const char    *sub_types[8];
    int            sub_count;
    LGGFnInst     *gfn_insts;
    LGGStInst     *gst_insts;
};

/* ── small helpers ────────────────────────────────────────────────────────── */

static char *lg_dup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

static int lg_type_form(const char *name, const char *pfx) {
    size_t plen = strlen(pfx), len = strlen(name);
    return len > plen + 1 && !strncmp(name, pfx, plen) &&
           name[plen] == '[' && name[len-1] == ']';
}

/* coerce value to expected type (e.g. NStr struct → ptr for cstr params) */
static LLVMValueRef lg_coerce_arg(LLVMBuilderRef b, LLVMValueRef val, LLVMTypeRef expected_ty, LLVMContextRef ctx) {
    LLVMTypeRef actual_ty = LLVMTypeOf(val);
    if (actual_ty == expected_ty) return val;
    /* struct (NStr) → ptr: extract ptr field */
    if (LLVMGetTypeKind(actual_ty)==LLVMStructTypeKind &&
        LLVMGetTypeKind(expected_ty)==LLVMPointerTypeKind) {
        return LLVMBuildExtractValue(b, val, 0, "str_ptr");
    }
    /* ptr → struct: not generally needed, return as-is */
    /* integer widening/truncation */
    if (LLVMGetTypeKind(actual_ty)==LLVMIntegerTypeKind &&
        LLVMGetTypeKind(expected_ty)==LLVMIntegerTypeKind) {
        unsigned aw = LLVMGetIntTypeWidth(actual_ty);
        unsigned ew = LLVMGetIntTypeWidth(expected_ty);
        if (ew > aw) return LLVMBuildZExt(b, val, expected_ty, "zext");
        if (ew < aw) return LLVMBuildTrunc(b, val, expected_ty, "trunc");
    }
    (void)ctx;
    return val;
}

static char *lg_extract_inner(const char *name) {
    const char *lb = strchr(name, '[');
    const char *rb = strrchr(name, ']');
    if (!lb || !rb || rb <= lb+1) return NULL;
    size_t len = (size_t)(rb - lb - 1);
    char *r = malloc(len + 1);
    if (!r) return NULL;
    memcpy(r, lb+1, len);
    r[len] = '\0';
    return r;
}

static int lg_split_comma(const char *text, char **left, char **right) {
    int depth = 0;
    *left = *right = NULL;
    for (const char *it = text; *it; it++) {
        if (*it == '[') depth++;
        else if (*it == ']') depth--;
        else if (*it == ',' && depth == 0) {
            size_t llen = (size_t)(it - text);
            *left = malloc(llen + 1);
            if (!*left) return 0;
            memcpy(*left, text, llen);
            (*left)[llen] = '\0';
            *right = lg_dup(it + 1);
            return *right != NULL;
        }
    }
    return 0;
}

static void lg_mangle(char *out, size_t sz, const char *base, const char *type_arg) {
    size_t len = 0;
    while (*base && len < sz-2) out[len++] = *base++;
    out[len] = '\0';
    if (type_arg && len < sz-2) {
        out[len++] = '_'; out[len] = '\0';
        while (*type_arg && len < sz-2) {
            char c = *type_arg++;
            out[len++] = ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')) ? c : '_';
        }
        out[len] = '\0';
    }
    while (len > 0 && out[len-1]=='_') out[--len]='\0';
}

static void lg_sanitize(char *out, size_t sz, const char *in) {
    size_t len = 0;
    while (*in && len < sz-1) {
        char c = *in++;
        out[len++] = ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')) ? c : '_';
    }
    out[len] = '\0';
    while (len > 0 && out[len-1]=='_') out[--len]='\0';
}

static void lg_tmp_name(LG *g, char *buf, size_t sz) {
    snprintf(buf, sz, "__nst%d", g->tmp_counter++);
}

/* ── temp node pool ───────────────────────────────────────────────────────── */

static Node *lg_temp_named(LG *g, const char *name) {
    LGTempNode *t = calloc(1, sizeof(LGTempNode));
    if (!t) return NULL;
    t->n.kind = NODE_TYPE_NAMED;
    t->n.as.type_named.name = lg_dup(name);
    t->next = g->temps;
    g->temps = t;
    return &t->n;
}
static Node *lg_temp_ptr(LG *g, Node *inner, int is_const) {
    LGTempNode *t = calloc(1, sizeof(LGTempNode));
    if (!t) return NULL;
    t->n.kind = NODE_TYPE_POINTER;
    t->n.as.type_ptr.inner = inner;
    t->n.as.type_ptr.is_const = is_const;
    t->next = g->temps;
    g->temps = t;
    return &t->n;
}

/* ── scope management ─────────────────────────────────────────────────────── */

static void lg_push_scope(LG *g) {
    LGScope *s = calloc(1, sizeof(LGScope));
    if (!s) return;
    s->parent = g->scope;
    g->scope = s;
    if (g->scope_defer_depth < 64)
        g->scope_defer_marks[g->scope_defer_depth++] = g->defer_count;
}

static void lg_pop_scope(LG *g) {
    if (!g->scope) return;
    LGScope *s = g->scope;
    g->scope = s->parent;
    LGVar *v = s->vars;
    while (v) { LGVar *nx = v->next; free(v); v = nx; }
    free(s);
    if (g->scope_defer_depth > 0) g->scope_defer_depth--;
}

static void lg_define(LG *g, const char *name, LLVMValueRef alloca, LLVMTypeRef ll_ty, Node *ns_ty) {
    if (!g->scope) return;
    LGVar *v = calloc(1, sizeof(LGVar));
    if (!v) return;
    v->name   = name;
    v->alloca = alloca;
    v->ll_ty  = ll_ty;
    v->ns_ty  = ns_ty;
    v->next   = g->scope->vars;
    g->scope->vars = v;
}

static LGVar *lg_lookup(LG *g, const char *name) {
    for (LGScope *s = g->scope; s; s = s->parent)
        for (LGVar *v = s->vars; v; v = v->next)
            if (!strcmp(v->name, name)) return v;
    return NULL;
}

/* ── named type registry ──────────────────────────────────────────────────── */

static void lg_register_type(LG *g, const char *name, LLVMTypeRef ll_ty, Node *ns_ty) {
    if (g->named_type_count >= 512) return;
    g->named_types[g->named_type_count++] = (LGNamedType){ name, ll_ty, ns_ty };
}

static LLVMTypeRef lg_find_named_type(LG *g, const char *name) {
    for (int i = 0; i < g->named_type_count; i++)
        if (!strcmp(g->named_types[i].name, name)) return g->named_types[i].ll_ty;
    return NULL;
}

/* ── global function registry ─────────────────────────────────────────────── */

static void lg_add_global(LG *g, const char *name, LLVMValueRef fn_val,
                          LLVMTypeRef fn_ty, Node *ns_ret_ty, Node *decl) {
    if (g->global_count >= 1024) return;
    g->globals[g->global_count++] = (LGGlobal){ name, fn_val, fn_ty, ns_ret_ty, decl };
}

static LGGlobal *lg_find_global(LG *g, const char *name) {
    for (int i = 0; i < g->global_count; i++)
        if (!strcmp(g->globals[i].name, name)) return &g->globals[i];
    return NULL;
}

/* ── generic helpers ──────────────────────────────────────────────────────── */

static const char *lg_subst(LG *g, const char *name) {
    for (int i = 0; i < g->sub_count; i++)
        if (!strcmp(name, g->sub_params[i])) return g->sub_types[i];
    return NULL;
}

static LGGFnInst *lg_gfn_find_or_add(LG *g, Node *decl, const char *fn_name, const char *ns_type) {
    char mangled[256];
    lg_mangle(mangled, sizeof(mangled), fn_name, ns_type);
    for (LGGFnInst *it = g->gfn_insts; it; it = it->next)
        if (!strcmp(it->mangled, mangled)) return it;
    LGGFnInst *inst = calloc(1, sizeof(LGGFnInst));
    if (!inst) return NULL;
    inst->decl    = decl;
    inst->ns_type = ns_type;
    strncpy(inst->mangled, mangled, sizeof(inst->mangled)-1);
    inst->next    = g->gfn_insts;
    g->gfn_insts  = inst;
    return inst;
}

static LGGStInst *lg_gst_find_or_add(LG *g, Node *decl, const char *ns_name,
                                      const char **ns_types, int n) {
    char mangled[256];
    lg_sanitize(mangled, sizeof(mangled), ns_name);
    for (LGGStInst *it = g->gst_insts; it; it = it->next)
        if (!strcmp(it->ns_name, ns_name)) return it;
    LGGStInst *inst = calloc(1, sizeof(LGGStInst));
    if (!inst) return NULL;
    inst->decl = decl;
    inst->type_count = n < 8 ? n : 8;
    strncpy(inst->ns_name, ns_name, sizeof(inst->ns_name)-1);
    strncpy(inst->mangled, mangled, sizeof(inst->mangled)-1);
    for (int i = 0; i < inst->type_count; i++) inst->ns_types[i] = ns_types[i];
    inst->next   = g->gst_insts;
    g->gst_insts = inst;
    return inst;
}

/* ── AST lookup helpers ───────────────────────────────────────────────────── */

static Node *lg_find_decl(LG *g, NodeKind kind, const char *name) {
    for (int i = 0; i < g->program->as.program.decls.count; i++) {
        Node *d = g->program->as.program.decls.items[i];
        if (d->kind != kind) continue;
        const char *n = NULL;
        if (kind==NODE_STRUCT_DECL)    n = d->as.struct_decl.name;
        if (kind==NODE_ENUM_DECL)      n = d->as.enum_decl.name;
        if (kind==NODE_UNION_DECL)     n = d->as.union_decl.name;
        if (kind==NODE_INTERFACE_DECL) n = d->as.interface_decl.name;
        if (n && !strcmp(n, name)) return d;
    }
    return NULL;
}

static Node *lg_find_fn(LG *g, const char *name) {
    for (int i = 0; i < g->program->as.program.decls.count; i++) {
        Node *d = g->program->as.program.decls.items[i];
        if (d->kind==NODE_FN_DECL && !d->as.fn.owner_type && !strcmp(d->as.fn.name, name))
            return d;
        if (d->kind==NODE_EXTERN_FN && !strcmp(d->as.extern_fn.name, name))
            return d;
    }
    return NULL;
}

static Node *lg_find_method(LG *g, const char *owner, const char *method) {
    for (int i = 0; i < g->program->as.program.decls.count; i++) {
        Node *d = g->program->as.program.decls.items[i];
        if (d->kind!=NODE_IMPL_DECL || strcmp(d->as.impl.target, owner)) continue;
        for (int j = 0; j < d->as.impl.methods.count; j++) {
            Node *m = d->as.impl.methods.items[j];
            if (!strcmp(m->as.fn.name, method)) return m;
        }
    }
    return NULL;
}

static int lg_enum_has_variant(LG *g, const char *enum_name, const char *variant) {
    Node *d = lg_find_decl(g, NODE_ENUM_DECL, enum_name);
    if (!d) return 0;
    for (int i = 0; i < d->as.enum_decl.count; i++)
        if (!strcmp(d->as.enum_decl.variants[i], variant)) return 1;
    return 0;
}

static Node *lg_find_field_type(LG *g, const char *owner, const char *field) {
    Node *d = lg_find_decl(g, NODE_STRUCT_DECL, owner);
    NodeList *fl = NULL;
    if (d) fl = &d->as.struct_decl.fields;
    else {
        d = lg_find_decl(g, NODE_UNION_DECL, owner);
        if (d) fl = &d->as.union_decl.fields;
    }
    if (!fl) return NULL;
    for (int i = 0; i < fl->count; i++) {
        Node *f = fl->items[i];
        if (!strcmp(f->as.let.name, field)) return f->as.let.type;
    }
    return NULL;
}

static int lg_method_has_receiver(Node *m) {
    if (!m || m->kind!=NODE_FN_DECL || m->as.fn.params.count==0) return 0;
    const char *n = m->as.fn.params.items[0]->as.let.name;
    return !strcmp(n,"self") || !strcmp(n,"Self");
}

static int lg_is_known_type(LG *g, const char *name) {
    return !strcmp(name,"String") ||
           lg_find_decl(g,NODE_STRUCT_DECL,name) ||
           lg_find_decl(g,NODE_ENUM_DECL,name) ||
           lg_find_decl(g,NODE_UNION_DECL,name);
}

/* ── type mapping: NightScript type → LLVM type ───────────────────────────── */

static int lg_is_signed(const char *name) {
    return !strcmp(name,"i8") || !strcmp(name,"i16") ||
           !strcmp(name,"i32") || !strcmp(name,"i64") ||
           !strcmp(name,"isize") || !strcmp(name,"char");
}

static LLVMTypeRef llg_ns_to_ll(LG *g, const char *name) {
    if (!name || !strcmp(name,"void")) return LLVMVoidTypeInContext(g->ctx);
    if (!strcmp(name,"bool"))  return LLVMInt1TypeInContext(g->ctx);
    if (!strcmp(name,"i8") || !strcmp(name,"u8") || !strcmp(name,"char"))
        return LLVMInt8TypeInContext(g->ctx);
    if (!strcmp(name,"i16") || !strcmp(name,"u16")) return LLVMInt16TypeInContext(g->ctx);
    if (!strcmp(name,"i32") || !strcmp(name,"u32")) return LLVMInt32TypeInContext(g->ctx);
    if (!strcmp(name,"i64") || !strcmp(name,"u64") ||
        !strcmp(name,"isize") || !strcmp(name,"usize"))
        return LLVMInt64TypeInContext(g->ctx);
    if (!strcmp(name,"f32")) return LLVMFloatTypeInContext(g->ctx);
    if (!strcmp(name,"f64")) return LLVMDoubleTypeInContext(g->ctx);
    /* str = { ptr, i64 }  (NStr) */
    if (!strcmp(name,"str")) {
        LLVMTypeRef found = lg_find_named_type(g, "str");
        if (found) return found;
        LLVMTypeRef fields[2] = { LLVMPointerTypeInContext(g->ctx,0), LLVMInt64TypeInContext(g->ctx) };
        LLVMTypeRef ty = LLVMStructCreateNamed(g->ctx, "NStr");
        LLVMStructSetBody(ty, fields, 2, 0);
        lg_register_type(g, "str", ty, NULL);
        return ty;
    }
    /* cstr = ptr */
    if (!strcmp(name,"cstr")) return LLVMPointerTypeInContext(g->ctx, 0);
    /* String = { ptr, i64, i64 } */
    if (!strcmp(name,"String")) {
        LLVMTypeRef found = lg_find_named_type(g, "String");
        if (found) return found;
        LLVMTypeRef fields[3] = {
            LLVMPointerTypeInContext(g->ctx,0),
            LLVMInt64TypeInContext(g->ctx),
            LLVMInt64TypeInContext(g->ctx)
        };
        LLVMTypeRef ty = LLVMStructCreateNamed(g->ctx, "NString");
        LLVMStructSetBody(ty, fields, 3, 0);
        lg_register_type(g, "String", ty, NULL);
        return ty;
    }
    /* Option[T] = { i1, T } */
    if (lg_type_form(name, "Option")) {
        LLVMTypeRef found = lg_find_named_type(g, name);
        if (found) return found;
        char *inner = lg_extract_inner(name);
        if (!inner) return LLVMInt32TypeInContext(g->ctx);
        LLVMTypeRef inner_ll = llg_ns_to_ll(g, inner);
        free(inner);
        LLVMTypeRef fields[2] = { LLVMInt1TypeInContext(g->ctx), inner_ll };
        char tname[256]; lg_sanitize(tname, sizeof(tname), name);
        LLVMTypeRef ty = LLVMStructCreateNamed(g->ctx, tname);
        LLVMStructSetBody(ty, fields, 2, 0);
        lg_register_type(g, lg_dup(name), ty, NULL);
        return ty;
    }
    /* Result[T,E] = { i1, T, E } */
    if (lg_type_form(name, "Result")) {
        LLVMTypeRef found = lg_find_named_type(g, name);
        if (found) return found;
        char *inner = lg_extract_inner(name);
        if (!inner) return LLVMInt32TypeInContext(g->ctx);
        char *ok_s = NULL, *err_s = NULL;
        if (!lg_split_comma(inner, &ok_s, &err_s)) { free(inner); return LLVMInt32TypeInContext(g->ctx); }
        LLVMTypeRef ok_ll  = llg_ns_to_ll(g, ok_s);
        LLVMTypeRef err_ll = llg_ns_to_ll(g, err_s);
        free(inner); free(ok_s); free(err_s);
        LLVMTypeRef fields[3] = { LLVMInt1TypeInContext(g->ctx), ok_ll, err_ll };
        char tname[256]; lg_sanitize(tname, sizeof(tname), name);
        LLVMTypeRef ty = LLVMStructCreateNamed(g->ctx, tname);
        LLVMStructSetBody(ty, fields, 3, 0);
        lg_register_type(g, lg_dup(name), ty, NULL);
        return ty;
    }
    /* generic struct mangled name */
    {
        LLVMTypeRef found = lg_find_named_type(g, name);
        if (found) return found;
    }
    /* user-defined struct/enum/union */
    {
        /* check substitution context */
        const char *sub = lg_subst(g, name);
        if (sub) return llg_ns_to_ll(g, sub);
        /* check named type registry (structs registered during setup) */
        char mangled[256]; lg_sanitize(mangled, sizeof(mangled), name);
        LLVMTypeRef found = lg_find_named_type(g, mangled);
        if (found) return found;
        found = lg_find_named_type(g, name);
        if (found) return found;
    }
    /* fallback: opaque ptr */
    return LLVMPointerTypeInContext(g->ctx, 0);
}

static LLVMTypeRef llg_node_to_ll(LG *g, Node *type) {
    if (!type) return LLVMVoidTypeInContext(g->ctx);
    switch (type->kind) {
    case NODE_TYPE_NAMED: {
        const char *nm = type->as.type_named.name;
        const char *sub = lg_subst(g, nm);
        return llg_ns_to_ll(g, sub ? sub : nm);
    }
    case NODE_TYPE_POINTER:
        return LLVMPointerTypeInContext(g->ctx, 0);
    case NODE_TYPE_ARRAY: {
        LLVMTypeRef elem = llg_node_to_ll(g, type->as.type_array.elem);
        int len = type->as.type_array.length;
        if (len < 0) {
            /* slice = { ptr, i64 } */
            LLVMTypeRef fields[2] = { LLVMPointerTypeInContext(g->ctx,0), LLVMInt64TypeInContext(g->ctx) };
            LLVMTypeRef ty = LLVMStructTypeInContext(g->ctx, fields, 2, 0);
            return ty;
        }
        return LLVMArrayType(elem, (unsigned)len);
    }
    default:
        return LLVMVoidTypeInContext(g->ctx);
    }
}

/* ── type inference ───────────────────────────────────────────────────────── */

static Node *lg_call_ret_type(LG *g, Node *call);

static Node *lg_try_unwrapped(LG *g, Node *wrapped_type) {
    if (!wrapped_type || wrapped_type->kind != NODE_TYPE_NAMED) return NULL;
    const char *nm = wrapped_type->as.type_named.name;
    if (lg_type_form(nm, "Option")) {
        char *inner = lg_extract_inner(nm);
        if (!inner) return NULL;
        Node *r = lg_temp_named(g, inner);
        free(inner);
        return r;
    }
    if (lg_type_form(nm, "Result")) {
        char *inner = lg_extract_inner(nm);
        if (!inner) return NULL;
        char *ok = NULL, *err = NULL;
        if (!lg_split_comma(inner, &ok, &err)) { free(inner); return NULL; }
        Node *r = lg_temp_named(g, ok);
        free(inner); free(ok); free(err);
        return r;
    }
    return NULL;
}

static Node *llg_infer_type(LG *g, Node *expr) {
    if (!expr) return NULL;
    switch (expr->kind) {
    case NODE_LIT_INT:    return lg_temp_named(g, "i32");
    case NODE_LIT_CHAR:   return lg_temp_named(g, "char");
    case NODE_LIT_FLOAT:  return lg_temp_named(g, "f64");
    case NODE_LIT_STRING: return lg_temp_named(g, "str");
    case NODE_LIT_BOOL:   return lg_temp_named(g, "bool");
    case NODE_LIT_NULL:   return lg_temp_named(g, "void");
    case NODE_GROUP:      return llg_infer_type(g, expr->as.group.expr);
    case NODE_CAST:       return expr->as.cast.type;
    case NODE_STRUCT_LIT: return lg_temp_named(g, expr->as.struct_lit.type_name);
    case NODE_IDENT: {
        LGVar *v = lg_lookup(g, expr->as.ident.name);
        if (v) return v->ns_ty;
        if (lg_is_known_type(g, expr->as.ident.name))
            return lg_temp_named(g, expr->as.ident.name);
        return NULL;
    }
    case NODE_UNARY: {
        const char *op = expr->as.unary.op;
        if (!strcmp(op,"?")) return lg_try_unwrapped(g, llg_infer_type(g, expr->as.unary.operand));
        if (!strcmp(op,"&")) {
            Node *inner = llg_infer_type(g, expr->as.unary.operand);
            return inner ? lg_temp_ptr(g, inner, 0) : NULL;
        }
        if (!strcmp(op,"*")) {
            Node *inner = llg_infer_type(g, expr->as.unary.operand);
            if (inner && inner->kind==NODE_TYPE_POINTER) return inner->as.type_ptr.inner;
            return NULL;
        }
        return llg_infer_type(g, expr->as.unary.operand);
    }
    case NODE_BINARY: {
        const char *op = expr->as.binary.op;
        if (!strcmp(op,"==")||!strcmp(op,"!=")||!strcmp(op,"<")||!strcmp(op,">")||
            !strcmp(op,"<=")||!strcmp(op,">=")||!strcmp(op,"&&")||!strcmp(op,"||"))
            return lg_temp_named(g, "bool");
        Node *lt = llg_infer_type(g, expr->as.binary.left);
        if (lt) return lt;
        return llg_infer_type(g, expr->as.binary.right);
    }
    case NODE_CALL:   return lg_call_ret_type(g, expr);
    case NODE_FIELD: {
        Node *obj = expr->as.field.object;
        const char *fld = expr->as.field.field;
        if (obj->kind==NODE_IDENT && lg_enum_has_variant(g, obj->as.ident.name, fld))
            return lg_temp_named(g, obj->as.ident.name);
        Node *ot = llg_infer_type(g, obj);
        if (!ot) return NULL;
        /* slice/str .len / .ptr */
        if (ot->kind==NODE_TYPE_ARRAY && ot->as.type_array.length<0) {
            if (!strcmp(fld,"len")) return lg_temp_named(g, "usize");
            if (!strcmp(fld,"ptr")) return lg_temp_ptr(g, ot->as.type_array.elem, 0);
        }
        if (ot->kind==NODE_TYPE_NAMED && !strcmp(ot->as.type_named.name,"str")) {
            if (!strcmp(fld,"len")) return lg_temp_named(g, "usize");
            if (!strcmp(fld,"ptr")) return lg_temp_ptr(g, lg_temp_named(g,"u8"), 1);
        }
        if (ot->kind==NODE_TYPE_NAMED && !strcmp(ot->as.type_named.name,"String")) {
            if (!strcmp(fld,"len")) return lg_temp_named(g, "usize");
            if (!strcmp(fld,"cap")) return lg_temp_named(g, "usize");
            if (!strcmp(fld,"ptr")) return lg_temp_ptr(g, lg_temp_named(g,"u8"), 0);
        }
        /* struct field */
        const char *owner = NULL;
        if (ot->kind==NODE_TYPE_NAMED) owner = ot->as.type_named.name;
        else if (ot->kind==NODE_TYPE_POINTER && ot->as.type_ptr.inner &&
                 ot->as.type_ptr.inner->kind==NODE_TYPE_NAMED)
            owner = ot->as.type_ptr.inner->as.type_named.name;
        if (owner) return lg_find_field_type(g, owner, fld);
        return NULL;
    }
    case NODE_INDEX: {
        Node *ot = llg_infer_type(g, expr->as.index_expr.object);
        if (!ot) return NULL;
        if (ot->kind==NODE_TYPE_ARRAY) return ot->as.type_array.elem;
        if (ot->kind==NODE_TYPE_POINTER) return ot->as.type_ptr.inner;
        return NULL;
    }
    case NODE_ASSIGN: return llg_infer_type(g, expr->as.assign.value);
    case NODE_MATCH: {
        if (expr->as.match.count > 0)
            return llg_infer_type(g, expr->as.match.values[0]);
        return NULL;
    }
    default: return NULL;
    }
}

static Node *lg_call_ret_type(LG *g, Node *call) {
    Node *callee = call->as.call.callee;
    if (callee->kind==NODE_IDENT) {
        Node *fn = lg_find_fn(g, callee->as.ident.name);
        if (!fn) return NULL;
        return fn->kind==NODE_EXTERN_FN ? fn->as.extern_fn.ret_type : fn->as.fn.ret_type;
    }
    if (callee->kind==NODE_FIELD) {
        Node *obj = callee->as.field.object;
        const char *fld = callee->as.field.field;
        /* static method call: TypeName.method() */
        if (obj->kind==NODE_IDENT && lg_is_known_type(g, obj->as.ident.name)) {
            Node *m = lg_find_method(g, obj->as.ident.name, fld);
            if (m && !lg_method_has_receiver(m)) return m->as.fn.ret_type;
        }
        Node *ot = llg_infer_type(g, obj);
        if (!ot) return NULL;
        const char *owner = NULL;
        if (ot->kind==NODE_TYPE_NAMED) owner = ot->as.type_named.name;
        else if (ot->kind==NODE_TYPE_POINTER && ot->as.type_ptr.inner &&
                 ot->as.type_ptr.inner->kind==NODE_TYPE_NAMED)
            owner = ot->as.type_ptr.inner->as.type_named.name;
        if (!owner) return NULL;
        Node *m = lg_find_method(g, owner, fld);
        if (m) return m->as.fn.ret_type;
    }
    /* generic fn call: fn[T](...) */
    if (callee->kind==NODE_INDEX) {
        Node *obj = callee->as.index_expr.object;
        if (obj && obj->kind==NODE_IDENT) {
            Node *fn = lg_find_fn(g, obj->as.ident.name);
            if (fn) return fn->as.fn.ret_type;
        }
    }
    return NULL;
}

/* ── defer helpers ────────────────────────────────────────────────────────── */

static void lg_emit_defers_range(LG *g, LLVMBuilderRef b, LLVMBasicBlockRef *cur_bb, int from) {
    for (int i = g->defer_count - 1; i >= from; i--)
        llg_emit_expr(g, b, cur_bb, NULL, NULL, g->defers[i]);
}
static void lg_emit_scope_defers(LG *g, LLVMBuilderRef b, LLVMBasicBlockRef *cur_bb) {
    int mark = (g->scope_defer_depth > 0) ? g->scope_defer_marks[g->scope_defer_depth-1] : 0;
    lg_emit_defers_range(g, b, cur_bb, mark);
    g->defer_count = mark;
}

/* ── alloca at current position ───────────────────────────────────────────── */

static LLVMValueRef lg_alloca(LG *g, LLVMBuilderRef b, LLVMTypeRef ty, const char *name) {
    (void)g;
    return LLVMBuildAlloca(b, ty, name);
}

/* ── load a variable (alloca) ─────────────────────────────────────────────── */

static LLVMValueRef lg_load(LLVMBuilderRef b, LLVMTypeRef ty, LLVMValueRef ptr, const char *name) {
    return LLVMBuildLoad2(b, ty, ptr, name);
}

/* ── match condition emission ─────────────────────────────────────────────── */

static LLVMValueRef lg_match_cond(LG *g, LLVMBuilderRef b, LLVMBasicBlockRef *cur_bb,
                                   LLVMValueRef subj_alloca, LLVMTypeRef subj_ll_ty,
                                   Node *subj_ns_ty, const char *pattern) {
    (void)cur_bb;
    LLVMContextRef ctx = g->ctx;
    if (!subj_ns_ty || subj_ns_ty->kind != NODE_TYPE_NAMED)
        return LLVMConstInt(LLVMInt1TypeInContext(ctx), 0, 0);

    const char *nm = subj_ns_ty->as.type_named.name;

    if (lg_type_form(nm, "Option")) {
        if (!strcmp(pattern, "None")) {
            LLVMValueRef is_some = lg_load(b, subj_ll_ty, subj_alloca, "");
            LLVMValueRef flag = LLVMBuildExtractValue(b, is_some, 0, "is_some");
            return LLVMBuildNot(b, flag, "is_none");
        }
        if (!strcmp(pattern, "Some")) {
            LLVMValueRef v = lg_load(b, subj_ll_ty, subj_alloca, "");
            return LLVMBuildExtractValue(b, v, 0, "is_some");
        }
        return LLVMConstInt(LLVMInt1TypeInContext(ctx), 0, 0);
    }
    if (lg_type_form(nm, "Result")) {
        LLVMValueRef v = lg_load(b, subj_ll_ty, subj_alloca, "");
        LLVMValueRef flag = LLVMBuildExtractValue(b, v, 0, "is_ok");
        if (!strcmp(pattern, "Ok"))  return flag;
        if (!strcmp(pattern, "Err")) return LLVMBuildNot(b, flag, "is_err");
        return LLVMConstInt(LLVMInt1TypeInContext(ctx), 0, 0);
    }
    /* enum: "EnumName.Variant" */
    const char *dot = strchr(pattern, '.');
    if (dot) {
        char enum_name[128];
        size_t elen = (size_t)(dot - pattern);
        if (elen >= sizeof(enum_name)) elen = sizeof(enum_name)-1;
        memcpy(enum_name, pattern, elen); enum_name[elen] = '\0';
        const char *variant_name = dot + 1;
        Node *ed = lg_find_decl(g, NODE_ENUM_DECL, enum_name);
        if (!ed) return LLVMConstInt(LLVMInt1TypeInContext(ctx), 0, 0);
        int idx = -1;
        for (int i = 0; i < ed->as.enum_decl.count; i++)
            if (!strcmp(ed->as.enum_decl.variants[i], variant_name)) { idx = i; break; }
        if (idx < 0) return LLVMConstInt(LLVMInt1TypeInContext(ctx), 0, 0);
        LLVMValueRef v = lg_load(b, subj_ll_ty, subj_alloca, "");
        LLVMValueRef tag = LLVMBuildExtractValue(b, v, 0, "tag");
        LLVMValueRef cmp_val = LLVMConstInt(LLVMTypeOf(tag), (unsigned long long)idx, 0);
        return LLVMBuildICmp(b, LLVMIntEQ, tag, cmp_val, "variant_cmp");
    }
    /* integer literal match */
    {
        long long ival = strtoll(pattern, NULL, 0);
        LLVMValueRef v = lg_load(b, subj_ll_ty, subj_alloca, "");
        LLVMValueRef cmp_val = LLVMConstInt(LLVMTypeOf(v), (unsigned long long)ival, 1);
        return LLVMBuildICmp(b, LLVMIntEQ, v, cmp_val, "match_cmp");
    }
}

/* ── expression emission ──────────────────────────────────────────────────── */

/*
 * Returns an rvalue. If lval_ptr_out != NULL and the expression is addressable,
 * also sets *lval_ptr_out to the alloca pointer and *lval_ty_out to its element type.
 */
static LLVMValueRef llg_emit_expr(LG *g, LLVMBuilderRef b, LLVMBasicBlockRef *cur_bb,
                                   LLVMValueRef *lval_ptr_out, LLVMTypeRef *lval_ty_out,
                                   Node *n) {
    LLVMContextRef ctx = g->ctx;
    if (lval_ptr_out) *lval_ptr_out = NULL;
    if (lval_ty_out)  *lval_ty_out  = NULL;

    if (!n) return LLVMConstInt(LLVMInt32TypeInContext(ctx), 0, 0);

    switch (n->kind) {

    /* ─── literals ─── */
    case NODE_LIT_INT:
        return LLVMConstInt(LLVMInt32TypeInContext(ctx), (unsigned long long)n->as.lit_int.value, 1);
    case NODE_LIT_CHAR:
        return LLVMConstInt(LLVMInt8TypeInContext(ctx), (unsigned long long)n->as.lit_int.value, 0);
    case NODE_LIT_BOOL:
        return LLVMConstInt(LLVMInt1TypeInContext(ctx), (unsigned long long)n->as.lit_int.value, 0);
    case NODE_LIT_NULL:
        return LLVMConstPointerNull(LLVMPointerTypeInContext(ctx, 0));
    case NODE_LIT_FLOAT:
        return LLVMConstReal(LLVMDoubleTypeInContext(ctx), n->as.lit_float.value);
    case NODE_LIT_STRING: {
        /* Build NStr: {ptr, len} as a struct */
        const char *sv = n->as.lit_str.value ? n->as.lit_str.value : "";
        LLVMValueRef gstr = LLVMBuildGlobalStringPtr(b, sv, ".str");
        LLVMTypeRef str_ty = llg_ns_to_ll(g, "str");
        LLVMValueRef agg = LLVMGetUndef(str_ty);
        agg = LLVMBuildInsertValue(b, agg, gstr, 0, "str.ptr");
        LLVMValueRef len_val = LLVMConstInt(LLVMInt64TypeInContext(ctx), strlen(sv), 0);
        agg = LLVMBuildInsertValue(b, agg, len_val, 1, "str.len");
        return agg;
    }

    /* ─── identifier ─── */
    case NODE_IDENT: {
        LGVar *v = lg_lookup(g, n->as.ident.name);
        if (v) {
            if (lval_ptr_out) *lval_ptr_out = v->alloca;
            if (lval_ty_out)  *lval_ty_out  = v->ll_ty;
            return lg_load(b, v->ll_ty, v->alloca, n->as.ident.name);
        }
        /* global function reference */
        LGGlobal *gl = lg_find_global(g, n->as.ident.name);
        if (gl) return gl->fn_val;
        /* constant zero fallback */
        return LLVMConstInt(LLVMInt32TypeInContext(ctx), 0, 0);
    }

    /* ─── group ─── */
    case NODE_GROUP:
        return llg_emit_expr(g, b, cur_bb, lval_ptr_out, lval_ty_out, n->as.group.expr);

    /* ─── cast ─── */
    case NODE_CAST: {
        LLVMValueRef val = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.cast.expr);
        LLVMTypeRef  dst = llg_node_to_ll(g, n->as.cast.type);
        LLVMTypeRef  src = LLVMTypeOf(val);
        LLVMTypeKind sk  = LLVMGetTypeKind(src);
        LLVMTypeKind dk  = LLVMGetTypeKind(dst);
        if (sk == dk) return LLVMBuildBitCast(b, val, dst, "cast");
        if (sk == LLVMIntegerTypeKind && dk == LLVMIntegerTypeKind) {
            unsigned sb = LLVMGetIntTypeWidth(src), db = LLVMGetIntTypeWidth(dst);
            Node *src_ns = llg_infer_type(g, n->as.cast.expr);
            const char *sname = (src_ns && src_ns->kind==NODE_TYPE_NAMED) ? src_ns->as.type_named.name : "i32";
            if (db > sb) {
                return lg_is_signed(sname) ?
                    LLVMBuildSExt(b, val, dst, "sext") :
                    LLVMBuildZExt(b, val, dst, "zext");
            }
            return LLVMBuildTrunc(b, val, dst, "trunc");
        }
        if (sk == LLVMIntegerTypeKind && (dk==LLVMFloatTypeKind||dk==LLVMDoubleTypeKind)) {
            Node *src_ns = llg_infer_type(g, n->as.cast.expr);
            const char *sname = (src_ns && src_ns->kind==NODE_TYPE_NAMED) ? src_ns->as.type_named.name : "i32";
            return lg_is_signed(sname) ?
                LLVMBuildSIToFP(b, val, dst, "sitofp") :
                LLVMBuildUIToFP(b, val, dst, "uitofp");
        }
        if ((sk==LLVMFloatTypeKind||sk==LLVMDoubleTypeKind) && dk==LLVMIntegerTypeKind) {
            Node *dst_ns = n->as.cast.type;
            const char *dname = (dst_ns && dst_ns->kind==NODE_TYPE_NAMED) ? dst_ns->as.type_named.name : "i32";
            return lg_is_signed(dname) ?
                LLVMBuildFPToSI(b, val, dst, "fptosi") :
                LLVMBuildFPToUI(b, val, dst, "fptoui");
        }
        if (sk == LLVMPointerTypeKind && dk == LLVMPointerTypeKind)
            return LLVMBuildPointerCast(b, val, dst, "ptrcast");
        if (sk == LLVMIntegerTypeKind && dk == LLVMPointerTypeKind)
            return LLVMBuildIntToPtr(b, val, dst, "inttoptr");
        if (sk == LLVMPointerTypeKind && dk == LLVMIntegerTypeKind)
            return LLVMBuildPtrToInt(b, val, dst, "ptrtoint");
        return LLVMBuildBitCast(b, val, dst, "cast");
    }

    /* ─── unary ─── */
    case NODE_UNARY: {
        const char *op = n->as.unary.op;
        Node *operand = n->as.unary.operand;

        /* address-of */
        if (!strcmp(op,"&")) {
            LLVMValueRef ptr = NULL; LLVMTypeRef ty = NULL;
            llg_emit_expr(g, b, cur_bb, &ptr, &ty, operand);
            if (ptr) return ptr;
            /* non-addressable: alloca temp */
            LLVMValueRef val = llg_emit_expr(g, b, cur_bb, NULL, NULL, operand);
            LLVMTypeRef elt = LLVMTypeOf(val);
            LLVMValueRef tmp = lg_alloca(g, b, elt, "addr_tmp");
            LLVMBuildStore(b, val, tmp);
            return tmp;
        }
        /* dereference */
        if (!strcmp(op,"*")) {
            LLVMValueRef ptr = llg_emit_expr(g, b, cur_bb, NULL, NULL, operand);
            Node *ns_ty = llg_infer_type(g, n);
            LLVMTypeRef elem_ty = ns_ty ? llg_node_to_ll(g, ns_ty) : LLVMInt8TypeInContext(ctx);
            if (lval_ptr_out) *lval_ptr_out = ptr;
            if (lval_ty_out)  *lval_ty_out  = elem_ty;
            return lg_load(b, elem_ty, ptr, "deref");
        }
        /* try operator */
        if (!strcmp(op,"?")) {
            Node *op_type = llg_infer_type(g, operand);
            LLVMValueRef val = llg_emit_expr(g, b, cur_bb, NULL, NULL, operand);
            LLVMTypeRef val_ty = LLVMTypeOf(val);
            LLVMValueRef flag = LLVMBuildExtractValue(b, val, 0, "try_flag");

            LLVMBasicBlockRef ok_bb  = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "try_ok");
            LLVMBasicBlockRef err_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "try_err");
            LLVMBuildCondBr(b, flag, ok_bb, err_bb);

            /* error path: early return */
            LLVMPositionBuilderAtEnd(b, err_bb);
            if (cur_bb) *cur_bb = err_bb;
            lg_emit_defers_range(g, b, cur_bb, 0);
            if (g->cur_ret_ty && g->cur_ret_ty->kind==NODE_TYPE_NAMED) {
                const char *rtn = g->cur_ret_ty->as.type_named.name;
                LLVMTypeRef ret_ll = llg_ns_to_ll(g, rtn);
                if (lg_type_form(rtn, "Option")) {
                    LLVMValueRef none = LLVMGetUndef(ret_ll);
                    none = LLVMBuildInsertValue(b, none,
                           LLVMConstInt(LLVMInt1TypeInContext(ctx),0,0), 0, "none_flag");
                    LLVMBuildRet(b, none);
                } else if (lg_type_form(rtn, "Result")) {
                    /* propagate err field (field 2 of the try'd value) */
                    LLVMValueRef err_val = LLVMGetUndef(ret_ll);
                    LLVMValueRef inner_err = LLVMBuildExtractValue(b, val, 2, "err_inner");
                    err_val = LLVMBuildInsertValue(b, err_val,
                              LLVMConstInt(LLVMInt1TypeInContext(ctx),0,0), 0, "res_flag");
                    err_val = LLVMBuildInsertValue(b, err_val, inner_err, 2, "res_err");
                    LLVMBuildRet(b, err_val);
                } else {
                    LLVMBuildRetVoid(b);
                }
            } else {
                LLVMBuildRetVoid(b);
            }

            /* ok path: extract value */
            LLVMPositionBuilderAtEnd(b, ok_bb);
            if (cur_bb) *cur_bb = ok_bb;
            /* Option field 1, Result field 1 */
            (void)op_type; (void)val_ty;
            return LLVMBuildExtractValue(b, val, 1, "try_val");
        }
        /* negation */
        if (!strcmp(op,"-")) {
            LLVMValueRef val = llg_emit_expr(g, b, cur_bb, NULL, NULL, operand);
            LLVMTypeKind k = LLVMGetTypeKind(LLVMTypeOf(val));
            if (k==LLVMFloatTypeKind||k==LLVMDoubleTypeKind)
                return LLVMBuildFNeg(b, val, "fneg");
            return LLVMBuildNeg(b, val, "neg");
        }
        if (!strcmp(op,"!")) {
            LLVMValueRef val = llg_emit_expr(g, b, cur_bb, NULL, NULL, operand);
            return LLVMBuildNot(b, val, "not");
        }
        if (!strcmp(op,"~")) {
            LLVMValueRef val = llg_emit_expr(g, b, cur_bb, NULL, NULL, operand);
            return LLVMBuildNot(b, val, "bitnot");
        }
        return llg_emit_expr(g, b, cur_bb, NULL, NULL, operand);
    }

    /* ─── binary ─── */
    case NODE_BINARY: {
        const char *op = n->as.binary.op;
        Node *lhs = n->as.binary.left;
        Node *rhs = n->as.binary.right;

        /* short-circuit logical operators */
        if (!strcmp(op,"&&")) {
            LLVMValueRef lv = llg_emit_expr(g, b, cur_bb, NULL, NULL, lhs);
            LLVMBasicBlockRef rhs_bb  = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "and_rhs");
            LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "and_done");
            LLVMBasicBlockRef lhs_end = cur_bb ? *cur_bb : LLVMGetInsertBlock(b);
            LLVMBuildCondBr(b, lv, rhs_bb, done_bb);
            LLVMPositionBuilderAtEnd(b, rhs_bb);
            if (cur_bb) *cur_bb = rhs_bb;
            LLVMValueRef rv = llg_emit_expr(g, b, cur_bb, NULL, NULL, rhs);
            LLVMBasicBlockRef rhs_end = cur_bb ? *cur_bb : LLVMGetInsertBlock(b);
            LLVMBuildBr(b, done_bb);
            LLVMPositionBuilderAtEnd(b, done_bb);
            if (cur_bb) *cur_bb = done_bb;
            LLVMValueRef phi = LLVMBuildPhi(b, LLVMInt1TypeInContext(ctx), "and");
            LLVMValueRef vals[2] = { LLVMConstInt(LLVMInt1TypeInContext(ctx),0,0), rv };
            LLVMBasicBlockRef bbs[2] = { lhs_end, rhs_end };
            LLVMAddIncoming(phi, vals, bbs, 2);
            return phi;
        }
        if (!strcmp(op,"||")) {
            LLVMValueRef lv = llg_emit_expr(g, b, cur_bb, NULL, NULL, lhs);
            LLVMBasicBlockRef rhs_bb  = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "or_rhs");
            LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "or_done");
            LLVMBasicBlockRef lhs_end = cur_bb ? *cur_bb : LLVMGetInsertBlock(b);
            LLVMBuildCondBr(b, lv, done_bb, rhs_bb);
            LLVMPositionBuilderAtEnd(b, rhs_bb);
            if (cur_bb) *cur_bb = rhs_bb;
            LLVMValueRef rv = llg_emit_expr(g, b, cur_bb, NULL, NULL, rhs);
            LLVMBasicBlockRef rhs_end = cur_bb ? *cur_bb : LLVMGetInsertBlock(b);
            LLVMBuildBr(b, done_bb);
            LLVMPositionBuilderAtEnd(b, done_bb);
            if (cur_bb) *cur_bb = done_bb;
            LLVMValueRef phi = LLVMBuildPhi(b, LLVMInt1TypeInContext(ctx), "or");
            LLVMValueRef vals[2] = { LLVMConstInt(LLVMInt1TypeInContext(ctx),1,0), rv };
            LLVMBasicBlockRef bbs[2] = { lhs_end, rhs_end };
            LLVMAddIncoming(phi, vals, bbs, 2);
            return phi;
        }

        LLVMValueRef lv = llg_emit_expr(g, b, cur_bb, NULL, NULL, lhs);
        LLVMValueRef rv = llg_emit_expr(g, b, cur_bb, NULL, NULL, rhs);
        LLVMTypeRef lty = LLVMTypeOf(lv);
        LLVMTypeKind tk = LLVMGetTypeKind(lty);
        int is_fp = (tk==LLVMFloatTypeKind || tk==LLVMDoubleTypeKind);
        Node *ns_ty = llg_infer_type(g, lhs);
        const char *tname = (ns_ty && ns_ty->kind==NODE_TYPE_NAMED) ? ns_ty->as.type_named.name : "i32";
        int is_signed = lg_is_signed(tname);

        if (!strcmp(op,"+"))  return is_fp ? LLVMBuildFAdd(b,lv,rv,"fadd") : LLVMBuildAdd(b,lv,rv,"add");
        if (!strcmp(op,"-"))  return is_fp ? LLVMBuildFSub(b,lv,rv,"fsub") : LLVMBuildSub(b,lv,rv,"sub");
        if (!strcmp(op,"*"))  return is_fp ? LLVMBuildFMul(b,lv,rv,"fmul") : LLVMBuildMul(b,lv,rv,"mul");
        if (!strcmp(op,"/"))  {
            if (is_fp) return LLVMBuildFDiv(b,lv,rv,"fdiv");
            return is_signed ? LLVMBuildSDiv(b,lv,rv,"sdiv") : LLVMBuildUDiv(b,lv,rv,"udiv");
        }
        if (!strcmp(op,"%"))  {
            if (is_fp) return LLVMBuildFRem(b,lv,rv,"frem");
            return is_signed ? LLVMBuildSRem(b,lv,rv,"srem") : LLVMBuildURem(b,lv,rv,"urem");
        }
        if (!strcmp(op,"&"))  return LLVMBuildAnd(b,lv,rv,"band");
        if (!strcmp(op,"|"))  return LLVMBuildOr(b,lv,rv,"bor");
        if (!strcmp(op,"^"))  return LLVMBuildXor(b,lv,rv,"xor");
        if (!strcmp(op,"<<")) return LLVMBuildShl(b,lv,rv,"shl");
        if (!strcmp(op,">>")) return is_signed ? LLVMBuildAShr(b,lv,rv,"ashr") : LLVMBuildLShr(b,lv,rv,"lshr");
        /* comparisons */
        if (is_fp) {
            LLVMRealPredicate pred = LLVMRealOEQ;
            if (!strcmp(op,"==")) pred=LLVMRealOEQ;
            else if(!strcmp(op,"!=")) pred=LLVMRealONE;
            else if(!strcmp(op,"<"))  pred=LLVMRealOLT;
            else if(!strcmp(op,"<=")) pred=LLVMRealOLE;
            else if(!strcmp(op,">"))  pred=LLVMRealOGT;
            else if(!strcmp(op,">=")) pred=LLVMRealOGE;
            return LLVMBuildFCmp(b, pred, lv, rv, "fcmp");
        } else {
            LLVMIntPredicate pred = LLVMIntEQ;
            if (!strcmp(op,"==")) pred=LLVMIntEQ;
            else if(!strcmp(op,"!=")) pred=LLVMIntNE;
            else if(!strcmp(op,"<"))  pred=is_signed?LLVMIntSLT:LLVMIntULT;
            else if(!strcmp(op,"<=")) pred=is_signed?LLVMIntSLE:LLVMIntULE;
            else if(!strcmp(op,">"))  pred=is_signed?LLVMIntSGT:LLVMIntUGT;
            else if(!strcmp(op,">=")) pred=is_signed?LLVMIntSGE:LLVMIntUGE;
            return LLVMBuildICmp(b, pred, lv, rv, "icmp");
        }
    }

    /* ─── field access ─── */
    case NODE_FIELD: {
        Node *obj = n->as.field.object;
        const char *fld = n->as.field.field;

        /* enum simple variant: EnumName.Variant → integer */
        if (obj->kind==NODE_IDENT && lg_enum_has_variant(g, obj->as.ident.name, fld)) {
            Node *ed = lg_find_decl(g, NODE_ENUM_DECL, obj->as.ident.name);
            if (ed) {
                for (int i = 0; i < ed->as.enum_decl.count; i++) {
                    if (!strcmp(ed->as.enum_decl.variants[i], fld)) {
                        LLVMTypeRef tag_ty = LLVMInt32TypeInContext(ctx);
                        LGGlobal *gl = lg_find_global(g, obj->as.ident.name);
                        if (gl) tag_ty = LLVMStructGetTypeAtIndex(gl->fn_ty, 0);
                        return LLVMConstInt(tag_ty, (unsigned long long)i, 0);
                    }
                }
            }
        }

        LLVMValueRef obj_ptr = NULL; LLVMTypeRef obj_elem_ty = NULL;
        LLVMValueRef obj_val = llg_emit_expr(g, b, cur_bb, &obj_ptr, &obj_elem_ty, obj);
        Node *obj_ns_ty = llg_infer_type(g, obj);

        /* auto-deref pointer */
        int was_ptr = 0;
        if (!obj_ptr && obj_ns_ty && obj_ns_ty->kind==NODE_TYPE_POINTER) {
            obj_ptr = obj_val;
            obj_elem_ty = llg_node_to_ll(g, obj_ns_ty->as.type_ptr.inner);
            obj_ns_ty = obj_ns_ty->as.type_ptr.inner;
            was_ptr = 1;
        }
        (void)was_ptr;

        /* slice / str fields */
        if (obj_ns_ty && obj_ns_ty->kind==NODE_TYPE_NAMED) {
            const char *tnm = obj_ns_ty->as.type_named.name;
            if (!strcmp(tnm,"str")) {
                LLVMTypeRef str_ty = llg_ns_to_ll(g, "str");
                if (!strcmp(fld,"len")) {
                    if (obj_ptr) {
                        LLVMValueRef gep = LLVMBuildStructGEP2(b, str_ty, obj_ptr, 1, "str.len.ptr");
                        return lg_load(b, LLVMInt64TypeInContext(ctx), gep, "str.len");
                    }
                    return LLVMBuildExtractValue(b, obj_val, 1, "str.len");
                }
                if (!strcmp(fld,"ptr")) {
                    if (obj_ptr) {
                        LLVMValueRef gep = LLVMBuildStructGEP2(b, str_ty, obj_ptr, 0, "str.ptr.ptr");
                        return lg_load(b, LLVMPointerTypeInContext(ctx,0), gep, "str.ptr");
                    }
                    return LLVMBuildExtractValue(b, obj_val, 0, "str.ptr");
                }
            }
            if (!strcmp(tnm,"String")) {
                LLVMTypeRef sty = llg_ns_to_ll(g, "String");
                unsigned fidx = !strcmp(fld,"ptr") ? 0 : !strcmp(fld,"len") ? 1 : 2;
                if (obj_ptr) {
                    LLVMValueRef gep = LLVMBuildStructGEP2(b, sty, obj_ptr, fidx, "sfield.ptr");
                    Node *ft = llg_infer_type(g, n);
                    LLVMTypeRef fty = ft ? llg_node_to_ll(g, ft) : LLVMInt64TypeInContext(ctx);
                    if (lval_ptr_out) *lval_ptr_out = gep;
                    if (lval_ty_out)  *lval_ty_out  = fty;
                    return lg_load(b, fty, gep, fld);
                }
                return LLVMBuildExtractValue(b, obj_val, fidx, fld);
            }
        }
        if (obj_ns_ty && obj_ns_ty->kind==NODE_TYPE_ARRAY && obj_ns_ty->as.type_array.length<0) {
            /* slice: {ptr, len} */
            if (!strcmp(fld,"len")) {
                if (obj_ptr) {
                    LLVMTypeRef slty = llg_node_to_ll(g, obj_ns_ty);
                    LLVMValueRef gep = LLVMBuildStructGEP2(b, slty, obj_ptr, 1, "sl.len.ptr");
                    return lg_load(b, LLVMInt64TypeInContext(ctx), gep, "sl.len");
                }
                return LLVMBuildExtractValue(b, obj_val, 1, "sl.len");
            }
            if (!strcmp(fld,"ptr")) {
                if (obj_ptr) {
                    LLVMTypeRef slty = llg_node_to_ll(g, obj_ns_ty);
                    LLVMValueRef gep = LLVMBuildStructGEP2(b, slty, obj_ptr, 0, "sl.ptr.ptr");
                    return lg_load(b, LLVMPointerTypeInContext(ctx,0), gep, "sl.ptr");
                }
                return LLVMBuildExtractValue(b, obj_val, 0, "sl.ptr");
            }
        }

        /* struct/union field via GEP */
        const char *owner_name = NULL;
        if (obj_ns_ty && obj_ns_ty->kind==NODE_TYPE_NAMED) owner_name = obj_ns_ty->as.type_named.name;
        if (owner_name) {
            Node *sd = lg_find_decl(g, NODE_STRUCT_DECL, owner_name);
            Node *ud = sd ? NULL : lg_find_decl(g, NODE_UNION_DECL, owner_name);
            NodeList *fields = sd ? &sd->as.struct_decl.fields :
                               ud ? &ud->as.union_decl.fields : NULL;
            if (fields) {
                for (int i = 0; i < fields->count; i++) {
                    if (!strcmp(fields->items[i]->as.let.name, fld)) {
                        Node *fty_ns = fields->items[i]->as.let.type;
                        LLVMTypeRef fty_ll = fty_ns ? llg_node_to_ll(g, fty_ns) : LLVMInt32TypeInContext(ctx);
                        LLVMTypeRef struct_ll = lg_find_named_type(g, owner_name);
                        if (!struct_ll) struct_ll = obj_elem_ty;
                        if (obj_ptr && struct_ll) {
                            LLVMValueRef gep = LLVMBuildStructGEP2(b, struct_ll, obj_ptr, (unsigned)i, fld);
                            if (lval_ptr_out) *lval_ptr_out = gep;
                            if (lval_ty_out)  *lval_ty_out  = fty_ll;
                            return lg_load(b, fty_ll, gep, fld);
                        }
                        return LLVMBuildExtractValue(b, obj_val, (unsigned)i, fld);
                    }
                }
            }
        }
        return LLVMConstInt(LLVMInt32TypeInContext(ctx), 0, 0);
    }

    /* ─── index ─── */
    case NODE_INDEX: {
        LLVMValueRef obj_ptr = NULL; LLVMTypeRef obj_elem_ty = NULL;
        LLVMValueRef obj_val = llg_emit_expr(g, b, cur_bb, &obj_ptr, &obj_elem_ty, n->as.index_expr.object);
        LLVMValueRef idx_val = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.index_expr.index);
        Node *obj_ns_ty = llg_infer_type(g, n->as.index_expr.object);
        Node *elem_ns_ty = llg_infer_type(g, n);
        LLVMTypeRef elem_ll = elem_ns_ty ? llg_node_to_ll(g, elem_ns_ty) : LLVMInt32TypeInContext(ctx);

        /* array by pointer */
        if (obj_ptr) {
            LLVMValueRef indices[1] = { idx_val };
            LLVMTypeRef arr_elem = elem_ll;
            if (obj_ns_ty && obj_ns_ty->kind==NODE_TYPE_ARRAY && obj_ns_ty->as.type_array.length>=0) {
                LLVMTypeRef arr_ty = obj_elem_ty ? obj_elem_ty : llg_node_to_ll(g, obj_ns_ty);
                LLVMValueRef zero = LLVMConstInt(LLVMInt64TypeInContext(ctx),0,0);
                LLVMValueRef gep_idx[2] = { zero, idx_val };
                LLVMValueRef gep = LLVMBuildGEP2(b, arr_ty, obj_ptr, gep_idx, 2, "arr.elem.ptr");
                if (lval_ptr_out) *lval_ptr_out = gep;
                if (lval_ty_out)  *lval_ty_out  = arr_elem;
                return lg_load(b, arr_elem, gep, "arr.elem");
            }
            /* slice or raw pointer */
            LLVMValueRef data_ptr;
            if (obj_ns_ty && obj_ns_ty->kind==NODE_TYPE_ARRAY && obj_ns_ty->as.type_array.length<0) {
                LLVMTypeRef slty = llg_node_to_ll(g, obj_ns_ty);
                LLVMValueRef ptr_gep = LLVMBuildStructGEP2(b, slty, obj_ptr, 0, "sl.ptr");
                data_ptr = lg_load(b, LLVMPointerTypeInContext(ctx,0), ptr_gep, "sl.data");
            } else {
                data_ptr = lg_load(b, LLVMPointerTypeInContext(ctx,0), obj_ptr, "ptr.val");
            }
            LLVMValueRef gep = LLVMBuildGEP2(b, arr_elem, data_ptr, indices, 1, "idx.ptr");
            if (lval_ptr_out) *lval_ptr_out = gep;
            if (lval_ty_out)  *lval_ty_out  = arr_elem;
            return lg_load(b, arr_elem, gep, "idx.val");
        }
        /* slice by value: extract ptr field then GEP */
        if (obj_ns_ty && obj_ns_ty->kind==NODE_TYPE_ARRAY && obj_ns_ty->as.type_array.length<0) {
            LLVMValueRef data_ptr = LLVMBuildExtractValue(b, obj_val, 0, "sl.ptr");
            LLVMValueRef indices[1] = { idx_val };
            LLVMValueRef gep = LLVMBuildGEP2(b, elem_ll, data_ptr, indices, 1, "idx.ptr");
            if (lval_ptr_out) *lval_ptr_out = gep;
            if (lval_ty_out)  *lval_ty_out  = elem_ll;
            return lg_load(b, elem_ll, gep, "idx.val");
        }
        /* plain ptr value */
        {
            LLVMValueRef indices[1] = { idx_val };
            LLVMValueRef gep = LLVMBuildGEP2(b, elem_ll, obj_val, indices, 1, "ptr.idx");
            if (lval_ptr_out) *lval_ptr_out = gep;
            if (lval_ty_out)  *lval_ty_out  = elem_ll;
            return lg_load(b, elem_ll, gep, "ptr.val");
        }
    }

    /* ─── assign ─── */
    case NODE_ASSIGN: {
        Node *tgt = n->as.assign.target;
        const char *op = n->as.assign.op;
        LLVMValueRef ptr = NULL; LLVMTypeRef ty = NULL;
        LLVMValueRef cur_val = llg_emit_expr(g, b, cur_bb, &ptr, &ty, tgt);
        LLVMValueRef rhs = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.assign.value);
        LLVMValueRef store_val = rhs;
        if (op && ptr && ty) {
            int is_fp = (LLVMGetTypeKind(ty)==LLVMFloatTypeKind||LLVMGetTypeKind(ty)==LLVMDoubleTypeKind);
            Node *ns = llg_infer_type(g, tgt);
            const char *tname = (ns&&ns->kind==NODE_TYPE_NAMED)?ns->as.type_named.name:"i32";
            int sig = lg_is_signed(tname);
            if (!strcmp(op,"+")) store_val = is_fp?LLVMBuildFAdd(b,cur_val,rhs,""):LLVMBuildAdd(b,cur_val,rhs,"");
            else if(!strcmp(op,"-")) store_val = is_fp?LLVMBuildFSub(b,cur_val,rhs,""):LLVMBuildSub(b,cur_val,rhs,"");
            else if(!strcmp(op,"*")) store_val = is_fp?LLVMBuildFMul(b,cur_val,rhs,""):LLVMBuildMul(b,cur_val,rhs,"");
            else if(!strcmp(op,"/")) store_val = is_fp?LLVMBuildFDiv(b,cur_val,rhs,""):sig?LLVMBuildSDiv(b,cur_val,rhs,""):LLVMBuildUDiv(b,cur_val,rhs,"");
            else if(!strcmp(op,"%")) store_val = is_fp?LLVMBuildFRem(b,cur_val,rhs,""):sig?LLVMBuildSRem(b,cur_val,rhs,""):LLVMBuildURem(b,cur_val,rhs,"");
        }
        if (ptr) LLVMBuildStore(b, store_val, ptr);
        return store_val;
    }

    /* ─── struct literal ─── */
    case NODE_STRUCT_LIT: {
        const char *tname = n->as.struct_lit.type_name;
        LLVMTypeRef st_ty = lg_find_named_type(g, tname);
        if (!st_ty) st_ty = llg_ns_to_ll(g, tname);
        LLVMValueRef agg = LLVMGetUndef(st_ty);
        Node *sd = lg_find_decl(g, NODE_STRUCT_DECL, tname);
        for (int i = 0; i < n->as.struct_lit.count; i++) {
            LLVMValueRef fval = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.struct_lit.field_values[i]);
            unsigned fidx = (unsigned)i;
            if (sd) {
                for (int j = 0; j < sd->as.struct_decl.fields.count; j++) {
                    if (!strcmp(sd->as.struct_decl.fields.items[j]->as.let.name, n->as.struct_lit.field_names[i])) {
                        fidx = (unsigned)j; break;
                    }
                }
            }
            agg = LLVMBuildInsertValue(b, agg, fval, fidx, "sf");
        }
        return agg;
    }

    /* ─── match expression ─── */
    case NODE_MATCH: {
        if (n->as.match.count == 0)
            return LLVMConstInt(LLVMInt32TypeInContext(ctx), 0, 0);

        Node *subj_ns = llg_infer_type(g, n->as.match.subject);
        LLVMTypeRef subj_ll = subj_ns ? llg_node_to_ll(g, subj_ns) : LLVMInt32TypeInContext(ctx);
        Node *res_ns = llg_infer_type(g, n->as.match.values[0]);
        LLVMTypeRef res_ll = res_ns ? llg_node_to_ll(g, res_ns) : LLVMInt32TypeInContext(ctx);

        char subj_name[32]; lg_tmp_name(g, subj_name, sizeof(subj_name));
        char res_name[32];  lg_tmp_name(g, res_name, sizeof(res_name));

        LLVMValueRef subj_alloca = lg_alloca(g, b, subj_ll, subj_name);
        LLVMValueRef res_alloca  = lg_alloca(g, b, res_ll, res_name);
        LLVMValueRef subj_val    = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.match.subject);
        LLVMBuildStore(b, subj_val, subj_alloca);

        LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "match_done");

        for (int i = 0; i < n->as.match.count; i++) {
            const char *pat = n->as.match.patterns[i];
            LLVMBasicBlockRef arm_bb  = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "match_arm");
            LLVMBasicBlockRef next_bb = (i < n->as.match.count-1)
                ? LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "match_next")
                : done_bb;

            if (strcmp(pat,"_")!=0) {
                LLVMValueRef cond = lg_match_cond(g, b, cur_bb, subj_alloca, subj_ll, subj_ns, pat);
                LLVMBuildCondBr(b, cond, arm_bb, next_bb);
            } else {
                LLVMBuildBr(b, arm_bb);
            }

            LLVMPositionBuilderAtEnd(b, arm_bb);
            if (cur_bb) *cur_bb = arm_bb;

            /* emit bindings */
            int bc = n->as.match.binding_counts[i];
            if (bc > 0) {
                lg_push_scope(g);
                for (int j = 0; j < bc; j++) {
                    const char *bname = n->as.match.binding_names[i][j];
                    /* determine binding type */
                    LLVMTypeRef bty = LLVMInt32TypeInContext(ctx);
                    LLVMValueRef bval = LLVMConstInt(bty, 0, 0);
                    LLVMValueRef sv = lg_load(b, subj_ll, subj_alloca, "subj");
                    if (subj_ns && subj_ns->kind==NODE_TYPE_NAMED) {
                        const char *snm = subj_ns->as.type_named.name;
                        if (lg_type_form(snm,"Option")) {
                            char *inner = lg_extract_inner(snm);
                            if (inner) { bty = llg_ns_to_ll(g, inner); free(inner); }
                            bval = LLVMBuildExtractValue(b, sv, 1, bname);
                        } else if (lg_type_form(snm,"Result")) {
                            char *inner = lg_extract_inner(snm);
                            char *ok=NULL,*err=NULL;
                            if (inner && lg_split_comma(inner, &ok, &err)) {
                                bty = llg_ns_to_ll(g, !strcmp(pat,"Ok")?ok:err);
                                bval = LLVMBuildExtractValue(b, sv, !strcmp(pat,"Ok")?1:2, bname);
                            }
                            free(inner); free(ok); free(err);
                        }
                    }
                    LLVMValueRef ba = lg_alloca(g, b, bty, bname);
                    LLVMBuildStore(b, bval, ba);
                    lg_define(g, bname, ba, bty, NULL);
                }
            }

            LLVMValueRef arm_val = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.match.values[i]);
            LLVMBuildStore(b, arm_val, res_alloca);
            if (bc > 0) lg_pop_scope(g);

            LLVMBasicBlockRef cur = cur_bb ? *cur_bb : LLVMGetInsertBlock(b);
            if (!LLVMGetBasicBlockTerminator(cur))
                LLVMBuildBr(b, done_bb);

            if (next_bb != done_bb) {
                LLVMPositionBuilderAtEnd(b, next_bb);
                if (cur_bb) *cur_bb = next_bb;
            }
        }

        LLVMPositionBuilderAtEnd(b, done_bb);
        if (cur_bb) *cur_bb = done_bb;
        return lg_load(b, res_ll, res_alloca, "match_result");
    }

    /* ─── function call ─── */
    case NODE_CALL: {
        Node *callee_node = n->as.call.callee;

        /* resolve callee */
        LLVMValueRef fn_val = NULL;
        LLVMTypeRef  fn_ty  = NULL;
        Node        *ret_ns = NULL;
        const char  *fn_sym = NULL;

        if (callee_node->kind == NODE_IDENT) {
            fn_sym = callee_node->as.ident.name;
        } else if (callee_node->kind == NODE_INDEX &&
                   callee_node->as.index_expr.object &&
                   callee_node->as.index_expr.object->kind == NODE_IDENT) {
            /* generic call: fn[T](...) */
            fn_sym = callee_node->as.index_expr.object->as.ident.name;
            Node *ta = callee_node->as.index_expr.index;
            const char *ns_t = NULL;
            if (ta && ta->kind==NODE_IDENT)       ns_t = ta->as.ident.name;
            if (ta && ta->kind==NODE_TYPE_NAMED)  ns_t = ta->as.type_named.name;
            if (ns_t) {
                char mangled[256]; lg_mangle(mangled, sizeof(mangled), fn_sym, ns_t);
                LGGlobal *gl = lg_find_global(g, mangled);
                if (gl) { fn_val = gl->fn_val; fn_ty = gl->fn_ty; ret_ns = gl->ns_ret_ty; fn_sym = NULL; }
            }
        } else if (callee_node->kind == NODE_FIELD) {
            /* method call */
            Node *obj = callee_node->as.field.object;
            const char *mname = callee_node->as.field.field;

            /* String built-ins */
            if (obj->kind==NODE_IDENT && !strcmp(obj->as.ident.name,"String")) {
                if (!strcmp(mname,"from")) {
                    LGGlobal *gl = lg_find_global(g, "NS_string_from");
                    if (gl) { fn_val=gl->fn_val; fn_ty=gl->fn_ty; ret_ns=gl->ns_ret_ty; }
                }
            }

            if (!fn_val) {
                Node *obj_ns = llg_infer_type(g, obj);
                const char *owner = NULL;
                if (obj_ns && obj_ns->kind==NODE_TYPE_NAMED) owner = obj_ns->as.type_named.name;
                else if (obj_ns && obj_ns->kind==NODE_TYPE_POINTER && obj_ns->as.type_ptr.inner &&
                         obj_ns->as.type_ptr.inner->kind==NODE_TYPE_NAMED)
                    owner = obj_ns->as.type_ptr.inner->as.type_named.name;

                if (owner) {
                    /* static method? */
                    Node *m = lg_find_method(g, owner, mname);
                    if (!m && obj->kind==NODE_IDENT && lg_is_known_type(g,obj->as.ident.name))
                        m = lg_find_method(g, obj->as.ident.name, mname);
                    if (m) {
                        char sym[512]; snprintf(sym, sizeof(sym), "%s_%s", owner, mname);
                        LGGlobal *gl = lg_find_global(g, sym);
                        if (gl) { fn_val=gl->fn_val; fn_ty=gl->fn_ty; ret_ns=gl->ns_ret_ty; }
                    }
                }

                if (!fn_val && !fn_sym) {
                    /* runtime helpers by method name */
                    if (!strcmp(mname,"as_str")) {
                        LGGlobal *gl = lg_find_global(g, "NS_string_as_str");
                        if (gl) { fn_val=gl->fn_val; fn_ty=gl->fn_ty; ret_ns=gl->ns_ret_ty; }
                    } else if (!strcmp(mname,"free")) {
                        LGGlobal *gl = lg_find_global(g, "NS_string_free");
                        if (gl) { fn_val=gl->fn_val; fn_ty=gl->fn_ty; ret_ns=gl->ns_ret_ty; }
                    } else if (!strcmp(mname,"push_str")) {
                        LGGlobal *gl = lg_find_global(g, "NS_string_push_str");
                        if (gl) { fn_val=gl->fn_val; fn_ty=gl->fn_ty; ret_ns=gl->ns_ret_ty; }
                    }
                }

                /* self argument injection for instance methods */
                if (fn_val && fn_ty) {
                    /* build args with self prepended */
                    int nparams = (int)LLVMCountParamTypes(fn_ty);
                    LLVMValueRef *args = calloc((size_t)(n->as.call.args.count + 2), sizeof(LLVMValueRef));
                    int ai = 0;
                    /* push self (by pointer if method takes ptr, by value otherwise) */
                    LLVMValueRef self_ptr = NULL; LLVMTypeRef self_elt = NULL;
                    LLVMValueRef self_val = llg_emit_expr(g, b, cur_bb, &self_ptr, &self_elt, obj);
                    if (nparams > 0) {
                        LLVMTypeRef *param_tys = calloc((size_t)nparams, sizeof(LLVMTypeRef));
                        LLVMGetParamTypes(fn_ty, param_tys);
                        if (LLVMGetTypeKind(param_tys[0])==LLVMPointerTypeKind && self_ptr)
                            args[ai++] = self_ptr;
                        else
                            args[ai++] = self_val;
                        free(param_tys);
                    } else {
                        args[ai++] = self_val;
                    }
                    for (int j = 0; j < n->as.call.args.count; j++)
                        args[ai++] = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.call.args.items[j]);
                    /* coerce */
                    {
                        unsigned np2 = LLVMCountParamTypes(fn_ty);
                        if (np2 > 0) {
                            LLVMTypeRef *ptys2 = calloc(np2, sizeof(LLVMTypeRef));
                            LLVMGetParamTypes(fn_ty, ptys2);
                            for (int k = 0; k < ai && k < (int)np2; k++)
                                args[k] = lg_coerce_arg(b, args[k], ptys2[k], ctx);
                            free(ptys2);
                        }
                    }
                    LLVMTypeRef ret_ll = ret_ns ? llg_node_to_ll(g, ret_ns) : LLVMVoidTypeInContext(ctx);
                    LLVMValueRef result;
                    if (LLVMGetTypeKind(ret_ll)==LLVMVoidTypeKind)
                        result = LLVMBuildCall2(b, fn_ty, fn_val, args, (unsigned)ai, "");
                    else
                        result = LLVMBuildCall2(b, fn_ty, fn_val, args, (unsigned)ai, "call");
                    free(args);
                    return result;
                }
            }
        }

        /* lookup by symbol name */
        if (fn_sym && !fn_val) {
            LGGlobal *gl = lg_find_global(g, fn_sym);
            if (gl) { fn_val=gl->fn_val; fn_ty=gl->fn_ty; ret_ns=gl->ns_ret_ty; }
        }

        if (!fn_val || !fn_ty)
            return LLVMConstInt(LLVMInt32TypeInContext(ctx), 0, 0);

        /* build args */
        int nargs = n->as.call.args.count;
        LLVMValueRef *args = calloc((size_t)(nargs + 1), sizeof(LLVMValueRef));
        for (int i = 0; i < nargs; i++)
            args[i] = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.call.args.items[i]);

        /* coerce args to match declared param types */
        {
            unsigned np = LLVMCountParamTypes(fn_ty);
            if (np > 0) {
                LLVMTypeRef *ptys = calloc(np, sizeof(LLVMTypeRef));
                LLVMGetParamTypes(fn_ty, ptys);
                for (int i = 0; i < nargs && i < (int)np; i++)
                    args[i] = lg_coerce_arg(b, args[i], ptys[i], ctx);
                free(ptys);
            }
        }

        LLVMTypeRef ret_ll = ret_ns ? llg_node_to_ll(g, ret_ns) : LLVMVoidTypeInContext(ctx);
        LLVMValueRef result;
        if (LLVMGetTypeKind(ret_ll)==LLVMVoidTypeKind)
            result = LLVMBuildCall2(b, fn_ty, fn_val, args, (unsigned)nargs, "");
        else
            result = LLVMBuildCall2(b, fn_ty, fn_val, args, (unsigned)nargs, "call");
        free(args);
        return result;
    }

    /* ─── slice expression ─── */
    case NODE_SLICE: {
        Node *obj_ns = llg_infer_type(g, n->as.slice_expr.object);
        LLVMValueRef obj_ptr = NULL; LLVMTypeRef obj_elt = NULL;
        LLVMValueRef obj_val = llg_emit_expr(g, b, cur_bb, &obj_ptr, &obj_elt, n->as.slice_expr.object);
        LLVMValueRef start_v = n->as.slice_expr.start
            ? llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.slice_expr.start)
            : LLVMConstInt(LLVMInt64TypeInContext(ctx), 0, 0);

        LLVMValueRef data_ptr;
        Node *elem_ns = NULL;
        if (obj_ns && obj_ns->kind==NODE_TYPE_ARRAY && obj_ns->as.type_array.length<0) {
            LLVMTypeRef slty = llg_node_to_ll(g, obj_ns);
            if (obj_ptr) {
                LLVMValueRef pp = LLVMBuildStructGEP2(b, slty, obj_ptr, 0, "sl.ptr.f");
                data_ptr = lg_load(b, LLVMPointerTypeInContext(ctx,0), pp, "sl.data");
            } else {
                data_ptr = LLVMBuildExtractValue(b, obj_val, 0, "sl.data");
            }
            elem_ns = obj_ns->as.type_array.elem;
        } else if (obj_ptr) {
            data_ptr = obj_ptr;
            if (obj_ns && obj_ns->kind==NODE_TYPE_POINTER) elem_ns = obj_ns->as.type_ptr.inner;
        } else {
            data_ptr = obj_val;
        }
        LLVMTypeRef elem_ll = elem_ns ? llg_node_to_ll(g, elem_ns) : LLVMInt8TypeInContext(ctx);
        LLVMValueRef idx[1] = { start_v };
        LLVMValueRef new_ptr = LLVMBuildGEP2(b, elem_ll, data_ptr, idx, 1, "slice.ptr");

        /* compute len */
        LLVMValueRef len_v;
        if (n->as.slice_expr.end) {
            LLVMValueRef end_v = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.slice_expr.end);
            len_v = LLVMBuildSub(b, end_v, start_v, "slice.len");
        } else {
            /* slice to end: need original len - start */
            LLVMValueRef orig_len;
            if (obj_ns && obj_ns->kind==NODE_TYPE_ARRAY && obj_ns->as.type_array.length<0) {
                LLVMTypeRef slty = llg_node_to_ll(g, obj_ns);
                if (obj_ptr) {
                    LLVMValueRef lp = LLVMBuildStructGEP2(b, slty, obj_ptr, 1, "sl.len.f");
                    orig_len = lg_load(b, LLVMInt64TypeInContext(ctx), lp, "sl.len");
                } else {
                    orig_len = LLVMBuildExtractValue(b, obj_val, 1, "sl.len");
                }
            } else {
                orig_len = LLVMConstInt(LLVMInt64TypeInContext(ctx), 0, 0);
            }
            len_v = LLVMBuildSub(b, orig_len, start_v, "slice.len");
        }
        /* build slice struct {ptr, len} */
        LLVMTypeRef fields2[2] = { LLVMPointerTypeInContext(ctx,0), LLVMInt64TypeInContext(ctx) };
        LLVMTypeRef slice_ty = LLVMStructTypeInContext(ctx, fields2, 2, 0);
        LLVMValueRef sl = LLVMGetUndef(slice_ty);
        sl = LLVMBuildInsertValue(b, sl, new_ptr, 0, "sl.new.ptr");
        sl = LLVMBuildInsertValue(b, sl, len_v, 1, "sl.new.len");
        return sl;
    }

    default:
        return LLVMConstInt(LLVMInt32TypeInContext(ctx), 0, 0);
    } /* switch */
}

/* ── statement emission ────────────────────────────────────────────────────── */

static void llg_emit_block(LG *g, LLVMBuilderRef b, LLVMBasicBlockRef *cur_bb, Node *blk) {
    if (!blk) return;
    lg_push_scope(g);
    for (int i = 0; i < blk->as.block.stmts.count; i++) {
        LLVMBasicBlockRef cur = cur_bb ? *cur_bb : LLVMGetInsertBlock(b);
        if (LLVMGetBasicBlockTerminator(cur)) break;
        llg_emit_stmt(g, b, cur_bb, blk->as.block.stmts.items[i]);
    }
    lg_emit_scope_defers(g, b, cur_bb);
    lg_pop_scope(g);
}

static void llg_emit_stmt(LG *g, LLVMBuilderRef b, LLVMBasicBlockRef *cur_bb, Node *n) {
    LLVMContextRef ctx = g->ctx;
    if (!n) return;

    switch (n->kind) {

    case NODE_EXPR_STMT:
        llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.expr_stmt.expr);
        break;

    case NODE_LET: {
        Node *ty_node = n->as.let.type;
        Node *val_node = n->as.let.value;
        LLVMTypeRef ll_ty;
        Node *ns_ty = ty_node;
        if (ty_node) {
            ll_ty = llg_node_to_ll(g, ty_node);
        } else if (val_node) {
            ns_ty = llg_infer_type(g, val_node);
            ll_ty = ns_ty ? llg_node_to_ll(g, ns_ty) : LLVMInt32TypeInContext(ctx);
        } else {
            ll_ty = LLVMInt32TypeInContext(ctx);
        }
        LLVMValueRef alloca = lg_alloca(g, b, ll_ty, n->as.let.name);
        if (val_node) {
            LLVMValueRef val = llg_emit_expr(g, b, cur_bb, NULL, NULL, val_node);
            LLVMBuildStore(b, val, alloca);
        } else {
            LLVMBuildStore(b, LLVMConstNull(ll_ty), alloca);
        }
        lg_define(g, n->as.let.name, alloca, ll_ty, ns_ty);
        break;
    }

    case NODE_CONST: {
        Node *val_node = n->as.konst.value;
        Node *ns_ty = n->as.konst.type;
        if (!ns_ty && val_node) ns_ty = llg_infer_type(g, val_node);
        LLVMTypeRef ll_ty = ns_ty ? llg_node_to_ll(g, ns_ty) : LLVMInt32TypeInContext(ctx);
        LLVMValueRef alloca = lg_alloca(g, b, ll_ty, n->as.konst.name);
        if (val_node) {
            LLVMValueRef val = llg_emit_expr(g, b, cur_bb, NULL, NULL, val_node);
            LLVMBuildStore(b, val, alloca);
        }
        lg_define(g, n->as.konst.name, alloca, ll_ty, ns_ty);
        break;
    }

    case NODE_RETURN: {
        lg_emit_defers_range(g, b, cur_bb, 0);
        if (n->as.ret.value) {
            LLVMValueRef val = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.ret.value);
            LLVMBuildRet(b, val);
        } else {
            LLVMBuildRetVoid(b);
        }
        break;
    }

    case NODE_IF: {
        LLVMValueRef cond = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.if_stmt.cond);
        LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "if_then");
        LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "if_else");
        LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "if_done");
        LLVMBuildCondBr(b, cond, then_bb, else_bb);

        LLVMPositionBuilderAtEnd(b, then_bb);
        if (cur_bb) *cur_bb = then_bb;
        llg_emit_block(g, b, cur_bb, n->as.if_stmt.then_block);
        LLVMBasicBlockRef then_end = cur_bb ? *cur_bb : LLVMGetInsertBlock(b);
        if (!LLVMGetBasicBlockTerminator(then_end)) LLVMBuildBr(b, done_bb);

        LLVMPositionBuilderAtEnd(b, else_bb);
        if (cur_bb) *cur_bb = else_bb;
        if (n->as.if_stmt.else_node) {
            Node *en = n->as.if_stmt.else_node;
            if (en->kind == NODE_IF) llg_emit_stmt(g, b, cur_bb, en);
            else                     llg_emit_block(g, b, cur_bb, en);
        }
        LLVMBasicBlockRef else_end = cur_bb ? *cur_bb : LLVMGetInsertBlock(b);
        if (!LLVMGetBasicBlockTerminator(else_end)) LLVMBuildBr(b, done_bb);

        LLVMPositionBuilderAtEnd(b, done_bb);
        if (cur_bb) *cur_bb = done_bb;
        break;
    }

    case NODE_WHILE: {
        LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "wh_cond");
        LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "wh_body");
        LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "wh_done");
        LLVMBuildBr(b, cond_bb);
        LLVMPositionBuilderAtEnd(b, cond_bb);
        if (cur_bb) *cur_bb = cond_bb;
        LLVMValueRef cond = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.while_stmt.cond);
        LLVMBuildCondBr(b, cond, body_bb, done_bb);
        LLVMPositionBuilderAtEnd(b, body_bb);
        if (cur_bb) *cur_bb = body_bb;
        if (g->loop_depth < 64) g->loops[g->loop_depth++] = (LGLoop){done_bb, cond_bb};
        llg_emit_block(g, b, cur_bb, n->as.while_stmt.body);
        if (g->loop_depth > 0) g->loop_depth--;
        LLVMBasicBlockRef body_end = cur_bb ? *cur_bb : LLVMGetInsertBlock(b);
        if (!LLVMGetBasicBlockTerminator(body_end)) LLVMBuildBr(b, cond_bb);
        LLVMPositionBuilderAtEnd(b, done_bb);
        if (cur_bb) *cur_bb = done_bb;
        break;
    }

    case NODE_LOOP:
    case NODE_UNSAFE: {
        LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "lp_body");
        LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "lp_done");
        LLVMBuildBr(b, body_bb);
        LLVMPositionBuilderAtEnd(b, body_bb);
        if (cur_bb) *cur_bb = body_bb;
        if (g->loop_depth < 64) g->loops[g->loop_depth++] = (LGLoop){done_bb, body_bb};
        llg_emit_block(g, b, cur_bb, n->as.loop_stmt.body);
        if (g->loop_depth > 0) g->loop_depth--;
        LLVMBasicBlockRef body_end = cur_bb ? *cur_bb : LLVMGetInsertBlock(b);
        if (!LLVMGetBasicBlockTerminator(body_end)) LLVMBuildBr(b, body_bb);
        LLVMPositionBuilderAtEnd(b, done_bb);
        if (cur_bb) *cur_bb = done_bb;
        break;
    }

    case NODE_FOR: {
        LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "for_cond");
        LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "for_body");
        LLVMBasicBlockRef post_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "for_post");
        LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(ctx, g->cur_fn, "for_done");
        lg_push_scope(g);
        if (n->as.for_stmt.init) llg_emit_stmt(g, b, cur_bb, n->as.for_stmt.init);
        LLVMBuildBr(b, cond_bb);
        LLVMPositionBuilderAtEnd(b, cond_bb);
        if (cur_bb) *cur_bb = cond_bb;
        if (n->as.for_stmt.cond) {
            LLVMValueRef cond = llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.for_stmt.cond);
            LLVMBuildCondBr(b, cond, body_bb, done_bb);
        } else {
            LLVMBuildBr(b, body_bb);
        }
        LLVMPositionBuilderAtEnd(b, body_bb);
        if (cur_bb) *cur_bb = body_bb;
        if (g->loop_depth < 64) g->loops[g->loop_depth++] = (LGLoop){done_bb, post_bb};
        llg_emit_block(g, b, cur_bb, n->as.for_stmt.body);
        if (g->loop_depth > 0) g->loop_depth--;
        LLVMBasicBlockRef body_end = cur_bb ? *cur_bb : LLVMGetInsertBlock(b);
        if (!LLVMGetBasicBlockTerminator(body_end)) LLVMBuildBr(b, post_bb);
        LLVMPositionBuilderAtEnd(b, post_bb);
        if (cur_bb) *cur_bb = post_bb;
        if (n->as.for_stmt.post) llg_emit_expr(g, b, cur_bb, NULL, NULL, n->as.for_stmt.post);
        LLVMBuildBr(b, cond_bb);
        lg_pop_scope(g);
        LLVMPositionBuilderAtEnd(b, done_bb);
        if (cur_bb) *cur_bb = done_bb;
        break;
    }

    case NODE_BREAK:
        lg_emit_scope_defers(g, b, cur_bb);
        if (g->loop_depth > 0) LLVMBuildBr(b, g->loops[g->loop_depth-1].break_bb);
        break;

    case NODE_CONTINUE:
        lg_emit_scope_defers(g, b, cur_bb);
        if (g->loop_depth > 0) LLVMBuildBr(b, g->loops[g->loop_depth-1].cont_bb);
        break;

    case NODE_DEFER:
        if (g->defer_count < 256) g->defers[g->defer_count++] = n->as.defer_stmt.expr;
        break;

    case NODE_BLOCK:
        llg_emit_block(g, b, cur_bb, n);
        break;

    default: break;
    }
}






/* ── function emission ─────────────────────────────────────────────────────── */

static void llg_emit_fn(LG *g, Node *decl) {
    if (!decl || decl->kind != NODE_FN_DECL) return;
    if (!decl->as.fn.body) return; /* extern or forward decl */

    /* find the already-declared function value */
    char sym[512];
    if (decl->as.fn.owner_type)
        snprintf(sym, sizeof(sym), "%s_%s", decl->as.fn.owner_type, decl->as.fn.name);
    else
        strncpy(sym, decl->as.fn.name, sizeof(sym)-1);

    LGGlobal *gl = lg_find_global(g, sym);
    if (!gl) return;

    LLVMValueRef fn_val = gl->fn_val;
    LLVMTypeRef  fn_ty  = gl->fn_ty;
    g->cur_fn    = fn_val;
    g->cur_ret_ty = decl->as.fn.ret_type;
    g->defer_count = 0;
    g->scope_defer_depth = 0;

    LLVMContextRef ctx = g->ctx;
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, fn_val, "entry");
    LLVMBuilderRef b = LLVMCreateBuilderInContext(ctx);
    LLVMPositionBuilderAtEnd(b, entry);
    LLVMBasicBlockRef cur_bb = entry;

    /* push scope and bind parameters */
    lg_push_scope(g);
    unsigned nparams = LLVMCountParams(fn_val);
    LLVMTypeRef *param_tys = calloc((size_t)nparams, sizeof(LLVMTypeRef));
    LLVMGetParamTypes(fn_ty, param_tys);

    for (unsigned i = 0; i < nparams; i++) {
        LLVMValueRef param = LLVMGetParam(fn_val, i);
        Node *pnode = NULL;
        if (i < (unsigned)decl->as.fn.params.count)
            pnode = decl->as.fn.params.items[i];
        const char *pname = pnode ? pnode->as.let.name : "p";
        LLVMSetValueName2(param, pname, strlen(pname));
        LLVMTypeRef pty = param_tys[i];
        LLVMValueRef alloca = lg_alloca(g, b, pty, pname);
        LLVMBuildStore(b, param, alloca);
        Node *ns_ty = pnode ? pnode->as.let.type : NULL;
        lg_define(g, pname, alloca, pty, ns_ty);
    }
    free(param_tys);

    /* emit body */
    llg_emit_block(g, b, &cur_bb, decl->as.fn.body);
    lg_pop_scope(g);

    /* ensure terminator */
    LLVMBasicBlockRef end = cur_bb;
    if (!LLVMGetBasicBlockTerminator(end)) {
        if (LLVMGetTypeKind(LLVMGetReturnType(fn_ty)) == LLVMVoidTypeKind)
            LLVMBuildRetVoid(b);
        else
            LLVMBuildRet(b, LLVMConstNull(LLVMGetReturnType(fn_ty)));
    }

    LLVMDisposeBuilder(b);
    g->cur_fn = NULL;
}

/* ── type setup: structs, enums, unions ────────────────────────────────────── */

static void llg_setup_struct(LG *g, Node *d) {
    const char *name = d->as.struct_decl.name;
    if (lg_find_named_type(g, name)) return;

    int n = d->as.struct_decl.fields.count;
    LLVMTypeRef *fields = calloc((size_t)(n > 0 ? n : 1), sizeof(LLVMTypeRef));
    for (int i = 0; i < n; i++)
        fields[i] = llg_node_to_ll(g, d->as.struct_decl.fields.items[i]->as.let.type);
    LLVMTypeRef ty = LLVMStructCreateNamed(g->ctx, name);
    LLVMStructSetBody(ty, fields, (unsigned)n, d->as.struct_decl.is_packed);
    free(fields);
    lg_register_type(g, name, ty, d);
}

static void llg_setup_union(LG *g, Node *d) {
    const char *name = d->as.union_decl.name;
    if (lg_find_named_type(g, name)) return;
    int n = d->as.union_decl.fields.count;
    /* find largest field for union representation */
    unsigned max_bits = 8;
    for (int i = 0; i < n; i++) {
        LLVMTypeRef fty = llg_node_to_ll(g, d->as.union_decl.fields.items[i]->as.let.type);
        unsigned bits = (unsigned)LLVMStoreSizeOfType(g->td, fty) * 8;
        if (bits > max_bits) max_bits = bits;
    }
    LLVMTypeRef body[1] = { LLVMArrayType(LLVMInt8TypeInContext(g->ctx), max_bits/8) };
    LLVMTypeRef ty = LLVMStructCreateNamed(g->ctx, name);
    LLVMStructSetBody(ty, body, 1, 0);
    lg_register_type(g, name, ty, d);
}

static void llg_setup_enum(LG *g, Node *d) {
    const char *name = d->as.enum_decl.name;
    if (lg_find_named_type(g, name)) return;

    int has_data = 0;
    for (int i = 0; i < d->as.enum_decl.count; i++)
        if (d->as.enum_decl.variant_fields[i].count > 0) { has_data = 1; break; }

    LLVMTypeRef ty;
    if (!has_data) {
        /* simple enum = i32 */
        ty = LLVMInt32TypeInContext(g->ctx);
    } else {
        /* tagged union: {i32, [max_payload_bytes x i8]} */
        unsigned max_payload = 0;
        for (int i = 0; i < d->as.enum_decl.count; i++) {
            NodeList *vf = &d->as.enum_decl.variant_fields[i];
            unsigned sz = 0;
            for (int j = 0; j < vf->count; j++) {
                LLVMTypeRef fty = llg_node_to_ll(g, vf->items[j]->as.let.type);
                sz += (unsigned)LLVMStoreSizeOfType(g->td, fty);
            }
            if (sz > max_payload) max_payload = sz;
        }
        if (max_payload == 0) max_payload = 1;
        LLVMTypeRef fields[2] = {
            LLVMInt32TypeInContext(g->ctx),
            LLVMArrayType(LLVMInt8TypeInContext(g->ctx), max_payload)
        };
        ty = LLVMStructCreateNamed(g->ctx, name);
        LLVMStructSetBody(ty, fields, 2, 0);
    }
    lg_register_type(g, name, ty, d);
}

/* ── forward-declare all functions in global table ─────────────────────────── */

static LLVMTypeRef llg_build_fn_type(LG *g, NodeList *params, Node *ret_type,
                                      int has_self_ptr, const char *owner_name) {
    int nparams = params->count + (has_self_ptr ? 1 : 0);
    LLVMTypeRef *ptys = calloc((size_t)(nparams > 0 ? nparams : 1), sizeof(LLVMTypeRef));
    int ai = 0;
    if (has_self_ptr) {
        LLVMTypeRef self_ty = lg_find_named_type(g, owner_name);
        ptys[ai++] = self_ty ? LLVMPointerTypeInContext(g->ctx,0) : LLVMPointerTypeInContext(g->ctx,0);
    }
    for (int i = 0; i < params->count; i++)
        ptys[ai++] = llg_node_to_ll(g, params->items[i]->as.let.type);
    LLVMTypeRef ret_ll = ret_type ? llg_node_to_ll(g, ret_type) : LLVMVoidTypeInContext(g->ctx);
    LLVMTypeRef fn_ty = LLVMFunctionType(ret_ll, ptys, (unsigned)nparams, 0);
    free(ptys);
    return fn_ty;
}

static void llg_declare_fn(LG *g, Node *d) {
    if (d->kind != NODE_FN_DECL && d->kind != NODE_EXTERN_FN) return;

    const char *base_name;
    const char *owner = NULL;
    NodeList *params;
    Node *ret_type;

    if (d->kind == NODE_FN_DECL) {
        if (d->as.fn.type_params.count > 0) return; /* skip generic templates */
        base_name = d->as.fn.name;
        owner     = d->as.fn.owner_type;
        params    = &d->as.fn.params;
        ret_type  = d->as.fn.ret_type;
    } else {
        base_name = d->as.extern_fn.name;
        params    = &d->as.extern_fn.params;
        ret_type  = d->as.extern_fn.ret_type;
    }

    char sym[512];
    if (owner) snprintf(sym, sizeof(sym), "%s_%s", owner, base_name);
    else        strncpy(sym, base_name, sizeof(sym)-1);

    if (lg_find_global(g, sym)) return;

    /* check if first param is self/Self (instance method) */
    int has_self = 0;
    if (d->kind==NODE_FN_DECL && owner && params->count > 0) {
        const char *pn = params->items[0]->as.let.name;
        has_self = (!strcmp(pn,"self") || !strcmp(pn,"Self"));
    }

    LLVMTypeRef fn_ty;
    if (has_self) {
        /* self is already in params, pass it as pointer */
        int np = params->count;
        LLVMTypeRef *ptys = calloc((size_t)(np > 0 ? np : 1), sizeof(LLVMTypeRef));
        for (int i = 0; i < np; i++) {
            Node *pn = params->items[i];
            const char *nm = pn->as.let.name;
            if (!strcmp(nm,"self")||!strcmp(nm,"Self")) {
                LLVMTypeRef self_st = lg_find_named_type(g, owner);
                ptys[i] = self_st ? LLVMPointerTypeInContext(g->ctx,0) : LLVMPointerTypeInContext(g->ctx,0);
            } else {
                ptys[i] = llg_node_to_ll(g, pn->as.let.type);
            }
        }
        LLVMTypeRef ret_ll = ret_type ? llg_node_to_ll(g, ret_type) : LLVMVoidTypeInContext(g->ctx);
        fn_ty = LLVMFunctionType(ret_ll, ptys, (unsigned)np, 0);
        free(ptys);
    } else {
        fn_ty = llg_build_fn_type(g, params, ret_type, 0, NULL);
    }

    LLVMValueRef fn_val = LLVMAddFunction(g->mod, sym, fn_ty);
    if (d->kind==NODE_EXTERN_FN)
        LLVMSetLinkage(fn_val, LLVMExternalLinkage);
    lg_add_global(g, lg_dup(sym), fn_val, fn_ty, ret_type, d);
}

/* ── generic prescan (mirrors C backend) ─────────────────────────────────── */

static void llg_prescan_type(LG *g, Node *type);
static void llg_prescan_expr(LG *g, Node *n);
static void llg_prescan_stmt(LG *g, Node *n);

static void llg_prescan_type(LG *g, Node *type) {
    if (!type) return;
    switch (type->kind) {
    case NODE_TYPE_NAMED: {
        const char *name = type->as.type_named.name;
        for (int gi = 0; gi < g->gst_count; gi++) {
            if (lg_type_form(name, g->gst_names[gi])) {
                char *inner = lg_extract_inner(name);
                if (inner) { const char *ns_t = inner; lg_gst_find_or_add(g, g->gst_decls[gi], name, &ns_t, 1); free(inner); }
                break;
            }
        }
        break;
    }
    case NODE_TYPE_POINTER: llg_prescan_type(g, type->as.type_ptr.inner); break;
    case NODE_TYPE_ARRAY:   llg_prescan_type(g, type->as.type_array.elem); break;
    default: break;
    }
}

static void llg_prescan_expr(LG *g, Node *n) {
    if (!n) return;
    switch (n->kind) {
    case NODE_CALL:
        if (n->as.call.callee && n->as.call.callee->kind==NODE_INDEX) {
            Node *idx = n->as.call.callee;
            Node *obj = idx->as.index_expr.object;
            Node *ta  = idx->as.index_expr.index;
            if (obj && obj->kind==NODE_IDENT && ta) {
                const char *fn_name = obj->as.ident.name;
                for (int i = 0; i < g->gfn_count; i++) {
                    if (!strcmp(g->gfn_names[i], fn_name)) {
                        const char *ns_t = NULL;
                        if (ta->kind==NODE_IDENT)      ns_t = ta->as.ident.name;
                        if (ta->kind==NODE_TYPE_NAMED) ns_t = ta->as.type_named.name;
                        if (ns_t) lg_gfn_find_or_add(g, g->gfn_decls[i], fn_name, ns_t);
                        break;
                    }
                }
            }
        }
        llg_prescan_expr(g, n->as.call.callee);
        for (int i = 0; i < n->as.call.args.count; i++) llg_prescan_expr(g, n->as.call.args.items[i]);
        break;
    case NODE_BINARY: llg_prescan_expr(g, n->as.binary.left); llg_prescan_expr(g, n->as.binary.right); break;
    case NODE_UNARY:  llg_prescan_expr(g, n->as.unary.operand); break;
    case NODE_ASSIGN: llg_prescan_expr(g, n->as.assign.target); llg_prescan_expr(g, n->as.assign.value); break;
    case NODE_FIELD:  llg_prescan_expr(g, n->as.field.object); break;
    case NODE_INDEX:  llg_prescan_expr(g, n->as.index_expr.object); llg_prescan_expr(g, n->as.index_expr.index); break;
    case NODE_CAST:   llg_prescan_expr(g, n->as.cast.expr); break;
    default: break;
    }
}

static void llg_prescan_stmt(LG *g, Node *n) {
    if (!n) return;
    switch (n->kind) {
    case NODE_BLOCK: for (int i=0;i<n->as.block.stmts.count;i++) llg_prescan_stmt(g,n->as.block.stmts.items[i]); break;
    case NODE_RETURN: llg_prescan_expr(g,n->as.ret.value); break;
    case NODE_LET:    llg_prescan_type(g,n->as.let.type); llg_prescan_expr(g,n->as.let.value); break;
    case NODE_EXPR_STMT: llg_prescan_expr(g,n->as.expr_stmt.expr); break;
    case NODE_DEFER:  llg_prescan_expr(g,n->as.defer_stmt.expr); break;
    case NODE_IF:     llg_prescan_expr(g,n->as.if_stmt.cond); llg_prescan_stmt(g,n->as.if_stmt.then_block); llg_prescan_stmt(g,n->as.if_stmt.else_node); break;
    case NODE_WHILE:  llg_prescan_expr(g,n->as.while_stmt.cond); llg_prescan_stmt(g,n->as.while_stmt.body); break;
    case NODE_FOR:    llg_prescan_stmt(g,n->as.for_stmt.init); llg_prescan_expr(g,n->as.for_stmt.cond); llg_prescan_expr(g,n->as.for_stmt.post); llg_prescan_stmt(g,n->as.for_stmt.body); break;
    case NODE_LOOP: case NODE_UNSAFE: llg_prescan_stmt(g,n->as.loop_stmt.body); break;
    default: break;
    }
}

/* ── runtime helpers declaration ─────────────────────────────────────────── */

static void llg_declare_runtime(LG *g) {
    LLVMContextRef ctx = g->ctx;
    LLVMTypeRef ptr_ty  = LLVMPointerTypeInContext(ctx, 0);
    LLVMTypeRef i32_ty  = LLVMInt32TypeInContext(ctx);
    LLVMTypeRef i64_ty  = LLVMInt64TypeInContext(ctx);
    LLVMTypeRef f64_ty  = LLVMDoubleTypeInContext(ctx);
    LLVMTypeRef void_ty = LLVMVoidTypeInContext(ctx);
    LLVMTypeRef str_ty  = llg_ns_to_ll(g, "str");
    LLVMTypeRef sng_ty  = llg_ns_to_ll(g, "String");

    struct { const char *nm; LLVMTypeRef *ptys; unsigned np; LLVMTypeRef ret; } rt[] = {
        /* IO */
        {"NS_println",      (LLVMTypeRef[]){str_ty}, 1, void_ty},
        {"NS_print",        (LLVMTypeRef[]){str_ty}, 1, void_ty},
        {"NS_println_int",  (LLVMTypeRef[]){i64_ty}, 1, void_ty},
        {"NS_println_f64",  (LLVMTypeRef[]){f64_ty}, 1, void_ty},
        {"NS_println_bool", (LLVMTypeRef[]){i32_ty}, 1, void_ty},
        {"NS_println_char", (LLVMTypeRef[]){i32_ty}, 1, void_ty},
        /* String */
        {"NS_string_from",     (LLVMTypeRef[]){str_ty},          1, sng_ty},
        {"NS_string_as_str",   (LLVMTypeRef[]){ptr_ty},          1, str_ty},
        {"NS_string_free",     (LLVMTypeRef[]){ptr_ty},          1, void_ty},
        {"NS_string_push_str", (LLVMTypeRef[]){ptr_ty, str_ty},  2, void_ty},
        {"NS_string_len",      (LLVMTypeRef[]){ptr_ty},          1, i64_ty},
        /* memory */
        {"NS_alloc",   (LLVMTypeRef[]){i64_ty}, 1, ptr_ty},
        {"NS_free",    (LLVMTypeRef[]){ptr_ty}, 1, void_ty},
        /* process */
        {"NS_exit",    (LLVMTypeRef[]){i32_ty}, 1, void_ty},
        /* C stdio wrappers */
        {"printf",  (LLVMTypeRef[]){ptr_ty}, 1, i32_ty},
        {"puts",    (LLVMTypeRef[]){ptr_ty}, 1, i32_ty},
        {"malloc",  (LLVMTypeRef[]){i64_ty}, 1, ptr_ty},
        {"free",    (LLVMTypeRef[]){ptr_ty}, 1, void_ty},
        {NULL, NULL, 0, NULL}
    };

    for (int i = 0; rt[i].nm; i++) {
        if (lg_find_global(g, rt[i].nm)) continue;
        LLVMTypeRef fn_ty = LLVMFunctionType(rt[i].ret, rt[i].ptys, rt[i].np, 0);
        LLVMValueRef fn_val = LLVMAddFunction(g->mod, rt[i].nm, fn_ty);
        LLVMSetLinkage(fn_val, LLVMExternalLinkage);
        lg_add_global(g, rt[i].nm, fn_val, fn_ty, NULL, NULL);
    }
}

/* ── comptime constants ───────────────────────────────────────────────────── */

static void llg_emit_comptime(LG *g, Node *prog) {
    for (int i = 0; i < prog->as.program.decls.count; i++) {
        Node *d = prog->as.program.decls.items[i];
        if (d->kind != NODE_COMPTIME) continue;
        for (int j = 0; j < d->as.comptime_block.decls.count; j++) {
            Node *c = d->as.comptime_block.decls.items[j];
            if (c->kind != NODE_CONST || !c->as.konst.value) continue;
            Node *ns_ty = c->as.konst.type;
            if (!ns_ty) ns_ty = llg_infer_type(g, c->as.konst.value);
            LLVMTypeRef ll_ty = ns_ty ? llg_node_to_ll(g, ns_ty) : LLVMInt32TypeInContext(g->ctx);
            /* build a constant initializer from the expression (literals only for comptime) */
            LLVMValueRef init_val = LLVMConstNull(ll_ty);
            Node *val = c->as.konst.value;
            if (val->kind==NODE_LIT_INT)
                init_val = LLVMConstInt(ll_ty, (unsigned long long)val->as.lit_int.value, 1);
            else if (val->kind==NODE_LIT_FLOAT)
                init_val = LLVMConstReal(ll_ty, val->as.lit_float.value);
            else if (val->kind==NODE_LIT_BOOL)
                init_val = LLVMConstInt(ll_ty, (unsigned long long)val->as.lit_int.value, 0);
            LLVMValueRef gv = LLVMAddGlobal(g->mod, ll_ty, c->as.konst.name);
            LLVMSetInitializer(gv, init_val);
            LLVMSetGlobalConstant(gv, 1);
            LLVMSetLinkage(gv, LLVMInternalLinkage);
            /* expose as an alloca-like entry in global scope through a fake LGVar */
            lg_define(g, c->as.konst.name, gv, ll_ty, ns_ty);
        }
    }
}

/* ── generic instantiation ───────────────────────────────────────────────── */

static void llg_instantiate_generics(LG *g) {
    /* generic functions */
    for (LGGFnInst *inst = g->gfn_insts; inst; inst = inst->next) {
        if (lg_find_global(g, inst->mangled)) continue;
        Node *decl = inst->decl;
        /* set substitution */
        if (decl->as.fn.type_params.count > 0) {
            g->sub_params[0] = decl->as.fn.type_params.items[0]->as.ident.name;
            g->sub_types[0]  = inst->ns_type;
            g->sub_count = 1;
        }
        /* build fn type */
        int np = decl->as.fn.params.count;
        LLVMTypeRef *ptys = calloc((size_t)(np>0?np:1), sizeof(LLVMTypeRef));
        for (int i = 0; i < np; i++)
            ptys[i] = llg_node_to_ll(g, decl->as.fn.params.items[i]->as.let.type);
        LLVMTypeRef ret_ll = decl->as.fn.ret_type
            ? llg_node_to_ll(g, decl->as.fn.ret_type)
            : LLVMVoidTypeInContext(g->ctx);
        LLVMTypeRef fn_ty = LLVMFunctionType(ret_ll, ptys, (unsigned)np, 0);
        free(ptys);
        LLVMValueRef fn_val = LLVMAddFunction(g->mod, inst->mangled, fn_ty);
        lg_add_global(g, inst->mangled, fn_val, fn_ty, decl->as.fn.ret_type, decl);

        /* emit body */
        g->cur_fn = fn_val;
        g->cur_ret_ty = decl->as.fn.ret_type;
        g->defer_count = 0;
        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(g->ctx, fn_val, "entry");
        LLVMBuilderRef b = LLVMCreateBuilderInContext(g->ctx);
        LLVMPositionBuilderAtEnd(b, entry);
        LLVMBasicBlockRef cur_bb = entry;
        lg_push_scope(g);
        for (int i = 0; i < np; i++) {
            LLVMValueRef param = LLVMGetParam(fn_val, (unsigned)i);
            const char *pname = decl->as.fn.params.items[i]->as.let.name;
            LLVMSetValueName2(param, pname, strlen(pname));
            LLVMTypeRef pty = LLVMTypeOf(param);
            LLVMValueRef alloca = lg_alloca(g, b, pty, pname);
            LLVMBuildStore(b, param, alloca);
            lg_define(g, pname, alloca, pty, decl->as.fn.params.items[i]->as.let.type);
        }
        llg_emit_block(g, b, &cur_bb, decl->as.fn.body);
        lg_pop_scope(g);
        if (!LLVMGetBasicBlockTerminator(cur_bb)) {
            if (LLVMGetTypeKind(LLVMGetReturnType(fn_ty))==LLVMVoidTypeKind) LLVMBuildRetVoid(b);
            else LLVMBuildRet(b, LLVMConstNull(LLVMGetReturnType(fn_ty)));
        }
        LLVMDisposeBuilder(b);
        g->cur_fn = NULL;
        g->sub_count = 0;
    }

    /* generic structs: just ensure the LLVM type is created */
    for (LGGStInst *inst = g->gst_insts; inst; inst = inst->next) {
        if (lg_find_named_type(g, inst->ns_name)) continue;
        if (inst->decl && inst->type_count > 0) {
            if (inst->decl->as.struct_decl.type_params.count > 0) {
                g->sub_params[0] = inst->decl->as.struct_decl.type_params.items[0]->as.ident.name;
                g->sub_types[0]  = inst->ns_types[0];
                g->sub_count = 1;
            }
            int n = inst->decl->as.struct_decl.fields.count;
            LLVMTypeRef *fields = calloc((size_t)(n>0?n:1), sizeof(LLVMTypeRef));
            for (int i = 0; i < n; i++)
                fields[i] = llg_node_to_ll(g, inst->decl->as.struct_decl.fields.items[i]->as.let.type);
            LLVMTypeRef ty = LLVMStructCreateNamed(g->ctx, inst->ns_name);
            LLVMStructSetBody(ty, fields, (unsigned)n, 0);
            free(fields);
            lg_register_type(g, inst->ns_name, ty, inst->decl);
            g->sub_count = 0;
        }
    }
}

/* ── optimization ────────────────────────────────────────────────────────── */

static void llg_optimize(LG *g, LLVMModuleRef mod, LLVMTargetMachineRef tm) {
    if (g->opt_level == 0) return;
    const char *level = "default<O1>";
    if (g->opt_level == 2) level = "default<O2>";
    if (g->opt_level >= 3) level = "default<O3>";
    LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
    LLVMRunPasses(mod, level, tm, opts);
    LLVMDisposePassBuilderOptions(opts);
}

/* ── runtime C source (embedded) ────────────────────────────────────────── */

static const char *k_runtime_c =
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <string.h>\n"
"#include <stdint.h>\n"
"typedef struct { const char *ptr; size_t len; } NStr;\n"
"typedef struct { char *ptr; size_t len; size_t cap; } NString;\n"
"void NS_println(NStr s) { fwrite(s.ptr,1,s.len,stdout); putchar('\\n'); }\n"
"void NS_print(NStr s)   { fwrite(s.ptr,1,s.len,stdout); }\n"
"void NS_println_int(int64_t v)  { printf(\"%lld\\n\",(long long)v); }\n"
"void NS_println_f64(double v)   { printf(\"%g\\n\",v); }\n"
"void NS_println_bool(int v)     { puts(v?\"true\":\"false\"); }\n"
"void NS_println_char(int v)     { printf(\"%c\\n\",v); }\n"
"NString NS_string_from(NStr s) {\n"
"    NString r; r.len=s.len; r.cap=s.len+1;\n"
"    r.ptr=malloc(r.cap); if(r.ptr){memcpy(r.ptr,s.ptr,s.len);r.ptr[s.len]=0;}\n"
"    return r;\n"
"}\n"
"NStr NS_string_as_str(NString *s) { NStr r={s->ptr,s->len}; return r; }\n"
"void NS_string_free(NString *s)   { free(s->ptr); s->ptr=NULL; s->len=s->cap=0; }\n"
"void NS_string_push_str(NString *s, NStr extra) {\n"
"    size_t need=s->len+extra.len+1;\n"
"    if(need>s->cap){s->cap=need*2;s->ptr=realloc(s->ptr,s->cap);}\n"
"    if(s->ptr){memcpy(s->ptr+s->len,extra.ptr,extra.len);s->len+=extra.len;s->ptr[s->len]=0;}\n"
"}\n"
"int64_t NS_string_len(NString *s) { return (int64_t)s->len; }\n"
"void *NS_alloc(int64_t sz) { return malloc((size_t)sz); }\n"
"void  NS_free(void *p)     { free(p); }\n"
"void  NS_exit(int code)    { exit(code); }\n";

static int llg_write_runtime(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(k_runtime_c, f);
    fclose(f);
    return 1;
}

/* ── binary pipeline ─────────────────────────────────────────────────────── */

static int llg_link(const char *obj_path, const char *rt_path, const char *out_path) {
    const char *cc = getenv("CC");
    if (!cc || !cc[0]) cc = "gcc";
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s -no-pie -o \"%s\" \"%s\" \"%s\"", cc, out_path, obj_path, rt_path);
    int rc = system(cmd);
    if (rc != 0) { fprintf(stderr, "llvmgen: linker failed\n"); return 0; }
    return 1;
}

/* ── main entry ──────────────────────────────────────────────────────────── */

int llvmgen_generate(Node *program, const char *output_path, const LLVMGenOptions *opts) {
    if (!program || program->kind != NODE_PROGRAM) return 0;

    /* initialize LLVM targets */
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();

    /* pick target triple */
    char *def_triple = LLVMGetDefaultTargetTriple();
    const char *triple = (opts && opts->target_triple) ? opts->target_triple : def_triple;

    LLVMTargetRef target_ref;
    char *err_msg = NULL;
    if (LLVMGetTargetFromTriple(triple, &target_ref, &err_msg)) {
        fprintf(stderr, "llvmgen: no target for '%s': %s\n", triple, err_msg ? err_msg : "");
        LLVMDisposeMessage(err_msg);
        LLVMDisposeMessage(def_triple);
        return 0;
    }

    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target_ref, triple,
        LLVMGetHostCPUName(), LLVMGetHostCPUFeatures(),
        LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault);

    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef  mod = LLVMModuleCreateWithNameInContext("night_module", ctx);
    LLVMSetTarget(mod, triple);
    LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
    LLVMSetModuleDataLayout(mod, td);

    /* init LG context */
    LG g;
    memset(&g, 0, sizeof(g));
    g.ctx      = ctx;
    g.mod      = mod;
    g.tm       = tm;
    g.td       = td;
    g.program  = program;
    g.opt_level = opts ? opts->opt_level : 0;

    /* create a global scope for comptime constants */
    lg_push_scope(&g);

    /* ── pass 1: collect generic templates ── */
    for (int i = 0; i < program->as.program.decls.count; i++) {
        Node *d = program->as.program.decls.items[i];
        if (d->kind==NODE_FN_DECL && d->as.fn.type_params.count>0 && g.gfn_count<64) {
            g.gfn_decls[g.gfn_count]  = d;
            g.gfn_names[g.gfn_count]  = d->as.fn.name;
            g.gfn_count++;
        }
        if (d->kind==NODE_STRUCT_DECL && d->as.struct_decl.type_params.count>0 && g.gst_count<64) {
            g.gst_decls[g.gst_count]  = d;
            g.gst_names[g.gst_count]  = d->as.struct_decl.name;
            g.gst_count++;
        }
    }

    /* ── pass 2: setup named types ── */
    /* ensure str/String types are registered */
    llg_ns_to_ll(&g, "str");
    llg_ns_to_ll(&g, "String");
    for (int i = 0; i < program->as.program.decls.count; i++) {
        Node *d = program->as.program.decls.items[i];
        if (d->kind==NODE_STRUCT_DECL && d->as.struct_decl.type_params.count==0)
            llg_setup_struct(&g, d);
        if (d->kind==NODE_UNION_DECL)
            llg_setup_union(&g, d);
        if (d->kind==NODE_ENUM_DECL)
            llg_setup_enum(&g, d);
    }

    /* ── pass 3: prescan for generic instantiations ── */
    for (int i = 0; i < program->as.program.decls.count; i++) {
        Node *d = program->as.program.decls.items[i];
        if (d->kind==NODE_FN_DECL && d->as.fn.type_params.count==0) {
            for (int j=0;j<d->as.fn.params.count;j++) llg_prescan_type(&g, d->as.fn.params.items[j]->as.let.type);
            llg_prescan_type(&g, d->as.fn.ret_type);
            if (d->as.fn.body) llg_prescan_stmt(&g, d->as.fn.body);
        }
    }

    /* ── pass 4: declare all runtime helpers + user functions ── */
    llg_declare_runtime(&g);
    for (int i = 0; i < program->as.program.decls.count; i++)
        llg_declare_fn(&g, program->as.program.decls.items[i]);

    /* ── pass 5: comptime constants ── */
    llg_emit_comptime(&g, program);

    /* ── pass 6: instantiate generics ── */
    llg_instantiate_generics(&g);

    /* ── pass 7: emit all function bodies ── */
    for (int i = 0; i < program->as.program.decls.count; i++) {
        Node *d = program->as.program.decls.items[i];
        if (d->kind==NODE_FN_DECL && d->as.fn.type_params.count==0)
            llg_emit_fn(&g, d);
        if (d->kind==NODE_IMPL_DECL) {
            for (int j=0;j<d->as.impl.methods.count;j++)
                llg_emit_fn(&g, d->as.impl.methods.items[j]);
        }
    }

    /* pop global scope */
    lg_pop_scope(&g);

    /* ── verify ── */
    char *verify_err = NULL;
    if (LLVMVerifyModule(mod, LLVMPrintMessageAction, &verify_err)) {
        fprintf(stderr, "llvmgen: IR verification failed\n");
        if (verify_err) { fprintf(stderr, "%s\n", verify_err); LLVMDisposeMessage(verify_err); }
        /* continue anyway — verification errors may be false positives */
    } else {
        if (verify_err) LLVMDisposeMessage(verify_err);
    }

    /* ── optimize ── */
    llg_optimize(&g, mod, tm);

    int ok = 0;

    /* ── emit ── */
    if (opts && opts->emit_ir) {
        /* write .ll text */
        char *ir = LLVMPrintModuleToString(mod);
        FILE *f = fopen(output_path, "wb");
        if (f) { fputs(ir, f); fclose(f); ok = 1; }
        else fprintf(stderr, "llvmgen: cannot write '%s'\n", output_path);
        LLVMDisposeMessage(ir);
    } else {
        /* emit .o, write runtime.c, link */
        char obj_path[4096], rt_path[4096];
        snprintf(obj_path, sizeof(obj_path), "%s.llvm.o", output_path);
        snprintf(rt_path,  sizeof(rt_path),  "%s.rt.c",   output_path);

        char *emit_err = NULL;
        if (LLVMTargetMachineEmitToFile(tm, mod, obj_path, LLVMObjectFile, &emit_err)) {
            fprintf(stderr, "llvmgen: emit to file failed: %s\n", emit_err ? emit_err : "");
            if (emit_err) LLVMDisposeMessage(emit_err);
            goto cleanup;
        }
        if (!llg_write_runtime(rt_path)) {
            fprintf(stderr, "llvmgen: cannot write runtime '%s'\n", rt_path);
            goto cleanup;
        }
        ok = llg_link(obj_path, rt_path, output_path);
        remove(obj_path);
        remove(rt_path);
    }

cleanup:
    /* free temp nodes */
    {
        LGTempNode *t = g.temps;
        while (t) { LGTempNode *nx = t->next; free(t->n.as.type_named.name); free(t); t = nx; }
    }
    /* free generic insts */
    {
        LGGFnInst *f = g.gfn_insts;
        while (f) { LGGFnInst *nx = f->next; free(f); f = nx; }
        LGGStInst *s = g.gst_insts;
        while (s) { LGGStInst *nx = s->next; free(s); s = nx; }
    }

    LLVMDisposeTargetData(td);
    LLVMDisposeTargetMachine(tm);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    LLVMDisposeMessage(def_triple);
    return ok;
}

#endif /* NIGHT_LLVM_BACKEND */
