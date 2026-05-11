from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class TokenKind(str, Enum):
    PACKAGE = "PACKAGE"
    IMPORT = "IMPORT"
    PUB = "PUB"
    FN = "FN"
    LET = "LET"
    CONST = "CONST"
    RETURN = "RETURN"
    IF = "IF"
    ELSE = "ELSE"
    WHILE = "WHILE"
    LOOP = "LOOP"
    BREAK = "BREAK"
    CONTINUE = "CONTINUE"
    STRUCT = "STRUCT"
    ENUM = "ENUM"
    UNION = "UNION"
    INTERFACE = "INTERFACE"
    IMPL = "IMPL"
    UNSAFE = "UNSAFE"
    EXTERN = "EXTERN"
    AS = "AS"
    MATCH = "MATCH"
    KERNEL = "KERNEL"
    NATIVE = "NATIVE"
    UI = "UI"
    ANDROID = "ANDROID"
    DRIVER = "DRIVER"
    APP = "APP"
    MODULE = "MODULE"
    DEFER = "DEFER"
    COMPTIME = "COMPTIME"
    TRUE = "TRUE"
    FALSE = "FALSE"
    NULL = "NULL"
    SELF = "SELF"
    UNDERSCORE = "UNDERSCORE"

    IDENTIFIER = "IDENTIFIER"
    INTEGER = "INTEGER"
    FLOAT = "FLOAT"
    STRING = "STRING"
    CHAR = "CHAR"

    LBRACE = "LBRACE"
    RBRACE = "RBRACE"
    LPAREN = "LPAREN"
    RPAREN = "RPAREN"
    LBRACKET = "LBRACKET"
    RBRACKET = "RBRACKET"
    SEMICOLON = "SEMICOLON"
    COLON = "COLON"
    COMMA = "COMMA"
    DOT = "DOT"
    ARROW = "ARROW"
    EQUAL = "EQUAL"
    PLUS = "PLUS"
    MINUS = "MINUS"
    STAR = "STAR"
    SLASH = "SLASH"
    PERCENT = "PERCENT"
    AMPERSAND = "AMPERSAND"
    PIPE = "PIPE"
    CARET = "CARET"
    TILDE = "TILDE"
    BANG = "BANG"
    LT = "LT"
    GT = "GT"
    QUESTION = "QUESTION"
    AT = "AT"

    EQEQ = "EQEQ"
    NE = "NE"
    LE = "LE"
    GE = "GE"
    ANDAND = "ANDAND"
    OROR = "OROR"

    EOF = "EOF"


KEYWORDS = {
    "package": TokenKind.PACKAGE,
    "import": TokenKind.IMPORT,
    "pub": TokenKind.PUB,
    "fn": TokenKind.FN,
    "let": TokenKind.LET,
    "const": TokenKind.CONST,
    "return": TokenKind.RETURN,
    "if": TokenKind.IF,
    "else": TokenKind.ELSE,
    "while": TokenKind.WHILE,
    "loop": TokenKind.LOOP,
    "break": TokenKind.BREAK,
    "continue": TokenKind.CONTINUE,
    "struct": TokenKind.STRUCT,
    "enum": TokenKind.ENUM,
    "union": TokenKind.UNION,
    "interface": TokenKind.INTERFACE,
    "impl": TokenKind.IMPL,
    "unsafe": TokenKind.UNSAFE,
    "extern": TokenKind.EXTERN,
    "as": TokenKind.AS,
    "match": TokenKind.MATCH,
    "kernel": TokenKind.KERNEL,
    "native": TokenKind.NATIVE,
    "ui": TokenKind.UI,
    "android": TokenKind.ANDROID,
    "driver": TokenKind.DRIVER,
    "app": TokenKind.APP,
    "module": TokenKind.MODULE,
    "defer": TokenKind.DEFER,
    "comptime": TokenKind.COMPTIME,
    "true": TokenKind.TRUE,
    "false": TokenKind.FALSE,
    "null": TokenKind.NULL,
    "Self": TokenKind.SELF,
    "_": TokenKind.UNDERSCORE,
}


@dataclass(frozen=True, slots=True)
class Token:
    kind: TokenKind
    lexeme: str
    line: int
    column: int
    end_line: int
    end_column: int
    literal: object | None = None

    def format(self) -> str:
        suffix = ""
        if self.literal is not None:
            suffix = f" value={self.literal!r}"
        return (
            f"{self.kind.value} {self.lexeme!r} "
            f"@ {self.line}:{self.column}-{self.end_line}:{self.end_column}{suffix}"
        )
