#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
busybox="$alpine_root/bin/busybox"
apk="$alpine_root/sbin/apk"
dns_server=${DNS_SERVER:-192.168.0.1}
valid_tls_url=${VALID_TLS_URL:-https://dl-cdn.alpinelinux.org/alpine/v3.24/main/armhf/APKINDEX.tar.gz}
timeout_command=${TIMEOUT_COMMAND:-timeout}
output_dir="$project_dir/build/package-smoke"
resolver="$alpine_root/etc/resolv.conf"
world="$alpine_root/etc/apk/world"
guest_index=/tmp/linuxemu-phase6-index.gz
guest_invalid=/tmp/linuxemu-phase6-invalid-cert
guest_empty_ca=/tmp/linuxemu-phase6-empty-ca.crt
package=nano
failures=0
had_resolver=0
package_added=0

mkdir -p "$output_dir"
cp "$world" "$output_dir/world.before"
if [ -f "$resolver" ]; then
    cp "$resolver" "$output_dir/resolv.before"
    had_resolver=1
fi

cleanup()
{
    if [ "$package_added" -eq 1 ]; then
        LINUXEMU_ROOT="$alpine_root" "$timeout_command" -s 9 120 \
            "$linuxemu" "$apk" del "$package" \
            >"$output_dir/cleanup-package.out" 2>&1 || true
    fi
    rm -f "$alpine_root$guest_index" "$alpine_root$guest_invalid" \
        "$alpine_root$guest_empty_ca"
    if [ "$had_resolver" -eq 1 ]; then
        cp "$output_dir/resolv.before" "$resolver"
    else
        rm -f "$resolver"
    fi
}
trap cleanup EXIT HUP INT TERM

printf 'nameserver %s\n' "$dns_server" >"$resolver"

echo "=== package guest: https-valid ==="
LINUXEMU_ROOT="$alpine_root" "$timeout_command" -s 9 45 \
    "$linuxemu" "$busybox" wget -T 25 -qO "$guest_index" \
    "$valid_tls_url" >"$output_dir/https-valid.out" 2>&1
status=$?
cat "$output_dir/https-valid.out"
echo "https-valid exit=$status expected=0"
if [ "$status" -ne 0 ] || ! gzip -t "$alpine_root$guest_index"; then
    failures=$((failures + 1))
fi

echo "=== package guest: https-invalid-certificate ==="
printf '' >"$alpine_root$guest_empty_ca"
SSL_CERT_FILE="$guest_empty_ca" LINUXEMU_ROOT="$alpine_root" \
    "$timeout_command" -s 9 30 \
    "$linuxemu" "$busybox" wget -T 15 -O "$guest_invalid" \
    "$valid_tls_url" >"$output_dir/https-invalid.out" 2>&1
status=$?
cat "$output_dir/https-invalid.out"
echo "https-invalid-certificate exit=$status expected=1"
if [ "$status" -ne 1 ] ||
    ! grep -q 'certificate verify failed' "$output_dir/https-invalid.out"; then
    failures=$((failures + 1))
fi

echo "=== package guest: apk-update ==="
LINUXEMU_ROOT="$alpine_root" "$timeout_command" -s 9 90 \
    "$linuxemu" "$apk" update >"$output_dir/apk-update.out" 2>&1
status=$?
cat "$output_dir/apk-update.out"
echo "apk-update exit=$status expected=0"
if [ "$status" -ne 0 ] ||
    ! grep -q 'distinct packages available' "$output_dir/apk-update.out"; then
    failures=$((failures + 1))
fi

LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$apk" info -e "$package" \
    >/dev/null 2>&1
if [ "$?" -eq 0 ]; then
    echo "package fixture already installed: $package" >&2
    exit 2
fi

echo "=== package guest: apk-add ==="
LINUXEMU_ROOT="$alpine_root" "$timeout_command" -s 9 120 \
    "$linuxemu" "$apk" add --no-cache --no-chown "$package" \
    >"$output_dir/apk-add.out" 2>&1
status=$?
cat "$output_dir/apk-add.out"
echo "apk-add exit=$status expected=0"
LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$apk" info -e "$package" \
    >/dev/null 2>&1
if [ "$?" -eq 0 ]; then package_added=1; fi
if [ "$status" -ne 0 ] ||
    ! grep -q 'Executing busybox-.*[.]trigger' "$output_dir/apk-add.out"; then
    failures=$((failures + 1))
fi

echo "=== package guest: installed-program ==="
LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$alpine_root/usr/bin/nano" \
    --version >"$output_dir/nano-version.out" 2>&1
status=$?
cat "$output_dir/nano-version.out"
echo "installed-program exit=$status expected=0"
if [ "$status" -ne 0 ] ||
    ! grep -q 'GNU nano' "$output_dir/nano-version.out"; then
    failures=$((failures + 1))
fi

echo "=== package guest: apk-del ==="
LINUXEMU_ROOT="$alpine_root" "$timeout_command" -s 9 120 \
    "$linuxemu" "$apk" del "$package" >"$output_dir/apk-del.out" 2>&1
status=$?
cat "$output_dir/apk-del.out"
echo "apk-del exit=$status expected=0"
if [ "$status" -eq 0 ]; then package_added=0; fi
if [ "$status" -ne 0 ] ||
    ! grep -q 'Executing busybox-.*[.]trigger' "$output_dir/apk-del.out"; then
    failures=$((failures + 1))
fi

if ! cmp "$world" "$output_dir/world.before"; then
    echo "apk world file was not restored" >&2
    failures=$((failures + 1))
fi

echo "linuxemu_package_smoke_failures=$failures"
test "$failures" -eq 0
