#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
linuxemu="$project_dir/build/linuxemu"
guest_dir="$project_dir/build/guest-tests"
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
timeout_command=${TIMEOUT_COMMAND:-timeout}
failures=0

run_guest()
{
    guest=$1
    expected=$2
    shift 2

    echo "=== guest: $guest ==="
    if [ "$#" -eq 0 ]; then
        "$timeout_command" 10 "$linuxemu" "$guest_dir/$guest"
    else
        "$timeout_command" 10 "$linuxemu" "$guest_dir/$guest" "$@"
    fi
    status=$?
    echo "$guest exit=$status expected=$expected"
    if [ "$status" -ne "$expected" ]; then
        failures=$((failures + 1))
    fi
}

run_guest write-exit 0
run_guest unknown-syscall 0
run_guest exit-status 37
run_guest initial-stack 0 alpha beta
run_guest memory-syscalls 0

echo "=== guest: file-syscalls ==="
LINUXEMU_ROOT="$alpine_root" "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/file-syscalls"
status=$?
echo "file-syscalls exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

echo "linuxemu_smoke_failures=$failures"
test "$failures" -eq 0
