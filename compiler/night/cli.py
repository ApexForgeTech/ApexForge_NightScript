from __future__ import annotations

import argparse
from pathlib import Path
from typing import Sequence

from . import __version__
from .ast import dump
from .build import build_source
from .check import check_source
from .codegen_c import generate_c
from .errors import LexError, ParseError, SemanticError, TypeCheckError
from .lexer import tokenize
from .parser import parse


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="night", description="NightScript prototype compiler")
    subparsers = parser.add_subparsers(dest="command", required=True)

    version_parser = subparsers.add_parser("version", help="print the compiler version")
    version_parser.set_defaults(handler=handle_version)

    lex_parser = subparsers.add_parser("lex", help="tokenize a NightScript source file")
    lex_parser.add_argument("path", help="path to the .afns source file")
    lex_parser.set_defaults(handler=handle_lex)

    parse_parser = subparsers.add_parser("parse", help="parse a NightScript source file into an AST")
    parse_parser.add_argument("path", help="path to the .afns source file")
    parse_parser.set_defaults(handler=handle_parse)

    check_parser = subparsers.add_parser("check", help="parse and type-check a NightScript source file")
    check_parser.add_argument("path", help="path to the .afns source file")
    check_parser.set_defaults(handler=handle_check)

    codegen_parser = subparsers.add_parser("codegen", help="emit C for a NightScript source file")
    codegen_parser.add_argument("path", help="path to the .afns source file")
    codegen_parser.set_defaults(handler=handle_codegen)

    build_parser = subparsers.add_parser("build", help="build a NightScript source file")
    build_parser.add_argument("path", help="path to the .afns source file")
    build_parser.add_argument("-o", "--output", help="output binary path")
    build_parser.add_argument("--emit-c", help="path to write generated C")
    build_parser.add_argument("--cc", help="C compiler to use")
    build_parser.set_defaults(handler=handle_build)

    return parser


def handle_version(_args: argparse.Namespace) -> int:
    print(f"night {__version__}")
    return 0


def handle_lex(args: argparse.Namespace) -> int:
    path = Path(args.path)
    source = path.read_text(encoding="utf-8")
    tokens = tokenize(source, source_name=str(path))
    for token in tokens:
        print(token.format())
    return 0


def handle_parse(args: argparse.Namespace) -> int:
    path = Path(args.path)
    source = path.read_text(encoding="utf-8")
    program = parse(source, source_name=str(path))
    print(dump(program))
    return 0


def handle_check(args: argparse.Namespace) -> int:
    path = Path(args.path)
    source = path.read_text(encoding="utf-8")
    result = check_source(source, source_name=str(path))
    package_name = "<root>"
    if result.semantic_model.package is not None:
        package_name = ".".join(result.semantic_model.package)
    print(
        f"OK: package={package_name} imports={len(result.semantic_model.imports)} "
        f"functions={len(result.semantic_model.functions)}"
    )
    return 0


def handle_codegen(args: argparse.Namespace) -> int:
    path = Path(args.path)
    source = path.read_text(encoding="utf-8")
    result = check_source(source, source_name=str(path))
    c_output = generate_c(result)
    print(c_output.source, end="")
    return 0


def handle_build(args: argparse.Namespace) -> int:
    path = Path(args.path)
    output = Path(args.output) if args.output else path.with_suffix("")
    emit_c = Path(args.emit_c) if args.emit_c else None
    artifacts = build_source(path, output_path=output, emit_c_path=emit_c, compiler=args.cc)
    print(f"OK: c={artifacts.c_path} binary={artifacts.binary_path}")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    handler = args.handler

    try:
        return handler(args)
    except (LexError, ParseError, SemanticError, TypeCheckError) as exc:
        print(exc)
        return 1
    except FileNotFoundError as exc:
        print(f"night: file not found: {exc.filename}")
        return 1
