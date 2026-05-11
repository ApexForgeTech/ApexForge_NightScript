from __future__ import annotations

from dataclasses import dataclass, field

from . import ast
from .errors import ParseError
from .lexer import tokenize
from .token import Token, TokenKind


@dataclass(slots=True)
class Parser:
    tokens: list[Token]
    source_name: str = "<memory>"
    index: int = field(init=False, default=0)

    def __post_init__(self) -> None:
        self.index = 0

    def parse_program(self) -> ast.Program:
        package = None
        imports: list[ast.ImportDecl] = []
        if self._match(TokenKind.PACKAGE):
            package = self._parse_package_decl()
        while self._match(TokenKind.IMPORT):
            imports.append(self._parse_import_decl())

        declarations: list[ast.Decl] = []
        while not self._check(TokenKind.EOF):
            declarations.append(self._parse_declaration())

        return ast.Program(package=package, imports=imports, declarations=declarations)

    def _parse_package_decl(self) -> ast.PackageDecl:
        package_token = self._previous()
        path = self._parse_path()
        self._expect(TokenKind.SEMICOLON, "expected ';' after package declaration")
        return ast.PackageDecl(path=path, line=package_token.line, column=package_token.column)

    def _parse_import_decl(self) -> ast.ImportDecl:
        import_token = self._previous()
        path = self._parse_path()
        self._expect(TokenKind.SEMICOLON, "expected ';' after import declaration")
        return ast.ImportDecl(path=path, line=import_token.line, column=import_token.column)

    def _parse_declaration(self) -> ast.Decl:
        is_public = self._match(TokenKind.PUB)

        if self._match(TokenKind.STRUCT):
            return self._parse_struct_decl(is_public=is_public)

        if self._match(TokenKind.EXTERN):
            return self._parse_extern_function(is_public=is_public)

        if self._match(TokenKind.FN):
            return self._parse_function_decl(is_public=is_public)

        token = self._current()
        raise self._error(token, f"unexpected top-level token {token.kind.value}")

    def _parse_struct_decl(self, is_public: bool) -> ast.StructDecl:
        struct_token = self._previous()
        name_token = self._expect(TokenKind.IDENTIFIER, "expected struct name")
        self._expect(TokenKind.LBRACE, "expected '{' after struct name")
        fields: list[ast.FieldDecl] = []
        while not self._check(TokenKind.RBRACE):
            field_name = self._expect(TokenKind.IDENTIFIER, "expected field name")
            self._expect(TokenKind.COLON, "expected ':' after field name")
            field_type = self._parse_type()
            self._expect(TokenKind.SEMICOLON, "expected ';' after struct field")
            fields.append(
                ast.FieldDecl(
                    name=field_name.lexeme,
                    type_ref=field_type,
                    line=field_name.line,
                    column=field_name.column,
                )
            )
        self._expect(TokenKind.RBRACE, "expected '}' after struct body")
        return ast.StructDecl(
            name=name_token.lexeme,
            fields=fields,
            is_public=is_public,
            line=struct_token.line,
            column=struct_token.column,
        )

    def _parse_extern_function(self, is_public: bool) -> ast.ExternFunctionDecl:
        extern_token = self._previous()
        calling_convention = None
        if self._check(TokenKind.STRING):
            calling_convention = self._advance().literal
            if not isinstance(calling_convention, str):
                raise self._error(self._previous(), "extern calling convention must be a string literal")

        self._expect(TokenKind.FN, "expected 'fn' after extern declaration")
        name_token = self._expect(TokenKind.IDENTIFIER, "expected function name")
        params = self._parse_params()
        self._expect(TokenKind.ARROW, "expected '->' before return type")
        return_type = self._parse_type()
        self._expect(TokenKind.SEMICOLON, "expected ';' after extern declaration")

        return ast.ExternFunctionDecl(
            name=name_token.lexeme,
            params=params,
            return_type=return_type,
            calling_convention=calling_convention,
            is_public=is_public,
            line=extern_token.line,
            column=extern_token.column,
        )

    def _parse_function_decl(self, is_public: bool) -> ast.FunctionDecl:
        fn_token = self._previous()
        name_token = self._expect(TokenKind.IDENTIFIER, "expected function name")
        params = self._parse_params()
        self._expect(TokenKind.ARROW, "expected '->' before return type")
        return_type = self._parse_type()
        body = self._parse_block()
        return ast.FunctionDecl(
            name=name_token.lexeme,
            params=params,
            return_type=return_type,
            body=body,
            is_public=is_public,
            line=fn_token.line,
            column=fn_token.column,
        )

    def _parse_params(self) -> list[ast.Param]:
        self._expect(TokenKind.LPAREN, "expected '('")
        params: list[ast.Param] = []
        if not self._check(TokenKind.RPAREN):
            while True:
                name = self._expect(TokenKind.IDENTIFIER, "expected parameter name")
                self._expect(TokenKind.COLON, "expected ':' after parameter name")
                type_ref = self._parse_type()
                params.append(ast.Param(name=name.lexeme, type_ref=type_ref, line=name.line, column=name.column))
                if not self._match(TokenKind.COMMA):
                    break
        self._expect(TokenKind.RPAREN, "expected ')' after parameter list")
        return params

    def _parse_type(self) -> ast.TypeRef:
        start_token = self._current()
        is_nullable = self._match(TokenKind.QUESTION)

        if self._match(TokenKind.STAR):
            is_const = self._match(TokenKind.CONST)
            inner = self._parse_type()
            return ast.PointerType(
                inner=inner,
                is_const=is_const,
                is_nullable=is_nullable,
                line=start_token.line,
                column=start_token.column,
            )

        if self._match(TokenKind.LBRACKET):
            length = None
            if self._check(TokenKind.INTEGER):
                length_token = self._advance()
                if not isinstance(length_token.literal, int):
                    raise self._error(length_token, "array length must be an integer literal")
                length = length_token.literal
            self._expect(TokenKind.RBRACKET, "expected ']' in array or slice type")
            element_type = self._parse_type()
            if is_nullable:
                raise self._error(self._previous(), "nullable array or slice types are not supported yet")
            return ast.ArrayType(
                element_type=element_type,
                length=length,
                line=start_token.line,
                column=start_token.column,
            )

        path = self._parse_path(allow_self=True)
        if is_nullable:
            raise self._error(self._previous(), "nullable is only supported on pointer types")
        return ast.NamedType(path=path, line=start_token.line, column=start_token.column)

    def _parse_path(self, allow_self: bool = False) -> list[str]:
        first_kinds = [TokenKind.IDENTIFIER]
        if allow_self:
            first_kinds.append(TokenKind.SELF)

        if self._check(*first_kinds):
            token = self._advance()
            path = [token.lexeme]
        else:
            raise self._error(self._current(), "expected identifier")

        while self._match(TokenKind.DOT):
            path.append(self._expect(TokenKind.IDENTIFIER, "expected identifier after '.'").lexeme)
        return path

    def _parse_block(self) -> ast.BlockStmt:
        brace = self._expect(TokenKind.LBRACE, "expected '{' to start block")
        statements: list[ast.Stmt] = []
        while not self._check(TokenKind.RBRACE):
            statements.append(self._parse_statement())
        self._expect(TokenKind.RBRACE, "expected '}' after block")
        return ast.BlockStmt(statements=statements, line=brace.line, column=brace.column)

    def _parse_statement(self) -> ast.Stmt:
        if self._match(TokenKind.CONST):
            return self._parse_const_stmt()
        if self._match(TokenKind.LET):
            return self._parse_let_stmt()
        if self._match(TokenKind.IF):
            return self._parse_if_stmt()
        if self._match(TokenKind.WHILE):
            return self._parse_while_stmt()
        if self._match(TokenKind.LOOP):
            return self._parse_loop_stmt()
        if self._match(TokenKind.BREAK):
            return self._parse_break_stmt()
        if self._match(TokenKind.CONTINUE):
            return self._parse_continue_stmt()
        if self._match(TokenKind.RETURN):
            return self._parse_return_stmt()
        expr = self._parse_expression()
        self._expect(TokenKind.SEMICOLON, "expected ';' after expression")
        return ast.ExprStmt(expr=expr, line=expr.line, column=expr.column)

    def _parse_const_stmt(self) -> ast.ConstStmt:
        const_token = self._previous()
        name = self._expect(TokenKind.IDENTIFIER, "expected constant name")
        type_ref = None
        if self._match(TokenKind.COLON):
            type_ref = self._parse_type()
        self._expect(TokenKind.EQUAL, "expected '=' in const statement")
        value = self._parse_expression()
        self._expect(TokenKind.SEMICOLON, "expected ';' after const statement")
        return ast.ConstStmt(
            name=name.lexeme,
            type_ref=type_ref,
            value=value,
            line=const_token.line,
            column=const_token.column,
        )

    def _parse_let_stmt(self) -> ast.LetStmt:
        let_token = self._previous()
        name = self._expect(TokenKind.IDENTIFIER, "expected variable name")
        type_ref = None
        value = None
        if self._match(TokenKind.COLON):
            type_ref = self._parse_type()
        if self._match(TokenKind.EQUAL):
            value = self._parse_expression()
        self._expect(TokenKind.SEMICOLON, "expected ';' after let statement")
        return ast.LetStmt(
            name=name.lexeme,
            type_ref=type_ref,
            value=value,
            line=let_token.line,
            column=let_token.column,
        )

    def _parse_if_stmt(self) -> ast.IfStmt:
        if_token = self._previous()
        condition = self._parse_expression()
        then_branch = self._parse_block()
        else_branch: ast.BlockStmt | ast.IfStmt | None = None
        if self._match(TokenKind.ELSE):
            if self._match(TokenKind.IF):
                else_branch = self._parse_if_stmt()
            else:
                else_branch = self._parse_block()
        return ast.IfStmt(
            condition=condition,
            then_branch=then_branch,
            else_branch=else_branch,
            line=if_token.line,
            column=if_token.column,
        )

    def _parse_while_stmt(self) -> ast.WhileStmt:
        while_token = self._previous()
        condition = self._parse_expression()
        body = self._parse_block()
        return ast.WhileStmt(condition=condition, body=body, line=while_token.line, column=while_token.column)

    def _parse_loop_stmt(self) -> ast.LoopStmt:
        loop_token = self._previous()
        body = self._parse_block()
        return ast.LoopStmt(body=body, line=loop_token.line, column=loop_token.column)

    def _parse_break_stmt(self) -> ast.BreakStmt:
        break_token = self._previous()
        self._expect(TokenKind.SEMICOLON, "expected ';' after break")
        return ast.BreakStmt(line=break_token.line, column=break_token.column)

    def _parse_continue_stmt(self) -> ast.ContinueStmt:
        continue_token = self._previous()
        self._expect(TokenKind.SEMICOLON, "expected ';' after continue")
        return ast.ContinueStmt(line=continue_token.line, column=continue_token.column)

    def _parse_return_stmt(self) -> ast.ReturnStmt:
        return_token = self._previous()
        value = None
        if not self._check(TokenKind.SEMICOLON):
            value = self._parse_expression()
        self._expect(TokenKind.SEMICOLON, "expected ';' after return statement")
        return ast.ReturnStmt(value=value, line=return_token.line, column=return_token.column)

    def _parse_expression(self) -> ast.Expr:
        return self._parse_assignment()

    def _parse_assignment(self) -> ast.Expr:
        expr = self._parse_logical_or()
        if self._match(TokenKind.EQUAL):
            value = self._parse_assignment()
            return ast.AssignExpr(target=expr, value=value, line=expr.line, column=expr.column)
        return expr

    def _parse_logical_or(self) -> ast.Expr:
        return self._parse_left_associative(self._parse_logical_and, {TokenKind.OROR})

    def _parse_logical_and(self) -> ast.Expr:
        return self._parse_left_associative(self._parse_equality, {TokenKind.ANDAND})

    def _parse_equality(self) -> ast.Expr:
        return self._parse_left_associative(self._parse_comparison, {TokenKind.EQEQ, TokenKind.NE})

    def _parse_comparison(self) -> ast.Expr:
        return self._parse_left_associative(
            self._parse_term,
            {TokenKind.LT, TokenKind.GT, TokenKind.LE, TokenKind.GE},
        )

    def _parse_term(self) -> ast.Expr:
        return self._parse_left_associative(self._parse_factor, {TokenKind.PLUS, TokenKind.MINUS})

    def _parse_factor(self) -> ast.Expr:
        return self._parse_left_associative(
            self._parse_cast,
            {TokenKind.STAR, TokenKind.SLASH, TokenKind.PERCENT},
        )

    def _parse_cast(self) -> ast.Expr:
        expr = self._parse_unary()
        while self._match(TokenKind.AS):
            expr = ast.CastExpr(
                expr=expr,
                type_ref=self._parse_type(),
                line=expr.line,
                column=expr.column,
            )
        return expr

    def _parse_unary(self) -> ast.Expr:
        if self._match(TokenKind.BANG, TokenKind.MINUS, TokenKind.STAR, TokenKind.AMPERSAND):
            token = self._previous()
            operator = token.lexeme
            operand = self._parse_unary()
            return ast.UnaryExpr(operator=operator, operand=operand, line=token.line, column=token.column)
        return self._parse_postfix()

    def _parse_postfix(self) -> ast.Expr:
        expr = self._parse_primary()
        while True:
            if self._match(TokenKind.LPAREN):
                args: list[ast.Expr] = []
                if not self._check(TokenKind.RPAREN):
                    while True:
                        args.append(self._parse_expression())
                        if not self._match(TokenKind.COMMA):
                            break
                self._expect(TokenKind.RPAREN, "expected ')' after arguments")
                expr = ast.CallExpr(callee=expr, arguments=args, line=expr.line, column=expr.column)
                continue
            if self._match(TokenKind.DOT):
                field_token = self._expect(TokenKind.IDENTIFIER, "expected field name after '.'")
                expr = ast.FieldAccessExpr(
                    object=expr,
                    field=field_token.lexeme,
                    line=field_token.line,
                    column=field_token.column,
                )
                continue
            break
        return expr

    def _parse_primary(self) -> ast.Expr:
        if self._match(TokenKind.INTEGER, TokenKind.FLOAT, TokenKind.STRING, TokenKind.CHAR):
            token = self._previous()
            return ast.LiteralExpr(value=token.literal, line=token.line, column=token.column)

        if self._match(TokenKind.TRUE):
            token = self._previous()
            return ast.LiteralExpr(value=True, line=token.line, column=token.column)
        if self._match(TokenKind.FALSE):
            token = self._previous()
            return ast.LiteralExpr(value=False, line=token.line, column=token.column)
        if self._match(TokenKind.NULL):
            token = self._previous()
            return ast.LiteralExpr(value=None, line=token.line, column=token.column)
        if self._match(TokenKind.IDENTIFIER, TokenKind.SELF):
            token = self._previous()
            if token.kind == TokenKind.IDENTIFIER and self._looks_like_struct_literal():
                return self._parse_struct_literal(token)
            return ast.IdentifierExpr(name=token.lexeme, line=token.line, column=token.column)
        if self._match(TokenKind.LPAREN):
            paren = self._previous()
            expr = self._parse_expression()
            self._expect(TokenKind.RPAREN, "expected ')' after expression")
            return ast.GroupExpr(expr=expr, line=paren.line, column=paren.column)

        raise self._error(self._current(), f"unexpected token in expression: {self._current().kind.value}")

    def _parse_struct_literal(self, type_token: Token) -> ast.StructLiteralExpr:
        self._expect(TokenKind.LBRACE, "expected '{' after struct literal type name")
        fields: list[ast.FieldInit] = []
        while not self._check(TokenKind.RBRACE):
            field_name = self._expect(TokenKind.IDENTIFIER, "expected field name in struct literal")
            self._expect(TokenKind.COLON, "expected ':' after field name")
            value = self._parse_expression()
            fields.append(
                ast.FieldInit(
                    name=field_name.lexeme,
                    value=value,
                    line=field_name.line,
                    column=field_name.column,
                )
            )
            if not self._match(TokenKind.COMMA):
                break
        self._expect(TokenKind.RBRACE, "expected '}' after struct literal")
        return ast.StructLiteralExpr(
            type_name=type_token.lexeme,
            fields=fields,
            line=type_token.line,
            column=type_token.column,
        )

    def _looks_like_struct_literal(self) -> bool:
        if not self._check(TokenKind.LBRACE):
            return False
        if self.index + 1 >= len(self.tokens):
            return False
        next_kind = self.tokens[self.index + 1].kind
        if next_kind == TokenKind.RBRACE:
            return True
        if next_kind != TokenKind.IDENTIFIER:
            return False
        if self.index + 2 >= len(self.tokens):
            return False
        return self.tokens[self.index + 2].kind == TokenKind.COLON

    def _parse_left_associative(
        self,
        parse_operand,
        operators: set[TokenKind],
    ) -> ast.Expr:
        expr = parse_operand()
        while self._match(*operators):
            token = self._previous()
            operator = token.lexeme
            right = parse_operand()
            expr = ast.BinaryExpr(left=expr, operator=operator, right=right, line=token.line, column=token.column)
        return expr

    def _match(self, *kinds: TokenKind) -> bool:
        if self._check(*kinds):
            self._advance()
            return True
        return False

    def _expect(self, kind: TokenKind, message: str) -> Token:
        if self._check(kind):
            return self._advance()
        raise self._error(self._current(), message)

    def _check(self, *kinds: TokenKind) -> bool:
        if self._is_at_end():
            return TokenKind.EOF in kinds
        return self._current().kind in kinds

    def _advance(self) -> Token:
        if not self._is_at_end():
            self.index += 1
        return self._previous()

    def _previous(self) -> Token:
        return self.tokens[self.index - 1]

    def _current(self) -> Token:
        return self.tokens[self.index]

    def _is_at_end(self) -> bool:
        return self._current().kind == TokenKind.EOF

    def _error(self, token: Token, message: str) -> ParseError:
        return ParseError(
            message=message,
            line=token.line,
            column=token.column,
            source_name=self.source_name,
        )


def parse(source: str, source_name: str = "<memory>") -> ast.Program:
    tokens = tokenize(source, source_name=source_name)
    parser = Parser(tokens=tokens, source_name=source_name)
    return parser.parse_program()
