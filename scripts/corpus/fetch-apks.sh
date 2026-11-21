#!/bin/sh
# Pin an app's offline package closure for the corpus. Run on the device:
#   sh scripts/corpus/fetch-apks.sh <app> ...
# Fetches corpus_packages and every dependency (apk fetch --recursive) from
# the Alpine 3.24 repositories into artifacts/corpus/<app>/ (git-ignored)
# and writes docs/corpus/<app>-apks.sha256 (committed). Override the
# resolver with DNS_SERVER.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
resolver="$alpine_root/etc/resolv.conf"
resolver_before="$project_dir/build/corpus-fetch-resolv.before"
fetch_dir=tmp/linuxemu-corpus-fetch
had_resolver=0

restore()
{
    rm -rf "$alpine_root/$fetch_dir"
    if [ "$had_resolver" -eq 1 ]; then
        cp "$resolver_before" "$resolver"
    else
        rm -f "$resolver"
    fi
    rm -f "$resolver_before"
}

[ "$#" -gt 0 ] || { echo "usage: $0 <app> ..." >&2; exit 2; }
mkdir -p "$project_dir/build" "$project_dir/docs/corpus"
if [ -f "$resolver" ]; then
    cp "$resolver" "$resolver_before"
    had_resolver=1
fi
trap restore EXIT HUP INT TERM
echo "nameserver ${DNS_SERVER:-192.168.0.1}" > "$resolver"

for app in "$@"; do
    packages=$(corpus_packages=; . "${CORPUS_APPS_DIR:-$script_dir/apps}/$app.sh"; \
        echo "$corpus_packages")
    archive="$project_dir/artifacts/corpus/$app"
    echo "=== fetch: $app ($packages) ==="
    rm -rf "$alpine_root/$fetch_dir" "$archive"
    mkdir -p "$alpine_root/$fetch_dir" "$archive"
    ( cd "$alpine_root" && LINUXEMU_ROOT="$alpine_root" "$linuxemu" \
        "$alpine_root/sbin/apk" fetch --recursive --output "$fetch_dir" \
        $packages )
    mv "$alpine_root/$fetch_dir"/*.apk "$archive"/
    ( cd "$project_dir" && sha256sum "artifacts/corpus/$app"/*.apk ) \
        > "$project_dir/docs/corpus/$app-apks.sha256"
    echo "$app: $(wc -l < "$project_dir/docs/corpus/$app-apks.sha256" |
        tr -d ' ') packages pinned"
done
