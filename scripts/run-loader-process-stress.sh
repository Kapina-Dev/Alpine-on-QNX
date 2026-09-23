#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
fixture_dir="$project_dir/guest-tests/fixtures"
timeout_command=${TIMEOUT_COMMAND:-timeout}
spawn_count=${SPAWN_COUNT:-100}
failures=0
module=/tmp/linuxemu-phase9-module.so

for fixture in phase9-module-armhf.so dlopen-stress-armhf \
    spawn-thread-stress-armhf; do
    if [ ! -f "$fixture_dir/$fixture" ]; then
        echo "missing fixture: $fixture_dir/$fixture" >&2
        exit 2
    fi
done

cp "$fixture_dir/phase9-module-armhf.so" "$alpine_root$module" || exit 2
trap 'rm -f "$alpine_root$module"' EXIT

echo "=== dlopen executable mapping stress ==="
"$timeout_command" 30 "$linuxemu" --linuxemu-exec "$alpine_root" / \
    "$fixture_dir/dlopen-stress-armhf" /tmp/dlopen-stress "$module"
status=$?
echo "dlopen-stress exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

echo "=== multithreaded posix_spawn stress: $spawn_count ==="
"$timeout_command" 120 "$linuxemu" --linuxemu-exec "$alpine_root" / \
    "$fixture_dir/spawn-thread-stress-armhf" /tmp/spawn-thread-stress \
    "$spawn_count"
status=$?
echo "spawn-thread-stress exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

echo "linuxemu_loader_process_stress_failures=$failures"
test "$failures" -eq 0
