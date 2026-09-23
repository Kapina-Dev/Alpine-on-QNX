#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
linuxemu="$project_dir/build/linuxemu"
guest="$project_dir/build/guest-tests/signal-frame-syscalls"
timeout_command=${TIMEOUT_COMMAND:-timeout}

echo "=== signal guest: frame-mask-return ==="
"$timeout_command" -s 9 10 "$linuxemu" "$guest"
status=$?
echo "signal-frame-syscalls exit=$status expected=0"
test "$status" -eq 0
