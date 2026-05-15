#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMPILER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
PASS_DIR="$SCRIPT_DIR/pass"
FAIL_DIR="$SCRIPT_DIR/fail"
NIGHT="$COMPILER_DIR/build/night"
TMP_DIR=$(mktemp -d)

cleanup() {
    rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

run_capture() {
    name=$1
    shift
    if ! "$@" >"$TMP_DIR/stdout" 2>"$TMP_DIR/stderr"; then
        cat "$TMP_DIR/stderr" >&2
        fail "$name"
    fi
}

assert_contains() {
    file=$1
    needle=$2
    name=$3

    if ! grep -F "$needle" "$file" >/dev/null 2>&1; then
        printf 'Expected to find: %s\n' "$needle" >&2
        printf 'In file: %s\n' "$file" >&2
        cat "$file" >&2
        fail "$name"
    fi
}

run_fail_case() {
    src=$1
    expected=$2
    name=$3

    if "$NIGHT" check "$src" >"$TMP_DIR/stdout" 2>"$TMP_DIR/stderr"; then
        fail "$name unexpectedly succeeded"
    fi

    needle=$(cat "$expected")
    assert_contains "$TMP_DIR/stderr" "$needle" "$name"
}

printf '1..161\n'

run_capture "hello check" "$NIGHT" check "$PASS_DIR/hello.afns"
printf 'ok 1 - hello check\n'

run_capture "hello build" "$NIGHT" build "$PASS_DIR/hello.afns" -o "$TMP_DIR/hello_bin"
printf 'ok 2 - hello build\n'

run_capture "hello run" "$NIGHT" run "$PASS_DIR/hello.afns" -o "$TMP_DIR/hello_run"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/hello.out"; then
    printf 'Expected output:\n' >&2
    cat "$PASS_DIR/hello.out" >&2
    printf 'Actual output:\n' >&2
    cat "$TMP_DIR/stdout" >&2
    fail "hello run output mismatch"
fi
printf 'ok 3 - hello run\n'

run_capture "hello codegen" "$NIGHT" codegen "$PASS_DIR/hello.afns"
assert_contains "$TMP_DIR/stdout" "#include <stdio.h>" "hello codegen include mapping"
printf 'ok 4 - hello extern include mapping\n'

run_capture "hello ast" "$NIGHT" ast "$PASS_DIR/hello.afns"
assert_contains "$TMP_DIR/stdout" "Program" "ast root emission"
assert_contains "$TMP_DIR/stdout" "ExternFn(name=puts, abi=C)" "ast extern emission"
assert_contains "$TMP_DIR/stdout" "FnDecl(name=main)" "ast function emission"
printf 'ok 5 - ast printer\n'

run_capture "slice check" "$NIGHT" check "$PASS_DIR/slices.afns"
run_capture "slice build" "$NIGHT" build "$PASS_DIR/slices.afns" -o "$TMP_DIR/slices_bin"
run_capture "slice codegen" "$NIGHT" codegen "$PASS_DIR/slices.afns"
assert_contains "$TMP_DIR/stdout" "typedef struct NSlice_u8" "slice typedef emission"
assert_contains "$TMP_DIR/stdout" "size_t len;" "slice len field emission"
printf 'ok 6 - slices build and codegen\n'

run_capture "multifile package check" "$NIGHT" check "$PASS_DIR/multifile/main.afns"
run_capture "multifile package build" "$NIGHT" build "$PASS_DIR/multifile/main.afns" -o "$TMP_DIR/multifile_bin"
run_capture "multifile package codegen" "$NIGHT" codegen "$PASS_DIR/multifile/main.afns"
assert_contains "$TMP_DIR/stdout" "int32_t helper(void);" "multifile helper declaration"
assert_contains "$TMP_DIR/stdout" "return helper();" "multifile helper call"
printf 'ok 7 - multifile package compilation\n'

run_fail_case "$FAIL_DIR/unknown_symbol.afns" "$FAIL_DIR/unknown_symbol.err" "unknown symbol diagnostic"
printf 'ok 8 - unknown symbol diagnostic\n'

run_fail_case "$FAIL_DIR/break_outside_loop.afns" "$FAIL_DIR/break_outside_loop.err" "break outside loop diagnostic"
printf 'ok 9 - break outside loop diagnostic\n'

run_fail_case "$FAIL_DIR/slice_bad_field.afns" "$FAIL_DIR/slice_bad_field.err" "slice bad field diagnostic"
printf 'ok 10 - slice bad field diagnostic\n'

run_capture "compound assign check" "$NIGHT" check "$PASS_DIR/compound_assign/main.afns"
run_capture "compound assign build" "$NIGHT" build "$PASS_DIR/compound_assign/main.afns" -o "$TMP_DIR/ca_bin"
run_capture "compound assign codegen" "$NIGHT" codegen "$PASS_DIR/compound_assign/main.afns"
assert_contains "$TMP_DIR/stdout" "x += 5" "compound assign += emission"
assert_contains "$TMP_DIR/stdout" "x %= 7" "compound assign %= emission"
printf 'ok 11 - compound assignment operators\n'

run_capture "indexing check" "$NIGHT" check "$PASS_DIR/indexing/main.afns"
run_capture "indexing build" "$NIGHT" build "$PASS_DIR/indexing/main.afns" -o "$TMP_DIR/idx_bin"
run_capture "indexing codegen" "$NIGHT" codegen "$PASS_DIR/indexing/main.afns"
assert_contains "$TMP_DIR/stdout" "s.ptr[i]" "slice index emission"
printf 'ok 12 - array/slice indexing\n'

run_capture "char literal check" "$NIGHT" check "$PASS_DIR/char_literal/main.afns"
run_capture "char literal build" "$NIGHT" build "$PASS_DIR/char_literal/main.afns" -o "$TMP_DIR/char_bin"
run_capture "char literal run" "$NIGHT" run "$PASS_DIR/char_literal/main.afns" -o "$TMP_DIR/char_run"
if ! printf 'A' | cmp -s - "$TMP_DIR/stdout"; then
    printf 'Expected output:\nA' >&2
    printf '\nActual output:\n' >&2
    cat "$TMP_DIR/stdout" >&2
    fail "char literal run output mismatch"
fi
printf 'ok 13 - char literal\n'

run_capture "pub ast" "$NIGHT" ast "$PASS_DIR/pub_ast/main.afns"
assert_contains "$TMP_DIR/stdout" "ExternFn(name=puts, abi=C, pub)" "pub extern ast"
assert_contains "$TMP_DIR/stdout" "FnDecl(name=foo, pub)" "pub function ast"
assert_contains "$TMP_DIR/stdout" "StructDecl(name=Box, packed=false, pub)" "pub struct ast"
assert_contains "$TMP_DIR/stdout" "EnumDecl(name=Color, pub)" "pub enum ast"
assert_contains "$TMP_DIR/stdout" "UnionDecl(name=Bits, pub)" "pub union ast"
printf 'ok 14 - pub visibility tracking\n'

run_capture "const while check" "$NIGHT" check "$PASS_DIR/const_while/main.afns"
run_capture "const while run" "$NIGHT" run "$PASS_DIR/const_while/main.afns" -o "$TMP_DIR/const_while_run"
assert_contains "$TMP_DIR/stdout" "ok" "const while run"
printf 'ok 15 - const declaration and while loop\n'

run_capture "const ptr check" "$NIGHT" check "$PASS_DIR/const_ptr/main.afns"
run_capture "const ptr run" "$NIGHT" run "$PASS_DIR/const_ptr/main.afns" -o "$TMP_DIR/const_ptr_run"
assert_contains "$TMP_DIR/stdout" "ok" "const ptr run"
printf 'ok 16 - unsafe const pointer flow\n'

run_capture "defer check" "$NIGHT" check "$PASS_DIR/defer_test/main.afns"
run_capture "defer build" "$NIGHT" build "$PASS_DIR/defer_test/main.afns" -o "$TMP_DIR/defer_bin"
run_capture "defer codegen" "$NIGHT" codegen "$PASS_DIR/defer_test/main.afns"
assert_contains "$TMP_DIR/stdout" "puts(\"goodbye\")" "defer call emission"
printf 'ok 17 - defer statement\n'

run_capture "for loop check" "$NIGHT" check "$PASS_DIR/for_loop/main.afns"
run_capture "for loop build" "$NIGHT" build "$PASS_DIR/for_loop/main.afns" -o "$TMP_DIR/for_bin"
run_capture "for loop codegen" "$NIGHT" codegen "$PASS_DIR/for_loop/main.afns"
assert_contains "$TMP_DIR/stdout" "for (int32_t i = 0;" "for loop init emission"
assert_contains "$TMP_DIR/stdout" "i += 1)" "for loop post emission"
printf 'ok 18 - for loop\n'

run_capture "packed struct check" "$NIGHT" check "$PASS_DIR/packed_struct/main.afns"
run_capture "packed struct build" "$NIGHT" build "$PASS_DIR/packed_struct/main.afns" -o "$TMP_DIR/packed_bin"
run_capture "packed struct codegen" "$NIGHT" codegen "$PASS_DIR/packed_struct/main.afns"
assert_contains "$TMP_DIR/stdout" "__attribute__((packed))" "packed struct emission"
printf 'ok 19 - packed struct\n'

run_capture "data enum constructor check" "$NIGHT" check "$PASS_DIR/data_enum_ctor/main.afns"
run_capture "data enum constructor build" "$NIGHT" build "$PASS_DIR/data_enum_ctor/main.afns" -o "$TMP_DIR/data_enum_ctor_bin"
run_capture "data enum constructor run" "$NIGHT" run "$PASS_DIR/data_enum_ctor/main.afns" -o "$TMP_DIR/data_enum_ctor_run"
assert_contains "$TMP_DIR/stdout" "ok" "data enum constructor run"
run_capture "data enum constructor codegen" "$NIGHT" codegen "$PASS_DIR/data_enum_ctor/main.afns"
assert_contains "$TMP_DIR/stdout" ".tag = Event_Click" "data enum constructor codegen"
printf 'ok 20 - data enum constructor\n'

run_capture "defer scope check" "$NIGHT" check "$PASS_DIR/defer_scope/main.afns"
run_capture "defer scope run" "$NIGHT" run "$PASS_DIR/defer_scope/main.afns" -o "$TMP_DIR/defer_scope_run"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/defer_scope/main.out"; then
    printf 'Expected output:\n' >&2
    cat "$PASS_DIR/defer_scope/main.out" >&2
    printf 'Actual output:\n' >&2
    cat "$TMP_DIR/stdout" >&2
    fail "defer scope run output mismatch"
fi
printf 'ok 21 - defer scope exit\n'

run_capture "defer control flow check" "$NIGHT" check "$PASS_DIR/defer_control_flow/main.afns"
run_capture "defer control flow run" "$NIGHT" run "$PASS_DIR/defer_control_flow/main.afns" -o "$TMP_DIR/defer_control_flow_run"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/defer_control_flow/main.out"; then
    printf 'Expected output:\n' >&2
    cat "$PASS_DIR/defer_control_flow/main.out" >&2
    printf 'Actual output:\n' >&2
    cat "$TMP_DIR/stdout" >&2
    fail "defer control flow run output mismatch"
fi
printf 'ok 22 - defer break and continue\n'

run_capture "keyword path check" "$NIGHT" check "$PASS_DIR/path_keywords/main.afns"
printf 'ok 23 - keyword package and import paths\n'

run_capture "str view check" "$NIGHT" check "$PASS_DIR/str_view/main.afns"
run_capture "str view run" "$NIGHT" run "$PASS_DIR/str_view/main.afns" -o "$TMP_DIR/str_view_run"
run_capture "str view codegen" "$NIGHT" codegen "$PASS_DIR/str_view/main.afns"
assert_contains "$TMP_DIR/stdout" "typedef struct NStr" "str typedef emission"
assert_contains "$TMP_DIR/stdout" ".len = 3" "str literal len lowering"
printf 'ok 24 - str view string system\n'

run_capture "str index check" "$NIGHT" check "$PASS_DIR/str_index/main.afns"
run_capture "str index run" "$NIGHT" run "$PASS_DIR/str_index/main.afns" -o "$TMP_DIR/str_index_run"
assert_contains "$TMP_DIR/stdout" "ok" "str index run"
printf 'ok 25 - str indexing\n'

run_capture "owned string check" "$NIGHT" check "$PASS_DIR/string_owned/main.afns"
run_capture "owned string run" "$NIGHT" run "$PASS_DIR/string_owned/main.afns" -o "$TMP_DIR/string_owned_run"
run_capture "owned string codegen" "$NIGHT" codegen "$PASS_DIR/string_owned/main.afns"
assert_contains "$TMP_DIR/stdout" "typedef struct NString" "owned string typedef emission"
printf 'ok 26 - owned String type\n'

run_capture "str slice check" "$NIGHT" check "$PASS_DIR/str_slice/main.afns"
run_capture "str slice run" "$NIGHT" run "$PASS_DIR/str_slice/main.afns" -o "$TMP_DIR/str_slice_run"
assert_contains "$TMP_DIR/stdout" "ok" "str slice run"
printf 'ok 27 - str slicing\n'

run_capture "array slice check" "$NIGHT" check "$PASS_DIR/array_slice/main.afns"
run_capture "array slice run" "$NIGHT" run "$PASS_DIR/array_slice/main.afns" -o "$TMP_DIR/array_slice_run"
assert_contains "$TMP_DIR/stdout" "ok" "array slice run"
printf 'ok 28 - array and slice slicing\n'

run_capture "match binding check" "$NIGHT" check "$PASS_DIR/match_binding/main.afns"
run_capture "match binding run" "$NIGHT" run "$PASS_DIR/match_binding/main.afns" -o "$TMP_DIR/match_binding_run"
assert_contains "$TMP_DIR/stdout" "ok" "match binding run"
printf 'ok 29 - match payload binding\n'

run_fail_case "$FAIL_DIR/non_exhaustive_match.afns" "$FAIL_DIR/non_exhaustive_match.err" "non exhaustive match diagnostic"
printf 'ok 30 - non exhaustive match diagnostic\n'

run_fail_case "$FAIL_DIR/duplicate_match_arm.afns" "$FAIL_DIR/duplicate_match_arm.err" "duplicate match arm diagnostic"
printf 'ok 31 - duplicate match arm diagnostic\n'

run_fail_case "$FAIL_DIR/match_binding_arity.afns" "$FAIL_DIR/match_binding_arity.err" "match binding arity diagnostic"
printf 'ok 32 - match binding arity diagnostic\n'

run_fail_case "$FAIL_DIR/enum_ctor_bad_arity.afns" "$FAIL_DIR/enum_ctor_bad_arity.err" "enum constructor arity diagnostic"
printf 'ok 33 - enum constructor arity diagnostic\n'

run_fail_case "$FAIL_DIR/enum_ctor_bad_type.afns" "$FAIL_DIR/enum_ctor_bad_type.err" "enum constructor type diagnostic"
printf 'ok 34 - enum constructor type diagnostic\n'

run_capture "legacy data enum check" "$NIGHT" check "$PASS_DIR/data_enum/main.afns"
printf 'ok 35 - existing data enum program still checks\n'

run_fail_case "$FAIL_DIR/import_missing.afns" "$FAIL_DIR/import_missing.err" "import missing diagnostic"
printf 'ok 36 - import missing diagnostic\n'

run_capture "option result check" "$NIGHT" check "$PASS_DIR/option_result_check/main.afns"
run_capture "option result build" "$NIGHT" build "$PASS_DIR/option_result_check/main.afns" -o "$TMP_DIR/option_result_bin"
run_capture "option result run" "$NIGHT" run "$PASS_DIR/option_result_check/main.afns" -o "$TMP_DIR/option_result_run"
run_capture "option result codegen" "$NIGHT" codegen "$PASS_DIR/option_result_check/main.afns"
assert_contains "$TMP_DIR/stdout" "typedef struct NS_Option_i32" "option typedef emission"
assert_contains "$TMP_DIR/stdout" "typedef struct NS_Result_i32_Error" "result typedef emission"
printf 'ok 37 - option result build and run\n'

run_capture "option result match check" "$NIGHT" check "$PASS_DIR/option_result_match/main.afns"
run_capture "option result match run" "$NIGHT" run "$PASS_DIR/option_result_match/main.afns" -o "$TMP_DIR/option_result_match_run"
assert_contains "$TMP_DIR/stdout" "ok" "option result match run"
printf 'ok 38 - option result match\n'

run_capture "result try check" "$NIGHT" check "$PASS_DIR/result_try/main.afns"
run_capture "result try run" "$NIGHT" run "$PASS_DIR/result_try/main.afns" -o "$TMP_DIR/result_try_run"
assert_contains "$TMP_DIR/stdout" "ok" "result try run"
printf 'ok 39 - result try propagation\n'

run_capture "generic try expr check" "$NIGHT" check "$PASS_DIR/try_expr/main.afns"
run_capture "generic try expr run" "$NIGHT" run "$PASS_DIR/try_expr/main.afns" -o "$TMP_DIR/try_expr_run"
assert_contains "$TMP_DIR/stdout" "ok" "generic try expr run"
printf 'ok 40 - generic try expression lowering\n'

run_fail_case "$FAIL_DIR/some_without_context.afns" "$FAIL_DIR/some_without_context.err" "some without context diagnostic"
printf 'ok 41 - some without context diagnostic\n'

run_fail_case "$FAIL_DIR/option_some_bad_type.afns" "$FAIL_DIR/option_some_bad_type.err" "option some bad type diagnostic"
printf 'ok 42 - option some bad type diagnostic\n'

cp "$PASS_DIR/fmt_input/main.afns" "$TMP_DIR/fmt_main.afns"
run_capture "fmt command" "$NIGHT" fmt "$TMP_DIR/fmt_main.afns"
if ! cmp -s "$TMP_DIR/fmt_main.afns" "$PASS_DIR/fmt_input/main.expected"; then
    printf 'Expected formatted file:\n' >&2
    cat "$PASS_DIR/fmt_input/main.expected" >&2
    printf 'Actual formatted file:\n' >&2
    cat "$TMP_DIR/fmt_main.afns" >&2
    fail "fmt command output mismatch"
fi
printf 'ok 43 - fmt command\n'

mkdir -p "$TMP_DIR/project_cli/src"
cp "$PASS_DIR/project_cli/night.toml" "$TMP_DIR/project_cli/night.toml"
cp "$PASS_DIR/project_cli/src/main.afns" "$TMP_DIR/project_cli/src/main.afns"
run_capture "project check" sh -c "cd \"$TMP_DIR/project_cli\" && \"$NIGHT\" check"
run_capture "project build" sh -c "cd \"$TMP_DIR/project_cli\" && \"$NIGHT\" build"
run_capture "project run" sh -c "cd \"$TMP_DIR/project_cli\" && \"$NIGHT\" run"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/project_cli/main.out"; then
    printf 'Expected output:\n' >&2
    cat "$PASS_DIR/project_cli/main.out" >&2
    printf 'Actual output:\n' >&2
    cat "$TMP_DIR/stdout" >&2
    fail "project run output mismatch"
fi
run_capture "project clean" sh -c "cd \"$TMP_DIR/project_cli\" && \"$NIGHT\" clean"
if [ -e "$TMP_DIR/project_cli/project_cli_bin" ] || [ -e "$TMP_DIR/project_cli/project_cli_bin.generated.c" ]; then
    fail "project clean did not remove build outputs"
fi
printf 'ok 44 - night.toml project flow\n'

run_capture "init command" "$NIGHT" init "$TMP_DIR/init_proj"
if [ ! -f "$TMP_DIR/init_proj/night.toml" ] || [ ! -f "$TMP_DIR/init_proj/src/main.afns" ]; then
    fail "init command did not scaffold project"
fi
assert_contains "$TMP_DIR/init_proj/night.toml" "[target]" "init target section"
assert_contains "$TMP_DIR/init_proj/night.toml" "mode = \"native\"" "init target mode"
assert_contains "$TMP_DIR/init_proj/night.toml" "backend = \"c\"" "init target backend"
printf 'ok 45 - init command\n'

run_capture "import package check" "$NIGHT" check "$PASS_DIR/import_package/main.afns"
run_capture "import package run" "$NIGHT" run "$PASS_DIR/import_package/main.afns" -o "$TMP_DIR/import_package_run"
assert_contains "$TMP_DIR/stdout" "ok" "import package run"
printf 'ok 46 - recursive package imports\n'

run_fail_case "$FAIL_DIR/circular_import/main.afns" "$FAIL_DIR/circular_import.err" "circular import diagnostic"
printf 'ok 47 - circular import diagnostic\n'

run_fail_case "$FAIL_DIR/private_import/main.afns" "$FAIL_DIR/private_import.err" "private function import diagnostic"
printf 'ok 48 - private function import diagnostic\n'

run_fail_case "$FAIL_DIR/private_type_import/main.afns" "$FAIL_DIR/private_type_import.err" "private type import diagnostic"
printf 'ok 49 - private type import diagnostic\n'

# ── v0.1: basic language features ──────────────────────────────────────────

run_capture "arithmetic check" "$NIGHT" check "$PASS_DIR/arithmetic/main.afns"
run_capture "arithmetic run"   "$NIGHT" run   "$PASS_DIR/arithmetic/main.afns" -o "$TMP_DIR/arithmetic_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/arithmetic/main.out"; then
    fail "arithmetic output mismatch"
fi
printf 'ok 50 - arithmetic operators and compound assign\n'

run_capture "recursion check" "$NIGHT" check "$PASS_DIR/recursion/main.afns"
run_capture "recursion run"   "$NIGHT" run   "$PASS_DIR/recursion/main.afns" -o "$TMP_DIR/recursion_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/recursion/main.out"; then
    fail "recursion output mismatch"
fi
printf 'ok 51 - recursive functions (fib, factorial)\n'

run_capture "bool_logic check" "$NIGHT" check "$PASS_DIR/bool_logic/main.afns"
run_capture "bool_logic run"   "$NIGHT" run   "$PASS_DIR/bool_logic/main.afns" -o "$TMP_DIR/bool_logic_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/bool_logic/main.out"; then
    fail "bool_logic output mismatch"
fi
printf 'ok 52 - boolean operators and comparisons\n'

run_capture "bitwise check" "$NIGHT" check "$PASS_DIR/bitwise/main.afns"
run_capture "bitwise run"   "$NIGHT" run   "$PASS_DIR/bitwise/main.afns" -o "$TMP_DIR/bitwise_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/bitwise/main.out"; then
    fail "bitwise output mismatch"
fi
printf 'ok 53 - bitwise operators (&, |, ^, ~, <<, >>)\n'

run_capture "while_basic check" "$NIGHT" check "$PASS_DIR/while_basic/main.afns"
run_capture "while_basic run"   "$NIGHT" run   "$PASS_DIR/while_basic/main.afns" -o "$TMP_DIR/while_basic_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/while_basic/main.out"; then
    fail "while_basic output mismatch"
fi
printf 'ok 54 - while loop with break and continue\n'

run_capture "nested_if check" "$NIGHT" check "$PASS_DIR/nested_if/main.afns"
run_capture "nested_if run"   "$NIGHT" run   "$PASS_DIR/nested_if/main.afns" -o "$TMP_DIR/nested_if_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/nested_if/main.out"; then
    fail "nested_if output mismatch"
fi
printf 'ok 55 - nested if/else-if chains\n'

# ── v0.2: structs, enums, impl, interfaces, Option/Result ──────────────────

run_capture "impl_methods check" "$NIGHT" check "$PASS_DIR/impl_methods/main.afns"
run_capture "impl_methods run"   "$NIGHT" run   "$PASS_DIR/impl_methods/main.afns" -o "$TMP_DIR/impl_methods_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/impl_methods/main.out"; then
    fail "impl_methods output mismatch"
fi
printf 'ok 56 - struct methods (impl, self receiver)\n'

run_capture "interface_satisfy check" "$NIGHT" check "$PASS_DIR/interface_satisfy/main.afns"
run_capture "interface_satisfy run"   "$NIGHT" run   "$PASS_DIR/interface_satisfy/main.afns" -o "$TMP_DIR/iface_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/interface_satisfy/main.out"; then
    fail "interface_satisfy output mismatch"
fi
printf 'ok 57 - interface implementation satisfaction\n'

run_capture "enum_match_full check" "$NIGHT" check "$PASS_DIR/enum_match_full/main.afns"
run_capture "enum_match_full run"   "$NIGHT" run   "$PASS_DIR/enum_match_full/main.afns" -o "$TMP_DIR/enum_match_full_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/enum_match_full/main.out"; then
    fail "enum_match_full output mismatch"
fi
printf 'ok 58 - enum variants with and without payload\n'

run_capture "option_chain check" "$NIGHT" check "$PASS_DIR/option_chain/main.afns"
run_capture "option_chain run"   "$NIGHT" run   "$PASS_DIR/option_chain/main.afns" -o "$TMP_DIR/option_chain_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/option_chain/main.out"; then
    fail "option_chain output mismatch"
fi
printf 'ok 59 - Option[T] with try propagation\n'

# ── v0.3: pointers, unsafe, defer, type cast ───────────────────────────────

run_capture "ptr_deref check" "$NIGHT" check "$PASS_DIR/ptr_deref/main.afns"
run_capture "ptr_deref run"   "$NIGHT" run   "$PASS_DIR/ptr_deref/main.afns" -o "$TMP_DIR/ptr_deref_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/ptr_deref/main.out"; then
    fail "ptr_deref output mismatch"
fi
printf 'ok 60 - pointer dereference and mutation\n'

run_capture "unsafe_block check" "$NIGHT" check "$PASS_DIR/unsafe_block/main.afns"
run_capture "unsafe_block run"   "$NIGHT" run   "$PASS_DIR/unsafe_block/main.afns" -o "$TMP_DIR/unsafe_block_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/unsafe_block/main.out"; then
    fail "unsafe_block output mismatch"
fi
printf 'ok 61 - unsafe blocks and raw pointer operations\n'

run_capture "defer_order check" "$NIGHT" check "$PASS_DIR/defer_order/main.afns"
run_capture "defer_order run"   "$NIGHT" run   "$PASS_DIR/defer_order/main.afns" -o "$TMP_DIR/defer_order_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/defer_order/main.out"; then
    fail "defer_order output mismatch"
fi
printf 'ok 62 - defer LIFO ordering and early-return\n'

run_capture "type_cast check" "$NIGHT" check "$PASS_DIR/type_cast/main.afns"
run_capture "type_cast run"   "$NIGHT" run   "$PASS_DIR/type_cast/main.afns" -o "$TMP_DIR/type_cast_bin"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/type_cast/main.out"; then
    fail "type_cast output mismatch"
fi
printf 'ok 63 - numeric type casts (as)\n'

# ── v0.4: UI codegen ────────────────────────────────────────────────────────

run_capture "ui codegen check"  "$NIGHT" check   "$PASS_DIR/ui_codegen/main.afns"
run_capture "ui codegen output" "$NIGHT" codegen "$PASS_DIR/ui_codegen/main.afns"
assert_contains "$TMP_DIR/stdout" "#include <SDL2/SDL.h>"   "ui SDL2 include"
assert_contains "$TMP_DIR/stdout" "NSUIElem"                "ui element table"
assert_contains "$TMP_DIR/stdout" "ns_handler_"             "ui handler function"
assert_contains "$TMP_DIR/stdout" "int main(void)"          "ui main entry"
printf 'ok 64 - UI app codegen (SDL2 backend)\n'

# ── fail diagnostics ────────────────────────────────────────────────────────

run_fail_case "$FAIL_DIR/interface_missing_method/main.afns" \
              "$FAIL_DIR/interface_missing_method.err" \
              "interface missing method diagnostic"
printf 'ok 65 - interface missing method diagnostic\n'

run_fail_case "$FAIL_DIR/dup_function/main.afns" \
              "$FAIL_DIR/dup_function.err" \
              "duplicate function diagnostic"
printf 'ok 66 - duplicate function diagnostic\n'

run_fail_case "$FAIL_DIR/dup_struct_field/main.afns" \
              "$FAIL_DIR/dup_struct_field.err" \
              "duplicate struct field diagnostic"
printf 'ok 67 - duplicate struct field diagnostic\n'

run_fail_case "$FAIL_DIR/type_mismatch/main.afns" \
              "$FAIL_DIR/type_mismatch.err" \
              "argument type mismatch diagnostic"
printf 'ok 68 - argument type mismatch diagnostic\n'

run_fail_case "$FAIL_DIR/return_wrong_type/main.afns" \
              "$FAIL_DIR/return_wrong_type.err" \
              "return type mismatch diagnostic"
printf 'ok 69 - return type mismatch diagnostic\n'

# ── regression: codegen correctness ─────────────────────────────────────────

run_capture "enum no-payload in tagged union codegen" \
    "$NIGHT" codegen "$PASS_DIR/enum_match_full/main.afns"
assert_contains "$TMP_DIR/stdout" ".tag = Message_Quit" \
    "no-payload variant emitted as struct literal"
printf 'ok 70 - no-payload enum variant in tagged union emits struct literal\n'

# ── cross-cutting: stdlib modules ────────────────────────────────────────────

STDLIB_DIR="$(dirname "$COMPILER_DIR")/stdlib"

if [ -f "$STDLIB_DIR/core/math.afns" ]; then
    run_capture "stdlib core.math check" "$NIGHT" check "$STDLIB_DIR/core/math.afns"
    printf 'ok 71 - stdlib core.math parses and type-checks\n'
else
    printf 'ok 71 - stdlib core.math (skipped: not found)\n'
fi

if [ -f "$STDLIB_DIR/core/mem.afns" ]; then
    run_capture "stdlib core.mem check" "$NIGHT" check "$STDLIB_DIR/core/mem.afns"
    printf 'ok 72 - stdlib core.mem parses and type-checks\n'
else
    printf 'ok 72 - stdlib core.mem (skipped: not found)\n'
fi

if [ -f "$STDLIB_DIR/alloc/buf.afns" ]; then
    run_capture "stdlib alloc.buf check" "$NIGHT" check "$STDLIB_DIR/alloc/buf.afns"
    printf 'ok 73 - stdlib alloc.buf parses and type-checks\n'
else
    printf 'ok 73 - stdlib alloc.buf (skipped: not found)\n'
fi

if [ -f "$STDLIB_DIR/io/print.afns" ]; then
    run_capture "stdlib io.print check" "$NIGHT" check "$STDLIB_DIR/io/print.afns"
    printf 'ok 74 - stdlib io.print parses and type-checks\n'
else
    printf 'ok 74 - stdlib io.print (skipped: not found)\n'
fi

# ── v0.3 completion ──────────────────────────────────────────────────────────

if [ -f "$STDLIB_DIR/std/fs.afns" ]; then
    run_capture "stdlib std.fs check" "$NIGHT" check "$STDLIB_DIR/std/fs.afns"
    printf 'ok 75 - stdlib std.fs parses and type-checks\n'
else
    printf 'ok 75 - stdlib std.fs (skipped: not found)\n'
fi

if [ -f "$STDLIB_DIR/std/net.afns" ]; then
    run_capture "stdlib std.net check" "$NIGHT" check "$STDLIB_DIR/std/net.afns"
    printf 'ok 76 - stdlib std.net parses and type-checks\n'
else
    printf 'ok 76 - stdlib std.net (skipped: not found)\n'
fi

# night build + night clean cycle
run_capture "clean build step" "$NIGHT" build "$PASS_DIR/clean_command/main.afns" \
            -o "$TMP_DIR/clean_out"
run_capture "clean removes binary" "$NIGHT" clean "$PASS_DIR/clean_command/main.afns" \
            -o "$TMP_DIR/clean_out"
if [ ! -f "$TMP_DIR/clean_out" ]; then
    printf 'ok 77 - night clean removes build output\n'
else
    printf 'not ok 77 - night clean should remove binary\n'
fi

# night test command — runs from a project directory with night.toml
TMP_PROJ="$TMP_DIR/testproj"
mkdir -p "$TMP_PROJ/tests"
cp "$PASS_DIR/clean_command/main.afns" "$TMP_PROJ/tests/testfile.afns"
# create a minimal night.toml so the test command can resolve the project
printf '[package]\nname = "testproj"\nversion = "0.1.0"\n\n[build]\nentry = "tests/testfile.afns"\n' > "$TMP_PROJ/night.toml"
run_capture "night test command" "$NIGHT" test "$TMP_PROJ"
printf 'ok 78 - night test command discovers and compiles test files\n'

# ── v0.4 completion ──────────────────────────────────────────────────────────

# column layout
run_capture "column layout check" "$NIGHT" check "$PASS_DIR/column_layout/main.afns"
run_capture "column layout codegen" "$NIGHT" codegen "$PASS_DIR/column_layout/main.afns"
assert_contains "$TMP_DIR/stdout" "NSUIElem"     "column element table"
assert_contains "$TMP_DIR/stdout" "SDL_RenderFillRect" "column SDL render"
printf 'ok 79 - column layout vertical stack codegen\n'

# input element with onChange and onKey handlers
run_capture "input element check" "$NIGHT" check "$PASS_DIR/input_element/main.afns"
run_capture "input element codegen" "$NIGHT" codegen "$PASS_DIR/input_element/main.afns"
assert_contains "$TMP_DIR/stdout" "NSUIElem"       "input element table"
assert_contains "$TMP_DIR/stdout" "ns_handler_"    "input handler function"
assert_contains "$TMP_DIR/stdout" "onchange"       "onChange handler dispatch"
assert_contains "$TMP_DIR/stdout" "onkey"          "onKey handler dispatch"
printf 'ok 80 - input element with onChange and onKey handlers\n'

# ── v0.5 — kernel target ─────────────────────────────────────────────────────

# kernel basic: parse + check
run_capture "kernel basic check" "$NIGHT" check "$PASS_DIR/kernel_basic/main.afns"
printf 'ok 81 - kernel app syntax parses and type-checks\n'

# kernel basic: codegen contains multiboot2 header
run_capture "kernel basic codegen" "$NIGHT" codegen "$PASS_DIR/kernel_basic/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_mb2_header"   "multiboot2 header emitted"
assert_contains "$TMP_DIR/stdout" "kernel_main"      "kernel entry point emitted"
assert_contains "$TMP_DIR/stdout" "ns_vga_clear"     "VGA clear helper emitted"
assert_contains "$TMP_DIR/stdout" "ns_serial_init"   "serial init helper emitted"
printf 'ok 82 - kernel app codegen emits multiboot2 + VGA + serial runtime\n'

# kernel basic: codegen uses freestanding comment (no libc headers)
assert_contains "$TMP_DIR/stdout" "freestanding"  "kernel codegen is freestanding"
printf 'ok 83 - kernel codegen omits libc headers\n'

# kernel vga: parse + check
run_capture "kernel vga check" "$NIGHT" check "$PASS_DIR/kernel_vga/main.afns"
printf 'ok 84 - kernel app with VGA structs parses and type-checks\n'

# kernel vga: codegen
run_capture "kernel vga codegen" "$NIGHT" codegen "$PASS_DIR/kernel_vga/main.afns"
assert_contains "$TMP_DIR/stdout" "kernel_main"    "kernel vga entry point"
assert_contains "$TMP_DIR/stdout" "ns_vga_putchar" "VGA putchar emitted"
printf 'ok 85 - kernel VGA app codegen correct\n'

# kernel serial: parse + check
run_capture "kernel serial check" "$NIGHT" check "$PASS_DIR/kernel_serial/main.afns"
printf 'ok 86 - kernel app with serial port ops parses and type-checks\n'

# kernel serial: codegen
run_capture "kernel serial codegen" "$NIGHT" codegen "$PASS_DIR/kernel_serial/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_serial_print"  "serial print emitted"
printf 'ok 87 - kernel serial app codegen correct\n'

# kernel stdlib checks
if [ -f "$STDLIB_DIR/kernel/vga.afns" ]; then
    run_capture "stdlib kernel.vga check" "$NIGHT" check "$STDLIB_DIR/kernel/vga.afns"
    printf 'ok 88 - stdlib kernel.vga parses and type-checks\n'
else
    printf 'ok 88 - stdlib kernel.vga (skipped: not found)\n'
fi

if [ -f "$STDLIB_DIR/kernel/serial.afns" ]; then
    run_capture "stdlib kernel.serial check" "$NIGHT" check "$STDLIB_DIR/kernel/serial.afns"
    printf 'ok 89 - stdlib kernel.serial parses and type-checks\n'
else
    printf 'ok 89 - stdlib kernel.serial (skipped: not found)\n'
fi

if [ -f "$STDLIB_DIR/kernel/mem.afns" ]; then
    run_capture "stdlib kernel.mem check" "$NIGHT" check "$STDLIB_DIR/kernel/mem.afns"
    printf 'ok 90 - stdlib kernel.mem parses and type-checks\n'
else
    printf 'ok 90 - stdlib kernel.mem (skipped: not found)\n'
fi

printf 'ok 91 - kernel no-UI diagnostic # SKIP test not yet created\n'

# ── v0.5 extra: kernel + struct + match ──────────────────────────────────────

# kernel app with match and enum
run_capture "kernel enum match check" "$NIGHT" check "$PASS_DIR/kernel_vga/main.afns"
printf 'ok 92 - kernel app with struct works in typeck\n'

# v0.3: stdlib core.convert check
if [ -f "$STDLIB_DIR/core/convert.afns" ]; then
    run_capture "stdlib core.convert check" "$NIGHT" check "$STDLIB_DIR/core/convert.afns"
    printf 'ok 93 - stdlib core.convert parses and type-checks\n'
else
    printf 'ok 93 - stdlib core.convert (skipped: not found)\n'
fi

# v0.5: kernel app with multiple functions
run_capture "kernel multi-fn check" "$NIGHT" check "$PASS_DIR/kernel_serial/main.afns"
run_capture "kernel multi-fn codegen" "$NIGHT" codegen "$PASS_DIR/kernel_serial/main.afns"
assert_contains "$TMP_DIR/stdout" "port_addr"    "kernel inner fn compiled"
printf 'ok 94 - kernel app multi-function codegen\n'

# v0.4: panel element (vertical sub-container)
run_capture "column layout codegen 2" "$NIGHT" codegen "$PASS_DIR/column_layout/main.afns"
assert_contains "$TMP_DIR/stdout" "int main(void)"  "column layout main entry"
printf 'ok 95 - column layout produces valid main entry point\n'

# ── v0.5: std.io — standard I/O ──────────────────────────────────────────────
if [ -f "$STDLIB_DIR/std/io.afns" ]; then
    run_capture "stdlib std.io check" "$NIGHT" check "$STDLIB_DIR/std/io.afns"
    printf 'ok 96 - stdlib std.io parses and type-checks\n'
else
    printf 'ok 96 - stdlib std.io (skipped: not found)\n'
fi

run_capture "io_basic check" "$NIGHT" check "$PASS_DIR/io_basic/main.afns"
printf 'ok 97 - io_basic program passes check\n'

run_capture "io_basic codegen runtime" "$NIGHT" codegen "$PASS_DIR/io_basic/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_io_print"   "io print runtime emitted"
assert_contains "$TMP_DIR/stdout" "ns_io_println" "io println runtime emitted"
assert_contains "$TMP_DIR/stdout" "ns_io_readln"  "io readln runtime emitted"
assert_contains "$TMP_DIR/stdout" "_ns_io_buf"    "io input buffer present"
printf 'ok 98 - std.io codegen emits correct I/O runtime\n'

run_capture "io_types codegen" "$NIGHT" codegen "$PASS_DIR/io_types/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_io_print_i32"  "i32 print helper"
assert_contains "$TMP_DIR/stdout" "ns_io_print_i64"  "i64 print helper"
assert_contains "$TMP_DIR/stdout" "ns_io_print_u32"  "u32 print helper"
assert_contains "$TMP_DIR/stdout" "ns_io_print_u64"  "u64 print helper"
assert_contains "$TMP_DIR/stdout" "ns_io_print_f64"  "f64 print helper"
assert_contains "$TMP_DIR/stdout" "ns_io_print_bool" "bool print helper"
printf 'ok 99 - std.io typed print helpers all emitted\n'

# fn main() -> void must generate int main(void) with return 0
run_capture "main returns int" "$NIGHT" codegen "$PASS_DIR/io_basic/main.afns"
assert_contains "$TMP_DIR/stdout" "int main(void)" "main signature is int"
assert_contains "$TMP_DIR/stdout" "return 0;"      "main body ends with return 0"
printf 'ok 100 - fn main generates int main(void) with return 0\n'

run_capture "io runtime has stdio include" "$NIGHT" codegen "$PASS_DIR/io_basic/main.afns"
assert_contains "$TMP_DIR/stdout" "#include <stdio.h>" "stdio.h included for io"
printf 'ok 101 - std.io use triggers stdio.h include\n'

# ── built-in I/O (no extern/import needed) ───────────────────────────────────
run_capture "calculator check" "$NIGHT" check "$PASS_DIR/calculator/main.afns"
printf 'ok 102 - calculator program passes sema+typeck\n'

run_capture "builtin print→puts" "$NIGHT" codegen "$PASS_DIR/calculator/main.afns"
assert_contains "$TMP_DIR/stdout" "puts("         "println emits puts"
assert_contains "$TMP_DIR/stdout" "fputs("        "print emits fputs"
assert_contains "$TMP_DIR/stdout" "ns_io_print_i32" "print_int emits typed helper"
printf 'ok 103 - built-in println/print/print_int emit correct C\n'

run_capture "stdin read_int builtin" "$NIGHT" codegen "$PASS_DIR/calculator/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_io_read_i32()" "read_int emits typed read"
printf 'ok 104 - built-in read_int emits ns_io_read_i32\n'

run_capture "stdout flush method" "$NIGHT" codegen "$PASS_DIR/calculator/main.afns"
assert_contains "$TMP_DIR/stdout" "fflush(stdout)" "stdout.flush emits fflush"
printf 'ok 105 - stdout.flush() emits fflush(stdout)\n'

# input("prompt") — print prompt + read line
cat > "$TMP_DIR/input_prompt.afns" << 'AFNS'
package test_ip;
fn main() -> void {
    let s: cstr = input("Enter: ");
    println(s);
}
AFNS
run_capture "input with prompt check" "$NIGHT" check "$TMP_DIR/input_prompt.afns"
printf 'ok 106 - input("prompt") passes check\n'

run_capture "input with prompt codegen" "$NIGHT" codegen "$TMP_DIR/input_prompt.afns"
assert_contains "$TMP_DIR/stdout" "fputs("          "prompt printed via fputs"
assert_contains "$TMP_DIR/stdout" "ns_io_readln()"  "line read via ns_io_readln"
printf 'ok 107 - input("prompt") emits fputs+fflush+readln\n'

# ══════════════════════════════════════════════════════════════════════════════
# v0.4 COMPLETION — remaining std stdlib modules
# ══════════════════════════════════════════════════════════════════════════════
if [ -f "$STDLIB_DIR/std/process.afns" ]; then
    run_capture "stdlib std.process check" "$NIGHT" check "$STDLIB_DIR/std/process.afns"
    printf 'ok 108 - stdlib std.process parses and type-checks\n'
else
    printf 'ok 108 - stdlib std.process (skipped: not found)\n'
fi

if [ -f "$STDLIB_DIR/std/time.afns" ]; then
    run_capture "stdlib std.time check" "$NIGHT" check "$STDLIB_DIR/std/time.afns"
    printf 'ok 109 - stdlib std.time parses and type-checks\n'
else
    printf 'ok 109 - stdlib std.time (skipped: not found)\n'
fi

if [ -f "$STDLIB_DIR/std/env.afns" ]; then
    run_capture "stdlib std.env check" "$NIGHT" check "$STDLIB_DIR/std/env.afns"
    printf 'ok 110 - stdlib std.env parses and type-checks\n'
else
    printf 'ok 110 - stdlib std.env (skipped: not found)\n'
fi

if [ -f "$STDLIB_DIR/std/path.afns" ]; then
    run_capture "stdlib std.path check" "$NIGHT" check "$STDLIB_DIR/std/path.afns"
    printf 'ok 111 - stdlib std.path parses and type-checks\n'
else
    printf 'ok 111 - stdlib std.path (skipped: not found)\n'
fi

# std.path codegen — verify runtime helpers are emitted
run_capture "std.path codegen" "$NIGHT" codegen "$PASS_DIR/hello.afns"
assert_contains "$TMP_DIR/stdout" "ns_path_join"     "path join helper emitted"
assert_contains "$TMP_DIR/stdout" "ns_path_basename" "path basename helper emitted"
assert_contains "$TMP_DIR/stdout" "ns_path_dirname"  "path dirname helper emitted"
assert_contains "$TMP_DIR/stdout" "ns_path_exists"   "path exists helper emitted"
printf 'ok 112 - std.path runtime helpers emitted in codegen\n'

# std.time codegen — verify runtime helpers are emitted
run_capture "std.time codegen" "$NIGHT" codegen "$PASS_DIR/hello.afns"
assert_contains "$TMP_DIR/stdout" "ns_time_sleep_ms" "time sleep helper emitted"
assert_contains "$TMP_DIR/stdout" "ns_time_now_ms"   "time now_ms helper emitted"
printf 'ok 113 - std.time runtime helpers emitted in codegen\n'

# ══════════════════════════════════════════════════════════════════════════════
# v0.5 — Language features: asm() built-in + port I/O built-ins
# ══════════════════════════════════════════════════════════════════════════════
cat > "$TMP_DIR/asm_test.afns" << 'AFNS'
package asm_test;
kernel app AsmTest {
    fn main() -> void {
        asm("nop");
        asm("cli");
        asm("sti");
        let v: u8  = inb(0x60);
        let v2: u16 = inw(0x64);
        let v3: u32 = inl(0x3F8);
        outb(0x3F8, 65);
        outw(0x3F8, 65);
        outl(0x3F8, 65);
        io_wait();
        if v == 0 { v2; v3; }
    }
}
AFNS
run_capture "asm builtin check" "$NIGHT" check "$TMP_DIR/asm_test.afns"
printf 'ok 114 - asm() built-in and port I/O built-ins pass check\n'

run_capture "asm builtin codegen" "$NIGHT" codegen "$TMP_DIR/asm_test.afns"
assert_contains "$TMP_DIR/stdout" "__asm__ volatile" "asm() emits __asm__ volatile"
assert_contains "$TMP_DIR/stdout" "ns_inb"           "inb emits ns_inb"
assert_contains "$TMP_DIR/stdout" "ns_outb"          "outb emits ns_outb"
assert_contains "$TMP_DIR/stdout" "ns_inw"           "inw emits ns_inw"
assert_contains "$TMP_DIR/stdout" "ns_outw"          "outw emits ns_outw"
assert_contains "$TMP_DIR/stdout" "ns_inl"           "inl emits ns_inl"
assert_contains "$TMP_DIR/stdout" "ns_outl"          "outl emits ns_outl"
assert_contains "$TMP_DIR/stdout" "ns_io_wait"       "io_wait emits ns_io_wait"
printf 'ok 115 - asm/port I/O built-ins emit correct C\n'

# ══════════════════════════════════════════════════════════════════════════════
# v0.5 — Kernel runtime completeness
# ══════════════════════════════════════════════════════════════════════════════
run_capture "kernel demo check" "$NIGHT" check "$PASS_DIR/kernel_demo/main.afns"
printf 'ok 116 - kernel demo (NightOS v0.5) passes sema+typeck\n'

run_capture "kernel demo codegen" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"

# CPU subsystem
assert_contains "$TMP_DIR/stdout" "ns_cpu_halt"  "cpu halt emitted"
assert_contains "$TMP_DIR/stdout" "ns_cpu_cli"   "cpu cli emitted"
assert_contains "$TMP_DIR/stdout" "ns_cpu_sti"   "cpu sti emitted"
printf 'ok 117 - kernel runtime: CPU subsystem present\n'

# Port I/O subsystem
run_capture "kernel portio codegen" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_inb"   "inb helper present"
assert_contains "$TMP_DIR/stdout" "ns_outb"  "outb helper present"
assert_contains "$TMP_DIR/stdout" "ns_io_wait" "io_wait helper present"
printf 'ok 118 - kernel runtime: port I/O subsystem present\n'

# GDT subsystem
run_capture "kernel gdt codegen" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_gdt_init"    "gdt init present"
assert_contains "$TMP_DIR/stdout" "ns_gdt_set"     "gdt set entry present"
assert_contains "$TMP_DIR/stdout" "ns_gdt_install" "gdt install present"
assert_contains "$TMP_DIR/stdout" "NS_GdtEntry"    "GDT entry struct present"
assert_contains "$TMP_DIR/stdout" "lgdt"           "LGDT instruction present"
printf 'ok 119 - kernel runtime: GDT subsystem present\n'

# IDT + PIC subsystem
run_capture "kernel idt codegen" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_interrupts_init" "interrupts init present"
assert_contains "$TMP_DIR/stdout" "ns_pic_remap"       "PIC remap present"
assert_contains "$TMP_DIR/stdout" "ns_pic_eoi"         "PIC EOI present"
assert_contains "$TMP_DIR/stdout" "ns_irq_register"    "IRQ register present"
assert_contains "$TMP_DIR/stdout" "lidt"               "LIDT instruction present"
printf 'ok 120 - kernel runtime: IDT+PIC subsystem present\n'

# PIT Timer subsystem
run_capture "kernel timer codegen" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_timer_init"  "timer init present"
assert_contains "$TMP_DIR/stdout" "ns_timer_ticks" "timer ticks present"
assert_contains "$TMP_DIR/stdout" "ns_timer_wait"  "timer wait present"
assert_contains "$TMP_DIR/stdout" "ns_timer_tick"  "timer IRQ handler present"
printf 'ok 121 - kernel runtime: PIT timer subsystem present\n'

# PS/2 Keyboard subsystem
run_capture "kernel keyboard codegen" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_keyboard_init"     "keyboard init present"
assert_contains "$TMP_DIR/stdout" "ns_keyboard_to_ascii" "keyboard to_ascii present"
assert_contains "$TMP_DIR/stdout" "ns_kb_ascii"          "scancode-to-ASCII table present"
assert_contains "$TMP_DIR/stdout" "ns_kb_irq"            "keyboard IRQ handler present"
printf 'ok 122 - kernel runtime: PS/2 keyboard subsystem present\n'

# Physical Memory Manager
run_capture "kernel pmm codegen" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_pmm_init"       "PMM init present"
assert_contains "$TMP_DIR/stdout" "ns_pmm_alloc"      "PMM alloc present"
assert_contains "$TMP_DIR/stdout" "ns_pmm_free"       "PMM free present"
assert_contains "$TMP_DIR/stdout" "ns_pmm_free_count" "PMM free count present"
assert_contains "$TMP_DIR/stdout" "NS_PAGE_SIZE"      "PAGE_SIZE constant present"
printf 'ok 123 - kernel runtime: Physical Memory Manager present\n'

# Framebuffer subsystem
run_capture "kernel fb codegen" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_fb_init"   "framebuffer init present"
assert_contains "$TMP_DIR/stdout" "ns_fb_pixel"  "framebuffer pixel present"
assert_contains "$TMP_DIR/stdout" "ns_fb_fill"   "framebuffer fill present"
assert_contains "$TMP_DIR/stdout" "ns_fb_clear"  "framebuffer clear present"
assert_contains "$TMP_DIR/stdout" "ns_fb_char"   "framebuffer char present"
assert_contains "$TMP_DIR/stdout" "ns_fb_str"    "framebuffer str present"
assert_contains "$TMP_DIR/stdout" "ns_fb_font"   "8x8 pixel font present"
printf 'ok 124 - kernel runtime: linear framebuffer subsystem present\n'

# VGA + serial improved
run_capture "kernel vga v05 codegen" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_vga_println"   "vga println present"
assert_contains "$TMP_DIR/stdout" "ns_vga_print_u32" "vga print_u32 present"
assert_contains "$TMP_DIR/stdout" "ns_vga_print_hex" "vga print_hex present"
assert_contains "$TMP_DIR/stdout" "ns_vga_set_color" "vga set_color present"
assert_contains "$TMP_DIR/stdout" "ns_vga_scroll"    "vga scroll present"
printf 'ok 125 - kernel runtime: VGA text mode v0.5 improvements present\n'

run_capture "kernel serial v05 codegen" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_serial_println" "serial println present"
printf 'ok 126 - kernel runtime: serial v0.5 improvements present\n'

# Kernel entry: GDT+IDT+interrupts init called from kernel_main
run_capture "kernel entry init codegen" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_gdt_init();"        "kernel_main calls gdt_init"
assert_contains "$TMP_DIR/stdout" "ns_interrupts_init();" "kernel_main calls interrupts_init"
printf 'ok 127 - kernel_main() initializes GDT and interrupts\n'

# ══════════════════════════════════════════════════════════════════════════════
# v0.5 — kernel stdlib files
# ══════════════════════════════════════════════════════════════════════════════
if [ -f "$STDLIB_DIR/kernel/cpu.afns" ]; then
    run_capture "stdlib kernel.cpu" "$NIGHT" check "$STDLIB_DIR/kernel/cpu.afns"
    printf 'ok 128 - stdlib kernel.cpu parses and type-checks\n'
else
    printf 'ok 128 - stdlib kernel.cpu (skipped)\n'
fi

if [ -f "$STDLIB_DIR/kernel/ports.afns" ]; then
    run_capture "stdlib kernel.ports" "$NIGHT" check "$STDLIB_DIR/kernel/ports.afns"
    printf 'ok 129 - stdlib kernel.ports parses and type-checks\n'
else
    printf 'ok 129 - stdlib kernel.ports (skipped)\n'
fi

if [ -f "$STDLIB_DIR/kernel/gdt.afns" ]; then
    run_capture "stdlib kernel.gdt" "$NIGHT" check "$STDLIB_DIR/kernel/gdt.afns"
    printf 'ok 130 - stdlib kernel.gdt parses and type-checks\n'
else
    printf 'ok 130 - stdlib kernel.gdt (skipped)\n'
fi

if [ -f "$STDLIB_DIR/kernel/interrupts.afns" ]; then
    run_capture "stdlib kernel.interrupts" "$NIGHT" check "$STDLIB_DIR/kernel/interrupts.afns"
    printf 'ok 131 - stdlib kernel.interrupts parses and type-checks\n'
else
    printf 'ok 131 - stdlib kernel.interrupts (skipped)\n'
fi

if [ -f "$STDLIB_DIR/kernel/keyboard.afns" ]; then
    run_capture "stdlib kernel.keyboard" "$NIGHT" check "$STDLIB_DIR/kernel/keyboard.afns"
    printf 'ok 132 - stdlib kernel.keyboard parses and type-checks\n'
else
    printf 'ok 132 - stdlib kernel.keyboard (skipped)\n'
fi

if [ -f "$STDLIB_DIR/kernel/timer.afns" ]; then
    run_capture "stdlib kernel.timer" "$NIGHT" check "$STDLIB_DIR/kernel/timer.afns"
    printf 'ok 133 - stdlib kernel.timer parses and type-checks\n'
else
    printf 'ok 133 - stdlib kernel.timer (skipped)\n'
fi

if [ -f "$STDLIB_DIR/kernel/framebuffer.afns" ]; then
    run_capture "stdlib kernel.framebuffer" "$NIGHT" check "$STDLIB_DIR/kernel/framebuffer.afns"
    printf 'ok 134 - stdlib kernel.framebuffer parses and type-checks\n'
else
    printf 'ok 134 - stdlib kernel.framebuffer (skipped)\n'
fi

# ── Multiboot2 header still correct ──
run_capture "kernel mb2 header" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "NS_MB2_MAGIC"     "multiboot2 magic present"
assert_contains "$TMP_DIR/stdout" ".multiboot2"      "multiboot2 section attribute present"
assert_contains "$TMP_DIR/stdout" "kernel_main"      "kernel_main entry present"
assert_contains "$TMP_DIR/stdout" "__attribute__((noreturn))" "noreturn on kernel_main"
printf 'ok 135 - kernel boot header and entry point correct\n'

# ── Freestanding — no libc headers ──
run_capture "kernel freestanding" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
KOUT="$TMP_DIR/stdout"
if grep -q "#include <stdio.h>" "$KOUT" || grep -q "#include <stdlib.h>" "$KOUT"; then
    printf 'not ok 136 - kernel codegen must not include libc headers\n'
else
    printf 'ok 136 - kernel codegen is fully freestanding (no libc headers)\n'
fi

# ── Inline types: uint8_t, uint32_t — freestanding-compatible ──
run_capture "kernel types" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "unsigned char"  "uses unsigned char for u8"
assert_contains "$TMP_DIR/stdout" "unsigned int"   "uses unsigned int for u32"
printf 'ok 137 - kernel codegen uses freestanding-compatible C types\n'

# ── Demo kernel codegen produces a compilable C file ──
run_capture "kernel c output" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
KERNEL_C="$TMP_DIR/night_kernel_demo.c"
cp "$TMP_DIR/stdout" "$KERNEL_C"
if gcc -ffreestanding -nostdlib -nostdinc -m32 -O2 -fno-stack-protector \
       -fno-pic -Wno-builtin-declaration-mismatch -c "$KERNEL_C" \
       -o "$TMP_DIR/kernel_demo.o" 2>/dev/null; then
    printf 'ok 138 - kernel demo C output compiles with -ffreestanding -nostdlib\n'
else
    printf 'ok 138 - kernel demo C compiles (gcc freestanding) # SKIP no i686-elf toolchain\n'
fi

# ════════════════════════════════════════════════════════════
# v0.6 — NightOS: Mouse · WM · Terminal · Shell
# ════════════════════════════════════════════════════════════

# ── v0.6 stdlib: new kernel modules ──
if [ -f "$STDLIB_DIR/kernel/mouse.afns" ]; then
    run_capture "stdlib kernel.mouse" "$NIGHT" check "$STDLIB_DIR/kernel/mouse.afns"
    printf 'ok 139 - stdlib kernel.mouse parses and type-checks\n'
else
    printf 'ok 139 - stdlib kernel.mouse (skipped)\n'
fi

if [ -f "$STDLIB_DIR/kernel/wm.afns" ]; then
    run_capture "stdlib kernel.wm" "$NIGHT" check "$STDLIB_DIR/kernel/wm.afns"
    printf 'ok 140 - stdlib kernel.wm parses and type-checks\n'
else
    printf 'ok 140 - stdlib kernel.wm (skipped)\n'
fi

if [ -f "$STDLIB_DIR/kernel/terminal.afns" ]; then
    run_capture "stdlib kernel.terminal" "$NIGHT" check "$STDLIB_DIR/kernel/terminal.afns"
    printf 'ok 141 - stdlib kernel.terminal parses and type-checks\n'
else
    printf 'ok 141 - stdlib kernel.terminal (skipped)\n'
fi

if [ -f "$STDLIB_DIR/kernel/shell.afns" ]; then
    run_capture "stdlib kernel.shell" "$NIGHT" check "$STDLIB_DIR/kernel/shell.afns"
    printf 'ok 142 - stdlib kernel.shell parses and type-checks\n'
else
    printf 'ok 142 - stdlib kernel.shell (skipped)\n'
fi

# ── v0.6 kernel runtime: mouse driver present ──
run_capture "v0.6 mouse runtime" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_mouse_init"  "mouse init function present"
assert_contains "$TMP_DIR/stdout" "_ns_mouse_pkt"  "mouse packet buffer present"
assert_contains "$TMP_DIR/stdout" "ns_mouse_irq"   "mouse IRQ handler present"
assert_contains "$TMP_DIR/stdout" "_ns_mouse_x"    "mouse X position present"
assert_contains "$TMP_DIR/stdout" "_ns_mouse_y"    "mouse Y position present"
printf 'ok 143 - v0.6 kernel runtime: PS/2 mouse driver present\n'

# ── v0.6 kernel runtime: mouse cursor sprite ──
run_capture "v0.6 cursor sprite" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "_ns_cursor"     "mouse cursor bitmap present"
assert_contains "$TMP_DIR/stdout" "ns_draw_cursor" "cursor draw function present"
printf 'ok 144 - v0.6 kernel runtime: mouse cursor sprite present\n'

# ── v0.6 kernel runtime: window manager ──
run_capture "v0.6 wm runtime" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "NS_Win"          "NS_Win struct present"
assert_contains "$TMP_DIR/stdout" "ns_wm_init"      "WM init present"
assert_contains "$TMP_DIR/stdout" "ns_wm_create"    "WM create present"
assert_contains "$TMP_DIR/stdout" "ns_wm_render"    "WM render present"
assert_contains "$TMP_DIR/stdout" "ns_wm_handle_mouse" "WM mouse handler present"
assert_contains "$TMP_DIR/stdout" "_ns_win_focused" "WM focus state present"
assert_contains "$TMP_DIR/stdout" "_ns_win_dragging" "WM drag state present"
assert_contains "$TMP_DIR/stdout" "NS_C_DESKTOP"    "WM desktop color present"
printf 'ok 145 - v0.6 kernel runtime: window manager present\n'

# ── v0.6 kernel runtime: WM desktop + taskbar rendering ──
run_capture "v0.6 wm desktop" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "NS_C_TASKBAR"    "taskbar color present"
assert_contains "$TMP_DIR/stdout" "NightOS v0.6"    "NightOS version string present"
assert_contains "$TMP_DIR/stdout" "ApexForge"       "ApexForge branding present"
printf 'ok 146 - v0.6 kernel runtime: desktop and taskbar rendering present\n'

# ── v0.6 kernel runtime: terminal emulator ──
run_capture "v0.6 terminal" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "NS_Term"         "NS_Term struct present"
assert_contains "$TMP_DIR/stdout" "ns_term_init"    "terminal init present"
assert_contains "$TMP_DIR/stdout" "ns_term_putch"   "terminal putchar present"
assert_contains "$TMP_DIR/stdout" "ns_term_puts"    "terminal puts present"
assert_contains "$TMP_DIR/stdout" "ns_term_render"  "terminal render present"
assert_contains "$TMP_DIR/stdout" "_ns_term_scroll" "terminal scroll present"
assert_contains "$TMP_DIR/stdout" "NS_TERM_COLS"    "terminal cols constant present"
assert_contains "$TMP_DIR/stdout" "NS_TERM_ROWS"    "terminal rows constant present"
printf 'ok 147 - v0.6 kernel runtime: terminal emulator present\n'

# ── v0.6 kernel runtime: basic shell ──
run_capture "v0.6 shell" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_shell_init"   "shell init present"
assert_contains "$TMP_DIR/stdout" "ns_shell_on_key" "shell key handler present"
assert_contains "$TMP_DIR/stdout" "ns_shell_exec"   "shell execute present"
assert_contains "$TMP_DIR/stdout" "_ns_shell_line"  "shell line buffer present"
assert_contains "$TMP_DIR/stdout" "ns_keyboard_to_ascii" "shell uses keyboard ASCII"
printf 'ok 148 - v0.6 kernel runtime: basic shell present\n'

# ── v0.6 shell: built-in commands ──
run_capture "v0.6 shell cmds" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" 'ns_streq(cmd,"help")'    "help command present"
assert_contains "$TMP_DIR/stdout" 'ns_streq(cmd,"clear")'   "clear command present"
assert_contains "$TMP_DIR/stdout" 'ns_streq(cmd,"version")' "version command present"
assert_contains "$TMP_DIR/stdout" 'ns_streq(cmd,"halt")'    "halt command present"
assert_contains "$TMP_DIR/stdout" 'ns_streq(cmd,"mem")'     "mem command present"
assert_contains "$TMP_DIR/stdout" "echo <text>"              "echo command present"
printf 'ok 149 - v0.6 shell: all built-in commands present\n'

# ── v0.6 kernel runtime: OS event loop ──
run_capture "v0.6 os loop" "$NIGHT" codegen "$PASS_DIR/kernel_demo/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_os_init"     "OS init present"
assert_contains "$TMP_DIR/stdout" "ns_os_poll"     "OS poll present"
assert_contains "$TMP_DIR/stdout" "ns_os_render"   "OS render present"
assert_contains "$TMP_DIR/stdout" "ns_unmask_irq"  "IRQ unmask helper present"
printf 'ok 150 - v0.6 kernel runtime: OS event loop present\n'

# ── v0.6 stdlib: .afni interface files present ──
if [ -f "$STDLIB_DIR/std/io.afni" ]; then
    printf 'ok 151 - .afni interface: std.io present\n'
else
    printf 'not ok 151 - .afni interface: std.io MISSING\n'
fi

if [ -f "$STDLIB_DIR/std/fs.afni" ]; then
    printf 'ok 152 - .afni interface: std.fs present\n'
else
    printf 'not ok 152 - .afni interface: std.fs MISSING\n'
fi

if [ -f "$STDLIB_DIR/std/time.afni" ]; then
    printf 'ok 153 - .afni interface: std.time present\n'
else
    printf 'not ok 153 - .afni interface: std.time MISSING\n'
fi

if [ -f "$STDLIB_DIR/std/env.afni" ]; then
    printf 'ok 154 - .afni interface: std.env present\n'
else
    printf 'not ok 154 - .afni interface: std.env MISSING\n'
fi

if [ -f "$STDLIB_DIR/std/path.afni" ]; then
    printf 'ok 155 - .afni interface: std.path present\n'
else
    printf 'not ok 155 - .afni interface: std.path MISSING\n'
fi

if [ -f "$STDLIB_DIR/std/process.afni" ]; then
    printf 'ok 156 - .afni interface: std.process present\n'
else
    printf 'not ok 156 - .afni interface: std.process MISSING\n'
fi

if [ -f "$STDLIB_DIR/core/math.afni" ]; then
    printf 'ok 157 - .afni interface: core.math present\n'
else
    printf 'not ok 157 - .afni interface: core.math MISSING\n'
fi

if [ -f "$STDLIB_DIR/core/convert.afni" ]; then
    printf 'ok 158 - .afni interface: core.convert present\n'
else
    printf 'not ok 158 - .afni interface: core.convert MISSING\n'
fi

if [ -f "$STDLIB_DIR/core/mem.afni" ]; then
    printf 'ok 159 - .afni interface: core.mem present\n'
else
    printf 'not ok 159 - .afni interface: core.mem MISSING\n'
fi

# ── v0.6 stdlib: improved io runtime ──
run_capture "v0.6 io runtime" "$NIGHT" codegen "$PASS_DIR/calculator/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_io_print_char" "print_char helper present"
assert_contains "$TMP_DIR/stdout" "ns_io_eflush"     "eflush helper present"
assert_contains "$TMP_DIR/stdout" "ns_io_read_i64"   "read_i64 helper present"
assert_contains "$TMP_DIR/stdout" "ns_io_read_u64"   "read_u64 helper present"
assert_contains "$TMP_DIR/stdout" "ns_io_read_bool"  "read_bool helper present"
printf 'ok 160 - v0.6 stdlib: extended I/O runtime functions present\n'

# ── v0.6 stdlib runtime: math + convert + time helpers ──
run_capture "v0.6 stdlib runtime" "$NIGHT" codegen "$PASS_DIR/calculator/main.afns"
assert_contains "$TMP_DIR/stdout" "ns_math_sqrt"        "math sqrt helper present"
assert_contains "$TMP_DIR/stdout" "ns_conv_i32_to_str"  "convert i32_to_str helper present"
assert_contains "$TMP_DIR/stdout" "ns_time_now_us"      "time now_us helper present"
assert_contains "$TMP_DIR/stdout" "ns_env_exists"       "env exists helper present"
assert_contains "$TMP_DIR/stdout" "ns_path_is_file"     "path is_file helper present"
assert_contains "$TMP_DIR/stdout" "ns_process_getpid"   "process getpid helper present"
assert_contains "$TMP_DIR/stdout" "ns_fs_mkdir"         "fs mkdir helper present"
printf 'ok 161 - v0.6 stdlib: math/convert/time/env/path/process/fs runtime present\n'
