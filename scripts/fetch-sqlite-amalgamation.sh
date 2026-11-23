#!/bin/sh
# Fetch the pinned SQLite amalgamation for the Phase 4 part 3 stripped
# decoding check (scripts/build-sqlite-thumb-fixture.sh). Run on a machine
# with network access, then copy artifacts/sqlite/ (git-ignored) to the
# device's dev tree.
#
# Version 3.54.1. sqlite.org's download page lists the zip's SHA3-256
# (92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30),
# which the download matched; its SHA-256 is pinned in
# docs/corpus/sqlite-amalgamation.sha256.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
archive_name=sqlite-amalgamation-3540100.zip
url="https://sqlite.org/2026/$archive_name"
artifact_dir="$project_dir/artifacts/sqlite"
expected=$(awk '{print $1}' "$project_dir/docs/corpus/sqlite-amalgamation.sha256")

mkdir -p "$artifact_dir"
if [ ! -f "$artifact_dir/$archive_name" ]; then
    curl -fsSL -o "$artifact_dir/$archive_name.part" "$url"
    mv "$artifact_dir/$archive_name.part" "$artifact_dir/$archive_name"
fi
actual=$(sha256sum "$artifact_dir/$archive_name" | awk '{print $1}')
if [ "$actual" != "$expected" ]; then
    echo "checksum mismatch for $archive_name" >&2
    echo "expected=$expected" >&2
    echo "actual=$actual" >&2
    exit 1
fi
echo "verified $artifact_dir/$archive_name"
