#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
busybox="$alpine_root/bin/busybox"
failures=0

run_guest()
{
    name=$1
    shift
    echo "=== terminal/time guest: $name ==="
    LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$@"
    status=$?
    echo "$name exit=$status expected=0"
    if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi
}

run_guest syscall-probe "$project_dir/build/guest-tests/terminal-time-syscalls"

epoch=$(LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$busybox" date +%s)
case "$epoch" in
    *[!0-9]*|'') echo "date returned invalid epoch: $epoch" >&2; failures=$((failures + 1));;
    *) if [ "$epoch" -lt 1700000000 ]; then
        echo "date returned implausible epoch: $epoch" >&2
        failures=$((failures + 1))
       fi;;
esac
echo "busybox_epoch=$epoch"

run_guest sleep "$busybox" sleep 0.01

if [ -t 0 ]; then
    interactive=$(LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$busybox" \
        sh -i -c 'echo interactive_shell_ok' 2>&1)
    interactive_status=$?
    echo "$interactive"
    if [ "$interactive_status" -ne 0 ] ||
        ! echo "$interactive" | grep -q '^interactive_shell_ok$'; then
        echo "interactive BusyBox shell failed" >&2
        failures=$((failures + 1))
    fi
    before=$(LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$busybox" stty -a 2>&1)
    echo "$before"
    case "$before" in *"speed "*" baud"*) :;;
        *) echo "stty did not report terminal speed" >&2; failures=$((failures + 1));;
    esac
    LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$busybox" stty -echo
    changed=$(LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$busybox" stty -a 2>&1)
    LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$busybox" stty echo
    case "$changed" in *" -echo "*|*" -echo"*) :;;
        *) echo "stty failed to disable echo" >&2; failures=$((failures + 1));;
    esac
else
    echo "terminal round-trip skipped: standard input is not a tty"
fi

echo "linuxemu_terminal_time_smoke_failures=$failures"
test "$failures" -eq 0
