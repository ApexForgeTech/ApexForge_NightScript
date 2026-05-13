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

printf '1..36\n'

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

run_capture "const while check" "$NIGHT" check "$PASS_DIR/const_while/main.afns"
run_capture "const while run" "$NIGHT" run "$PASS_DIR/const_while/main.afns" -o "$TMP_DIR/const_while_run"
assert_contains "$TMP_DIR/stdout" "ok" "const while run"
printf 'ok 14 - const declaration and while loop\n'

run_capture "const ptr check" "$NIGHT" check "$PASS_DIR/const_ptr/main.afns"
run_capture "const ptr run" "$NIGHT" run "$PASS_DIR/const_ptr/main.afns" -o "$TMP_DIR/const_ptr_run"
assert_contains "$TMP_DIR/stdout" "ok" "const ptr run"
printf 'ok 15 - unsafe const pointer flow\n'

run_capture "defer check" "$NIGHT" check "$PASS_DIR/defer_test/main.afns"
run_capture "defer build" "$NIGHT" build "$PASS_DIR/defer_test/main.afns" -o "$TMP_DIR/defer_bin"
run_capture "defer codegen" "$NIGHT" codegen "$PASS_DIR/defer_test/main.afns"
assert_contains "$TMP_DIR/stdout" "puts(\"goodbye\")" "defer call emission"
printf 'ok 16 - defer statement\n'

run_capture "for loop check" "$NIGHT" check "$PASS_DIR/for_loop/main.afns"
run_capture "for loop build" "$NIGHT" build "$PASS_DIR/for_loop/main.afns" -o "$TMP_DIR/for_bin"
run_capture "for loop codegen" "$NIGHT" codegen "$PASS_DIR/for_loop/main.afns"
assert_contains "$TMP_DIR/stdout" "for (int32_t i = 0;" "for loop init emission"
assert_contains "$TMP_DIR/stdout" "i += 1)" "for loop post emission"
printf 'ok 17 - for loop\n'

run_capture "packed struct check" "$NIGHT" check "$PASS_DIR/packed_struct/main.afns"
run_capture "packed struct build" "$NIGHT" build "$PASS_DIR/packed_struct/main.afns" -o "$TMP_DIR/packed_bin"
run_capture "packed struct codegen" "$NIGHT" codegen "$PASS_DIR/packed_struct/main.afns"
assert_contains "$TMP_DIR/stdout" "__attribute__((packed))" "packed struct emission"
printf 'ok 18 - packed struct\n'

run_capture "data enum constructor check" "$NIGHT" check "$PASS_DIR/data_enum_ctor/main.afns"
run_capture "data enum constructor build" "$NIGHT" build "$PASS_DIR/data_enum_ctor/main.afns" -o "$TMP_DIR/data_enum_ctor_bin"
run_capture "data enum constructor run" "$NIGHT" run "$PASS_DIR/data_enum_ctor/main.afns" -o "$TMP_DIR/data_enum_ctor_run"
assert_contains "$TMP_DIR/stdout" "ok" "data enum constructor run"
run_capture "data enum constructor codegen" "$NIGHT" codegen "$PASS_DIR/data_enum_ctor/main.afns"
assert_contains "$TMP_DIR/stdout" ".tag = Event_Click" "data enum constructor codegen"
printf 'ok 19 - data enum constructor\n'

run_capture "defer scope check" "$NIGHT" check "$PASS_DIR/defer_scope/main.afns"
run_capture "defer scope run" "$NIGHT" run "$PASS_DIR/defer_scope/main.afns" -o "$TMP_DIR/defer_scope_run"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/defer_scope/main.out"; then
    printf 'Expected output:\n' >&2
    cat "$PASS_DIR/defer_scope/main.out" >&2
    printf 'Actual output:\n' >&2
    cat "$TMP_DIR/stdout" >&2
    fail "defer scope run output mismatch"
fi
printf 'ok 20 - defer scope exit\n'

run_capture "defer control flow check" "$NIGHT" check "$PASS_DIR/defer_control_flow/main.afns"
run_capture "defer control flow run" "$NIGHT" run "$PASS_DIR/defer_control_flow/main.afns" -o "$TMP_DIR/defer_control_flow_run"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/defer_control_flow/main.out"; then
    printf 'Expected output:\n' >&2
    cat "$PASS_DIR/defer_control_flow/main.out" >&2
    printf 'Actual output:\n' >&2
    cat "$TMP_DIR/stdout" >&2
    fail "defer control flow run output mismatch"
fi
printf 'ok 21 - defer break and continue\n'

run_capture "keyword path check" "$NIGHT" check "$PASS_DIR/path_keywords/main.afns"
printf 'ok 22 - keyword package and import paths\n'

run_fail_case "$FAIL_DIR/non_exhaustive_match.afns" "$FAIL_DIR/non_exhaustive_match.err" "non exhaustive match diagnostic"
printf 'ok 23 - non exhaustive match diagnostic\n'

run_fail_case "$FAIL_DIR/duplicate_match_arm.afns" "$FAIL_DIR/duplicate_match_arm.err" "duplicate match arm diagnostic"
printf 'ok 24 - duplicate match arm diagnostic\n'

run_fail_case "$FAIL_DIR/enum_ctor_bad_arity.afns" "$FAIL_DIR/enum_ctor_bad_arity.err" "enum constructor arity diagnostic"
printf 'ok 25 - enum constructor arity diagnostic\n'

run_fail_case "$FAIL_DIR/enum_ctor_bad_type.afns" "$FAIL_DIR/enum_ctor_bad_type.err" "enum constructor type diagnostic"
printf 'ok 26 - enum constructor type diagnostic\n'

run_capture "legacy data enum check" "$NIGHT" check "$PASS_DIR/data_enum/main.afns"
printf 'ok 27 - existing data enum program still checks\n'

run_fail_case "$FAIL_DIR/import_missing.afns" "$FAIL_DIR/import_missing.err" "import missing diagnostic"
printf 'ok 28 - import missing diagnostic\n'

run_capture "option result check" "$NIGHT" check "$PASS_DIR/option_result_check/main.afns"
run_capture "option result build" "$NIGHT" build "$PASS_DIR/option_result_check/main.afns" -o "$TMP_DIR/option_result_bin"
run_capture "option result run" "$NIGHT" run "$PASS_DIR/option_result_check/main.afns" -o "$TMP_DIR/option_result_run"
run_capture "option result codegen" "$NIGHT" codegen "$PASS_DIR/option_result_check/main.afns"
assert_contains "$TMP_DIR/stdout" "typedef struct NS_Option_i32" "option typedef emission"
assert_contains "$TMP_DIR/stdout" "typedef struct NS_Result_i32_Error" "result typedef emission"
printf 'ok 29 - option result build and run\n'

run_capture "option result match check" "$NIGHT" check "$PASS_DIR/option_result_match/main.afns"
run_capture "option result match run" "$NIGHT" run "$PASS_DIR/option_result_match/main.afns" -o "$TMP_DIR/option_result_match_run"
assert_contains "$TMP_DIR/stdout" "ok" "option result match run"
printf 'ok 30 - option result match\n'

run_capture "result try check" "$NIGHT" check "$PASS_DIR/result_try/main.afns"
run_capture "result try run" "$NIGHT" run "$PASS_DIR/result_try/main.afns" -o "$TMP_DIR/result_try_run"
assert_contains "$TMP_DIR/stdout" "ok" "result try run"
printf 'ok 31 - result try propagation\n'

run_fail_case "$FAIL_DIR/some_without_context.afns" "$FAIL_DIR/some_without_context.err" "some without context diagnostic"
printf 'ok 32 - some without context diagnostic\n'

run_fail_case "$FAIL_DIR/option_some_bad_type.afns" "$FAIL_DIR/option_some_bad_type.err" "option some bad type diagnostic"
printf 'ok 33 - option some bad type diagnostic\n'

cp "$PASS_DIR/fmt_input/main.afns" "$TMP_DIR/fmt_main.afns"
run_capture "fmt command" "$NIGHT" fmt "$TMP_DIR/fmt_main.afns"
if ! cmp -s "$TMP_DIR/fmt_main.afns" "$PASS_DIR/fmt_input/main.expected"; then
    printf 'Expected formatted file:\n' >&2
    cat "$PASS_DIR/fmt_input/main.expected" >&2
    printf 'Actual formatted file:\n' >&2
    cat "$TMP_DIR/fmt_main.afns" >&2
    fail "fmt command output mismatch"
fi
printf 'ok 34 - fmt command\n'

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
printf 'ok 35 - night.toml project flow\n'

run_capture "init command" "$NIGHT" init "$TMP_DIR/init_proj"
if [ ! -f "$TMP_DIR/init_proj/night.toml" ] || [ ! -f "$TMP_DIR/init_proj/src/main.afns" ]; then
    fail "init command did not scaffold project"
fi
assert_contains "$TMP_DIR/init_proj/night.toml" "[target]" "init target section"
assert_contains "$TMP_DIR/init_proj/night.toml" "mode = \"native\"" "init target mode"
assert_contains "$TMP_DIR/init_proj/night.toml" "backend = \"c\"" "init target backend"
printf 'ok 36 - init command\n'
