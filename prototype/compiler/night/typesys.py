from __future__ import annotations

from dataclasses import dataclass


class Type:
    pass


@dataclass(frozen=True, slots=True)
class BuiltinType(Type):
    name: str


@dataclass(frozen=True, slots=True)
class PointerType(Type):
    inner: Type
    is_const: bool = False
    is_nullable: bool = False


@dataclass(frozen=True, slots=True)
class ArrayType(Type):
    element_type: Type
    length: int | None = None


@dataclass(frozen=True, slots=True)
class FunctionType(Type):
    param_types: tuple[Type, ...]
    return_type: Type
    calling_convention: str | None = None


@dataclass(frozen=True, slots=True)
class StructField:
    name: str
    type_ref: Type


@dataclass(frozen=True, slots=True)
class StructType(Type):
    name: str
    fields: tuple[StructField, ...]


@dataclass(frozen=True, slots=True)
class UnionField:
    name: str
    type_ref: Type


@dataclass(frozen=True, slots=True)
class UnionType(Type):
    name: str
    fields: tuple[UnionField, ...]


@dataclass(frozen=True, slots=True)
class EnumVariant:
    name: str
    value: int


@dataclass(frozen=True, slots=True)
class EnumType(Type):
    name: str
    variants: tuple[EnumVariant, ...]


@dataclass(frozen=True, slots=True)
class LiteralIntType(Type):
    pass


@dataclass(frozen=True, slots=True)
class LiteralFloatType(Type):
    pass


@dataclass(frozen=True, slots=True)
class LiteralStringType(Type):
    pass


@dataclass(frozen=True, slots=True)
class NullType(Type):
    pass


I8 = BuiltinType("i8")
I16 = BuiltinType("i16")
I32 = BuiltinType("i32")
I64 = BuiltinType("i64")
ISIZE = BuiltinType("isize")
U8 = BuiltinType("u8")
U16 = BuiltinType("u16")
U32 = BuiltinType("u32")
U64 = BuiltinType("u64")
USIZE = BuiltinType("usize")
F32 = BuiltinType("f32")
F64 = BuiltinType("f64")
BOOL = BuiltinType("bool")
CHAR = BuiltinType("char")
VOID = BuiltinType("void")
NEVER = BuiltinType("never")
STR = BuiltinType("str")
CSTR = BuiltinType("cstr")

INT_LITERAL = LiteralIntType()
FLOAT_LITERAL = LiteralFloatType()
STRING_LITERAL = LiteralStringType()
NULL = NullType()

BUILTINS: dict[str, Type] = {
    "i8": I8,
    "i16": I16,
    "i32": I32,
    "i64": I64,
    "isize": ISIZE,
    "u8": U8,
    "u16": U16,
    "u32": U32,
    "u64": U64,
    "usize": USIZE,
    "f32": F32,
    "f64": F64,
    "bool": BOOL,
    "char": CHAR,
    "void": VOID,
    "never": NEVER,
    "str": STR,
    "cstr": CSTR,
}


INTEGER_TYPES = {I8, I16, I32, I64, ISIZE, U8, U16, U32, U64, USIZE}
FLOAT_TYPES = {F32, F64}
NUMERIC_TYPES = INTEGER_TYPES | FLOAT_TYPES


def describe(type_ref: Type) -> str:
    if isinstance(type_ref, BuiltinType):
        return type_ref.name
    if isinstance(type_ref, PointerType):
        prefix = "?*" if type_ref.is_nullable else "*"
        if type_ref.is_const:
            return f"{prefix}const {describe(type_ref.inner)}"
        return f"{prefix}{describe(type_ref.inner)}"
    if isinstance(type_ref, ArrayType):
        if type_ref.length is None:
            return f"[]{describe(type_ref.element_type)}"
        return f"[{type_ref.length}]{describe(type_ref.element_type)}"
    if isinstance(type_ref, FunctionType):
        params = ", ".join(describe(param) for param in type_ref.param_types)
        return f"fn({params}) -> {describe(type_ref.return_type)}"
    if isinstance(type_ref, StructType):
        return type_ref.name
    if isinstance(type_ref, UnionType):
        return type_ref.name
    if isinstance(type_ref, EnumType):
        return type_ref.name
    if isinstance(type_ref, LiteralIntType):
        return "<int literal>"
    if isinstance(type_ref, LiteralFloatType):
        return "<float literal>"
    if isinstance(type_ref, LiteralStringType):
        return "<string literal>"
    if isinstance(type_ref, NullType):
        return "null"
    return repr(type_ref)


def is_numeric(type_ref: Type) -> bool:
    return type_ref in NUMERIC_TYPES or isinstance(type_ref, (LiteralIntType, LiteralFloatType))


def is_integer_like(type_ref: Type) -> bool:
    return type_ref in INTEGER_TYPES or isinstance(type_ref, LiteralIntType)


def is_float_like(type_ref: Type) -> bool:
    return type_ref in FLOAT_TYPES or isinstance(type_ref, LiteralFloatType)


def is_assignable(expected: Type, actual: Type) -> bool:
    if expected == actual:
        return True

    if isinstance(actual, LiteralIntType) and expected in INTEGER_TYPES:
        return True

    if isinstance(actual, LiteralFloatType) and expected in FLOAT_TYPES:
        return True

    if isinstance(actual, LiteralStringType) and expected in {STR, CSTR}:
        return True

    if isinstance(actual, NullType) and isinstance(expected, PointerType) and expected.is_nullable:
        return True

    if isinstance(expected, PointerType) and isinstance(actual, PointerType):
        return (
            expected.is_const == actual.is_const
            and expected.is_nullable == actual.is_nullable
            and is_assignable(expected.inner, actual.inner)
        )

    if isinstance(expected, ArrayType) and isinstance(actual, ArrayType):
        return expected.length == actual.length and is_assignable(expected.element_type, actual.element_type)

    if isinstance(expected, StructType) and isinstance(actual, StructType):
        return expected.name == actual.name

    if isinstance(expected, UnionType) and isinstance(actual, UnionType):
        return expected.name == actual.name

    if isinstance(expected, EnumType) and isinstance(actual, EnumType):
        return expected.name == actual.name

    return False


def is_castable(source: Type, target: Type) -> bool:
    if is_assignable(target, source):
        return True

    if is_numeric(source) and is_numeric(target):
        return True

    if isinstance(source, LiteralIntType) and isinstance(target, PointerType):
        return True

    if isinstance(source, PointerType) and is_integer_like(target):
        return True

    if isinstance(source, PointerType) and isinstance(target, PointerType):
        return True

    return False
