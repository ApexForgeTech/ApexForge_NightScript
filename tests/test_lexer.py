from __future__ import annotations

from pathlib import Path
import subprocess

from night.check import check_source
from night.cli import main
from night.errors import LexError, SemanticError, TypeCheckError
from night.lexer import tokenize
from night.parser import parse
from night.token import TokenKind


def token_kinds(source: str) -> list[TokenKind]:
    return [token.kind for token in tokenize(source)]


def test_lexes_keywords_and_structure() -> None:
    source = """
package main;

fn main() -> i32 {
    return 0;
}
"""
    assert token_kinds(source) == [
        TokenKind.PACKAGE,
        TokenKind.IDENTIFIER,
        TokenKind.SEMICOLON,
        TokenKind.FN,
        TokenKind.IDENTIFIER,
        TokenKind.LPAREN,
        TokenKind.RPAREN,
        TokenKind.ARROW,
        TokenKind.IDENTIFIER,
        TokenKind.LBRACE,
        TokenKind.RETURN,
        TokenKind.INTEGER,
        TokenKind.SEMICOLON,
        TokenKind.RBRACE,
        TokenKind.EOF,
    ]


def test_lexes_literals_comments_and_operators() -> None:
    source = """
// line comment
/* block
   /* nested */
*/
let ptr: *u8 = 0xB8000 as *u8;
let f: f64 = 10.5e1;
let s: str = "hi\\n";
let c: char = '\\t';
if true && false || null == null {
    ptr = ptr + 1;
}
"""
    tokens = tokenize(source)
    assert any(token.literal == 0xB8000 for token in tokens)
    assert any(token.kind == TokenKind.FLOAT and token.literal == 105.0 for token in tokens)
    assert any(token.kind == TokenKind.STRING and token.literal == "hi\n" for token in tokens)
    assert any(token.kind == TokenKind.CHAR and token.literal == "\t" for token in tokens)
    assert TokenKind.ANDAND in [token.kind for token in tokens]
    assert TokenKind.OROR in [token.kind for token in tokens]
    assert TokenKind.EQEQ in [token.kind for token in tokens]


def test_tracks_token_locations() -> None:
    tokens = tokenize("let answer = 42;\n")
    answer = tokens[1]
    number = tokens[3]
    assert (answer.line, answer.column, answer.end_line, answer.end_column) == (1, 5, 1, 11)
    assert (number.line, number.column, number.end_line, number.end_column) == (1, 14, 1, 16)


def test_reports_unterminated_string() -> None:
    try:
        tokenize('let s = "oops')
    except LexError as exc:
        assert "unterminated string literal" in str(exc)
    else:
        raise AssertionError("expected LexError")


def test_cli_lex_smoke(tmp_path: Path, capsys) -> None:
    source_file = tmp_path / "hello.afns"
    source_file.write_text("fn main() -> i32 { return 0; }\n", encoding="utf-8")

    exit_code = main(["lex", str(source_file)])
    output = capsys.readouterr().out

    assert exit_code == 0
    assert "FN 'fn'" in output
    assert "RETURN 'return'" in output
    assert "EOF ''" in output


def test_parser_builds_core_ast() -> None:
    program = parse(
        """
package main;
import std.io;

extern "C" fn puts(s: cstr) -> i32;

fn main() -> i32 {
    let message: str = "Hello";
    puts(message);
    return 0;
}
"""
    )

    assert program.package is not None
    assert program.package.path == ["main"]
    assert program.imports[0].path == ["std", "io"]
    assert len(program.declarations) == 2
    extern_decl = program.declarations[0]
    fn_decl = program.declarations[1]
    assert extern_decl.name == "puts"
    assert fn_decl.name == "main"
    assert fn_decl.body.statements[0].name == "message"
    assert fn_decl.body.statements[1].expr.callee.name == "puts"


def test_parser_builds_control_flow_ast() -> None:
    program = parse(
        """
fn main() -> i32 {
    const limit: i32 = 3;
    let i: i32 = 0;
    while i < limit {
        if i == 1 {
            i = i + 1;
            continue;
        } else {
            break;
        }
    }
    loop {
        break;
    }
    return 0;
}
"""
    )

    fn_decl = program.declarations[0]
    while_stmt = fn_decl.body.statements[2]
    loop_stmt = fn_decl.body.statements[3]
    assert while_stmt.condition.operator == "<"
    assert while_stmt.body.statements[0].condition.operator == "=="
    assert loop_stmt.body.statements[0].__class__.__name__ == "BreakStmt"


def test_parser_builds_struct_ast() -> None:
    program = parse(
        """
struct Point {
    x: i32;
    y: i32;
}

fn main() -> i32 {
    let point = Point { x: 1, y: 2 };
    return point.x;
}
"""
    )

    struct_decl = program.declarations[0]
    fn_decl = program.declarations[1]
    assert struct_decl.name == "Point"
    assert struct_decl.fields[0].name == "x"
    assert fn_decl.body.statements[0].value.type_name == "Point"
    assert fn_decl.body.statements[1].value.field == "x"


def test_cli_parse_smoke(tmp_path: Path, capsys) -> None:
    source_file = tmp_path / "hello.afns"
    source_file.write_text("fn main() -> i32 { return 0; }\n", encoding="utf-8")

    exit_code = main(["parse", str(source_file)])
    output = capsys.readouterr().out

    assert exit_code == 0
    assert "Program(" in output
    assert "FunctionDecl(" in output


def test_check_accepts_valid_program() -> None:
    result = check_source(
        """
package main;

extern "C" fn puts(s: cstr) -> i32;

fn main() -> i32 {
    puts("Hello");
    return 0;
}
"""
    )
    assert "main" in result.semantic_model.functions


def test_check_reports_unknown_symbol() -> None:
    try:
        check_source(
            """
fn main() -> i32 {
    return missing;
}
"""
        )
    except SemanticError as exc:
        assert "unknown symbol 'missing'" in str(exc)
    else:
        raise AssertionError("expected SemanticError")


def test_check_reports_type_mismatch() -> None:
    try:
        check_source(
            """
fn main() -> i32 {
    let flag: bool = 1;
    return 0;
}
"""
        )
    except TypeCheckError as exc:
        assert "cannot assign <int literal> to bool" in str(exc)
    else:
        raise AssertionError("expected TypeCheckError")


def test_check_reports_break_outside_loop() -> None:
    try:
        check_source(
            """
fn main() -> i32 {
    break;
}
"""
        )
    except SemanticError as exc:
        assert "'break' can only be used inside a loop" in str(exc)
    else:
        raise AssertionError("expected SemanticError")


def test_check_reports_assignment_to_const() -> None:
    try:
        check_source(
            """
fn main() -> i32 {
    const x: i32 = 1;
    x = 2;
    return 0;
}
"""
        )
    except TypeCheckError as exc:
        assert "cannot assign to immutable binding 'x'" in str(exc)
    else:
        raise AssertionError("expected TypeCheckError")


def test_check_accepts_structs_and_field_updates() -> None:
    result = check_source(
        """
struct Counter {
    value: i32;
}

fn main() -> i32 {
    let counter: Counter = Counter { value: 1 };
    counter.value = counter.value + 1;
    return counter.value;
}
"""
    )

    assert "Counter" in result.semantic_model.structs


def test_check_reports_unknown_struct_field() -> None:
    try:
        check_source(
            """
struct Point {
    x: i32;
}

fn main() -> i32 {
    let point = Point { x: 1 };
    return point.y;
}
"""
        )
    except TypeCheckError as exc:
        assert "unknown field 'y' for struct 'Point'" in str(exc)
    else:
        raise AssertionError("expected TypeCheckError")


def test_cli_check_smoke(tmp_path: Path, capsys) -> None:
    source_file = tmp_path / "hello.afns"
    source_file.write_text(
        """
package main;
extern "C" fn puts(s: cstr) -> i32;
fn main() -> i32 {
    puts("Hello");
    return 0;
}
""",
        encoding="utf-8",
    )

    exit_code = main(["check", str(source_file)])
    output = capsys.readouterr().out

    assert exit_code == 0
    assert "OK: package=main imports=0 functions=2" in output


def test_cli_codegen_smoke(tmp_path: Path, capsys) -> None:
    source_file = tmp_path / "hello.afns"
    source_file.write_text(
        """
extern "C" fn puts(s: cstr) -> i32;
fn main() -> i32 {
    puts("Hello");
    return 0;
}
""",
        encoding="utf-8",
    )

    exit_code = main(["codegen", str(source_file)])
    output = capsys.readouterr().out

    assert exit_code == 0
    assert "#include <stdint.h>" in output
    assert "int32_t main(void)" in output


def test_cli_codegen_struct_smoke(tmp_path: Path, capsys) -> None:
    source_file = tmp_path / "point.afns"
    source_file.write_text(
        """
struct Point {
    x: i32;
    y: i32;
}

fn main() -> i32 {
    let point = Point { x: 3, y: 4 };
    return point.x + point.y;
}
""",
        encoding="utf-8",
    )

    exit_code = main(["codegen", str(source_file)])
    output = capsys.readouterr().out

    assert exit_code == 0
    assert "typedef struct Point {" in output
    assert "Point point = (Point){ .x = 3, .y = 4 };" in output


def test_cli_build_smoke(tmp_path: Path, capsys) -> None:
    source_file = tmp_path / "hello.afns"
    binary_file = tmp_path / "hello-bin"
    c_file = tmp_path / "hello.c"
    source_file.write_text(
        """
extern "C" fn puts(s: cstr) -> i32;
fn main() -> i32 {
    puts("Hello NightScript");
    return 0;
}
""",
        encoding="utf-8",
    )

    exit_code = main(["build", str(source_file), "-o", str(binary_file), "--emit-c", str(c_file)])
    output = capsys.readouterr().out

    assert exit_code == 0
    assert binary_file.exists()
    assert c_file.exists()
    run = subprocess.run([str(binary_file)], check=True, capture_output=True, text=True)
    assert run.stdout.strip() == "Hello NightScript"
    assert "binary=" in output


def test_cli_build_control_flow_program(tmp_path: Path, capsys) -> None:
    source_file = tmp_path / "control.afns"
    binary_file = tmp_path / "control-bin"
    source_file.write_text(
        """
fn main() -> i32 {
    const limit: i32 = 5;
    let i: i32 = 0;
    let sum: i32 = 0;

    while i < limit {
        i = i + 1;
        if i == 2 {
            continue;
        } else if i == 5 {
            break;
        }

        sum = sum + i;
    }

    loop {
        break;
    }

    if sum == 8 {
        return 0;
    } else {
        return 1;
    }
}
""",
        encoding="utf-8",
    )

    exit_code = main(["build", str(source_file), "-o", str(binary_file)])
    _ = capsys.readouterr().out

    assert exit_code == 0
    run = subprocess.run([str(binary_file)], capture_output=True, text=True, check=False)
    assert run.returncode == 0


def test_cli_build_struct_program(tmp_path: Path, capsys) -> None:
    source_file = tmp_path / "struct.afns"
    binary_file = tmp_path / "struct-bin"
    source_file.write_text(
        """
struct Point {
    x: i32;
    y: i32;
}

fn main() -> i32 {
    let point: Point = Point { x: 2, y: 3 };
    point.x = point.x + 5;
    if point.x == 7 {
        return 0;
    }
    return 1;
}
""",
        encoding="utf-8",
    )

    exit_code = main(["build", str(source_file), "-o", str(binary_file)])
    _ = capsys.readouterr().out

    assert exit_code == 0
    run = subprocess.run([str(binary_file)], capture_output=True, text=True, check=False)
    assert run.returncode == 0
