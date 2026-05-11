from __future__ import annotations

from dataclasses import dataclass, fields, is_dataclass


class Node:
    pass


@dataclass(frozen=True, slots=True)
class Program(Node):
    package: PackageDecl | None
    imports: list[ImportDecl]
    declarations: list[Decl]


@dataclass(frozen=True, slots=True)
class PackageDecl(Node):
    path: list[str]
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class ImportDecl(Node):
    path: list[str]
    line: int
    column: int


class Decl(Node):
    pass


@dataclass(frozen=True, slots=True)
class StructDecl(Decl):
    name: str
    fields: list[FieldDecl]
    is_public: bool = False
    line: int = 0
    column: int = 0


@dataclass(frozen=True, slots=True)
class FunctionDecl(Decl):
    name: str
    params: list[Param]
    return_type: TypeRef
    body: BlockStmt
    is_public: bool = False
    line: int = 0
    column: int = 0


@dataclass(frozen=True, slots=True)
class ExternFunctionDecl(Decl):
    name: str
    params: list[Param]
    return_type: TypeRef
    calling_convention: str | None = None
    is_public: bool = False
    line: int = 0
    column: int = 0


@dataclass(frozen=True, slots=True)
class Param(Node):
    name: str
    type_ref: TypeRef
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class FieldDecl(Node):
    name: str
    type_ref: TypeRef
    line: int
    column: int


class TypeRef(Node):
    pass


@dataclass(frozen=True, slots=True)
class NamedType(TypeRef):
    path: list[str]
    line: int = 0
    column: int = 0


@dataclass(frozen=True, slots=True)
class PointerType(TypeRef):
    inner: TypeRef
    is_const: bool = False
    is_nullable: bool = False
    line: int = 0
    column: int = 0


@dataclass(frozen=True, slots=True)
class ArrayType(TypeRef):
    element_type: TypeRef
    length: int | None = None
    line: int = 0
    column: int = 0


class Stmt(Node):
    pass


@dataclass(frozen=True, slots=True)
class BlockStmt(Stmt):
    statements: list[Stmt]
    line: int = 0
    column: int = 0


@dataclass(frozen=True, slots=True)
class ConstStmt(Stmt):
    name: str
    type_ref: TypeRef | None
    value: Expr
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class LetStmt(Stmt):
    name: str
    type_ref: TypeRef | None
    value: Expr | None
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class IfStmt(Stmt):
    condition: Expr
    then_branch: BlockStmt
    else_branch: BlockStmt | IfStmt | None
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class WhileStmt(Stmt):
    condition: Expr
    body: BlockStmt
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class LoopStmt(Stmt):
    body: BlockStmt
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class BreakStmt(Stmt):
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class ContinueStmt(Stmt):
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class ReturnStmt(Stmt):
    value: Expr | None
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class ExprStmt(Stmt):
    expr: Expr
    line: int = 0
    column: int = 0


class Expr(Node):
    pass


@dataclass(frozen=True, slots=True)
class IdentifierExpr(Expr):
    name: str
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class LiteralExpr(Expr):
    value: object
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class GroupExpr(Expr):
    expr: Expr
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class UnaryExpr(Expr):
    operator: str
    operand: Expr
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class BinaryExpr(Expr):
    left: Expr
    operator: str
    right: Expr
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class AssignExpr(Expr):
    target: Expr
    value: Expr
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class CallExpr(Expr):
    callee: Expr
    arguments: list[Expr]
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class CastExpr(Expr):
    expr: Expr
    type_ref: TypeRef
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class FieldAccessExpr(Expr):
    object: Expr
    field: str
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class FieldInit(Node):
    name: str
    value: Expr
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class StructLiteralExpr(Expr):
    type_name: str
    fields: list[FieldInit]
    line: int
    column: int


def dump(node: object, indent: int = 0) -> str:
    prefix = "  " * indent
    if isinstance(node, list):
        if not node:
            return prefix + "[]"
        lines = [prefix + "["]
        for item in node:
            lines.append(dump(item, indent + 1) + ",")
        lines.append(prefix + "]")
        return "\n".join(lines)

    if is_dataclass(node):
        node_fields = fields(node)
        lines = [prefix + f"{type(node).__name__}("]
        for field_info in node_fields:
            value = getattr(node, field_info.name)
            rendered = dump(value, indent + 1)
            first_line, *rest = rendered.splitlines()
            lines.append(f"{'  ' * (indent + 1)}{field_info.name}={first_line.strip()}")
            for line in rest:
                lines.append(line)
        lines.append(prefix + ")")
        return "\n".join(lines)

    return prefix + repr(node)
