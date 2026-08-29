#!/usr/bin/env bash
# Contract: analyzer targets reject missing/failing toolchains before a clean
# gate or build can hide the failure.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MAKEFILE_UNDER_TEST="${MAKEFILE_UNDER_TEST:-$ROOT/Makefile.cbm}"
REAL_MAKE="$(command -v make)"
FIXTURE="$(mktemp -d "${TMPDIR:-/tmp}/cbm-tool-preflight.XXXXXX")"
trap 'rm -rf -- "$FIXTURE"' EXIT
cd "$ROOT"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

run_make() {
    local log="$1"
    shift
    "$REAL_MAKE" -s -f "$MAKEFILE_UNDER_TEST" "$@" >"$log" 2>&1
}

assert_log_contains() {
    local log="$1" expected="$2"
    case "$(cat "$log")" in
        *"$expected"*) ;;
        *) fail "$(basename "$log") did not report: $expected" ;;
    esac
}

# A missing clang-tidy used to make xargs fail while the final Python process
# accepted an empty stream and returned a false green.
missing_tidy="$FIXTURE/missing/clang-tidy"
missing_log="$FIXTURE/lint-missing.log"
if run_make "$missing_log" lint-mem-ci \
    "CLANG_TIDY=$missing_tidy" "LINT_SRCS=fixture-a.c" "CFLAGS_COMMON="; then
    fail "lint-mem-ci accepted a missing clang-tidy"
fi
assert_log_contains "$missing_log" \
    "ERROR: clang-tidy executable not found or not executable: $missing_tidy"

# An executable analyzer that fails must also fail the target; xargs' status
# cannot be replaced by the gate's clean status.
failing_tidy="$FIXTURE/clang-tidy-fails"
printf '%s\n' '#!/bin/sh' 'exit 42' >"$failing_tidy"
chmod +x "$failing_tidy"
failing_log="$FIXTURE/lint-failing.log"
if run_make "$failing_log" lint-mem-ci \
    "CLANG_TIDY=$failing_tidy" "LINT_SRCS=fixture-a.c" "CFLAGS_COMMON="; then
    fail "lint-mem-ci hid a failing clang-tidy invocation"
fi
assert_log_contains "$failing_log" "ERROR: clang-tidy execution failed"

# The successful path still feeds every translation unit to the real gate and
# exposes the gate's result to callers.
successful_tidy="$FIXTURE/clang-tidy-succeeds"
calls="$FIXTURE/clang-tidy.calls"
printf '%s\n' '#!/bin/sh' "printf '%s\\n' \"\$*\" >>\"\$FAKE_TIDY_CALLS\"" 'exit 0' \
    >"$successful_tidy"
chmod +x "$successful_tidy"
export FAKE_TIDY_CALLS="$calls"
success_log="$FIXTURE/lint-success.log"
run_make "$success_log" lint-mem-ci \
    "CLANG_TIDY=$successful_tidy" \
    "LINT_SRCS=fixture-a.c fixture-b.c" "CFLAGS_COMMON=" ||
    fail "lint-mem-ci rejected a successful clang-tidy run"
assert_log_contains "$success_log" "=== memory gate clean ==="
[ "$(wc -l <"$calls" | tr -d ' ')" = "2" ] ||
    fail "lint-mem-ci did not analyze both controlled translation units"
for source in fixture-a.c fixture-b.c; do
    case "$(cat "$calls")" in
        *"$source"*) ;;
        *) fail "lint-mem-ci did not invoke clang-tidy for $source" ;;
    esac
done

# A successful analyzer process can still report a real finding. Its stdout
# must reach the Python gate, and the gate's nonzero status must reach make.
finding_source="$FIXTURE/unwhitelisted-finding.c"
printf '%s\n' \
    'int controlled_finding(void)' \
    '{' \
    '    return 0;' \
    '}' >"$finding_source"
finding_tidy="$FIXTURE/clang-tidy-finding"
printf '%s\n' '#!/bin/sh' \
    "printf '%s\\n' \"\$3:3:5: warning: controlled unwhitelisted finding [clang-analyzer-unix.Malloc]\"" \
    'exit 0' >"$finding_tidy"
chmod +x "$finding_tidy"
finding_log="$FIXTURE/lint-finding.log"
if run_make "$finding_log" lint-mem-ci \
    "CLANG_TIDY=$finding_tidy" "LINT_SRCS=$finding_source" "CFLAGS_COMMON="; then
    fail "lint-mem-ci hid an unwhitelisted analyzer finding"
fi
assert_log_contains "$finding_log" "=== memory-analyzer findings (gate FAILS) ==="
assert_log_contains "$finding_log" "controlled unwhitelisted finding"

# LSan must reject either missing upstream compiler before recursive make can
# begin or clang --version can run. The markers cover both boundaries.
for missing_compiler in clang clang++; do
    llvm_bin="$FIXTURE/llvm-missing-$missing_compiler"
    mkdir -p "$llvm_bin"
    compiler_marker="$FIXTURE/compiler-started-$missing_compiler"
    export FAKE_COMPILER_MARKER="$compiler_marker"
    for compiler in clang clang++; do
        if [ "$compiler" != "$missing_compiler" ]; then
            printf '%s\n' '#!/bin/sh' ": >\"\$FAKE_COMPILER_MARKER\"" \
                'printf '\''%s\n'\'' "fake upstream clang"' 'exit 0' >"$llvm_bin/$compiler"
            chmod +x "$llvm_bin/$compiler"
        fi
    done

    build_marker="$FIXTURE/build-started-$missing_compiler"
    recursive_make="$FIXTURE/make-$missing_compiler"
    printf '%s\n' '#!/bin/sh' ": >\"\$FAKE_BUILD_MARKER\"" 'exit 99' >"$recursive_make"
    chmod +x "$recursive_make"
    export FAKE_BUILD_MARKER="$build_marker"

    lsan_log="$FIXTURE/lsan-missing-$missing_compiler.log"
    if run_make "$lsan_log" test-lsan \
        "LSAN_LLVM_BIN=$llvm_bin" "MAKE=$recursive_make"; then
        fail "test-lsan accepted missing $llvm_bin/$missing_compiler"
    fi
    assert_log_contains "$lsan_log" \
        "ERROR: test-lsan requires executable $llvm_bin/$missing_compiler"
    [ ! -e "$compiler_marker" ] ||
        fail "test-lsan executed a compiler before checking $missing_compiler"
    [ ! -e "$build_marker" ] ||
        fail "test-lsan started the build before checking $missing_compiler"
done

echo "PASS: Makefile analyzer tool preflights fail closed"
