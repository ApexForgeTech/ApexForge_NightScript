CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -pedantic -g \
          -Isrc -Isrc/lexer -Isrc/parser -Isrc/common \
          -Isrc/sema -Isrc/typeck -Isrc/codegen

SRC     = src/main.c \
          src/common/arena.c \
          src/lexer/token.c \
          src/lexer/lexer.c \
          src/parser/parser.c \
          src/parser/ast_dump.c \
          src/sema/sema.c \
          src/typeck/typeck.c \
          src/codegen/codegen.c

TARGET  = night

.PHONY: clean test

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

test: $(TARGET)
	./tests/run_tests.sh

clean:
	rm -f $(TARGET)
