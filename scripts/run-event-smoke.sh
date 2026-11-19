#!/bin/sh
# Phase 3 event objects: eventfd (and later epoll) guest fixtures.
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu=${LINUXEMU:-"$project_dir/build/linuxemu"}
failures=0

if [ ! -f "$alpine_root/bin/busybox" ]; then
    echo "event fixture missing under $alpine_root" >&2
    exit 2
fi

# C guest fixtures run from inside the rootfs, so a fixture that execs
# itself finds its own path.
run_rootfs_fixture()
{
    name=$1
    fixture="$project_dir/guest-tests/fixtures/$name-armhf"
    guest_copy=/tmp/linuxemu-fixture-$name
    echo "=== event guest: $name ==="
    cp "$fixture" "$alpine_root$guest_copy"
    "$linuxemu" --linuxemu-exec "$alpine_root" / "$alpine_root$guest_copy" \
        "$guest_copy"
    status=$?
    rm -f "$alpine_root$guest_copy"
    echo "$name exit=$status expected=0"
    if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi
}

run_guest()
{
    name=$1
    echo "=== event guest: $name ==="
    LINUXEMU_ROOT="$alpine_root" "$linuxemu" \
        "$project_dir/build/guest-tests/$name"
    status=$?
    echo "$name exit=$status expected=0"
    if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi
}

run_rootfs_fixture eventfd-semantics
run_rootfs_fixture eventfd-blocking
run_rootfs_fixture eventfd-share
run_guest wake-eventfd-dup

echo "linuxemu_event_smoke_failures=$failures"
test "$failures" -eq 0
