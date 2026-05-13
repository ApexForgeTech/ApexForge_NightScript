#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMPILER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
PASS_DIR="$SCRIPT_DIR/pass"
FAIL_DIR="$SCRIPT_DIR/fail"
NIGHT="$COMPILER_DIR/night"
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

printf '1..24\n'

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

run_capture "defer check" "$NIGHT" check "$PASS_DIR/defer_test/main.afns"
run_capture "defer build" "$NIGHT" build "$PASS_DIR/defer_test/main.afns" -o "$TMP_DIR/defer_bin"
run_capture "defer codegen" "$NIGHT" codegen "$PASS_DIR/defer_test/main.afns"
assert_contains "$TMP_DIR/stdout" "puts(\"goodbye\")" "defer call emission"
printf 'ok 13 - defer statement\n'

run_capture "for loop check" "$NIGHT" check "$PASS_DIR/for_loop/main.afns"
run_capture "for loop build" "$NIGHT" build "$PASS_DIR/for_loop/main.afns" -o "$TMP_DIR/for_bin"
run_capture "for loop codegen" "$NIGHT" codegen "$PASS_DIR/for_loop/main.afns"
assert_contains "$TMP_DIR/stdout" "for (int32_t i = 0;" "for loop init emission"
assert_contains "$TMP_DIR/stdout" "i += 1)" "for loop post emission"
printf 'ok 14 - for loop\n'

run_capture "packed struct check" "$NIGHT" check "$PASS_DIR/packed_struct/main.afns"
run_capture "packed struct build" "$NIGHT" build "$PASS_DIR/packed_struct/main.afns" -o "$TMP_DIR/packed_bin"
run_capture "packed struct codegen" "$NIGHT" codegen "$PASS_DIR/packed_struct/main.afns"
assert_contains "$TMP_DIR/stdout" "__attribute__((packed))" "packed struct emission"
printf 'ok 15 - packed struct\n'

run_capture "data enum constructor check" "$NIGHT" check "$PASS_DIR/data_enum_ctor/main.afns"
run_capture "data enum constructor build" "$NIGHT" build "$PASS_DIR/data_enum_ctor/main.afns" -o "$TMP_DIR/data_enum_ctor_bin"
run_capture "data enum constructor run" "$NIGHT" run "$PASS_DIR/data_enum_ctor/main.afns" -o "$TMP_DIR/data_enum_ctor_run"
assert_contains "$TMP_DIR/stdout" "ok" "data enum constructor run"
run_capture "data enum constructor codegen" "$NIGHT" codegen "$PASS_DIR/data_enum_ctor/main.afns"
assert_contains "$TMP_DIR/stdout" ".tag = Event_Click" "data enum constructor codegen"
printf 'ok 16 - data enum constructor\n'

run_capture "defer scope check" "$NIGHT" check "$PASS_DIR/defer_scope/main.afns"
run_capture "defer scope run" "$NIGHT" run "$PASS_DIR/defer_scope/main.afns" -o "$TMP_DIR/defer_scope_run"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/defer_scope/main.out"; then
    printf 'Expected output:\n' >&2
    cat "$PASS_DIR/defer_scope/main.out" >&2
    printf 'Actual output:\n' >&2
    cat "$TMP_DIR/stdout" >&2
    fail "defer scope run output mismatch"
fi
printf 'ok 17 - defer scope exit\n'

run_capture "defer control flow check" "$NIGHT" check "$PASS_DIR/defer_control_flow/main.afns"
run_capture "defer control flow run" "$NIGHT" run "$PASS_DIR/defer_control_flow/main.afns" -o "$TMP_DIR/defer_control_flow_run"
if ! cmp -s "$TMP_DIR/stdout" "$PASS_DIR/defer_control_flow/main.out"; then
    printf 'Expected output:\n' >&2
    cat "$PASS_DIR/defer_control_flow/main.out" >&2
    printf 'Actual output:\n' >&2
    cat "$TMP_DIR/stdout" >&2
    fail "defer control flow run output mismatch"
fi
printf 'ok 18 - defer break and continue\n'

run_capture "keyword path check" "$NIGHT" check "$PASS_DIR/path_keywords/main.afns"
printf 'ok 19 - keyword package and import paths\n'

run_fail_case "$FAIL_DIR/non_exhaustive_match.afns" "$FAIL_DIR/non_exhaustive_match.err" "non exhaustive match diagnostic"
printf 'ok 20 - non exhaustive match diagnostic\n'

run_fail_case "$FAIL_DIR/duplicate_match_arm.afns" "$FAIL_DIR/duplicate_match_arm.err" "duplicate match arm diagnostic"
printf 'ok 21 - duplicate match arm diagnostic\n'

run_fail_case "$FAIL_DIR/enum_ctor_bad_arity.afns" "$FAIL_DIR/enum_ctor_bad_arity.err" "enum constructor arity diagnostic"
printf 'ok 22 - enum constructor arity diagnostic\n'

run_fail_case "$FAIL_DIR/enum_ctor_bad_type.afns" "$FAIL_DIR/enum_ctor_bad_type.err" "enum constructor type diagnostic"
printf 'ok 23 - enum constructor type diagnostic\n'

run_capture "legacy data enum check" "$NIGHT" check "$PASS_DIR/data_enum/main.afns"
printf 'ok 24 - existing data enum program still checks\n'
