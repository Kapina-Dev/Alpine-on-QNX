#!/bin/sh
# Phase 4 part 4 Android gate, on the device. Needs build/android-gate.tar
# from scripts/build-android-gate.sh on the laptop (upload it without
# ssh -tt). Unpacks it to build/android-gate-rootfs and runs, with that
# directory as the guest root:
#   /data/android-syscall  static, stripped, Thumb, own entry point;
#   /data/android-gate     dynamic: Android 6.0's /system/bin/linker loads
#                          libc, libm, libutils and their 7 dependencies.
# Checks: no file refused (undecodable-code), no "CANNOT LINK EXECUTABLE",
# each file's patch sites as the laptop counted them (expected-sites.txt;
# the largest patch-sites line per file: the linker also maps small
# read-only pieces of a file), the static program's output and exit 0.
# The dynamic program aims at main() (android_gate_dynamic=main); if
# start-up stops past linking, it reports android_gate_dynamic=fallback
# with the trace tail and passes on the other checks (the design's
# accepted fallback: pids, tids and missing calls belong to later
# phases). Not part of the full regression: the Android files aren't in
# the repo.
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
linuxemu=${LINUXEMU:-"$project_dir/build/linuxemu"}
tarball=${ANDROID_GATE_TAR:-"$project_dir/build/android-gate.tar"}
root="$project_dir/build/android-gate-rootfs"
failures=0

if [ ! -f "$tarball" ]; then
    echo "missing $tarball: build it on the laptop with" \
        "scripts/build-android-gate.sh and upload it" >&2
    exit 2
fi
rm -rf "$root"
mkdir -p "$root" || exit 2
(cd "$root" && tar -xf "$tarball") || exit 2
work="$project_dir/build/android-gate-work"
rm -rf "$work"
mkdir -p "$work" || exit 2

# run NAME GUEST_PATH [ENV=VALUE...]: output in $work/NAME.{out,err,diag}.
# Its own time limit (BerryCore's timeout never fires): after
# $time_limit seconds the run is killed and its status is 124.
time_limit=${ANDROID_GATE_TIME_LIMIT:-60}
run() {
    name=$1
    program=$2
    shift 2
    env LINUXEMU_VERBOSE=1 LINUXEMU_DIAGNOSTICS="$work/$name.diag" ${1+"$@"} \
        "$linuxemu" --linuxemu-exec "$root" / \
        "$root$program" "$program" >"$work/$name.out" 2>"$work/$name.err" &
    pid=$!
    waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt "$time_limit" ]; do
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        echo "$name: still running after ${time_limit}s; killed" \
            >>"$work/$name.err"
        echo 124 >"$work/$name.status"
    else
        wait "$pid"
        echo $? >"$work/$name.status"
    fi
    touch "$work/$name.diag"
}

# check_common NAME: refusals, link errors, patch sites.
check_common() {
    name=$1
    if grep -q "detail=undecodable-code" "$work/$name.diag" ||
        grep -q "can't be decoded safely" "$work/$name.err" ||
        grep -q "linuxemu load: Exec format error" "$work/$name.err"; then
        # (a refused interpreter fails the load before any diagnostics)
        echo "$name refused=yes expected=no"
        grep "undecodable-code" "$work/$name.diag"
        failures=$((failures + 1))
    else
        echo "$name refused=no expected=no"
    fi
    if grep -q "CANNOT LINK EXECUTABLE" "$work/$name.err" "$work/$name.out"
    then
        echo "$name link=failed expected=ok"
        grep "CANNOT LINK" "$work/$name.err" "$work/$name.out"
        failures=$((failures + 1))
    fi
}

# check_sites NAME FILE...: each FILE (relative to the root) has
# patch-sites lines in NAME's run, and the largest equals the laptop's.
check_sites() {
    name=$1
    shift
    for file in "$@"; do
        ino=$(ls -i "$root/$file" | awk '{print $1}')
        want=$(awk -v f="$file" '$1 == f { print $2 }' \
            "$root/expected-sites.txt")
        got=$(awk -v ino="ino=$ino" '
            $1 == "linuxemu:" && $2 == "patch-sites" && $3 == ino {
                n = $6; sub("sites=", "", n); n += 0
                if (!seen || n > max) max = n; seen = 1 }
            END { print seen ? max : "none" }' "$work/$name.err")
        echo "$name sites $file reported=$got expected=$want"
        if [ "$got" != "$want" ]; then failures=$((failures + 1)); fi
    done
}

echo "=== static stripped Thumb NDK program: raw syscall() ==="
run static /data/android-syscall
status=$(cat "$work/static.status")
echo "static exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi
if grep -qx "android-syscall write" "$work/static.out"; then
    echo "static output=ok"
else
    echo "static output=missing"
    failures=$((failures + 1))
fi
check_common static
check_sites static data/android-syscall

echo "=== dynamic Thumb program through Android's linker ==="
run dynamic /data/android-gate
status=$(cat "$work/dynamic.status")
check_common dynamic
# libdl.so is not in the list: bionic's linker provides it itself and
# never maps the file.
check_sites dynamic system/bin/linker system/lib/libc.so system/lib/libm.so \
    system/lib/libutils.so system/lib/libbacktrace.so \
    system/lib/libcutils.so system/lib/liblog.so system/lib/libc++.so \
    system/lib/libbase.so system/lib/libunwind.so data/android-gate
total=$(awk '$1 == "linuxemu:" && $2 == "patch-sites" {
    n = $6; sub("sites=", "", n); t += n } END { print t + 0 }' \
    "$work/dynamic.err")
echo "dynamic patch-sites total=$total"
if grep -qx "android-gate main" "$work/dynamic.out" &&
    grep -qx "android-gate syscall" "$work/dynamic.out" &&
    [ "$status" -eq 0 ]; then
    echo "dynamic exit=$status main=ran"
    echo "android_gate_dynamic=main"
else
    echo "dynamic exit=$status main=not-reached-or-failed"
    echo "--- dynamic stdout"
    cat "$work/dynamic.out"
    echo "--- dynamic stderr (last 20 lines)"
    tail -20 "$work/dynamic.err"
    echo "--- diagnostics"
    cat "$work/dynamic.diag"
    run trace /data/android-gate LINUXEMU_SYSTRACE=1
    echo "--- traced rerun: exit $(cat "$work/trace.status"), last 20 lines"
    tail -20 "$work/trace.err"
    echo "android_gate_dynamic=fallback"
fi

echo "android_gate_failures=$failures"
[ "$failures" -eq 0 ]
