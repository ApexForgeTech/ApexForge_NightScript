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

printf '1..74\n'

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
