#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=${LINUXEMU_VERSION:-0.1.1}
output_dir=${RELEASE_OUTPUT_DIR:-"$project_dir/build/release"}
release_base=${1:?usage: render-release-installer.sh RELEASE_BASE [OUTPUT]}
output=${2:-"$output_dir/install-linuxemu.sh"}
bundle="$output_dir/linuxemu-runtime-$version-bb10-qnx8-arm.tar.gz"

[ -f "$bundle" ] || { echo "release bundle is missing: $bundle" >&2; exit 2; }
case "$release_base" in *'|'*|*'&'*) echo "unsupported character in release URL" >&2; exit 2;; esac
set -- $(sha256sum "$bundle")
bundle_sha256=$1
mkdir -p "$(dirname "$output")"
sed -e "s|@LINUXEMU_RELEASE_BASE@|$release_base|g" \
    -e "s|@LINUXEMU_BUNDLE_SHA256@|$bundle_sha256|g" \
    "$project_dir/install-linuxemu.sh" >"$output"
chmod 755 "$output"
echo "installer=$output"
echo "bundle_sha256=$bundle_sha256"
