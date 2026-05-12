from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class SourcePosition:
    line: int
    column: int

    def __str__(self) -> str:
        return f"{self.line}:{self.column}"


class NightScriptError(Exception):
    """Base class for compiler-facing user errors."""


@dataclass(slots=True)
class LexError(NightScriptError):
    message: str
    line: int
    column: int
    source_name: str = "<memory>"

    def __str__(self) -> str:
        return f"{self.source_name}:{self.line}:{self.column}: {self.message}"


@dataclass(slots=True)
class ParseError(NightScriptError):
    message: str
    line: int
    column: int
    source_name: str = "<memory>"

    def __str__(self) -> str:
        return f"{self.source_name}:{self.line}:{self.column}: {self.message}"


@dataclass(slots=True)
class SemanticError(NightScriptError):
    message: str
    line: int
    column: int
    source_name: str = "<memory>"

    def __str__(self) -> str:
        return f"{self.source_name}:{self.line}:{self.column}: {self.message}"


@dataclass(slots=True)
class TypeCheckError(NightScriptError):
    message: str
    line: int
    column: int
    source_name: str = "<memory>"

    def __str__(self) -> str:
        return f"{self.source_name}:{self.line}:{self.column}: {self.message}"
