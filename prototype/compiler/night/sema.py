from __future__ import annotations

from dataclasses import dataclass

from . import ast
from .errors import SemanticError
from . import typesys


@dataclass(frozen=True, slots=True)
class FunctionSymbol:
    name: str
    decl: ast.FunctionDecl | ast.ExternFunctionDecl
    type_ref: typesys.FunctionType
    c_name: str
    owner_type: str | None = None
    receiver_type: typesys.Type | None = None


@dataclass(frozen=True, slots=True)
class StructSymbol:
    name: str
    decl: ast.StructDecl
    type_ref: typesys.StructType


@dataclass(frozen=True, slots=True)
class EnumSymbol:
    name: str
    decl: ast.EnumDecl
    type_ref: typesys.EnumType


@dataclass(frozen=True, slots=True)
class UnionSymbol:
    name: str
    decl: ast.UnionDecl
    type_ref: typesys.UnionType


@dataclass(frozen=True, slots=True)
class SemanticModel:
    package: tuple[str, ...] | None
    imports: tuple[tuple[str, ...], ...]
    functions: dict[str, FunctionSymbol]
    structs: dict[str, StructSymbol]
    enums: dict[str, EnumSymbol]
    unions: dict[str, UnionSymbol]
    methods: dict[tuple[str, str], FunctionSymbol]


class Scope:
    def __init__(self, parent: Scope | None = None) -> None:
        self.parent = parent
        self.symbols: dict[str, object] = {}

    def define(self, name: str, value: object) -> bool:
        if name in self.symbols:
            return False
        self.symbols[name] = value
        return True

    def resolve(self, name: str) -> object | None:
        scope: Scope | None = self
        while scope is not None:
            if name in scope.symbols:
                return scope.symbols[name]
            scope = scope.parent
        return None


class SemanticAnalyzer:
    def __init__(self, source_name: str = "<memory>") -> None:
        self.source_name = source_name
        self.functions: dict[str, FunctionSymbol] = {}
        self.structs: dict[str, StructSymbol] = {}
        self.enums: dict[str, EnumSymbol] = {}
        self.unions: dict[str, UnionSymbol] = {}
        self.methods: dict[tuple[str, str], FunctionSymbol] = {}

    def analyze(self, program: ast.Program) -> SemanticModel:
        for import_decl in program.imports:
            if not import_decl.path:
                raise self._error(import_decl.line, import_decl.column, "import path cannot be empty")

        for decl in program.declarations:
            if isinstance(decl, ast.StructDecl):
                self._register_struct(decl)
            elif isinstance(decl, ast.EnumDecl):
                self._register_enum(decl)
            elif isinstance(decl, ast.UnionDecl):
                self._register_union(decl)

        for decl in program.declarations:
            if isinstance(decl, ast.StructDecl):
                self._validate_struct_decl(decl)
            elif isinstance(decl, ast.EnumDecl):
                self._validate_enum_decl(decl)
            elif isinstance(decl, ast.UnionDecl):
                self._validate_union_decl(decl)

        for decl in program.declarations:
            if isinstance(decl, ast.FunctionDecl | ast.ExternFunctionDecl):
                self._register_function(decl)
            elif isinstance(decl, ast.ImplDecl):
                self._register_impl(decl)
            elif not isinstance(decl, ast.StructDecl | ast.EnumDecl | ast.UnionDecl):
                raise self._error(0, 0, f"unsupported declaration: {type(decl).__name__}")

        for decl in program.declarations:
            if isinstance(decl, ast.FunctionDecl):
                self._analyze_function_body(decl)
            elif isinstance(decl, ast.ImplDecl):
                for method in decl.methods:
                    self._analyze_function_body(method)

        package = tuple(program.package.path) if program.package is not None else None
        imports = tuple(tuple(import_decl.path) for import_decl in program.imports)
        return SemanticModel(
            package=package,
            imports=imports,
            functions=dict(self.functions),
            structs=dict(self.structs),
            enums=dict(self.enums),
            unions=dict(self.unions),
            methods=dict(self.methods),
        )

    def _register_struct(self, decl: ast.StructDecl) -> None:
        if decl.name in self.structs:
            previous = self.structs[decl.name].decl
            raise self._error(
                decl.line,
                decl.column,
                f"duplicate struct '{decl.name}' (previous declaration at {previous.line}:{previous.column})",
            )
        self.structs[decl.name] = StructSymbol(
            name=decl.name,
            decl=decl,
            type_ref=typesys.StructType(name=decl.name, fields=()),
        )

    def _validate_struct_decl(self, decl: ast.StructDecl) -> None:
        fields: list[typesys.StructField] = []
        seen_fields: set[str] = set()
        for field in decl.fields:
            if field.name in seen_fields:
                raise self._error(field.line, field.column, f"duplicate struct field '{field.name}'")
            seen_fields.add(field.name)
            self._validate_type_ref(field.type_ref)
            fields.append(
                typesys.StructField(
                    name=field.name,
                    type_ref=self._resolve_type_ref(field.type_ref),
                )
            )
        self.structs[decl.name] = StructSymbol(
            name=decl.name,
            decl=decl,
            type_ref=typesys.StructType(name=decl.name, fields=tuple(fields)),
        )

    def _register_enum(self, decl: ast.EnumDecl) -> None:
        if decl.name in self.enums:
            previous = self.enums[decl.name].decl
            raise self._error(
                decl.line,
                decl.column,
                f"duplicate enum '{decl.name}' (previous declaration at {previous.line}:{previous.column})",
            )
        self.enums[decl.name] = EnumSymbol(
            name=decl.name,
            decl=decl,
            type_ref=typesys.EnumType(name=decl.name, variants=()),
        )

    def _validate_enum_decl(self, decl: ast.EnumDecl) -> None:
        variants: list[typesys.EnumVariant] = []
        seen_variants: set[str] = set()
        for index, variant in enumerate(decl.variants):
            if variant.name in seen_variants:
                raise self._error(variant.line, variant.column, f"duplicate enum variant '{variant.name}'")
            seen_variants.add(variant.name)
            variants.append(typesys.EnumVariant(name=variant.name, value=index))
        self.enums[decl.name] = EnumSymbol(
            name=decl.name,
            decl=decl,
            type_ref=typesys.EnumType(name=decl.name, variants=tuple(variants)),
        )

    def _register_union(self, decl: ast.UnionDecl) -> None:
        if decl.name in self.unions:
            previous = self.unions[decl.name].decl
            raise self._error(
                decl.line,
                decl.column,
                f"duplicate union '{decl.name}' (previous declaration at {previous.line}:{previous.column})",
            )
        self.unions[decl.name] = UnionSymbol(
            name=decl.name,
            decl=decl,
            type_ref=typesys.UnionType(name=decl.name, fields=()),
        )

    def _validate_union_decl(self, decl: ast.UnionDecl) -> None:
        fields: list[typesys.UnionField] = []
        seen_fields: set[str] = set()
        for field in decl.fields:
            if field.name in seen_fields:
                raise self._error(field.line, field.column, f"duplicate union field '{field.name}'")
            seen_fields.add(field.name)
            self._validate_type_ref(field.type_ref)
            fields.append(
                typesys.UnionField(
                    name=field.name,
                    type_ref=self._resolve_type_ref(field.type_ref),
                )
            )
        self.unions[decl.name] = UnionSymbol(
            name=decl.name,
            decl=decl,
            type_ref=typesys.UnionType(name=decl.name, fields=tuple(fields)),
        )

    def _register_function(self, decl: ast.FunctionDecl | ast.ExternFunctionDecl) -> None:
        if decl.name in self.functions:
            previous = self.functions[decl.name].decl
            raise self._error(
                decl.line,
                decl.column,
                f"duplicate function '{decl.name}' (previous declaration at {previous.line}:{previous.column})",
            )

        self._validate_type_ref(decl.return_type)
        param_types = []
        seen_params: set[str] = set()
        for param in decl.params:
            if param.name in seen_params:
                raise self._error(param.line, param.column, f"duplicate parameter '{param.name}'")
            seen_params.add(param.name)
            self._validate_type_ref(param.type_ref)
            param_types.append(self._resolve_type_ref(param.type_ref))

        signature = typesys.FunctionType(
            param_types=tuple(param_types),
            return_type=self._resolve_type_ref(decl.return_type),
            calling_convention=getattr(decl, "calling_convention", None),
        )
        self.functions[decl.name] = FunctionSymbol(
            name=decl.name,
            decl=decl,
            type_ref=signature,
            c_name=decl.name,
        )

    def _register_impl(self, decl: ast.ImplDecl) -> None:
        if decl.target_type not in self.structs:
            raise self._error(decl.line, decl.column, f"unknown impl target '{decl.target_type}'")
        for method in decl.methods:
            self._register_method(decl.target_type, method)

    def _register_method(self, owner_type: str, decl: ast.FunctionDecl) -> None:
        method_key = (owner_type, decl.name)
        if method_key in self.methods:
            previous = self.methods[method_key].decl
            raise self._error(
                decl.line,
                decl.column,
                f"duplicate method '{owner_type}.{decl.name}' (previous declaration at {previous.line}:{previous.column})",
            )

        self._validate_type_ref(decl.return_type)
        param_types: list[typesys.Type] = []
        seen_params: set[str] = set()
        receiver_type: typesys.Type | None = None
        for index, param in enumerate(decl.params):
            if param.name in seen_params:
                raise self._error(param.line, param.column, f"duplicate parameter '{param.name}'")
            seen_params.add(param.name)
            self._validate_type_ref(param.type_ref)
            resolved = self._resolve_type_ref(param.type_ref)
            if index == 0 and param.name == "self":
                receiver_type = resolved
                if not self._is_valid_receiver_type(owner_type, resolved):
                    raise self._error(
                        param.line,
                        param.column,
                        f"self parameter for '{owner_type}.{decl.name}' must be {owner_type} or *{owner_type}",
                    )
            param_types.append(resolved)

        signature = typesys.FunctionType(
            param_types=tuple(param_types),
            return_type=self._resolve_type_ref(decl.return_type),
            calling_convention=None,
        )
        self.methods[method_key] = FunctionSymbol(
            name=decl.name,
            decl=decl,
            type_ref=signature,
            c_name=f"{owner_type}_{decl.name}",
            owner_type=owner_type,
            receiver_type=receiver_type,
        )

    def _analyze_function_body(self, decl: ast.FunctionDecl) -> None:
        scope = Scope()
        for function_name, symbol in self.functions.items():
            scope.define(function_name, symbol)

        for param in decl.params:
            if not scope.define(param.name, param):
                raise self._error(param.line, param.column, f"duplicate parameter '{param.name}'")

        for stmt in decl.body.statements:
            self._analyze_stmt(stmt, scope, loop_depth=0)

    def _analyze_stmt(self, stmt: ast.Stmt, scope: Scope, loop_depth: int) -> None:
        if isinstance(stmt, ast.ConstStmt):
            if stmt.type_ref is not None:
                self._validate_type_ref(stmt.type_ref)
            self._analyze_expr(stmt.value, scope)
            if not scope.define(stmt.name, stmt):
                raise self._error(stmt.line, stmt.column, f"duplicate local '{stmt.name}'")
            return

        if isinstance(stmt, ast.LetStmt):
            if stmt.type_ref is not None:
                self._validate_type_ref(stmt.type_ref)
            if stmt.value is not None:
                self._analyze_expr(stmt.value, scope)
            if not scope.define(stmt.name, stmt):
                raise self._error(stmt.line, stmt.column, f"duplicate local '{stmt.name}'")
            return

        if isinstance(stmt, ast.IfStmt):
            self._analyze_expr(stmt.condition, scope)
            self._analyze_block(stmt.then_branch, Scope(parent=scope), loop_depth=loop_depth)
            if stmt.else_branch is not None:
                if isinstance(stmt.else_branch, ast.BlockStmt):
                    self._analyze_block(stmt.else_branch, Scope(parent=scope), loop_depth=loop_depth)
                else:
                    self._analyze_stmt(stmt.else_branch, Scope(parent=scope), loop_depth=loop_depth)
            return

        if isinstance(stmt, ast.WhileStmt):
            self._analyze_expr(stmt.condition, scope)
            self._analyze_block(stmt.body, Scope(parent=scope), loop_depth=loop_depth + 1)
            return

        if isinstance(stmt, ast.LoopStmt):
            self._analyze_block(stmt.body, Scope(parent=scope), loop_depth=loop_depth + 1)
            return

        if isinstance(stmt, ast.UnsafeStmt):
            self._analyze_block(stmt.body, Scope(parent=scope), loop_depth=loop_depth)
            return

        if isinstance(stmt, ast.BreakStmt | ast.ContinueStmt):
            if loop_depth <= 0:
                keyword = "break" if isinstance(stmt, ast.BreakStmt) else "continue"
                raise self._error(stmt.line, stmt.column, f"'{keyword}' can only be used inside a loop")
            return

        if isinstance(stmt, ast.ReturnStmt):
            if stmt.value is not None:
                self._analyze_expr(stmt.value, scope)
            return

        if isinstance(stmt, ast.ExprStmt):
            self._analyze_expr(stmt.expr, scope)
            return

        raise self._error(getattr(stmt, "line", 0), getattr(stmt, "column", 0), f"unsupported statement: {type(stmt).__name__}")

    def _analyze_block(self, block: ast.BlockStmt, scope: Scope, loop_depth: int) -> None:
        for stmt in block.statements:
            self._analyze_stmt(stmt, scope, loop_depth)

    def _analyze_expr(self, expr: ast.Expr, scope: Scope) -> None:
        if isinstance(expr, ast.IdentifierExpr):
            if scope.resolve(expr.name) is None:
                raise self._error(expr.line, expr.column, f"unknown symbol '{expr.name}'")
            return

        if isinstance(expr, ast.LiteralExpr):
            return

        if isinstance(expr, ast.GroupExpr):
            self._analyze_expr(expr.expr, scope)
            return

        if isinstance(expr, ast.UnaryExpr):
            self._analyze_expr(expr.operand, scope)
            return

        if isinstance(expr, ast.BinaryExpr):
            self._analyze_expr(expr.left, scope)
            self._analyze_expr(expr.right, scope)
            return

        if isinstance(expr, ast.AssignExpr):
            self._analyze_expr(expr.target, scope)
            self._analyze_expr(expr.value, scope)
            return

        if isinstance(expr, ast.CallExpr):
            self._analyze_expr(expr.callee, scope)
            for argument in expr.arguments:
                self._analyze_expr(argument, scope)
            return

        if isinstance(expr, ast.CastExpr):
            self._analyze_expr(expr.expr, scope)
            self._validate_type_ref(expr.type_ref)
            return

        if isinstance(expr, ast.FieldAccessExpr):
            if isinstance(expr.object, ast.IdentifierExpr) and (
                expr.object.name in self.structs or expr.object.name in self.enums or expr.object.name in self.unions
            ):
                return
            self._analyze_expr(expr.object, scope)
            return

        if isinstance(expr, ast.StructLiteralExpr):
            if expr.type_name in self.structs or expr.type_name in self.unions:
                seen_fields: set[str] = set()
                for field in expr.fields:
                    if field.name in seen_fields:
                        raise self._error(field.line, field.column, f"duplicate struct literal field '{field.name}'")
                    seen_fields.add(field.name)
                    self._analyze_expr(field.value, scope)
                return
            raise self._error(expr.line, expr.column, f"unknown composite type '{expr.type_name}'")

        if isinstance(expr, ast.MatchExpr):
            self._analyze_expr(expr.subject, scope)
            for arm in expr.arms:
                self._analyze_expr(arm.value, scope)
            return

        raise self._error(getattr(expr, "line", 0), getattr(expr, "column", 0), f"unsupported expression: {type(expr).__name__}")

    def _validate_type_ref(self, type_ref: ast.TypeRef) -> None:
        if isinstance(type_ref, ast.NamedType):
            if len(type_ref.path) != 1:
                raise self._error(type_ref.line, type_ref.column, f"unknown type '{'.'.join(type_ref.path)}'")
            name = type_ref.path[0]
            if (
                name not in typesys.BUILTINS
                and name not in self.structs
                and name not in self.enums
                and name not in self.unions
            ):
                raise self._error(type_ref.line, type_ref.column, f"unknown type '{name}'")
            return

        if isinstance(type_ref, ast.PointerType):
            self._validate_type_ref(type_ref.inner)
            return

        if isinstance(type_ref, ast.ArrayType):
            self._validate_type_ref(type_ref.element_type)
            return

        raise self._error(getattr(type_ref, "line", 0), getattr(type_ref, "column", 0), "unsupported type syntax")

    def _resolve_type_ref(self, type_ref: ast.TypeRef) -> typesys.Type:
        if isinstance(type_ref, ast.NamedType):
            if type_ref.path[0] in typesys.BUILTINS:
                return typesys.BUILTINS[type_ref.path[0]]
            if type_ref.path[0] in self.structs:
                return self.structs[type_ref.path[0]].type_ref
            if type_ref.path[0] in self.enums:
                return self.enums[type_ref.path[0]].type_ref
            return self.unions[type_ref.path[0]].type_ref
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

    def _error(self, line: int, column: int, message: str) -> SemanticError:
        return SemanticError(message=message, line=line, column=column, source_name=self.source_name)

    def _is_valid_receiver_type(self, owner_type: str, receiver_type: typesys.Type) -> bool:
        owner_struct = self.structs[owner_type].type_ref
        if receiver_type == owner_struct:
            return True
        return (
            isinstance(receiver_type, typesys.PointerType)
            and receiver_type.inner == owner_struct
        )


def analyze(program: ast.Program, source_name: str = "<memory>") -> SemanticModel:
    return SemanticAnalyzer(source_name=source_name).analyze(program)
