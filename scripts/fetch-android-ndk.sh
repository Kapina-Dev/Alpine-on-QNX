#!/bin/sh
# Fetch Android NDK r27c for the Phase 4 part 4 Android gate test programs
# (scripts/build-android-gate.sh). Run on the laptop. Unpacks to
# DIR/android-ndk-r27c (default build/android, git-ignored) and deletes the
# zip. SHA-1 from Google's repository manifest (repository2-3.xml).
#
#   sh scripts/fetch-android-ndk.sh [DIR]
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
dir=${1:-"$project_dir/build/android"}
url=https://dl.google.com/android/repository/android-ndk-r27c-linux.zip
expected=090e8083a715fdb1a3e402d0763c388abb03fb4e
revision=27.2.12479018

if [ -f "$dir/android-ndk-r27c/source.properties" ] &&
    grep -q "^Pkg.Revision = $revision\$" "$dir/android-ndk-r27c/source.properties"; then
    echo "already present: $dir/android-ndk-r27c"
    exit 0
fi
mkdir -p "$dir"
zip="$dir/android-ndk-r27c-linux.zip"
trap 'rm -f "$zip"' EXIT
curl -fsSL -o "$zip" "$url"
actual=$(sha1sum "$zip" | awk '{print $1}')
if [ "$actual" != "$expected" ]; then
    echo "checksum mismatch: expected=$expected actual=$actual" >&2
    exit 1
fi
unzip -q "$zip" -d "$dir"
echo "unpacked $dir/android-ndk-r27c"
