CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -pedantic -g \
          -Isrc -Isrc/lexer -Isrc/parser -Isrc/common \
          -Isrc/sema -Isrc/typeck -Isrc/llvmgen
DEPFLAGS = -MMD -MP

BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/obj

SRC       = src/main.c \
            src/common/arena.c \
            src/lexer/token.c \
            src/lexer/lexer.c \
            src/parser/parser.c \
            src/parser/ast_dump.c \
            src/sema/sema.c \
            src/typeck/typeck.c

OBJ       = $(SRC:src/%.c=$(OBJ_DIR)/%.o)
DEP       = $(OBJ:.o=.d)
TARGET    = $(BUILD_DIR)/night
LDFLAGS   =

# ── LLVM backend (auto-detect llvm-config-18, then llvm-config) ────────────
LLVM_CONFIG := $(shell command -v llvm-config-18 2>/dev/null || command -v llvm-config 2>/dev/null)
ifneq ($(LLVM_CONFIG),)
    LLVM_CFLAGS  := $(shell $(LLVM_CONFIG) --cflags)
    LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --libs core analysis target native passes 2>/dev/null || \
                             $(LLVM_CONFIG) --ldflags --libs all)
    CFLAGS       += $(LLVM_CFLAGS) -DNIGHT_LLVM_BACKEND=1
    LDFLAGS      += $(LLVM_LDFLAGS)
    SRC          += src/llvmgen/llvmgen.c
    OBJ           = $(SRC:src/%.c=$(OBJ_DIR)/%.o)
    $(info LLVM backend enabled via $(LLVM_CONFIG))
else
    $(info LLVM not found — building without LLVM backend)
endif

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
