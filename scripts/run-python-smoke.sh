#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
archive_dir=${PYTHON_APK_ARCHIVE:-"$project_dir/artifacts/python-3.14.7-alpine-3.24-armhf"}
manifest="$project_dir/docs/python-runtime-apks.sha256"
linuxemu="$project_dir/build/linuxemu"
apk="$alpine_root/sbin/apk"
staging="$alpine_root/tmp/linuxemu-python-apks"
guest_test=/tmp/python-runtime-smoke.py
world_before="$alpine_root/tmp/linuxemu-python-world.before"
resolver="$alpine_root/etc/resolv.conf"
resolver_before="$alpine_root/tmp/linuxemu-python-resolv.before"
virtual=.linuxemu-python-runtime
resolver_existed=0
installed=0

run_apk()
{
    LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$apk" "$@"
}

cleanup()
{
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$installed" -eq 1 ]; then
        run_apk del "$virtual" || status=1
    fi
    rm -rf "$staging"
    rm -f "$alpine_root$guest_test"
    if [ "$resolver_existed" -eq 1 ]; then
        cp "$resolver_before" "$resolver"
    else
        rm -f "$resolver"
    fi
    rm -f "$resolver_before"
    if ! cmp -s "$world_before" "$alpine_root/etc/apk/world"; then
        echo "Python smoke did not restore the Alpine world file" >&2
        status=1
    fi
    rm -f "$world_before"
    if [ "$status" -eq 0 ]; then
        echo "python_runtime_package_cleanup=ok"
    fi
    exit "$status"
}

if [ ! -x "$linuxemu" ] || [ ! -f "$apk" ]; then
    echo "Linuxemu or Alpine apk is missing" >&2
    exit 2
fi
if [ ! -f "$manifest" ] || [ ! -d "$archive_dir" ]; then
    echo "Pinned Python APK archive or manifest is missing" >&2
    exit 2
fi
if run_apk info -e python3 >/dev/null 2>&1 ||
        run_apk info -e "$virtual" >/dev/null 2>&1; then
    echo "Python smoke requires a clean rootfs without Python installed" >&2
    exit 2
fi

cd "$project_dir"
sha256sum -c "$manifest"
cp "$alpine_root/etc/apk/world" "$world_before"
if [ -f "$resolver" ]; then
    cp "$resolver" "$resolver_before"
    resolver_existed=1
fi
trap cleanup EXIT HUP INT TERM

mkdir -p "$staging"
cp "$archive_dir"/*.apk "$staging"/
cp "$project_dir/guest-tests/python-runtime-smoke.py" \
    "$alpine_root$guest_test"
echo "nameserver ${DNS_SERVER:-192.168.0.1}" > "$resolver"

installed=1
(
    cd "$alpine_root"
    LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$apk" add \
        --no-network --no-chown --allow-untrusted --virtual "$virtual" \
        tmp/linuxemu-python-apks/*.apk
)

run_apk info -e 'python3=3.14.7-r1'
run_apk info -e 'py3-certifi=2026.2.25-r1'
LINUXEMU_ROOT="$alpine_root" "$linuxemu" \
    "$alpine_root/usr/bin/python3" "$guest_test"
