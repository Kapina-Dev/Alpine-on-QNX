#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
linuxemu="$project_dir/build/linuxemu"
guest="$project_dir/build/guest-tests/signal-frame-syscalls"
queue_guest="$project_dir/build/guest-tests/signal-queue-wait-syscalls"
timeout_command=${TIMEOUT_COMMAND:-timeout}

echo "=== signal guest: frame-mask-return ==="
"$timeout_command" -s 9 10 "$linuxemu" "$guest"
status=$?
echo "signal-frame-syscalls exit=$status expected=0"
test "$status" -eq 0

echo "=== signal guest: realtime-queue-atomic-waits ==="
iteration=1
while test "$iteration" -le 20; do
    "$timeout_command" -s 9 10 "$linuxemu" "$queue_guest"
    status=$?
    echo "signal-queue-wait-syscalls iteration=$iteration exit=$status expected=0"
    test "$status" -eq 0 || exit "$status"
    iteration=$((iteration + 1))
done
