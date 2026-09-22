#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)

sdk_root=${QNX_HOST:-"$project_dir/sdk"}
target_root=${QNX_TARGET:-"$sdk_root/target_10_3_1_995/qnx6"}
build_dir=${BUILD_DIR:-"$project_dir/build/native-probes"}
cc="$sdk_root/bin/gcc"

if [ ! -x "$cc" ]; then
    echo "compiler not found or not executable: $cc" >&2
    exit 1
fi

mkdir -p "$build_dir"
export QNX_HOST="$sdk_root"
export QNX_TARGET="$target_root"
export PATH="$sdk_root/bin:$PATH"
export LD_LIBRARY_PATH="$sdk_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

common_flags="--sysroot=$target_root -Wall -Wextra -O2"

echo "compiler=$($cc -dumpversion)"
echo "target=$($cc -dumpmachine)"
echo "qnx_host=$QNX_HOST"
echo "qnx_target=$QNX_TARGET"
echo "sdk_library_path=$sdk_root/lib"

set -x
"$cc" $common_flags -o "$build_dir/hello" \
    "$project_dir/native-probes/hello.c"
"$cc" $common_flags -o "$build_dir/bb10-native-baseline" \
    "$project_dir/root-analysis/bb10-native-baseline.c"
"$cc" $common_flags -o "$build_dir/thread-signal-stress" \
    "$project_dir/native-probes/thread-signal-stress.c"
"$cc" $common_flags -marm -DPROBE_MODE='"arm"' \
    -o "$build_dir/instruction-trap-arm" \
    "$project_dir/native-probes/instruction-trap.c"
"$cc" $common_flags -mthumb -DPROBE_THUMB=1 -DPROBE_MODE='"thumb"' \
    -o "$build_dir/instruction-trap-thumb" \
    "$project_dir/native-probes/instruction-trap.c"
"$cc" $common_flags -DPROBE_MODE='"arm"' \
    -o "$build_dir/executable-memory-arm" \
    "$project_dir/native-probes/executable-memory.c"
"$cc" $common_flags -DGENERATED_THUMB=1 -DPROBE_MODE='"thumb"' \
    -o "$build_dir/executable-memory-thumb" \
    "$project_dir/native-probes/executable-memory.c"
"$cc" $common_flags -DINITIAL_RWX=1 -DPROBE_MODE='"arm-rwx"' \
    -o "$build_dir/executable-memory-arm-rwx" \
    "$project_dir/native-probes/executable-memory.c"
"$cc" $common_flags -DINITIAL_RWX=1 -DGENERATED_THUMB=1 \
    -DPROBE_MODE='"thumb-rwx"' \
    -o "$build_dir/executable-memory-thumb-rwx" \
    "$project_dir/native-probes/executable-memory.c"
set +x

"$sdk_root/bin/readelf" -h "$build_dir/hello"
"$sdk_root/bin/readelf" -h "$build_dir/bb10-native-baseline"
"$sdk_root/bin/readelf" -h "$build_dir/thread-signal-stress"
"$sdk_root/bin/readelf" -h "$build_dir/instruction-trap-arm"
"$sdk_root/bin/readelf" -h "$build_dir/instruction-trap-thumb"
"$sdk_root/bin/readelf" -h "$build_dir/executable-memory-arm"
"$sdk_root/bin/readelf" -h "$build_dir/executable-memory-thumb"
"$sdk_root/bin/readelf" -h "$build_dir/executable-memory-arm-rwx"
"$sdk_root/bin/readelf" -h "$build_dir/executable-memory-thumb-rwx"
