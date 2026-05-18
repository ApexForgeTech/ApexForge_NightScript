#ifndef NIGHT_AST_H
#define NIGHT_AST_H

/* ── node kinds ────────────────────────────────────────────────────────── */

typedef enum {
    /* types */
    NODE_TYPE_NAMED,
    NODE_TYPE_POINTER,
    NODE_TYPE_ARRAY,

    /* expressions */
    NODE_LIT_INT,
    NODE_LIT_CHAR,
    NODE_LIT_FLOAT,
    NODE_LIT_STRING,
    NODE_LIT_BOOL,
    NODE_LIT_NULL,
    NODE_IDENT,
    NODE_BINARY,
    NODE_UNARY,
    NODE_CALL,
    NODE_FIELD,
    NODE_INDEX,
    NODE_SLICE,
    NODE_CAST,
    NODE_ASSIGN,
    NODE_STRUCT_LIT,
    NODE_MATCH,
    NODE_GROUP,

    /* statements */
    NODE_BLOCK,
    NODE_LET,
    NODE_CONST,
    NODE_RETURN,
    NODE_IF,
    NODE_WHILE,
    NODE_LOOP,
    NODE_UNSAFE,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_DEFER,
    NODE_FOR,
    NODE_EXPR_STMT,

    /* declarations */
    NODE_PROGRAM,
    NODE_PACKAGE,
    NODE_IMPORT,
    NODE_FN_DECL,
    NODE_EXTERN_FN,
    NODE_STRUCT_DECL,
    NODE_ENUM_DECL,
    NODE_UNION_DECL,
    NODE_IMPL_DECL,
    NODE_INTERFACE_DECL,
    NODE_COMPTIME,       /* comptime { const ... } */

    /* UI declarations (v0.4) */
    NODE_UI_APP,
    NODE_UI_ELEMENT,
    NODE_UI_HANDLER,
    NODE_UI_PROPERTY,

    /* Kernel declarations (v0.5) */
    NODE_KERNEL_APP,
} NodeKind;

/* ── forward declaration ───────────────────────────────────────────────── */

typedef struct Node Node;

/* ── node list (variable-length child array) ───────────────────────────── */

typedef struct {
    Node **items;
    int    count;
} NodeList;

/* ── main node struct ──────────────────────────────────────────────────── */

struct Node {
    NodeKind kind;
    int      line;
    int      col;

    union {

        /* NODE_TYPE_NAMED */
        struct { char *name; } type_named;

        /* NODE_TYPE_POINTER  — inner type + flags */
        struct {
            Node *inner;
            int   is_const;
            int   is_nullable;
        } type_ptr;

        /* NODE_TYPE_ARRAY — element type, length=-1 means slice */
        struct {
            Node *elem;
            int   length;
        } type_array;

        /* NODE_LIT_INT / NODE_LIT_CHAR / NODE_LIT_BOOL / NODE_LIT_NULL */
        struct { long long value; } lit_int;

        /* NODE_LIT_FLOAT */
        struct { double value; } lit_float;

        /* NODE_LIT_STRING */
        struct { char *value; } lit_str;

        /* NODE_IDENT */
        struct { char *name; } ident;

        /* NODE_BINARY */
        struct {
            char *op;
            Node *left;
            Node *right;
        } binary;

        /* NODE_UNARY */
        struct {
            char *op;
            Node *operand;
        } unary;

        /* NODE_CALL */
        struct {
            Node    *callee;
            NodeList args;
        } call;

        /* NODE_FIELD */
        struct {
            Node *object;
            char *field;
        } field;

        /* NODE_INDEX */
        struct {
            Node *object;
            Node *index;
        } index_expr;

        /* NODE_SLICE */
        struct {
            Node *object;
            Node *start; /* may be NULL */
            Node *end;   /* may be NULL */
        } slice_expr;

        /* NODE_CAST */
        struct {
            Node *expr;
            Node *type;
        } cast;

        /* NODE_ASSIGN — op is NULL for plain '=', or "+","-","*","/","%" for compound */
        struct {
            Node *target;
            Node *value;
            char *op;
        } assign;

        /* NODE_STRUCT_LIT */
        struct {
            char    *type_name;
            char   **field_names;   /* parallel arrays */
            Node   **field_values;
            int      count;
        } struct_lit;

        /* NODE_MATCH */
        struct {
            Node    *subject;
            /* each arm stores a pattern key plus optional binding names */
            char   **patterns;
            char  ***binding_names;
            int    *binding_counts;
            Node   **values;
            int      count;
        } match;

        /* NODE_GROUP */
        struct { Node *expr; } group;

        /* NODE_BLOCK */
        struct { NodeList stmts; } block;

        /* NODE_LET */
        struct {
            char *name;
            Node *type;    /* may be NULL */
            Node *value;   /* may be NULL */
        } let;

        /* NODE_CONST */
        struct {
            char *name;
            Node *type;    /* may be NULL */
            Node *value;
        } konst;

        /* NODE_RETURN */
        struct { Node *value; } ret;  /* value may be NULL */

        /* NODE_IF */
        struct {
            Node *cond;
            Node *then_block;
            Node *else_node;  /* NULL | NODE_BLOCK | NODE_IF */
        } if_stmt;

        /* NODE_WHILE */
        struct {
            Node *cond;
            Node *body;
        } while_stmt;

        /* NODE_LOOP / NODE_UNSAFE */
        struct { Node *body; } loop_stmt;

        /* NODE_DEFER */
        struct { Node *expr; } defer_stmt;

        /* NODE_FOR — C-style: for init; cond; post { body }
           init is NODE_LET or NODE_EXPR_STMT (or NULL), cond may be NULL (infinite),
           post is a bare expression (or NULL) */
        struct {
            Node *init;
            Node *cond;
            Node *post;
            Node *body;
        } for_stmt;

        /* NODE_EXPR_STMT */
        struct { Node *expr; } expr_stmt;

        /* NODE_PROGRAM */
        struct {
            Node    *package;   /* may be NULL */
            NodeList imports;
            NodeList decls;
        } program;

        /* NODE_PACKAGE / NODE_IMPORT */
        struct { char *path; } pkg;

        /* NODE_FN_DECL */
        struct {
            char    *name;
            char    *owner_type;  /* NULL for free functions */
            char    *package_name; /* declaring package */
            NodeList type_params; /* generic type params: [T, E] */
            NodeList params;      /* each param: NODE_LET (name+type) */
            Node    *ret_type;
            Node    *body;        /* NULL for extern */
            int      is_public;
        } fn;

        /* NODE_EXTERN_FN */
        struct {
            char    *name;
            char    *package_name; /* declaring package */
            char    *calling_conv; /* "C" or NULL */
            NodeList params;
            Node    *ret_type;
            int      is_public;
        } extern_fn;

        /* NODE_STRUCT_DECL */
        struct {
            char    *name;
            char    *package_name; /* declaring package */
            NodeList type_params; /* generic type params: [T] */
            NodeList fields;   /* each field: NODE_LET (name+type) */
            int      is_public;
            int      is_packed;
        } struct_decl;

        /* NODE_ENUM_DECL
           variant_fields[i] is NULL for simple variants, or a NodeList* for data-carrying */
        struct {
            char      *name;
            char      *package_name; /* declaring package */
            char     **variants;       /* variant names */
            NodeList  *variant_fields; /* parallel: NULL entry = simple variant */
            int        count;
            int        is_public;
        } enum_decl;

        /* NODE_UNION_DECL */
        struct {
            char    *name;
            char    *package_name; /* declaring package */
            NodeList fields;
            int      is_public;
        } union_decl;

        /* NODE_IMPL_DECL */
        struct {
            char    *target;
            char    *package_name; /* declaring package */
            char    *interface_name;  /* NULL for plain impl, set for impl T : Interface */
            NodeList methods;
        } impl;

        /* NODE_INTERFACE_DECL */
        struct {
            char    *name;
            char    *package_name; /* declaring package */
            NodeList methods;  /* each: NODE_FN_DECL with body=NULL */
            int      is_public;
        } interface_decl;

        /* NODE_UI_APP */
        struct {
            char    *name;
            char    *package_name;
            NodeList children;   /* NODE_UI_ELEMENT items */
            int      is_public;
        } ui_app;

        /* NODE_UI_ELEMENT — window, button, label, input, row, column, canvas, panel, menu */
        struct {
            int      elem_kind;  /* UIElemKind */
            char    *text;       /* title/label text, may be NULL */
            NodeList properties; /* NODE_UI_PROPERTY items */
            NodeList children;   /* nested UI elements */
            NodeList handlers;   /* NODE_UI_HANDLER items */
        } ui_element;

        /* NODE_UI_HANDLER — onClick, onKey, onChange */
        struct {
            int   handler_kind; /* UIHandlerKind */
            Node *body;         /* NODE_BLOCK */
        } ui_handler;

        /* NODE_UI_PROPERTY — name: value */
        struct {
            char *name;
            Node *value; /* expression */
        } ui_property;

        /* NODE_KERNEL_APP */
        struct {
            char    *name;
            char    *package_name;
            NodeList fns;        /* NODE_FN_DECL items */
            int      is_public;
        } kernel_app;

        /* NODE_COMPTIME */
        struct {
            NodeList decls;  /* NODE_CONST items */
        } comptime_block;

    } as;
};

/* UI element kinds (stored in ui_element.elem_kind) */
typedef enum {
    UI_ELEM_WINDOW,
    UI_ELEM_BUTTON,
    UI_ELEM_LABEL,
    UI_ELEM_INPUT,
    UI_ELEM_ROW,
    UI_ELEM_COLUMN,
    UI_ELEM_CANVAS,
    UI_ELEM_PANEL,
    UI_ELEM_MENU,
} UIElemKind;

/* UI handler kinds (stored in ui_handler.handler_kind) */
typedef enum {
    UI_HANDLER_CLICK,
    UI_HANDLER_KEY,
    UI_HANDLER_CHANGE,
} UIHandlerKind;

#endif /* NIGHT_AST_H */
