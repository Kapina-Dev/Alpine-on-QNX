#!/bin/sh
# Fetch the pinned libuv release source for the libuv test suite. Run on a
# machine with network access (the device has none), then copy
# artifacts/libuv/ (git-ignored) to the device's dev tree.
#
# Version: the libuv that Alpine 3.24 ships, the same branch the corpus
# closures and the test rootfs come from. In
# https://dl-cdn.alpinelinux.org/alpine/v3.24/main/armhf/APKINDEX.tar.gz
# the libuv package is 1.52.1-r0 (same index: gcc 15.2.0-r5, matching
# docs/corpus/gcc-make-apks.sha256). The sha256 below was computed from the
# download; its sha512 matches the one in Alpine's 3.24-stable
# main/libuv/APKBUILD.
#
# The build also needs the kernel headers (src/unix/udp.c includes
# <linux/errqueue.h>), which the gcc/make/musl-dev closure lacks: the
# linux-headers package from the same index (7.0.0-r1, no dependencies) is
# fetched next to the source with its own pinned sha256.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=1.52.1
archive_name="libuv-v$version.tar.gz"
expected_sha256=66d511b9e6e334c0e62279eb234fbfb2b3110b1479c09b95b44c7afca8cff9e7
artifact_dir="$project_dir/artifacts/libuv"
archive="$artifact_dir/$archive_name"
url="https://dist.libuv.org/dist/v$version/$archive_name"
headers_apk=linux-headers-7.0.0-r1.apk
headers_sha256=aafb48ac40fd2b8066796bf992692436a5ab882f9e36e6c77fbcb7e349ec7f5d
headers_url="https://dl-cdn.alpinelinux.org/alpine/v3.24/main/armhf/$headers_apk"

# check <file> <expected sha256>
check()
{
    actual=$(sha256sum "$1" | awk '{print $1}')
    if [ "$actual" != "$2" ]; then
        echo "checksum mismatch for $1" >&2
        echo "expected=$2" >&2
        echo "actual=$actual" >&2
        exit 1
    fi
}

mkdir -p "$artifact_dir"
if [ ! -f "$archive" ]; then
    curl -fL -o "$archive.part" "$url"
    mv "$archive.part" "$archive"
fi

if [ ! -f "$artifact_dir/$headers_apk" ]; then
    curl -fL -o "$artifact_dir/$headers_apk.part" "$headers_url"
    mv "$artifact_dir/$headers_apk.part" "$artifact_dir/$headers_apk"
fi
check "$archive" "$expected_sha256"
check "$artifact_dir/$headers_apk" "$headers_sha256"

rm -rf "$artifact_dir/libuv-v$version"
tar -xzf "$archive" -C "$artifact_dir"
echo "libuv_source=$artifact_dir/libuv-v$version"
echo "sha256=$expected_sha256"
