#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
busybox="$alpine_root/bin/busybox"
timeout_command=${TIMEOUT_COMMAND:-timeout}
output_dir="$project_dir/build/filesystem-smoke"
host_secret="$project_dir/phase2-host-secret"
failures=0

absolute_link="$alpine_root/tmp/linuxemu-absolute-link"
relative_link="$alpine_root/tmp/linuxemu-relative-link"
escape_link="$alpine_root/tmp/linuxemu-host-escape"
redirect_file="$alpine_root/tmp/linuxemu-phase2.txt"

cleanup()
{
    rm -f "$absolute_link" "$relative_link" "$escape_link" \
        "$redirect_file" "$host_secret"
}
trap cleanup 0 1 2 15

if [ ! -f "$busybox" ]; then
    echo "filesystem fixture missing under $alpine_root" >&2
    exit 2
fi
mkdir -p "$output_dir"
cleanup
echo host_secret_must_not_leak > "$host_secret"
ln -s /etc/alpine-release "$absolute_link"
ln -s ../../../../etc/alpine-release "$relative_link"
ln -s "$host_secret" "$escape_link"

run_guest()
{
    name=$1
    expected=$2
    shift 2
    output="$output_dir/$name.out"
    echo "=== filesystem guest: $name ==="
    LINUXEMU_ROOT="$alpine_root" "$timeout_command" 10 \
        "$linuxemu" "$busybox" "$@" > "$output" 2>&1
    status=$?
    cat "$output"
    echo "$name exit=$status expected=$expected"
    if [ "$status" -ne "$expected" ]; then failures=$((failures + 1)); fi
}

require_line()
{
    name=$1
    pattern=$2
    if ! grep -q "$pattern" "$output_dir/$name.out"; then
        echo "$name missing expected output: $pattern" >&2
        failures=$((failures + 1))
    fi
}

run_guest pwd 0 pwd
require_line pwd '^/$'
run_guest cat-release 0 cat /etc/alpine-release
require_line cat-release '^3[.]24[.]2$'
run_guest lexical-clamp 0 cat /../../etc/alpine-release
require_line lexical-clamp '^3[.]24[.]2$'
run_guest list-etc 0 ls /etc
require_line list-etc '^alpine-release$'
require_line list-etc '^passwd$'
run_guest readlink 0 readlink /bin/sh
require_line readlink '^/bin/busybox$'
run_guest absolute-symlink 0 cat /tmp/linuxemu-absolute-link
require_line absolute-symlink '^3[.]24[.]2$'
run_guest relative-symlink 0 cat /tmp/linuxemu-relative-link
require_line relative-symlink '^3[.]24[.]2$'
run_guest missing-file 1 cat /does-not-exist
run_guest host-escape 1 cat /tmp/linuxemu-host-escape
if grep -q host_secret_must_not_leak "$output_dir/host-escape.out"; then
    echo "host-escape exposed host-only content" >&2
    failures=$((failures + 1))
fi
run_guest redirect 0 sh -c 'echo phase2_redirect_works > /tmp/linuxemu-phase2.txt'
run_guest redirected-cat 0 cat /tmp/linuxemu-phase2.txt
require_line redirected-cat '^phase2_redirect_works$'

echo "linuxemu_filesystem_smoke_failures=$failures"
test "$failures" -eq 0
