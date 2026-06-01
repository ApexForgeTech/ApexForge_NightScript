CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -pedantic -g \
          -Isrc -Isrc/lexer -Isrc/parser -Isrc/common \
          -Isrc/sema -Isrc/typeck -Isrc/llvmgen
DEPFLAGS = -MMD -MP

BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/obj

# ── LLVM backend (required — auto-detect llvm-config-18, then llvm-config) ──
LLVM_CONFIG := $(shell command -v llvm-config-18 2>/dev/null || command -v llvm-config 2>/dev/null)
ifeq ($(LLVM_CONFIG),)
    $(error LLVM not found. Install llvm-dev (e.g. apt install llvm-18-dev))
endif

LLVM_CFLAGS  := $(shell $(LLVM_CONFIG) --cflags)
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --libs core analysis target native passes 2>/dev/null || \
                         $(LLVM_CONFIG) --ldflags --libs all)
CFLAGS       += $(LLVM_CFLAGS)
LDFLAGS       = $(LLVM_LDFLAGS)

$(info LLVM backend enabled via $(LLVM_CONFIG))

SRC = src/main.c \
      src/common/arena.c \
      src/lexer/token.c \
      src/lexer/lexer.c \
      src/parser/parser.c \
      src/parser/ast_dump.c \
      src/sema/sema.c \
      src/typeck/typeck.c \
      src/llvmgen/llvmgen.c

OBJ    = $(SRC:src/%.c=$(OBJ_DIR)/%.o)
DEP    = $(OBJ:.o=.d)
TARGET = $(BUILD_DIR)/night

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ) $(LDFLAGS)

$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

test: $(TARGET)
	./tests/run_tests.sh

clean:
	rm -rf $(BUILD_DIR)

-include $(DEP)
