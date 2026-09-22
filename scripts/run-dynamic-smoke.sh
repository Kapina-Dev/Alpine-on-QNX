#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
busybox="$alpine_root/bin/busybox"
interpreter="$alpine_root/lib/ld-musl-armhf.so.1"
timeout_command=${TIMEOUT_COMMAND:-timeout}
failures=0

if [ ! -f "$busybox" ] || [ ! -f "$interpreter" ]; then
    echo "dynamic fixture missing under $alpine_root" >&2
    exit 2
fi

run_guest()
{
    name=$1
    shift
    echo "=== dynamic guest: $name ==="
    LINUXEMU_ROOT="$alpine_root" "$timeout_command" 10 \
        "$linuxemu" "$busybox" "$@"
    status=$?
    echo "$name exit=$status expected=0"
    if [ "$status" -ne 0 ]; then
        failures=$((failures + 1))
    fi
}

run_guest true true
run_guest echo echo dynamic-musl-ok

echo "linuxemu_dynamic_smoke_failures=$failures"
test "$failures" -eq 0
