#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir="$project_dir/build"
fixture_dir="$build_dir/loader-negative"
fixture_tool="$build_dir/elf-fixture-tool"
linuxemu="$build_dir/linuxemu"
test_linuxemu="$build_dir/linuxemu-loader-test"
static_guest="$build_dir/guest-tests/write-exit"
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
busybox="$alpine_root/bin/busybox"
timeout_command=${TIMEOUT_COMMAND:-timeout}
failures=0

mkdir -p "$fixture_dir"

expect_rejected()
{
    name=$1
    executable=$2
    guest=$3
    shift 3
    echo "=== rejected guest: $name ==="
    if [ "$#" -eq 0 ]; then
        "$timeout_command" 10 "$executable" "$guest"
    else
        "$timeout_command" 10 "$executable" "$guest" "$@"
    fi
    status=$?
    echo "$name exit=$status expected=1"
    if [ "$status" -ne 1 ]; then failures=$((failures + 1)); fi
}

for mode in magic machine thumb-entry no-sections truncated alignment \
    segment-bounds wx-segment section-mismatch overlap-load; do
    "$fixture_tool" "$static_guest" "$fixture_dir/$mode" "$mode" || exit 2
    expect_rejected "$mode" "$linuxemu" "$fixture_dir/$mode"
done

if [ ! -f "$busybox" ]; then
    echo "dynamic fixture missing under $alpine_root" >&2
    exit 2
fi
"$fixture_tool" "$busybox" "$fixture_dir/relative-interp" relative-interp || exit 2
LINUXEMU_ROOT="$alpine_root" expect_rejected relative-interp \
    "$linuxemu" "$fixture_dir/relative-interp"

echo "=== rejected guest: occupied-range ==="
LINUXEMU_TEST_OCCUPY_STATIC=1 "$timeout_command" 10 \
    "$test_linuxemu" "$static_guest"
status=$?
echo "occupied-range exit=$status expected=1"
if [ "$status" -ne 1 ]; then failures=$((failures + 1)); fi

echo "=== dynamic guest: occupied-first-bias ==="
LINUXEMU_ROOT="$alpine_root" LINUXEMU_TEST_OCCUPY_DYNAMIC=1 \
    "$timeout_command" 10 "$test_linuxemu" "$busybox" true
status=$?
echo "occupied-first-bias exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

echo "linuxemu_loader_negative_failures=$failures"
test "$failures" -eq 0
