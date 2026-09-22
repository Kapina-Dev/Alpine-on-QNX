#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=3.24.2
archive_name="alpine-minirootfs-$version-armhf.tar.gz"
expected_sha256=d86af88b58954d8a90c211004f1e61f208a62d1bb5a2f525f0c1cc26408ab92b
artifact_dir="$project_dir/artifacts/alpine-$version-armhf"
archive="$artifact_dir/$archive_name"
rootfs="$artifact_dir/rootfs"
url="https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/armhf/$archive_name"

mkdir -p "$artifact_dir" "$rootfs"
if [ ! -f "$archive" ]; then
    curl -fL -o "$archive" "$url"
fi

actual_sha256=$(sha256sum "$archive" | awk '{print $1}')
if [ "$actual_sha256" != "$expected_sha256" ]; then
    echo "checksum mismatch for $archive" >&2
    echo "expected=$expected_sha256" >&2
    echo "actual=$actual_sha256" >&2
    exit 1
fi

tar -xzf "$archive" -C "$rootfs"
echo "alpine_root=$rootfs"
echo "sha256=$actual_sha256"
