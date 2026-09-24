#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
output_dir=${REGRESSION_OUTPUT_DIR:-"$project_dir/build/full-regression"}
world="$alpine_root/etc/apk/world"
resolver="$alpine_root/etc/resolv.conf"
summary="$output_dir/summary.txt"
world_before="$output_dir/world.before"
resolver_before="$output_dir/resolv.before"
resolver_existed=0

[ -t 0 ] && [ -t 1 ] || {
    echo "full regression requires a pseudo-terminal; use ssh -tt" >&2
    exit 2
}
[ -x "$project_dir/build/linuxemu" ] || {
    echo "build/linuxemu is missing; run scripts/build-native-probes.sh" >&2
    exit 2
}
[ -f "$world" ] || { echo "test rootfs is missing" >&2; exit 2; }

rm -rf "$output_dir"
mkdir -p "$output_dir"
cp "$world" "$world_before"
if [ -f "$resolver" ]; then
    cp "$resolver" "$resolver_before"
    resolver_existed=1
fi
: >"$summary"

run_suite()
{
    suite=$1
    echo "=== $suite ==="
    if sh "$project_dir/scripts/$suite.sh" >"$output_dir/$suite.log" 2>&1; then
        echo "$suite=ok" | tee -a "$summary"
    else
        status=$?
        echo "$suite=failed:$status" | tee -a "$summary"
        tail -100 "$output_dir/$suite.log"
        exit "$status"
    fi
}

run_suite run-native-probes
run_suite run-linuxemu-smoke
run_suite run-loader-negative-tests
run_suite run-dynamic-smoke
run_suite run-filesystem-smoke
run_suite run-process-smoke
run_suite run-terminal-time-smoke
run_suite run-network-smoke
run_suite run-package-smoke
run_suite run-thread-smoke
run_suite run-signal-smoke
run_suite run-loader-process-stress
run_suite run-python-smoke
run_suite run-git-smoke

cmp "$world_before" "$world" || {
    echo "rootfs_world_restore=failed" | tee -a "$summary"
    exit 1
}
if [ "$resolver_existed" -eq 1 ]; then
    cmp "$resolver_before" "$resolver" || {
        echo "rootfs_resolver_restore=failed" | tee -a "$summary"
        exit 1
    }
elif [ -f "$resolver" ]; then
    echo "rootfs_resolver_restore=failed" | tee -a "$summary"
    exit 1
fi
package_count=$(LINUXEMU_ROOT="$alpine_root" "$project_dir/build/linuxemu" \
    "$alpine_root/sbin/apk" info | wc -l | awk '{print $1}')
[ "$package_count" -eq 16 ] || {
    echo "rootfs_package_count=failed:$package_count" | tee -a "$summary"
    exit 1
}
echo "rootfs_world_restore=ok" | tee -a "$summary"
echo "rootfs_resolver_restore=ok" | tee -a "$summary"
echo "rootfs_package_count=16" | tee -a "$summary"
echo "linuxemu_full_regression_failures=0" | tee -a "$summary"
