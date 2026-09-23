#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
busybox="$alpine_root/bin/busybox"
timeout_command=${TIMEOUT_COMMAND:-timeout}
output_dir="$project_dir/build/process-smoke"
failures=0

if [ ! -f "$busybox" ]; then
    echo "process fixture missing under $alpine_root" >&2
    exit 2
fi
mkdir -p "$output_dir"
guest_shebang="/tmp/linuxemu-shebang-smoke-$$"
host_shebang="$alpine_root$guest_shebang"
printf '%s\n' '#!/bin/sh' 'echo phase6_shebang_ok' >"$host_shebang"
chmod 755 "$host_shebang"
trap 'rm -f "$host_shebang"' EXIT HUP INT TERM

run_shell()
{
    name=$1
    expected=$2
    command=$3
    output="$output_dir/$name.out"
    echo "=== process guest: $name ==="
    LINUXEMU_ROOT="$alpine_root" "$timeout_command" -s 9 10 \
        "$linuxemu" "$busybox" sh -c "$command" > "$output" 2>&1
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

run_shell pipeline 0 'printf phase3_pipeline | /bin/cat'
require_line pipeline '^phase3_pipeline$'
run_shell multipipe 0 'printf multi_stage | /bin/cat | /bin/cat'
require_line multipipe '^multi_stage$'
run_shell cwd-exec 0 'cd /etc && /bin/pwd && /bin/cat alpine-release'
require_line cwd-exec '^/etc$'
require_line cwd-exec '^3[.]24[.]2$'
run_shell child-status 0 '/bin/false; echo child_status=$?'
require_line child-status '^child_status=1$'
run_shell subshell-status 0 '(exit 23); echo subshell_status=$?'
require_line subshell-status '^subshell_status=23$'
run_shell exec-failure 0 '/does-not-exist; echo exec_status=$?'
require_line exec-failure '^exec_status=127$'
run_shell background-wait 0 '/bin/true & wait; echo wait_status=$?'
require_line background-wait '^wait_status=0$'
run_shell environment 0 \
    'FOO=phase3 /usr/bin/env | /bin/grep -E "^(FOO|LINUXEMU_ROOT|LD_LIBRARY_PATH)="'
require_line environment '^FOO=phase3$'
if grep -q '^LINUXEMU_ROOT=\|^LD_LIBRARY_PATH=' \
        "$output_dir/environment.out"; then
    echo "environment exposed a host-only variable" >&2
    failures=$((failures + 1))
fi
run_shell shebang-exec 0 "$guest_shebang"
require_line shebang-exec '^phase6_shebang_ok$'
run_shell shell-exit 37 'exit 37'

echo "linuxemu_process_smoke_failures=$failures"
test "$failures" -eq 0
