from __future__ import annotations

from dataclasses import dataclass

from . import ast, typesys
from .errors import TypeCheckError
from .sema import SemanticModel


@dataclass(frozen=True, slots=True)
class Binding:
    type_ref: typesys.Type
    mutable: bool


class Scope:
    def __init__(self, parent: Scope | None = None) -> None:
        self.parent = parent
        self.symbols: dict[str, Binding] = {}

    def define(self, name: str, type_ref: typesys.Type, *, mutable: bool) -> None:
        self.symbols[name] = Binding(type_ref=type_ref, mutable=mutable)

    def resolve(self, name: str) -> Binding | None:
        scope: Scope | None = self
        while scope is not None:
            if name in scope.symbols:
                return scope.symbols[name]
            scope = scope.parent
        return None


@dataclass(frozen=True, slots=True)
class CheckResult:
    functions: dict[str, typesys.FunctionType]
    expression_types: dict[int, typesys.Type]
    binding_types: dict[int, typesys.Type]


class TypeChecker:
    def __init__(self, semantic_model: SemanticModel, source_name: str = "<memory>") -> None:
        self.semantic_model = semantic_model
        self.source_name = source_name
        self.expression_types: dict[int, typesys.Type] = {}
        self.binding_types: dict[int, typesys.Type] = {}

    def check(self, program: ast.Program) -> CheckResult:
        for decl in program.declarations:
            if isinstance(decl, ast.FunctionDecl):
                self._check_function(decl)
        return CheckResult(
            functions={name: symbol.type_ref for name, symbol in self.semantic_model.functions.items()},
            expression_types=dict(self.expression_types),
            binding_types=dict(self.binding_types),
        )

    def _check_function(self, decl: ast.FunctionDecl) -> None:
        signature = self.semantic_model.functions[decl.name].type_ref
        scope = Scope()
        for name, symbol in self.semantic_model.functions.items():
            scope.define(name, symbol.type_ref, mutable=False)
        for param, param_type in zip(decl.params, signature.param_types, strict=True):
            scope.define(param.name, param_type, mutable=True)

        has_return = False
        for stmt in decl.body.statements:
            stmt_return = self._check_stmt(stmt, scope, signature.return_type)
            has_return = has_return or stmt_return

        if signature.return_type != typesys.VOID and not has_return:
            raise self._error(decl.line, decl.column, f"function '{decl.name}' must return {typesys.describe(signature.return_type)}")

    def _check_stmt(self, stmt: ast.Stmt, scope: Scope, expected_return_type: typesys.Type) -> bool:
        if isinstance(stmt, ast.ConstStmt):
            value_type = self._check_expr(stmt.value, scope)
            declared_type = self._resolve_type_ref(stmt.type_ref) if stmt.type_ref is not None else None
            if declared_type is not None and not typesys.is_assignable(declared_type, value_type):
                raise self._error(
                    stmt.line,
                    stmt.column,
                    f"cannot assign {typesys.describe(value_type)} to {typesys.describe(declared_type)}",
                )
            final_type = declared_type or value_type
            self.binding_types[id(stmt)] = final_type
            scope.define(stmt.name, final_type, mutable=False)
            return False

        if isinstance(stmt, ast.LetStmt):
            value_type = None
            if stmt.value is not None:
                value_type = self._check_expr(stmt.value, scope)
            declared_type = self._resolve_type_ref(stmt.type_ref) if stmt.type_ref is not None else None
            if declared_type is None and value_type is None:
                raise self._error(stmt.line, stmt.column, "let statement needs a type annotation or initializer")
            if declared_type is not None and value_type is not None and not typesys.is_assignable(declared_type, value_type):
                raise self._error(
                    stmt.line,
                    stmt.column,
                    f"cannot assign {typesys.describe(value_type)} to {typesys.describe(declared_type)}",
                )
            final_type = declared_type or value_type
            self.binding_types[id(stmt)] = final_type
            scope.define(stmt.name, final_type, mutable=True)
            return False

        if isinstance(stmt, ast.IfStmt):
            condition_type = self._check_expr(stmt.condition, scope)
            if not typesys.is_assignable(typesys.BOOL, condition_type):
                raise self._error(
                    stmt.line,
                    stmt.column,
                    f"if condition must be bool, got {typesys.describe(condition_type)}",
                )
            then_returns = self._check_block(stmt.then_branch, Scope(parent=scope), expected_return_type)
            else_returns = False
            if stmt.else_branch is not None:
                if isinstance(stmt.else_branch, ast.BlockStmt):
                    else_returns = self._check_block(stmt.else_branch, Scope(parent=scope), expected_return_type)
                else:
                    else_returns = self._check_stmt(stmt.else_branch, Scope(parent=scope), expected_return_type)
            return then_returns and else_returns

        if isinstance(stmt, ast.WhileStmt):
            condition_type = self._check_expr(stmt.condition, scope)
            if not typesys.is_assignable(typesys.BOOL, condition_type):
                raise self._error(
                    stmt.line,
                    stmt.column,
                    f"while condition must be bool, got {typesys.describe(condition_type)}",
                )
            self._check_block(stmt.body, Scope(parent=scope), expected_return_type)
            return False

        if isinstance(stmt, ast.LoopStmt):
            self._check_block(stmt.body, Scope(parent=scope), expected_return_type)
            return False

        if isinstance(stmt, ast.BreakStmt | ast.ContinueStmt):
            return False

        if isinstance(stmt, ast.ReturnStmt):
            actual_type = typesys.VOID if stmt.value is None else self._check_expr(stmt.value, scope)
            if not typesys.is_assignable(expected_return_type, actual_type):
                raise self._error(
                    stmt.line,
                    stmt.column,
                    f"return type mismatch: expected {typesys.describe(expected_return_type)}, got {typesys.describe(actual_type)}",
                )
            return True

        if isinstance(stmt, ast.ExprStmt):
            self._check_expr(stmt.expr, scope)
            return False

        raise self._error(getattr(stmt, "line", 0), getattr(stmt, "column", 0), f"unsupported statement: {type(stmt).__name__}")

    def _check_block(self, block: ast.BlockStmt, scope: Scope, expected_return_type: typesys.Type) -> bool:
        definitely_returns = False
        for stmt in block.statements:
            stmt_returns = self._check_stmt(stmt, scope, expected_return_type)
            definitely_returns = definitely_returns or stmt_returns
            if stmt_returns:
                break
        return definitely_returns

    def _check_expr(self, expr: ast.Expr, scope: Scope) -> typesys.Type:
        if isinstance(expr, ast.IdentifierExpr):
            resolved = scope.resolve(expr.name)
            if resolved is None:
                raise self._error(expr.line, expr.column, f"unknown symbol '{expr.name}'")
            self.expression_types[id(expr)] = resolved.type_ref
            return resolved.type_ref

        if isinstance(expr, ast.LiteralExpr):
            if expr.value is None:
                result = typesys.NULL
                self.expression_types[id(expr)] = result
                return result
            if isinstance(expr.value, bool):
                self.expression_types[id(expr)] = typesys.BOOL
                return typesys.BOOL
            if isinstance(expr.value, int):
                self.expression_types[id(expr)] = typesys.INT_LITERAL
                return typesys.INT_LITERAL
            if isinstance(expr.value, float):
                self.expression_types[id(expr)] = typesys.FLOAT_LITERAL
                return typesys.FLOAT_LITERAL
            if isinstance(expr.value, str):
                self.expression_types[id(expr)] = typesys.STRING_LITERAL
                return typesys.STRING_LITERAL
            raise self._error(expr.line, expr.column, f"unsupported literal type: {type(expr.value).__name__}")

        if isinstance(expr, ast.GroupExpr):
            result = self._check_expr(expr.expr, scope)
            self.expression_types[id(expr)] = result
            return result

        if isinstance(expr, ast.UnaryExpr):
            operand_type = self._check_expr(expr.operand, scope)
            if expr.operator == "!":
                if not typesys.is_assignable(typesys.BOOL, operand_type):
                    raise self._error(expr.line, expr.column, f"operator ! requires bool, got {typesys.describe(operand_type)}")
                self.expression_types[id(expr)] = typesys.BOOL
                return typesys.BOOL
            if expr.operator == "-":
                if not typesys.is_numeric(operand_type):
                    raise self._error(expr.line, expr.column, f"operator - requires numeric type, got {typesys.describe(operand_type)}")
                self.expression_types[id(expr)] = operand_type
                return operand_type
            if expr.operator == "&":
                result = typesys.PointerType(inner=operand_type)
                self.expression_types[id(expr)] = result
                return result
            if expr.operator == "*":
                if not isinstance(operand_type, typesys.PointerType):
                    raise self._error(expr.line, expr.column, f"operator * requires pointer type, got {typesys.describe(operand_type)}")
                self.expression_types[id(expr)] = operand_type.inner
                return operand_type.inner
            raise self._error(expr.line, expr.column, f"unsupported unary operator '{expr.operator}'")

        if isinstance(expr, ast.BinaryExpr):
            left_type = self._check_expr(expr.left, scope)
            right_type = self._check_expr(expr.right, scope)
            if expr.operator in {"+", "-", "*", "/", "%"}:
                if not typesys.is_numeric(left_type) or not typesys.is_numeric(right_type):
                    raise self._error(
                        expr.line,
                        expr.column,
                        f"operator {expr.operator} requires numeric operands, got {typesys.describe(left_type)} and {typesys.describe(right_type)}",
                    )
                if left_type == typesys.FLOAT_LITERAL or right_type == typesys.FLOAT_LITERAL:
                    self.expression_types[id(expr)] = typesys.FLOAT_LITERAL
                    return typesys.FLOAT_LITERAL
                if left_type in typesys.FLOAT_TYPES or right_type in typesys.FLOAT_TYPES:
                    self.expression_types[id(expr)] = typesys.F64
                    return typesys.F64
                result = left_type if left_type != typesys.INT_LITERAL else right_type
                self.expression_types[id(expr)] = result
                return result
            if expr.operator in {"==", "!="}:
                if not (typesys.is_assignable(left_type, right_type) or typesys.is_assignable(right_type, left_type)):
                    raise self._error(
                        expr.line,
                        expr.column,
                        f"cannot compare {typesys.describe(left_type)} and {typesys.describe(right_type)}",
                    )
                self.expression_types[id(expr)] = typesys.BOOL
                return typesys.BOOL
            if expr.operator in {"<", ">", "<=", ">="}:
                if not typesys.is_numeric(left_type) or not typesys.is_numeric(right_type):
                    raise self._error(
                        expr.line,
                        expr.column,
                        f"comparison requires numeric operands, got {typesys.describe(left_type)} and {typesys.describe(right_type)}",
                    )
                self.expression_types[id(expr)] = typesys.BOOL
                return typesys.BOOL
            if expr.operator in {"&&", "||"}:
                if not typesys.is_assignable(typesys.BOOL, left_type) or not typesys.is_assignable(typesys.BOOL, right_type):
                    raise self._error(
                        expr.line,
                        expr.column,
                        f"logical operator {expr.operator} requires bool operands, got {typesys.describe(left_type)} and {typesys.describe(right_type)}",
                    )
                self.expression_types[id(expr)] = typesys.BOOL
                return typesys.BOOL
            raise self._error(expr.line, expr.column, f"unsupported binary operator '{expr.operator}'")

        if isinstance(expr, ast.AssignExpr):
            if not self._is_assignable_target(expr.target):
                raise self._error(expr.line, expr.column, "left side of assignment is not assignable")
            self._ensure_mutable_target(expr.target, scope)
            target_type = self._check_expr(expr.target, scope)
            value_type = self._check_expr(expr.value, scope)
            if not typesys.is_assignable(target_type, value_type):
                raise self._error(
                    expr.line,
                    expr.column,
                    f"cannot assign {typesys.describe(value_type)} to {typesys.describe(target_type)}",
                )
            self.expression_types[id(expr)] = target_type
            return target_type

        if isinstance(expr, ast.CallExpr):
            callee_type = self._check_expr(expr.callee, scope)
            if not isinstance(callee_type, typesys.FunctionType):
                raise self._error(expr.line, expr.column, f"cannot call non-function value of type {typesys.describe(callee_type)}")
            if len(expr.arguments) != len(callee_type.param_types):
                raise self._error(
                    expr.line,
                    expr.column,
                    f"expected {len(callee_type.param_types)} arguments, got {len(expr.arguments)}",
                )
            for argument, expected_type in zip(expr.arguments, callee_type.param_types, strict=True):
                actual_type = self._check_expr(argument, scope)
                if not typesys.is_assignable(expected_type, actual_type):
                    raise self._error(
                        argument.line,
                        argument.column,
                        f"argument type mismatch: expected {typesys.describe(expected_type)}, got {typesys.describe(actual_type)}",
                    )
            self.expression_types[id(expr)] = callee_type.return_type
            return callee_type.return_type

        if isinstance(expr, ast.CastExpr):
            expr_type = self._check_expr(expr.expr, scope)
            target_type = self._resolve_type_ref(expr.type_ref)
            if not typesys.is_castable(expr_type, target_type):
                raise self._error(
                    expr.line,
                    expr.column,
                    f"cannot cast {typesys.describe(expr_type)} to {typesys.describe(target_type)}",
                )
            self.expression_types[id(expr)] = target_type
            return target_type

        if isinstance(expr, ast.FieldAccessExpr):
            object_type = self._check_expr(expr.object, scope)
            if not isinstance(object_type, typesys.StructType):
                raise self._error(
                    expr.line,
                    expr.column,
                    f"field access requires struct value, got {typesys.describe(object_type)}",
                )
            field_type = self._lookup_struct_field(object_type, expr.field, expr.line, expr.column)
            self.expression_types[id(expr)] = field_type
            return field_type

        if isinstance(expr, ast.StructLiteralExpr):
            if expr.type_name not in self.semantic_model.structs:
                raise self._error(expr.line, expr.column, f"unknown struct '{expr.type_name}'")
            struct_type = self.semantic_model.structs[expr.type_name].type_ref
            provided: set[str] = set()
            field_map = {field.name: field.type_ref for field in struct_type.fields}
            for field in expr.fields:
                if field.name not in field_map:
                    raise self._error(field.line, field.column, f"unknown field '{field.name}' for struct '{struct_type.name}'")
                if field.name in provided:
                    raise self._error(field.line, field.column, f"duplicate struct literal field '{field.name}'")
                provided.add(field.name)
                actual_type = self._check_expr(field.value, scope)
                expected_type = field_map[field.name]
                if not typesys.is_assignable(expected_type, actual_type):
                    raise self._error(
                        field.line,
                        field.column,
                        f"field '{field.name}' expects {typesys.describe(expected_type)}, got {typesys.describe(actual_type)}",
                    )
            missing = [field.name for field in struct_type.fields if field.name not in provided]
            if missing:
                raise self._error(
                    expr.line,
                    expr.column,
                    f"missing fields for struct '{struct_type.name}': {', '.join(missing)}",
                )
            self.expression_types[id(expr)] = struct_type
            return struct_type

        raise self._error(getattr(expr, "line", 0), getattr(expr, "column", 0), f"unsupported expression: {type(expr).__name__}")

    def _resolve_type_ref(self, type_ref: ast.TypeRef | None) -> typesys.Type | None:
        if type_ref is None:
            return None
        if isinstance(type_ref, ast.NamedType):
            if type_ref.path[0] in typesys.BUILTINS:
                return typesys.BUILTINS[type_ref.path[0]]
            return self.semantic_model.structs[type_ref.path[0]].type_ref
        if isinstance(type_ref, ast.PointerType):
            return typesys.PointerType(
                inner=self._resolve_type_ref(type_ref.inner),
                is_const=type_ref.is_const,
                is_nullable=type_ref.is_nullable,
            )
        if isinstance(type_ref, ast.ArrayType):
            return typesys.ArrayType(
                element_type=self._resolve_type_ref(type_ref.element_type),
                length=type_ref.length,
            )
        raise self._error(getattr(type_ref, "line", 0), getattr(type_ref, "column", 0), "unsupported type syntax")

    def _is_assignable_target(self, expr: ast.Expr) -> bool:
        return (
            isinstance(expr, ast.IdentifierExpr)
            or isinstance(expr, ast.FieldAccessExpr)
            or (isinstance(expr, ast.UnaryExpr) and expr.operator == "*")
        )

    def _ensure_mutable_target(self, expr: ast.Expr, scope: Scope) -> None:
        if isinstance(expr, ast.IdentifierExpr):
            resolved = scope.resolve(expr.name)
            if resolved is None:
                raise self._error(expr.line, expr.column, f"unknown symbol '{expr.name}'")
            if not resolved.mutable:
                raise self._error(expr.line, expr.column, f"cannot assign to immutable binding '{expr.name}'")
            return
        if isinstance(expr, ast.FieldAccessExpr):
            self._ensure_mutable_target(expr.object, scope)
            return
        if isinstance(expr, ast.UnaryExpr) and expr.operator == "*":
            return

    def _lookup_struct_field(self, struct_type: typesys.StructType, field_name: str, line: int, column: int) -> typesys.Type:
        for field in struct_type.fields:
            if field.name == field_name:
                return field.type_ref
        raise self._error(line, column, f"unknown field '{field_name}' for struct '{struct_type.name}'")

    def _error(self, line: int, column: int, message: str) -> TypeCheckError:
        return TypeCheckError(message=message, line=line, column=column, source_name=self.source_name)


def check(program: ast.Program, semantic_model: SemanticModel, source_name: str = "<memory>") -> CheckResult:
    return TypeChecker(semantic_model=semantic_model, source_name=source_name).check(program)
