#include "ast_dump.h"

#include <stdio.h>
#include <string.h>

static void dump_node(FILE *out, Node *node, int indent);

static void print_indent(FILE *out, int indent) {
    for (int i = 0; i < indent; i++)
        fputs("  ", out);
}

static void print_line(FILE *out, int indent, const char *text) {
    print_indent(out, indent);
    fprintf(out, "%s\n", text);
}

static void print_child_label(FILE *out, int indent, const char *label) {
    print_indent(out, indent);
    fprintf(out, "%s:\n", label);
}

static void print_index_label(FILE *out, int indent, const char *label, int index) {
    print_indent(out, indent);
    fprintf(out, "%s[%d]:\n", label, index);
}

static const char *bool_word(int value) {
    return value ? "true" : "false";
}

static void dump_node_list(FILE *out, const char *label, NodeList *list, int indent) {
    if (!list || list->count == 0)
        return;

    for (int i = 0; i < list->count; i++) {
        print_index_label(out, indent, label, i);
        dump_node(out, list->items[i], indent + 1);
    }
}

static void dump_type_array(FILE *out, Node *node, int indent) {
    if (node->as.type_array.length >= 0) {
        print_indent(out, indent);
        fprintf(out, "TypeArray(length=%d)\n", node->as.type_array.length);
    } else {
        print_line(out, indent, "TypeSlice");
    }
    print_child_label(out, indent + 1, "elem");
    dump_node(out, node->as.type_array.elem, indent + 2);
}

static void dump_call(FILE *out, Node *node, int indent) {
    print_line(out, indent, "Call");
    print_child_label(out, indent + 1, "callee");
    dump_node(out, node->as.call.callee, indent + 2);
    dump_node_list(out, "args", &node->as.call.args, indent + 1);
}

static void dump_match(FILE *out, Node *node, int indent) {
    print_line(out, indent, "Match");
    print_child_label(out, indent + 1, "subject");
    dump_node(out, node->as.match.subject, indent + 2);
    for (int i = 0; i < node->as.match.count; i++) {
        print_indent(out, indent + 1);
        fprintf(out, "arm[%d](pattern=%s):\n", i, node->as.match.patterns[i]);
        dump_node(out, node->as.match.values[i], indent + 2);
    }
}

static void dump_struct_lit(FILE *out, Node *node, int indent) {
    print_indent(out, indent);
    fprintf(out, "StructLit(type=%s)\n", node->as.struct_lit.type_name);
    for (int i = 0; i < node->as.struct_lit.count; i++) {
        print_indent(out, indent + 1);
        fprintf(out, "field[%d](name=%s):\n", i, node->as.struct_lit.field_names[i]);
        dump_node(out, node->as.struct_lit.field_values[i], indent + 2);
    }
}

static void dump_fn_like(FILE *out, const char *kind, const char *name, const char *owner,
                         NodeList *params, Node *ret_type, Node *body, int indent) {
    print_indent(out, indent);
    if (owner)
        fprintf(out, "%s(name=%s, owner=%s)\n", kind, name, owner);
    else
        fprintf(out, "%s(name=%s)\n", kind, name);

    dump_node_list(out, "params", params, indent + 1);
    if (ret_type) {
        print_child_label(out, indent + 1, "ret");
        dump_node(out, ret_type, indent + 2);
    }
    if (body) {
        print_child_label(out, indent + 1, "body");
        dump_node(out, body, indent + 2);
    }
}

static void dump_node(FILE *out, Node *node, int indent) {
    if (!node) {
        print_line(out, indent, "<null>");
        return;
    }

    switch (node->kind) {
        case NODE_PROGRAM:
            print_line(out, indent, "Program");
            if (node->as.program.package) {
                print_child_label(out, indent + 1, "package");
                dump_node(out, node->as.program.package, indent + 2);
            }
            dump_node_list(out, "imports", &node->as.program.imports, indent + 1);
            dump_node_list(out, "decls", &node->as.program.decls, indent + 1);
            return;
        case NODE_PACKAGE:
            print_indent(out, indent);
            fprintf(out, "Package(path=%s)\n", node->as.pkg.path);
            return;
        case NODE_IMPORT:
            print_indent(out, indent);
            fprintf(out, "Import(path=%s)\n", node->as.pkg.path);
            return;
        case NODE_TYPE_NAMED:
            print_indent(out, indent);
            fprintf(out, "TypeNamed(name=%s)\n", node->as.type_named.name);
            return;
        case NODE_TYPE_POINTER:
            print_indent(out, indent);
            fprintf(out, "TypePointer(const=%s, nullable=%s)\n",
                    bool_word(node->as.type_ptr.is_const),
                    bool_word(node->as.type_ptr.is_nullable));
            print_child_label(out, indent + 1, "inner");
            dump_node(out, node->as.type_ptr.inner, indent + 2);
            return;
        case NODE_TYPE_ARRAY:
            dump_type_array(out, node, indent);
            return;
        case NODE_LIT_INT:
            print_indent(out, indent);
            fprintf(out, "IntLiteral(value=%lld)\n", node->as.lit_int.value);
            return;
        case NODE_LIT_FLOAT:
            print_indent(out, indent);
            fprintf(out, "FloatLiteral(value=%g)\n", node->as.lit_float.value);
            return;
        case NODE_LIT_STRING:
            print_indent(out, indent);
            fprintf(out, "StringLiteral(value=\"%s\")\n", node->as.lit_str.value);
            return;
        case NODE_LIT_BOOL:
            print_indent(out, indent);
            fprintf(out, "BoolLiteral(value=%s)\n", bool_word((int)node->as.lit_int.value));
            return;
        case NODE_LIT_NULL:
            print_line(out, indent, "NullLiteral");
            return;
        case NODE_IDENT:
            print_indent(out, indent);
            fprintf(out, "Identifier(name=%s)\n", node->as.ident.name);
            return;
        case NODE_BINARY:
            print_indent(out, indent);
            fprintf(out, "Binary(op=%s)\n", node->as.binary.op);
            print_child_label(out, indent + 1, "left");
            dump_node(out, node->as.binary.left, indent + 2);
            print_child_label(out, indent + 1, "right");
            dump_node(out, node->as.binary.right, indent + 2);
            return;
        case NODE_UNARY:
            print_indent(out, indent);
            fprintf(out, "Unary(op=%s)\n", node->as.unary.op);
            print_child_label(out, indent + 1, "operand");
            dump_node(out, node->as.unary.operand, indent + 2);
            return;
        case NODE_CALL:
            dump_call(out, node, indent);
            return;
        case NODE_FIELD:
            print_indent(out, indent);
            fprintf(out, "Field(name=%s)\n", node->as.field.field);
            print_child_label(out, indent + 1, "object");
            dump_node(out, node->as.field.object, indent + 2);
            return;
        case NODE_CAST:
            print_line(out, indent, "Cast");
            print_child_label(out, indent + 1, "expr");
            dump_node(out, node->as.cast.expr, indent + 2);
            print_child_label(out, indent + 1, "type");
            dump_node(out, node->as.cast.type, indent + 2);
            return;
        case NODE_ASSIGN:
            if (node->as.assign.op) {
                print_indent(out, indent);
                fprintf(out, "CompoundAssign(op=%s=)\n", node->as.assign.op);
            } else {
                print_line(out, indent, "Assign");
            }
            print_child_label(out, indent + 1, "target");
            dump_node(out, node->as.assign.target, indent + 2);
            print_child_label(out, indent + 1, "value");
            dump_node(out, node->as.assign.value, indent + 2);
            return;
        case NODE_INDEX:
            print_line(out, indent, "Index");
            print_child_label(out, indent + 1, "object");
            dump_node(out, node->as.index_expr.object, indent + 2);
            print_child_label(out, indent + 1, "index");
            dump_node(out, node->as.index_expr.index, indent + 2);
            return;
        case NODE_STRUCT_LIT:
            dump_struct_lit(out, node, indent);
            return;
        case NODE_MATCH:
            dump_match(out, node, indent);
            return;
        case NODE_GROUP:
            print_line(out, indent, "Group");
            print_child_label(out, indent + 1, "expr");
            dump_node(out, node->as.group.expr, indent + 2);
            return;
        case NODE_BLOCK:
            print_line(out, indent, "Block");
            dump_node_list(out, "stmts", &node->as.block.stmts, indent + 1);
            return;
        case NODE_LET:
            print_indent(out, indent);
            fprintf(out, "Let(name=%s)\n", node->as.let.name);
            if (node->as.let.type) {
                print_child_label(out, indent + 1, "type");
                dump_node(out, node->as.let.type, indent + 2);
            }
            if (node->as.let.value) {
                print_child_label(out, indent + 1, "value");
                dump_node(out, node->as.let.value, indent + 2);
            }
            return;
        case NODE_CONST:
            print_indent(out, indent);
            fprintf(out, "Const(name=%s)\n", node->as.konst.name);
            if (node->as.konst.type) {
                print_child_label(out, indent + 1, "type");
                dump_node(out, node->as.konst.type, indent + 2);
            }
            print_child_label(out, indent + 1, "value");
            dump_node(out, node->as.konst.value, indent + 2);
            return;
        case NODE_RETURN:
            print_line(out, indent, "Return");
            if (node->as.ret.value) {
                print_child_label(out, indent + 1, "value");
                dump_node(out, node->as.ret.value, indent + 2);
            }
            return;
        case NODE_IF:
            print_line(out, indent, "If");
            print_child_label(out, indent + 1, "cond");
            dump_node(out, node->as.if_stmt.cond, indent + 2);
            print_child_label(out, indent + 1, "then");
            dump_node(out, node->as.if_stmt.then_block, indent + 2);
            if (node->as.if_stmt.else_node) {
                print_child_label(out, indent + 1, "else");
                dump_node(out, node->as.if_stmt.else_node, indent + 2);
            }
            return;
        case NODE_WHILE:
            print_line(out, indent, "While");
            print_child_label(out, indent + 1, "cond");
            dump_node(out, node->as.while_stmt.cond, indent + 2);
            print_child_label(out, indent + 1, "body");
            dump_node(out, node->as.while_stmt.body, indent + 2);
            return;
        case NODE_FOR:
            print_line(out, indent, "For");
            if (node->as.for_stmt.init) {
                print_child_label(out, indent + 1, "init");
                dump_node(out, node->as.for_stmt.init, indent + 2);
            }
            if (node->as.for_stmt.cond) {
                print_child_label(out, indent + 1, "cond");
                dump_node(out, node->as.for_stmt.cond, indent + 2);
            }
            if (node->as.for_stmt.post) {
                print_child_label(out, indent + 1, "post");
                dump_node(out, node->as.for_stmt.post, indent + 2);
            }
            print_child_label(out, indent + 1, "body");
            dump_node(out, node->as.for_stmt.body, indent + 2);
            return;
        case NODE_LOOP:
            print_line(out, indent, "Loop");
            print_child_label(out, indent + 1, "body");
            dump_node(out, node->as.loop_stmt.body, indent + 2);
            return;
        case NODE_UNSAFE:
            print_line(out, indent, "Unsafe");
            print_child_label(out, indent + 1, "body");
            dump_node(out, node->as.loop_stmt.body, indent + 2);
            return;
        case NODE_BREAK:
            print_line(out, indent, "Break");
            return;
        case NODE_CONTINUE:
            print_line(out, indent, "Continue");
            return;
        case NODE_DEFER:
            print_line(out, indent, "Defer");
            print_child_label(out, indent + 1, "expr");
            dump_node(out, node->as.defer_stmt.expr, indent + 2);
            return;
        case NODE_EXPR_STMT:
            print_line(out, indent, "ExprStmt");
            print_child_label(out, indent + 1, "expr");
            dump_node(out, node->as.expr_stmt.expr, indent + 2);
            return;
        case NODE_FN_DECL:
            dump_fn_like(out, "FnDecl", node->as.fn.name, node->as.fn.owner_type,
                         &node->as.fn.params, node->as.fn.ret_type, node->as.fn.body, indent);
            return;
        case NODE_EXTERN_FN:
            print_indent(out, indent);
            fprintf(out, "ExternFn(name=%s, abi=%s)\n",
                    node->as.extern_fn.name,
                    node->as.extern_fn.calling_conv ? node->as.extern_fn.calling_conv : "default");
            dump_node_list(out, "params", &node->as.extern_fn.params, indent + 1);
            if (node->as.extern_fn.ret_type) {
                print_child_label(out, indent + 1, "ret");
                dump_node(out, node->as.extern_fn.ret_type, indent + 2);
            }
            return;
        case NODE_STRUCT_DECL:
            print_indent(out, indent);
            fprintf(out, "StructDecl(name=%s, packed=%s)\n",
                    node->as.struct_decl.name,
                    bool_word(node->as.struct_decl.is_packed));
            dump_node_list(out, "fields", &node->as.struct_decl.fields, indent + 1);
            return;
        case NODE_ENUM_DECL:
            print_indent(out, indent);
            fprintf(out, "EnumDecl(name=%s)\n", node->as.enum_decl.name);
            for (int i = 0; i < node->as.enum_decl.count; i++) {
                print_indent(out, indent + 1);
                fprintf(out, "variant[%d]=%s\n", i, node->as.enum_decl.variants[i]);
            }
            return;
        case NODE_UNION_DECL:
            print_indent(out, indent);
            fprintf(out, "UnionDecl(name=%s)\n", node->as.union_decl.name);
            dump_node_list(out, "fields", &node->as.union_decl.fields, indent + 1);
            return;
        case NODE_IMPL_DECL:
            print_indent(out, indent);
            if (node->as.impl.interface_name)
                fprintf(out, "ImplDecl(target=%s, interface=%s)\n",
                        node->as.impl.target, node->as.impl.interface_name);
            else
                fprintf(out, "ImplDecl(target=%s)\n", node->as.impl.target);
            dump_node_list(out, "methods", &node->as.impl.methods, indent + 1);
            return;
        case NODE_INTERFACE_DECL:
            print_indent(out, indent);
            fprintf(out, "InterfaceDecl(name=%s%s)\n",
                    node->as.interface_decl.name,
                    node->as.interface_decl.is_public ? ", pub" : "");
            dump_node_list(out, "methods", &node->as.interface_decl.methods, indent + 1);
            return;
        default:
            print_line(out, indent, "<unknown node>");
            return;
    }
}

void ast_dump_node(FILE *out, Node *node) {
    dump_node(out, node, 0);
}
