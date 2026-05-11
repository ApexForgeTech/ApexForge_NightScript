from __future__ import annotations

from dataclasses import dataclass

from .ast import Program
from .parser import parse
from .sema import SemanticModel, analyze
from .typecheck import CheckResult, check


@dataclass(frozen=True, slots=True)
class FullCheckResult:
    program: Program
    semantic_model: SemanticModel
    type_result: CheckResult


def check_source(source: str, source_name: str = "<memory>") -> FullCheckResult:
    program = parse(source, source_name=source_name)
    semantic_model = analyze(program, source_name=source_name)
    type_result = check(program, semantic_model, source_name=source_name)
    return FullCheckResult(program=program, semantic_model=semantic_model, type_result=type_result)
