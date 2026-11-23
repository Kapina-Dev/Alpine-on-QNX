#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
linuxemu="$project_dir/build/linuxemu"
guest_dir="$project_dir/build/guest-tests"
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
timeout_command=${TIMEOUT_COMMAND:-timeout}
failures=0

run_guest()
{
    guest=$1
    expected=$2
    shift 2

    echo "=== guest: $guest ==="
    if [ "$#" -eq 0 ]; then
        "$timeout_command" 10 "$linuxemu" "$guest_dir/$guest"
    else
        "$timeout_command" 10 "$linuxemu" "$guest_dir/$guest" "$@"
    fi
    status=$?
    echo "$guest exit=$status expected=$expected"
    if [ "$status" -ne "$expected" ]; then
        failures=$((failures + 1))
    fi
}

run_guest write-exit 0
run_guest unknown-syscall 0
run_guest exit-status 37
run_guest initial-stack 0 alpha beta
run_guest memory-syscalls 0
run_guest runtime-exec-plain 0
run_guest runtime-exec-reserved 0
run_guest jit-patch-hazard 0
# Generated system calls in anonymous memory need LINUXEMU_PATCH_ANONYMOUS=1
# (Phase 4 part 1).
export LINUXEMU_PATCH_ANONYMOUS=1
run_guest runtime-exec-syscalls 0
# These open or execve host paths, so they need the host root; the second
# checks that the switch survives an execve.
export LINUXEMU_ROOT=/
run_guest exec-argv1 0 "$guest_dir/runtime-exec-syscalls"
unset LINUXEMU_PATCH_ANONYMOUS
run_guest runtime-exec-mixed 0
run_guest runtime-thumb-map 0
run_guest runtime-thumb-map 0 flip
unset LINUXEMU_ROOT
run_guest entropy-syscalls 0
run_guest cacheflush-syscalls 0
run_guest thread-futex-syscalls 0

echo "=== guest: file-syscalls ==="
LINUXEMU_ROOT="$alpine_root" "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/file-syscalls"
status=$?
echo "file-syscalls exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

# execve needs a guest root; "/" maps guest paths to host paths unchanged.
echo "=== guest: android-process-syscalls ==="
LINUXEMU_ROOT=/ "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/android-process-syscalls"
status=$?
echo "android-process-syscalls exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

# mremap-syscalls maps /dev/zero, so it needs the host root.
echo "=== guest: mremap-syscalls ==="
LINUXEMU_ROOT=/ "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/mremap-syscalls"
status=$?
echo "mremap-syscalls exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi
echo "=== guest: mremap-syscalls patched ==="
LINUXEMU_ROOT=/ LINUXEMU_PATCH_ANONYMOUS=1 "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/mremap-syscalls" patched
status=$?
echo "mremap-syscalls-patched exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

diag_dir="$project_dir/build/diag"
rm -rf "$diag_dir"
mkdir -p "$diag_dir"

# resource-syscalls must also leave itimer-virtual and rlimit-resource lines
# in the diagnostics report.
echo "=== guest: resource-syscalls ==="
resource_report="$diag_dir/resource.txt"
LINUXEMU_DIAGNOSTICS="$resource_report" "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/resource-syscalls"
status=$?
echo "resource-syscalls exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi
for detail in itimer-virtual rlimit-resource; do
    if ! grep -q "detail=$detail" "$resource_report" 2>/dev/null; then
        echo "resource-syscalls: diagnostics report lacks detail=$detail" >&2
        failures=$((failures + 1))
    fi
done

# process-resource-syscalls must leave the refused and unsupported cases in
# the diagnostics report.
echo "=== guest: process-resource-syscalls ==="
process_resource_report="$diag_dir/process-resource.txt"
LINUXEMU_DIAGNOSTICS="$process_resource_report" "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/process-resource-syscalls"
status=$?
echo "process-resource-syscalls exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi
for detail in sched-realtime capset-nonzero pdeathsig-delivery \
        membarrier-cmd priority-no-effect; do
    if ! grep -q "detail=$detail" "$process_resource_report" 2>/dev/null; then
        echo "process-resource-syscalls: diagnostics report lacks detail=$detail" >&2
        failures=$((failures + 1))
    fi
done

diag_fail()
{
    echo "$1: FAIL $2"
    failures=$((failures + 1))
}

diag_field()
{
    sed -n "s/.* $2=\([^ ]*\).*/\1/p" "$1"
}

echo "=== diagnostics: diag-basic ==="
report="$diag_dir/basic.txt"
LINUXEMU_ROOT=/ LINUXEMU_DIAGNOSTICS="$report" "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/diagnostics-syscalls"
status=$?
grep 'nr=999 name=unknown error=ENOSYS' "$report" > "$diag_dir/basic.999" \
    2>/dev/null
lines=$(wc -l < "$diag_dir/basic.999" | tr -d ' ')
grep 'count=2 ' "$diag_dir/basic.999" > "$diag_dir/basic.c2"
grep 'count=1 ' "$diag_dir/basic.999" > "$diag_dir/basic.c1"
grep 'count=3 ' "$diag_dir/basic.999" > "$diag_dir/basic.c3"
parent_pid=$(diag_field "$diag_dir/basic.c2" pid)
child_pid=$(diag_field "$diag_dir/basic.c1" pid)
exec_pid=$(diag_field "$diag_dir/basic.c3" pid)
echo "diag-basic exit=$status lines=$lines parent=$parent_pid" \
    "child=$child_pid exec=$exec_pid"
if [ "$status" -ne 0 ] || [ "$lines" != 3 ] || [ -z "$parent_pid" ] ||
    [ -z "$child_pid" ] || [ "$exec_pid" != "$parent_pid" ] ||
    [ "$child_pid" = "$parent_pid" ]; then
    diag_fail diag-basic "unexpected report"
    cat "$report" 2>/dev/null
fi

echo "=== diagnostics: diag-prctl ==="
report="$diag_dir/prctl.txt"
LINUXEMU_ROOT=/ LINUXEMU_DIAGNOSTICS="$report" "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/android-process-syscalls"
status=$?
lines=$(grep -c 'nr=172 name=prctl error=EINVAL ' "$report" 2>/dev/null)
echo "diag-prctl exit=$status prctl_lines=${lines:-0}"
if [ "$status" -ne 0 ] || [ "${lines:-0}" != 2 ] ||
    ! grep -q 'count=1 detail=prctl-set-vma ' "$report" ||
    ! grep -q 'count=1 detail=prctl-option ' "$report"; then
    diag_fail diag-prctl "missing prctl records"
fi

echo "=== diagnostics: diag-fatal ==="
report="$diag_dir/fatal.txt"
LINUXEMU_DIAGNOSTICS="$report" "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/diagnostics-fatal"
status=$?
echo "diag-fatal exit=$status expected=125"
if [ "$status" -ne 125 ] ||
    ! grep -q 'nr=999 name=unknown error=ENOSYS count=1 ' "$report" \
        2>/dev/null; then
    diag_fail diag-fatal "no report from the fatal path"
fi

echo "=== diagnostics: diag-abort ==="
report="$diag_dir/abort.txt"
LINUXEMU_DIAGNOSTICS="$report" "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/diagnostics-abort"
status=$?
echo "diag-abort exit=$status expected=134"
if [ "$status" -ne 134 ] ||
    ! grep -q 'nr=999 name=unknown error=ENOSYS count=1 ' "$report" \
        2>/dev/null; then
    diag_fail diag-abort "no report before default-action SIGABRT"
fi

echo "=== diagnostics: diag-sigpipe ==="
report="$diag_dir/sigpipe.txt"
LINUXEMU_DIAGNOSTICS="$report" "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/diagnostics-sigpipe"
status=$?
echo "diag-sigpipe exit=$status expected=141"
if [ "$status" -ne 141 ] ||
    ! grep -q 'nr=999 name=unknown error=ENOSYS count=1 ' "$report" \
        2>/dev/null; then
    diag_fail diag-sigpipe "no report before kernel-sent SIGPIPE"
fi

# SIGTERM to a fresh fork child, with diagnostics on, launched as a
# background job (as the corpus does). The diagnostics terminate handler
# once deadlocked such children (about 2 in 10).
echo "=== diagnostics: diag-sigterm-race ==="
race_failures=0
race_round=0
while [ "$race_round" -lt 20 ]; do
    race_round=$((race_round + 1))
    LINUXEMU_ROOT="$alpine_root" LINUXEMU_DIAGNOSTICS="$diag_dir/race.txt" \
        "$linuxemu" "$alpine_root/bin/busybox" sh -c \
        'sleep 30 & p=$!; kill -TERM $p; wait $p; echo $?' \
        > "$diag_dir/race.out" 2>&1 &
    race_pid=$!
    ( sleep 15; kill -9 "$race_pid" ) </dev/null >/dev/null 2>&1 &
    race_watchdog=$!
    wait "$race_pid"
    status=$?
    kill -9 "$race_watchdog" 2>/dev/null
    if [ "$status" -ne 0 ] ||
        [ "$(tail -n 1 "$diag_dir/race.out")" != 143 ]; then
        race_failures=$((race_failures + 1))
    fi
done
echo "diag-sigterm-race failures=$race_failures/20"
if [ "$race_failures" -ne 0 ]; then
    diag_fail diag-sigterm-race "fork child hung after SIGTERM"
fi

echo "=== diagnostics: diag-off ==="
before=$(ls "$diag_dir" | wc -l | tr -d ' ')
LINUXEMU_ROOT=/ "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/diagnostics-syscalls"
status=$?
after=$(ls "$diag_dir" | wc -l | tr -d ' ')
echo "diag-off exit=$status files_before=$before files_after=$after"
if [ "$status" -ne 0 ] || [ "$before" != "$after" ]; then
    diag_fail diag-off "unset variable changed behaviour"
fi

echo "=== diagnostics: diag-unwritable ==="
LINUXEMU_ROOT=/ LINUXEMU_DIAGNOSTICS=/nonexistent-dir/r.txt \
    "$timeout_command" 10 "$linuxemu" "$guest_dir/diagnostics-syscalls"
status=$?
echo "diag-unwritable exit=$status expected=0"
if [ "$status" -ne 0 ]; then
    diag_fail diag-unwritable "guest status changed"
fi

# Anonymous memory made executable that holds an svc-like word is reported
# (the guest's own status is checked by its run_guest line, not here).
echo "=== diagnostics: diag-anon-exec ==="
report="$diag_dir/anon-exec.txt"
rm -f "$report"
LINUXEMU_DIAGNOSTICS="$report" "$timeout_command" 30 \
    "$linuxemu" "$guest_dir/jit-patch-hazard" 2>/dev/null
status=$?
lines=$(grep 'name=mprotect ' "$report" 2>/dev/null |
    grep -c 'detail=anon-exec-svc')
echo "diag-anon-exec exit=$status records=${lines:-0}"
if [ "${lines:-0}" -lt 1 ]; then
    diag_fail diag-anon-exec "no anon-exec-svc record"
    cat "$report" 2>/dev/null
fi

# Without LINUXEMU_PATCH_ANONYMOUS, a generated svc in anonymous memory is
# not patched (it runs as a QNX kernel call and the guest dies); it must at
# least be named in the report, and the guest must not hang.
echo "=== diagnostics: diag-anon-svc-unpatched ==="
report="$diag_dir/anon-svc-unpatched.txt"
rm -f "$report"
LINUXEMU_DIAGNOSTICS="$report" "$timeout_command" 10 \
    "$linuxemu" "$guest_dir/runtime-exec-syscalls" 2>/dev/null
status=$?
lines=$(grep 'name=mprotect ' "$report" 2>/dev/null |
    grep -c 'detail=anon-exec-svc')
echo "diag-anon-svc-unpatched exit=$status records=${lines:-0}"
if [ "${lines:-0}" -lt 1 ] || [ "$status" -eq 124 ]; then
    diag_fail diag-anon-svc-unpatched "no record, or the guest hung"
fi

echo "linuxemu_smoke_failures=$failures"
test "$failures" -eq 0
