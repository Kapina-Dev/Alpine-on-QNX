#!/bin/sh
# Build SQLite as a Thumb-2 library with unwind tables, unstripped and
# stripped, for the Phase 4 part 3 real-world decoding check: the
# loader-process stress suite runs stripped-compare on the pair when it is
# present. Run on the device after scripts/fetch-sqlite-amalgamation.sh
# and copying artifacts/sqlite/ over. ALPINE_ROOT must have Alpine
# build-base installed. Output (not committed, over 2 MB):
# build/sqlite-thumb/libsqlite3.so and libsqlite3.stripped.so.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
archive_name=sqlite-amalgamation-3540100.zip
archive="$project_dir/artifacts/sqlite/$archive_name"
expected=$(awk '{print $1}' "$project_dir/docs/corpus/sqlite-amalgamation.sha256")
output_dir="$project_dir/build/sqlite-thumb"
work=/tmp/linuxemu-sqlite

if [ ! -x "$alpine_root/usr/bin/gcc" ]; then
    echo "guest gcc is required; install Alpine build-base first" >&2
    exit 2
fi
actual=$(sha256sum "$archive" | awk '{print $1}')
if [ "$actual" != "$expected" ]; then
    echo "checksum mismatch for $archive (expected=$expected actual=$actual)" >&2
    exit 1
fi

# guest <host binary under the rootfs> <argv0> <args>
guest() {
    binary=$1
    shift
    "$linuxemu" --linuxemu-exec "$alpine_root" / "$alpine_root$binary" "$@"
}

rm -rf "$alpine_root$work"
mkdir -p "$alpine_root$work"
trap 'rm -rf "$alpine_root$work"' EXIT
cp "$archive" "$alpine_root$work/$archive_name"
# unzip is a busybox applet (a symlink); run busybox itself.
guest /bin/busybox unzip -q -o -d $work $work/$archive_name
guest /usr/bin/gcc /usr/bin/gcc -O2 -fPIC -shared -march=armv7-a -mthumb -mfpu=vfp \
    -mfloat-abi=hard -funwind-tables -static-libgcc \
    -o $work/libsqlite3.so $work/sqlite-amalgamation-3540100/sqlite3.c
guest /usr/bin/strip /usr/bin/strip -o $work/libsqlite3.stripped.so $work/libsqlite3.so

mkdir -p "$output_dir"
cp "$alpine_root$work/libsqlite3.so" "$output_dir/libsqlite3.so"
cp "$alpine_root$work/libsqlite3.stripped.so" \
    "$output_dir/libsqlite3.stripped.so"
echo "wrote $output_dir/libsqlite3.so and libsqlite3.stripped.so"
