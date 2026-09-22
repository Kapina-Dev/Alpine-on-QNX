#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir=${BUILD_DIR:-"$project_dir/build/native-probes"}
failures=0

run_probe()
{
    name=$1
    echo "=== $name ==="
    "$build_dir/$name"
    status=$?
    echo "$name exit=$status"
    if [ "$status" -ne 0 ]; then
        failures=$((failures + 1))
    fi
}

run_probe hello
run_probe bb10-native-baseline
run_probe thread-signal-stress
run_probe instruction-trap-arm
run_probe instruction-trap-thumb
run_probe executable-memory-arm
run_probe executable-memory-thumb
run_probe tls-registers
run_probe emulated-tls-read-arm
run_probe emulated-tls-read-thumb

echo "native_probe_suite_failures=$failures"
test "$failures" -eq 0
