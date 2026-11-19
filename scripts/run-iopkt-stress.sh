#!/bin/sh
# WARNING: THIS CAN CRASH io-pkt ON BB10.  Its first run on the Passport
# (2026-11-19, docs/FINDINGS.md) killed io-pkt: the network stayed down
# until the phone was rebooted.  Run it only with someone at the phone to
# reboot it.  It is NOT part of the regression (run-full-regression.sh).
#
# Signal-wait channel / io-pkt stress (guest-tests/iopkt-signal-stress.c),
# kept for isolating that crash.  One run lasts SECONDS (default 60).
#   sh scripts/run-iopkt-stress.sh [SECONDS] [RUNS]
# LINUXEMU picks the binary (default build/linuxemu).
# BerryCore timeout never fires: the fixture has its own watchdog and alarm.
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu=${LINUXEMU:-"$project_dir/build/linuxemu"}
seconds=${1:-60}
runs=${2:-1}
name=iopkt-signal-stress
fixture="$project_dir/guest-tests/fixtures/$name-armhf"
guest_copy=/tmp/linuxemu-fixture-$name
failures=0

if [ ! -f "$alpine_root/bin/busybox" ]; then
    echo "stress fixture rootfs missing under $alpine_root" >&2
    exit 2
fi

cp "$fixture" "$alpine_root$guest_copy"
run=1
while [ "$run" -le "$runs" ]; do
    echo "=== $name run $run/$runs (${seconds}s, $(basename "$linuxemu")) ==="
    "$linuxemu" --linuxemu-exec "$alpine_root" / "$alpine_root$guest_copy" \
        "$guest_copy" "$seconds"
    status=$?
    echo "$name run=$run exit=$status expected=0"
    if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi
    run=$((run + 1))
done
rm -f "$alpine_root$guest_copy"
echo "linuxemu_iopkt_stress_failures=$failures"
test "$failures" -eq 0
