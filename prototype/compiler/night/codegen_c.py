from __future__ import annotations

import json
from dataclasses import dataclass

from . import ast, typesys
from .check import FullCheckResult


@dataclass(frozen=True, slots=True)
class COutput:
    source: str


class CGenerator:
    def __init__(self, check_result: FullCheckResult) -> None:
        self.check_result = check_result

    def generate(self) -> COutput:
        lines = [
            "#include <stdbool.h>",
            "#include <stddef.h>",
            "#include <stdint.h>",
            "",
        ]
        lines.extend(self._emit_struct_definitions())
        if lines[-1] != "":
            lines.append("")
        lines.extend(self._emit_prototypes())
        lines.append("")
        lines.extend(self._emit_definitions())
        return COutput(source="\n".join(lines).rstrip() + "\n")

    def _emit_struct_definitions(self) -> list[str]:
        lines: list[str] = []
        for decl in self.check_result.program.declarations:
            if isinstance(decl, ast.EnumDecl):
                lines.append(f"typedef enum {decl.name} {{")
                enum_type = self.check_result.semantic_model.enums[decl.name].type_ref
                for variant in enum_type.variants:
                    lines.append(f"    {decl.name}_{variant.name} = {variant.value},")
                lines.append(f"}} {decl.name};")
                lines.append("")
                continue
            if isinstance(decl, ast.UnionDecl):
                lines.append(f"typedef union {decl.name} {{")
                union_type = self.check_result.semantic_model.unions[decl.name].type_ref
                for field in union_type.fields:
                    field_type = self._c_type(field.type_ref)
                    lines.append(f"    {field_type} {field.name};")
                lines.append(f"}} {decl.name};")
                lines.append("")
                continue
            if not isinstance(decl, ast.StructDecl):
                continue
            lines.append(f"typedef struct {decl.name} {{")
            struct_type = self.check_result.semantic_model.structs[decl.name].type_ref
            for field in struct_type.fields:
                field_type = self._c_type(field.type_ref)
                lines.append(f"    {field_type} {field.name};")
            lines.append(f"}} {decl.name};")
            lines.append("")
        if lines:
            lines.pop()
        return lines

    def _emit_prototypes(self) -> list[str]:
        lines: list[str] = []
        for decl in self.check_result.program.declarations:
            if isinstance(decl, ast.ExternFunctionDecl):
                lines.append(self._format_function_signature(decl) + ";")
        for decl in self.check_result.program.declarations:
            if isinstance(decl, ast.FunctionDecl):
                lines.append(self._format_function_signature(decl) + ";")
            elif isinstance(decl, ast.ImplDecl):
                for method in decl.methods:
                    lines.append(self._format_function_signature(method) + ";")
        return lines

    def _emit_definitions(self) -> list[str]:
        lines: list[str] = []
        for decl in self.check_result.program.declarations:
            if isinstance(decl, ast.FunctionDecl):
                lines.extend(self._emit_function_definition(decl))
            elif isinstance(decl, ast.ImplDecl):
                for method in decl.methods:
                    lines.extend(self._emit_function_definition(method))
        if lines:
            lines.pop()
        return lines

    def _emit_function_definition(self, decl: ast.FunctionDecl) -> list[str]:
        lines = [self._format_function_signature(decl), "{"]
        for stmt in decl.body.statements:
            lines.extend(self._indent(self._emit_stmt(stmt)))
        lines.append("}")
        lines.append("")
        return lines

    def _emit_stmt(self, stmt: ast.Stmt) -> list[str]:
        if isinstance(stmt, ast.ConstStmt):
            if isinstance(stmt.value, ast.MatchExpr):
                raise NotImplementedError("const initialization from match is not implemented yet")
            c_type = self._c_type(self.check_result.type_result.binding_types[id(stmt)])
            return [f"const {c_type} {stmt.name} = {self._emit_expr(stmt.value)};"]
        if isinstance(stmt, ast.LetStmt):
            c_type = self._c_type(self.check_result.type_result.binding_types[id(stmt)])
            if stmt.value is None:
                return [f"{c_type} {stmt.name};"]
            if isinstance(stmt.value, ast.MatchExpr):
                lines = [f"{c_type} {stmt.name};"]
                lines.extend(self._emit_match_assignment(stmt.name, stmt.value))
                return lines
            return [f"{c_type} {stmt.name} = {self._emit_expr(stmt.value)};"]
        if isinstance(stmt, ast.IfStmt):
            return self._emit_if_chain(stmt)
        if isinstance(stmt, ast.WhileStmt):
            lines = [f"while ({self._emit_condition_expr(stmt.condition)})", "{"]
            lines.extend(self._indent(self._emit_block(stmt.body)))
            lines.append("}")
            return lines
        if isinstance(stmt, ast.LoopStmt):
            lines = ["for (;;)", "{"]
            lines.extend(self._indent(self._emit_block(stmt.body)))
            lines.append("}")
            return lines
        if isinstance(stmt, ast.UnsafeStmt):
            lines = ["{"]
            lines.extend(self._indent(self._emit_block(stmt.body)))
            lines.append("}")
            return lines
        if isinstance(stmt, ast.BreakStmt):
            return ["break;"]
        if isinstance(stmt, ast.ContinueStmt):
            return ["continue;"]
        if isinstance(stmt, ast.ReturnStmt):
            if stmt.value is None:
                return ["return;"]
            if isinstance(stmt.value, ast.MatchExpr):
                return self._emit_match_return(stmt.value)
            return [f"return {self._emit_expr(stmt.value)};"]
        if isinstance(stmt, ast.ExprStmt):
            if isinstance(stmt.expr, ast.MatchExpr):
                return self._emit_match_expr_stmt(stmt.expr)
            return [f"{self._emit_expr(stmt.expr)};"]
        raise NotImplementedError(f"unsupported statement: {type(stmt).__name__}")

    def _emit_block(self, block: ast.BlockStmt) -> list[str]:
        lines: list[str] = []
        for stmt in block.statements:
            lines.extend(self._emit_stmt(stmt))
        return lines

    def _emit_if_chain(self, stmt: ast.IfStmt) -> list[str]:
        lines = [f"if ({self._emit_condition_expr(stmt.condition)})", "{"]
        lines.extend(self._indent(self._emit_block(stmt.then_branch)))
        lines.append("}")
        if stmt.else_branch is not None:
            if isinstance(stmt.else_branch, ast.BlockStmt):
                lines.append("else")
                lines.append("{")
                lines.extend(self._indent(self._emit_block(stmt.else_branch)))
                lines.append("}")
            else:
                nested = self._emit_if_chain(stmt.else_branch)
                first, *rest = nested
                lines.append(f"else {first}")
                lines.extend(rest)
        return lines

    def _emit_condition_expr(self, expr: ast.Expr) -> str:
        rendered = self._emit_expr(expr)
        if rendered.startswith("(") and rendered.endswith(")"):
            return rendered[1:-1]
        return rendered

    def _emit_expr(self, expr: ast.Expr) -> str:
        if isinstance(expr, ast.IdentifierExpr):
            return expr.name
        if isinstance(expr, ast.LiteralExpr):
            if expr.value is None:
                return "NULL"
            if isinstance(expr.value, bool):
                return "true" if expr.value else "false"
            if isinstance(expr.value, int):
                return str(expr.value)
            if isinstance(expr.value, float):
                return repr(expr.value)
            if isinstance(expr.value, str):
                return json.dumps(expr.value)
            raise NotImplementedError(f"unsupported literal value: {expr.value!r}")
        if isinstance(expr, ast.GroupExpr):
            return f"({self._emit_expr(expr.expr)})"
        if isinstance(expr, ast.UnaryExpr):
            return f"({expr.operator}{self._emit_expr(expr.operand)})"
        if isinstance(expr, ast.BinaryExpr):
            return f"({self._emit_expr(expr.left)} {expr.operator} {self._emit_expr(expr.right)})"
        if isinstance(expr, ast.AssignExpr):
            return f"({self._emit_expr(expr.target)} = {self._emit_expr(expr.value)})"
        if isinstance(expr, ast.CallExpr):
            resolution = self.check_result.type_result.call_resolutions.get(id(expr))
            if resolution is not None and isinstance(expr.callee, ast.FieldAccessExpr):
                arguments: list[str] = []
                if resolution.receiver_strategy == "address_of":
                    arguments.append(f"&({self._emit_expr(expr.callee.object)})")
                elif resolution.receiver_strategy == "direct":
                    arguments.append(self._emit_expr(expr.callee.object))
                extra_arguments = expr.arguments
                arguments.extend(self._emit_expr(argument) for argument in extra_arguments)
                return f"{resolution.lowered_name}({', '.join(arguments)})"
            arguments = ", ".join(self._emit_expr(argument) for argument in expr.arguments)
            return f"{self._emit_expr(expr.callee)}({arguments})"
        if isinstance(expr, ast.CastExpr):
            return f"(({self._c_type(self._type_of_expr(expr))}) {self._emit_expr(expr.expr)})"
        if isinstance(expr, ast.FieldAccessExpr):
            if isinstance(expr.object, ast.IdentifierExpr) and expr.object.name in self.check_result.semantic_model.enums:
                return f"{expr.object.name}_{expr.field}"
            object_rendered = self._emit_expr(expr.object)
            object_type = self._type_of_expr(expr.object)
            if isinstance(object_type, typesys.PointerType) and isinstance(object_type.inner, typesys.StructType):
                return f"{object_rendered}->{expr.field}"
            return f"{object_rendered}.{expr.field}"
        if isinstance(expr, ast.StructLiteralExpr):
            fields = ", ".join(f".{field.name} = {self._emit_expr(field.value)}" for field in expr.fields)
            return f"({expr.type_name}){{ {fields} }}"
        if isinstance(expr, ast.MatchExpr):
            raise NotImplementedError("match expression must be lowered at statement level")
        raise NotImplementedError(f"unsupported expression: {type(expr).__name__}")

    def _format_function_signature(self, decl: ast.FunctionDecl | ast.ExternFunctionDecl) -> str:
        signature = self._signature_for_decl(decl)
        return_type = self._c_type(signature.return_type)
        params: list[str] = []
        for param, param_type in zip(decl.params, signature.param_types, strict=True):
            params.append(f"{self._c_type(param_type)} {param.name}")
        if not params:
            params_text = "void"
        else:
            params_text = ", ".join(params)
        return f"{return_type} {self._c_name_for_decl(decl)}({params_text})"

    def _type_of_expr(self, expr: ast.Expr) -> typesys.Type:
        return self.check_result.type_result.expression_types[id(expr)]

    def _signature_for_decl(self, decl: ast.FunctionDecl | ast.ExternFunctionDecl) -> typesys.FunctionType:
        if isinstance(decl, ast.FunctionDecl) and decl.owner_type is not None:
            return self.check_result.semantic_model.methods[(decl.owner_type, decl.name)].type_ref
        return self.check_result.type_result.functions[decl.name]

    def _c_name_for_decl(self, decl: ast.FunctionDecl | ast.ExternFunctionDecl) -> str:
        if isinstance(decl, ast.FunctionDecl) and decl.owner_type is not None:
            return self.check_result.semantic_model.methods[(decl.owner_type, decl.name)].c_name
        return decl.name

    def _c_type(self, type_ref: typesys.Type) -> str:
        if type_ref == typesys.I8:
            return "int8_t"
        if type_ref == typesys.I16:
            return "int16_t"
        if type_ref == typesys.I32:
            return "int32_t"
        if type_ref == typesys.I64:
            return "int64_t"
        if type_ref == typesys.ISIZE:
            return "ptrdiff_t"
        if type_ref == typesys.U8:
            return "uint8_t"
        if type_ref == typesys.U16:
            return "uint16_t"
        if type_ref == typesys.U32:
            return "uint32_t"
        if type_ref == typesys.U64:
            return "uint64_t"
        if type_ref == typesys.USIZE:
            return "size_t"
        if type_ref == typesys.F32:
            return "float"
        if type_ref == typesys.F64:
            return "double"
        if type_ref == typesys.BOOL:
            return "bool"
        if type_ref == typesys.CHAR:
            return "char"
        if type_ref == typesys.VOID:
            return "void"
        if type_ref in {typesys.STR, typesys.CSTR}:
            return "const char*"
        if isinstance(type_ref, typesys.PointerType):
            inner = self._c_type(type_ref.inner)
            if type_ref.is_const and not inner.startswith("const "):
                inner = f"const {inner}"
            return f"{inner}*"
        if isinstance(type_ref, typesys.ArrayType):
            inner = self._c_type(type_ref.element_type)
            if type_ref.length is None:
                return f"{inner}*"
            return f"{inner}[{type_ref.length}]"
        if isinstance(type_ref, typesys.FunctionType):
            raise NotImplementedError("function pointer types are not implemented")
        if isinstance(type_ref, typesys.StructType):
            return type_ref.name
        if isinstance(type_ref, typesys.UnionType):
            return type_ref.name
        if isinstance(type_ref, typesys.EnumType):
            return type_ref.name
        raise NotImplementedError(f"unsupported type: {typesys.describe(type_ref)}")

    def _indent(self, lines: list[str]) -> list[str]:
        return [f"    {line}" for line in lines]

    def _emit_match_assignment(self, target_name: str, expr: ast.MatchExpr) -> list[str]:
        resolution = self.check_result.type_result.match_resolutions[id(expr)]
        lines = [f"switch ({self._emit_expr(expr.subject)})", "{"]
        for variant_name, arm_expr in resolution.cases:
            lines.append(f"case {resolution.enum_name}_{variant_name}:")
            lines.extend(self._indent([f"{target_name} = {self._emit_expr(arm_expr)};", "break;"]))
        if resolution.default_expr is not None:
            lines.append("default:")
            lines.extend(self._indent([f"{target_name} = {self._emit_expr(resolution.default_expr)};", "break;"]))
        lines.append("}")
        return lines

    def _emit_match_return(self, expr: ast.MatchExpr) -> list[str]:
        resolution = self.check_result.type_result.match_resolutions[id(expr)]
        lines = [f"switch ({self._emit_expr(expr.subject)})", "{"]
        for variant_name, arm_expr in resolution.cases:
            lines.append(f"case {resolution.enum_name}_{variant_name}:")
            lines.extend(self._indent([f"return {self._emit_expr(arm_expr)};"]))
        if resolution.default_expr is not None:
            lines.append("default:")
            lines.extend(self._indent([f"return {self._emit_expr(resolution.default_expr)};"]))
        lines.append("}")
        return lines

    def _emit_match_expr_stmt(self, expr: ast.MatchExpr) -> list[str]:
        resolution = self.check_result.type_result.match_resolutions[id(expr)]
        lines = [f"switch ({self._emit_expr(expr.subject)})", "{"]
        for variant_name, arm_expr in resolution.cases:
            lines.append(f"case {resolution.enum_name}_{variant_name}:")
            lines.extend(self._indent([f"{self._emit_expr(arm_expr)};", "break;"]))
        if resolution.default_expr is not None:
            lines.append("default:")
            lines.extend(self._indent([f"{self._emit_expr(resolution.default_expr)};", "break;"]))
        lines.append("}")
        return lines


def generate_c(check_result: FullCheckResult) -> COutput:
    return CGenerator(check_result).generate()
