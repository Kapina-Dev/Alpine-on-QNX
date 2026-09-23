#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
linuxemu="$project_dir/build/linuxemu"
guest_dir="$project_dir/build/guest-tests"
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
musl_fixture="$project_dir/guest-tests/fixtures/pthread-smoke-armhf"
timeout_command=${TIMEOUT_COMMAND:-timeout}
failures=0

run_thread_guest()
{
    guest=$1
    limit=$2
    echo "=== thread guest: $guest ==="
    "$timeout_command" -s 9 "$limit" "$linuxemu" "$guest_dir/$guest"
    status=$?
    echo "$guest exit=$status expected=0"
    if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi
}

run_thread_guest thread-futex-syscalls 15
run_thread_guest clone-vfork-syscalls 15
run_thread_guest futex-contention-stress 30
run_thread_guest thread-lifecycle-stress 120

echo "=== thread guest: pthread-smoke-musl ==="
LINUXEMU_ROOT="$alpine_root" "$timeout_command" -s 9 30 \
    "$linuxemu" "$musl_fixture"
status=$?
echo "pthread-smoke-musl exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

echo "linuxemu_thread_smoke_failures=$failures"
test "$failures" -eq 0
