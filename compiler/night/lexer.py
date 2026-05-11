from __future__ import annotations

from dataclasses import dataclass, field

from .errors import LexError
from .token import KEYWORDS, Token, TokenKind


SINGLE_CHAR_TOKENS = {
    "{": TokenKind.LBRACE,
    "}": TokenKind.RBRACE,
    "(": TokenKind.LPAREN,
    ")": TokenKind.RPAREN,
    "[": TokenKind.LBRACKET,
    "]": TokenKind.RBRACKET,
    ";": TokenKind.SEMICOLON,
    ":": TokenKind.COLON,
    ",": TokenKind.COMMA,
    ".": TokenKind.DOT,
    "=": TokenKind.EQUAL,
    "+": TokenKind.PLUS,
    "-": TokenKind.MINUS,
    "*": TokenKind.STAR,
    "/": TokenKind.SLASH,
    "%": TokenKind.PERCENT,
    "&": TokenKind.AMPERSAND,
    "|": TokenKind.PIPE,
    "^": TokenKind.CARET,
    "~": TokenKind.TILDE,
    "!": TokenKind.BANG,
    "<": TokenKind.LT,
    ">": TokenKind.GT,
    "?": TokenKind.QUESTION,
    "@": TokenKind.AT,
}

DOUBLE_CHAR_TOKENS = {
    "->": TokenKind.ARROW,
    "==": TokenKind.EQEQ,
    "!=": TokenKind.NE,
    "<=": TokenKind.LE,
    ">=": TokenKind.GE,
    "&&": TokenKind.ANDAND,
    "||": TokenKind.OROR,
}

ESCAPE_SEQUENCES = {
    "n": "\n",
    "r": "\r",
    "t": "\t",
    "0": "\0",
    "\\": "\\",
    "\"": "\"",
    "'": "'",
}


@dataclass(slots=True)
class Lexer:
    source: str
    source_name: str = "<memory>"
    index: int = field(init=False, default=0)
    line: int = field(init=False, default=1)
    column: int = field(init=False, default=1)

    def __post_init__(self) -> None:
        self.index = 0
        self.line = 1
        self.column = 1

    def tokenize(self) -> list[Token]:
        tokens: list[Token] = []

        while not self._is_at_end():
            self._skip_whitespace_and_comments()
            if self._is_at_end():
                break
            tokens.append(self._next_token())

        tokens.append(
            Token(
                kind=TokenKind.EOF,
                lexeme="",
                line=self.line,
                column=self.column,
                end_line=self.line,
                end_column=self.column,
            )
        )
        return tokens

    def _next_token(self) -> Token:
        start_index = self.index
        start_line = self.line
        start_column = self.column

        char = self._peek()
        two_chars = self.source[self.index : self.index + 2]

        if two_chars in DOUBLE_CHAR_TOKENS:
            self._advance()
            self._advance()
            return self._make_token(
                DOUBLE_CHAR_TOKENS[two_chars],
                start_index,
                start_line,
                start_column,
            )

        if char.isalpha() or char == "_":
            return self._identifier_or_keyword(start_index, start_line, start_column)

        if char.isdigit():
            return self._number(start_index, start_line, start_column)

        if char == "\"":
            return self._string(start_index, start_line, start_column)

        if char == "'":
            return self._char(start_index, start_line, start_column)

        token_kind = SINGLE_CHAR_TOKENS.get(char)
        if token_kind is not None:
            self._advance()
            return self._make_token(token_kind, start_index, start_line, start_column)

        raise self._error(f"unexpected character {char!r}", start_line, start_column)

    def _identifier_or_keyword(self, start: int, line: int, column: int) -> Token:
        while self._peek().isalnum() or self._peek() == "_":
            self._advance()
        lexeme = self.source[start : self.index]
        kind = KEYWORDS.get(lexeme, TokenKind.IDENTIFIER)
        literal = None
        if kind == TokenKind.TRUE:
            literal = True
        elif kind == TokenKind.FALSE:
            literal = False
        elif kind == TokenKind.NULL:
            literal = None
        return Token(kind, lexeme, line, column, self.line, self.column, literal)

    def _number(self, start: int, line: int, column: int) -> Token:
        if self._peek() == "0" and self._peek_next() in {"x", "X"}:
            self._advance()
            self._advance()
            if not self._peek().isalnum():
                raise self._error("expected hexadecimal digits after 0x", line, column)
            while self._peek().isdigit() or self._peek().lower() in "abcdef" or self._peek() == "_":
                self._advance()
            lexeme = self.source[start : self.index]
            value = int(lexeme.replace("_", ""), 16)
            return Token(TokenKind.INTEGER, lexeme, line, column, self.line, self.column, value)

        while self._peek().isdigit() or self._peek() == "_":
            self._advance()

        is_float = False
        if self._peek() == "." and self._peek_next().isdigit():
            is_float = True
            self._advance()
            while self._peek().isdigit() or self._peek() == "_":
                self._advance()

        if self._peek().lower() == "e":
            is_float = True
            self._advance()
            if self._peek() in {"+", "-"}:
                self._advance()
            if not self._peek().isdigit():
                raise self._error("expected digits after exponent marker", self.line, self.column)
            while self._peek().isdigit() or self._peek() == "_":
                self._advance()

        lexeme = self.source[start : self.index]
        clean = lexeme.replace("_", "")
        if is_float:
            value = float(clean)
            return Token(TokenKind.FLOAT, lexeme, line, column, self.line, self.column, value)
        value = int(clean, 10)
        return Token(TokenKind.INTEGER, lexeme, line, column, self.line, self.column, value)

    def _string(self, start: int, line: int, column: int) -> Token:
        self._advance()
        chars: list[str] = []

        while not self._is_at_end() and self._peek() != "\"":
            if self._peek() == "\n":
                raise self._error("unterminated string literal", line, column)
            chars.append(self._consume_string_char())

        if self._is_at_end():
            raise self._error("unterminated string literal", line, column)

        self._advance()
        lexeme = self.source[start : self.index]
        return Token(TokenKind.STRING, lexeme, line, column, self.line, self.column, "".join(chars))

    def _char(self, start: int, line: int, column: int) -> Token:
        self._advance()
        if self._is_at_end() or self._peek() == "\n":
            raise self._error("unterminated character literal", line, column)

        value = self._consume_string_char()
        if self._peek() != "'":
            raise self._error("character literal must contain exactly one character", line, column)

        self._advance()
        lexeme = self.source[start : self.index]
        return Token(TokenKind.CHAR, lexeme, line, column, self.line, self.column, value)

    def _consume_string_char(self) -> str:
        char = self._advance()
        if char != "\\":
            return char

        escape = self._advance()
        if escape == "\0":
            raise self._error("unterminated escape sequence", self.line, self.column)
        try:
            return ESCAPE_SEQUENCES[escape]
        except KeyError as exc:
            raise self._error(f"unsupported escape sequence \\{escape}", self.line, self.column) from exc

    def _skip_whitespace_and_comments(self) -> None:
        while not self._is_at_end():
            char = self._peek()
            if char in {" ", "\t", "\r", "\n"}:
                self._advance()
                continue
            if char == "/" and self._peek_next() == "/":
                self._advance()
                self._advance()
                while not self._is_at_end() and self._peek() != "\n":
                    self._advance()
                continue
            if char == "/" and self._peek_next() == "*":
                self._advance()
                self._advance()
                self._skip_block_comment()
                continue
            break

    def _skip_block_comment(self) -> None:
        depth = 1
        while depth > 0:
            if self._is_at_end():
                raise self._error("unterminated block comment", self.line, self.column)

            if self._peek() == "/" and self._peek_next() == "*":
                depth += 1
                self._advance()
                self._advance()
                continue

            if self._peek() == "*" and self._peek_next() == "/":
                depth -= 1
                self._advance()
                self._advance()
                continue

            self._advance()

    def _make_token(self, kind: TokenKind, start: int, line: int, column: int) -> Token:
        return Token(kind, self.source[start : self.index], line, column, self.line, self.column)

    def _advance(self) -> str:
        if self._is_at_end():
            return "\0"

        char = self.source[self.index]
        self.index += 1
        if char == "\n":
            self.line += 1
            self.column = 1
        else:
            self.column += 1
        return char

    def _peek(self) -> str:
        if self._is_at_end():
            return "\0"
        return self.source[self.index]

    def _peek_next(self) -> str:
        if self.index + 1 >= len(self.source):
            return "\0"
        return self.source[self.index + 1]

    def _is_at_end(self) -> bool:
        return self.index >= len(self.source)

    def _error(self, message: str, line: int, column: int) -> LexError:
        return LexError(message=message, line=line, column=column, source_name=self.source_name)


def tokenize(source: str, source_name: str = "<memory>") -> list[Token]:
    return Lexer(source=source, source_name=source_name).tokenize()
