#!/bin/sh
# libuv's own tests under Linuxemu (Phase 3 part 4 gate). Runs each test
# in scripts/libuv-subset.txt with build/libuv/uv_run_tests (from
# scripts/build-libuv-tests.sh), copied with its fixtures to
# /tmp/linuxemu-libuv in the test rootfs and run from there.
#
#   sh scripts/run-libuv-smoke.sh         the subset: one
#                                         "NAME exit=N expected=0" line per
#                                         test, then linuxemu_libuv_failures=N
#   sh scripts/run-libuv-smoke.sh --all   every test in the suite, then
#                                         libuv_all pass=P fail=F skip=S
#
# A test runs as "uv_run_tests NAME NAME" (the test body in one process).
# A test with a helper process (an echo server; "--list" names it) runs as
# "uv_run_tests NAME" instead, so libuv's runner starts the helper, waits
# until it is ready and stops it afterwards; its own timeouts are scaled
# by UV_TEST_TIMEOUT_MULTIPLIER (default 10). Each test is ended after
# LIBUV_TEST_TIMEOUT seconds (default 60) and then scores exit=124. Exit
# 7 is libuv's skip. Per-test output and LINUXEMU_DIAGNOSTICS reports go
# to build/libuv-smoke/.
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu=${LINUXEMU:-"$project_dir/build/linuxemu"}
libuv_dir=${LIBUV_BUILD_DIR:-"$project_dir/build/libuv"}
subset=${LIBUV_SUBSET:-"$script_dir/libuv-subset.txt"}
test_timeout=${LIBUV_TEST_TIMEOUT:-60}
output_dir=${LIBUV_OUTPUT_DIR:-"$project_dir/build/libuv-smoke"}
multiplier=${UV_TEST_TIMEOUT_MULTIPLIER:-10}
guest_dir=/tmp/linuxemu-libuv
host_dir="$alpine_root$guest_dir"
timeout_marker="$output_dir/.timed-out"

mode=subset
case ${1:-} in
    '') ;;
    --all) mode=all ;;
    *) echo "usage: $0 [--all]" >&2; exit 2 ;;
esac

if [ ! -f "$libuv_dir/uv_run_tests" ] || [ ! -d "$libuv_dir/fixtures" ]; then
    echo "libuv tests missing; run scripts/build-libuv-tests.sh" >&2
    exit 2
fi
if [ ! -f "$alpine_root/bin/busybox" ]; then
    echo "test rootfs missing under $alpine_root" >&2
    exit 2
fi

rm -rf "$output_dir" "$host_dir"
mkdir -p "$output_dir" "$host_dir/test" || exit 2
cp "$libuv_dir/uv_run_tests" "$host_dir/uv_run_tests" &&
    cp -R "$libuv_dir/fixtures" "$host_dir/test/fixtures" || exit 2
trap 'rm -rf "$host_dir"' EXIT
trap 'exit 130' HUP INT TERM

# timed <seconds> <command> [args...]: like corpus_timeout, but a run that
# was ended returns 124.
timed()
{
    timed_seconds=$1
    shift
    rm -f "$timeout_marker"
    "$@" &
    timed_pid=$!
    ( sleep "$timed_seconds"; : >"$timeout_marker"; kill -KILL "$timed_pid" ) \
        </dev/null >/dev/null 2>&1 &
    timed_watchdog=$!
    wait "$timed_pid"
    timed_status=$?
    kill -KILL "$timed_watchdog" 2>/dev/null
    if [ -f "$timeout_marker" ]; then
        rm -f "$timeout_marker"
        timed_status=124
    fi
    return "$timed_status"
}

# uv <diagnostics file> [args...]: run uv_run_tests in the guest from
# /tmp/linuxemu-libuv.
uv()
{
    uv_diagnostics=$1
    shift
    timed "$test_timeout" env UV_TEST_TIMEOUT_MULTIPLIER="$multiplier" \
        LINUXEMU_DIAGNOSTICS="$uv_diagnostics" \
        "$linuxemu" --linuxemu-exec "$alpine_root" "$guest_dir" \
        "$host_dir/uv_run_tests" "$guest_dir/uv_run_tests" ${1+"$@"}
}

# Files a test leaves in its directory (sockets, watched files) are removed
# before the next one starts.
clean_guest_dir()
{
    for entry in "$host_dir"/* "$host_dir"/.[!.]*; do
        case ${entry##*/} in
            uv_run_tests|test|'*'|'.[!.]*') ;;
            *) rm -rf "$entry" ;;
        esac
    done
}

uv "$output_dir/list.diagnostics" --list >"$output_dir/list.txt" 2>&1
list_status=$?
if [ "$list_status" -ne 0 ]; then
    cat "$output_dir/list.txt"
    echo "uv_run_tests --list exit=$list_status expected=0"
    echo "linuxemu_libuv_failures=1"
    exit 1
fi
awk '/\(helpers:/ { print $1 }' "$output_dir/list.txt" >"$output_dir/helpers.txt"

if [ "$mode" = all ]; then
    awk 'NF { print $1 }' "$output_dir/list.txt" >"$output_dir/tests.txt"
else
    sed 's/#.*//' "$subset" | awk 'NF { print $1 }' >"$output_dir/tests.txt"
fi

failures=0
pass=0
skip=0
while read -r name; do
    clean_guest_dir
    log="$output_dir/$name.log"
    if awk -v n="$name" '$0 == n { found = 1 } END { exit !found }' \
            "$output_dir/helpers.txt"; then
        uv "$output_dir/$name.diagnostics" "$name" >"$log" 2>&1 </dev/null
    else
        uv "$output_dir/$name.diagnostics" "$name" "$name" \
            >"$log" 2>&1 </dev/null
    fi
    status=$?
    [ -s "$output_dir/$name.diagnostics" ] ||
        rm -f "$output_dir/$name.diagnostics"
    echo "$name exit=$status expected=0"
    case $status in
        0) pass=$((pass + 1)) ;;
        7) skip=$((skip + 1)) ;;
    esac
    if [ "$status" -ne 0 ]; then
        failures=$((failures + 1))
        first=$(awk '/Assertion failed|No test (part )?with that name/ {
                print; found = 1; exit }
            END { if (!found) exit 1 }' "$log" ||
            awk 'NF { print; exit }' "$log")
        echo "# $name: $(printf '%s' "$first" | cut -c1-200)"
    fi
done <"$output_dir/tests.txt"

if [ "$mode" = all ]; then
    echo "libuv_all pass=$pass fail=$((failures - skip)) skip=$skip"
    test "$failures" -eq "$skip"
else
    echo "linuxemu_libuv_failures=$failures"
    test "$failures" -eq 0
fi
