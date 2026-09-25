#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=${LINUXEMU_VERSION:-0.1.3}
binary=${LINUXEMU_BINARY:-"$project_dir/build/linuxemu"}
output_dir=${RELEASE_OUTPUT_DIR:-"$project_dir/build/release"}
name="linuxemu-runtime-$version-bb10-qnx8-arm"
stage="$output_dir/$name.stage"
archive="$output_dir/$name.tar.gz"

[ -x "$binary" ] || { echo "Linuxemu binary is missing: $binary" >&2; exit 2; }
rm -rf "$stage"
mkdir -p "$stage/libexec" "$output_dir"
cp "$binary" "$stage/libexec/linuxemu-core"
chmod 755 "$stage/libexec/linuxemu-core"
printf '%s\n' "$version" >"$stage/VERSION"
cp "$project_dir/docs/compatibility.md" "$stage/COMPATIBILITY.md"
if [ -f "$project_dir/LICENSE" ]; then cp "$project_dir/LICENSE" "$stage/LICENSE"; fi

tar --sort=name --format=ustar --mtime='2026-09-24 00:00:00Z' \
    --owner=0 --group=0 --numeric-owner -czf "$archive" -C "$stage" .
rm -rf "$stage"
sha256sum "$archive" >"$archive.sha256"
cat "$archive.sha256"
