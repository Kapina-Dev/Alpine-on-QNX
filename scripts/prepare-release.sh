#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=${LINUXEMU_VERSION:-0.1.3}
output_dir=${RELEASE_OUTPUT_DIR:-"$project_dir/build/release"}
summary=${REGRESSION_SUMMARY:-"$project_dir/build/full-regression/summary.txt"}
release_base=${1:?usage: prepare-release.sh RELEASE_BASE}
bundle="linuxemu-runtime-$version-bb10-qnx8-arm.tar.gz"
installer=install-linuxemu.sh
compatibility="linuxemu-compatibility-$version.md"
results="full-regression-summary-$version.txt"
stage="$output_dir.stage.$$"

cleanup()
{
    rm -rf "$stage"
}
trap cleanup EXIT HUP INT TERM

[ -f "$summary" ] || {
    echo "full regression summary is missing: $summary" >&2
    exit 2
}
[ -f "$project_dir/LICENSE" ] || {
    echo "project license is missing" >&2
    exit 2
}

mkdir -p "$stage"
RELEASE_OUTPUT_DIR="$stage" \
    sh "$script_dir/build-release-bundle.sh"
RELEASE_OUTPUT_DIR="$stage" \
    sh "$script_dir/render-release-installer.sh" "$release_base"
cp "$project_dir/docs/compatibility.md" "$stage/$compatibility"
cp "$summary" "$stage/$results"
cp "$project_dir/LICENSE" "$stage/LICENSE.txt"

(
    cd "$stage"
    sha256sum "$installer" "$bundle" "$compatibility" "$results" \
        LICENSE.txt >SHA256SUMS
)

rm -rf "$output_dir"
mv "$stage" "$output_dir"
stage=
trap - EXIT HUP INT TERM
echo "release_dir=$output_dir"
cat "$output_dir/SHA256SUMS"
