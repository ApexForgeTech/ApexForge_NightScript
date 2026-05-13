CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -pedantic -g \
          -Isrc -Isrc/lexer -Isrc/parser -Isrc/common \
          -Isrc/sema -Isrc/typeck -Isrc/codegen

BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/obj

SRC       = src/main.c \
            src/common/arena.c \
            src/lexer/token.c \
            src/lexer/lexer.c \
            src/parser/parser.c \
            src/parser/ast_dump.c \
            src/sema/sema.c \
            src/typeck/typeck.c \
            src/codegen/codegen.c

OBJ       = $(SRC:src/%.c=$(OBJ_DIR)/%.o)
TARGET    = $(BUILD_DIR)/night

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	./tests/run_tests.sh

clean:
	rm -rf $(BUILD_DIR)
