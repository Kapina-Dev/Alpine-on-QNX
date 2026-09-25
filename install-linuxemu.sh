#!/bin/sh
set -eu

version=0.1.3
default_release_base='@LINUXEMU_RELEASE_BASE@'
default_bundle_sha256='@LINUXEMU_BUNDLE_SHA256@'
alpine_version=3.24.2
alpine_sha256=d86af88b58954d8a90c211004f1e61f208a62d1bb5a2f525f0c1cc26408ab92b
alpine_name="alpine-minirootfs-$alpine_version-armhf.tar.gz"
alpine_url="https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/armhf/$alpine_name"
bundle_name="linuxemu-runtime-$version-bb10-qnx8-arm.tar.gz"
prefix=${LINUXEMU_PREFIX:-/accounts/1000/shared/misc/linuxemu}
profile=${LINUXEMU_PROFILE:-"${HOME:?HOME is not set}/.profile"}
dns_server=${DNS_SERVER:-192.168.0.1}
release_base=${LINUXEMU_RELEASE_BASE:-$default_release_base}
bundle_url=${LINUXEMU_BUNDLE_URL:-"$release_base/$bundle_name"}
bundle_sha256=${LINUXEMU_BUNDLE_SHA256:-$default_bundle_sha256}
bundle_file=${LINUXEMU_BUNDLE_FILE:-}
rootfs_file=${LINUXEMU_ROOTFS_FILE:-}
placeholder_release_base='@LINUXEMU_''RELEASE_BASE@'
placeholder_bundle_sha256='@LINUXEMU_''BUNDLE_SHA256@'
stage=
download_dir=

say()
{
    printf '%s\n' "$*"
}

fail()
{
    printf 'install-linuxemu: %s\n' "$*" >&2
    exit 1
}

usage()
{
    cat <<EOF
usage: sh install-linuxemu.sh [--prefix DIRECTORY]

Environment overrides:
  DNS_SERVER                 guest resolver (default: 192.168.0.1)
  LINUXEMU_RELEASE_BASE      release asset base URL
  LINUXEMU_BUNDLE_URL        complete runtime bundle URL
  LINUXEMU_BUNDLE_SHA256     expected runtime bundle SHA-256
  LINUXEMU_PREFIX            installation directory
  LINUXEMU_PROFILE           QNX login profile to update

LINUXEMU_BUNDLE_FILE and LINUXEMU_ROOTFS_FILE are developer test overrides.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --prefix)
        [ "$#" -ge 2 ] || fail "--prefix requires a directory"
        prefix=$2
        shift 2
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *) fail "unknown argument: $1" ;;
    esac
done

case "$prefix" in
*[[:space:]]*|*"'"*) fail "the installation path cannot contain whitespace or apostrophes" ;;
/*) ;;
*) fail "the installation path must be absolute" ;;
esac

cleanup()
{
    status=$?
    trap - EXIT HUP INT TERM
    [ -z "$stage" ] || rm -rf "$stage"
    [ -z "$download_dir" ] || rm -rf "$download_dir"
    [ -z "${profile_tmp:-}" ] || rm -f "$profile_tmp"
    exit "$status"
}
trap cleanup EXIT HUP INT TERM

[ "$(uname -s)" = QNX ] || fail "this release supports QNX only"
[ -f /accounts/1000/shared/misc/berrycore/env.sh ] ||
    fail "BerryCore by sw7ft is required"
for command in wget sha256sum tar gzip awk grep mkdir mv rm cp chmod ln uname \
        dirname basename; do
    command -v "$command" >/dev/null 2>&1 ||
        fail "required command is missing: $command"
done

if [ "$bundle_sha256" = "$placeholder_bundle_sha256" ]; then
    fail "this development installer has not been rendered for a release"
fi
if [ -z "$bundle_file" ] && [ "$release_base" = "$placeholder_release_base" ]; then
    fail "this development installer has not been rendered for a release"
fi

parent=$(dirname "$prefix")
name=$(basename "$prefix")
stage="$parent/.$name.install.$$"
download_dir="$parent/.$name.download.$$"
previous="$prefix.previous"
profile_tmp="$profile.linuxemu.$$"
mkdir -p "$parent" "$stage" "$download_dir"

verify_sha256()
{
    file=$1
    expected=$2
    set -- $(sha256sum "$file")
    actual=$1
    [ "$actual" = "$expected" ] ||
        fail "checksum mismatch for $(basename "$file"): expected $expected, got $actual"
}

say "Linuxemu $version device installer"
say "Checking the runtime bundle..."
if [ -n "$bundle_file" ]; then
    cp "$bundle_file" "$download_dir/$bundle_name"
else
    wget -O "$download_dir/$bundle_name" "$bundle_url"
fi
verify_sha256 "$download_dir/$bundle_name" "$bundle_sha256"

say "Checking Alpine $alpine_version armhf..."
if [ -n "$rootfs_file" ]; then
    cp "$rootfs_file" "$download_dir/$alpine_name"
else
    wget -O "$download_dir/$alpine_name" "$alpine_url"
fi
verify_sha256 "$download_dir/$alpine_name" "$alpine_sha256"

say "Preparing the installation..."
tar -xzf "$download_dir/$bundle_name" -C "$stage"
mkdir -p "$stage/rootfs"
tar -xzf "$download_dir/$alpine_name" -C "$stage/rootfs"
[ -x "$stage/libexec/linuxemu-core" ] || fail "runtime bundle has no Linuxemu binary"
[ -x "$stage/rootfs/bin/busybox" ] || fail "Alpine rootfs has no BusyBox"

mkdir -p "$stage/rootfs/home/linuxemu" "$stage/rootfs/etc/profile.d" \
    "$stage/bin"
printf 'nameserver %s\n' "$dns_server" >"$stage/rootfs/etc/resolv.conf"
say "Installing Alpine base packages..."
LINUXEMU_ROOT="$stage/rootfs" "$stage/libexec/linuxemu-core" \
    "$stage/rootfs/sbin/apk" add --no-cache --no-chown bash
awk -F: 'BEGIN { OFS=":" } $1 == "root" { $7="/bin/bash" } { print }' \
    "$stage/rootfs/etc/passwd" >"$stage/rootfs/etc/passwd.linuxemu"
mv "$stage/rootfs/etc/passwd.linuxemu" "$stage/rootfs/etc/passwd"
cat >"$stage/rootfs/etc/profile.d/linuxemu.sh" <<EOF
export HOME=/home/linuxemu
export SHELL=/bin/bash
case \$- in
*i*)
    printf '\nLinuxemu $version / Alpine $alpine_version armhf\n'
    printf 'Type "exit" to return to QNX. Install with "apk add --no-chown PACKAGE".\n\n'
    PS1='alpinx:\w\$ '
    ;;
esac
EOF

cat >"$stage/bin/linuxemu" <<EOF
#!/bin/sh
set -eu
prefix='$prefix'
root="\$prefix/rootfs"
core="\$prefix/libexec/linuxemu-core"
export LINUXEMU_ROOT="\$root"
export HOME=/home/linuxemu
export USER=linuxemu
export LOGNAME=linuxemu
export SHELL=/bin/bash
if [ "\$#" -eq 0 ]; then
    exec "\$core" --linuxemu-exec "\$root" /home/linuxemu \
        "\$root/bin/bash" /bin/bash --login
fi
if [ "\$1" = -c ]; then
    shift
    [ "\$#" -gt 0 ] || { echo 'linuxemu: -c requires a command' >&2; exit 2; }
    exec "\$core" --linuxemu-exec "\$root" /home/linuxemu \
        "\$root/bin/bash" /bin/bash -c "\$1"
fi
exec "\$core" --linuxemu-exec "\$root" /home/linuxemu \
    "\$root/bin/bash" /bin/bash -c 'exec "\$@"' linuxemu "\$@"
EOF
chmod 755 "$stage/bin/linuxemu"
(cd "$stage/bin" && ln -s linuxemu alpinx)

cat >"$stage/bin/linuxemu-uninstall" <<EOF
#!/bin/sh
set -eu
prefix='$prefix'
profile='$profile'
tmp="\$profile.linuxemu.\$\$"
if [ -f "\$profile" ]; then
    awk 'BEGIN { skip=0 }
        /^# >>> linuxemu >>>\$/ { skip=1; next }
        /^# <<< linuxemu <<<\$/ { skip=0; next }
        !skip { print }' "\$profile" >"\$tmp"
    mv "\$tmp" "\$profile"
fi
rm -rf "\$prefix"
echo "Linuxemu was removed. Reconnect or reload \$profile."
EOF
chmod 755 "$stage/bin/linuxemu-uninstall"

cat >"$stage/bin/linuxemu-rollback" <<EOF
#!/bin/sh
set -eu
current='$prefix'
previous='$previous'
temporary="\$current.rollback.\$\$"
[ -d "\$previous" ] || { echo 'No previous Linuxemu installation exists.' >&2; exit 1; }
mv "\$current" "\$temporary"
if mv "\$previous" "\$current"; then
    mv "\$temporary" "\$previous"
    echo 'Linuxemu installations were exchanged. Reconnect before use.'
else
    mv "\$temporary" "\$current"
    exit 1
fi
EOF
chmod 755 "$stage/bin/linuxemu-rollback"

say "Running the installation smoke test..."
LINUXEMU_ROOT="$stage/rootfs" "$stage/libexec/linuxemu-core" \
    --linuxemu-exec "$stage/rootfs" /home/linuxemu \
    "$stage/rootfs/bin/bash" /bin/bash -c \
    'test -x /bin/bash && test "$(pwd)" = /home/linuxemu &&
        grep -q "^root:.*:/bin/bash$" /etc/passwd &&
        echo linuxemu_install_smoke=ok'

rm -rf "$previous"
if [ -d "$prefix" ]; then mv "$prefix" "$previous"; fi
if ! mv "$stage" "$prefix"; then
    [ ! -d "$previous" ] || mv "$previous" "$prefix"
    fail "could not activate the installation"
fi
stage=

profile_dir=$(dirname "$profile")
[ -d "$profile_dir" ] || mkdir -p "$profile_dir"
[ -f "$profile" ] || : >"$profile"
awk 'BEGIN { skip=0 }
    /^# >>> linuxemu >>>$/ { skip=1; next }
    /^# <<< linuxemu <<<$/{ skip=0; next }
    !skip { print }' "$profile" >"$profile_tmp"
cat >>"$profile_tmp" <<EOF
# >>> linuxemu >>>
case ":\$PATH:" in
*":$prefix/bin:"*) ;;
*) PATH="$prefix/bin:\$PATH" ;;
esac
export PATH
# <<< linuxemu <<<
EOF
mv "$profile_tmp" "$profile"

trap - EXIT HUP INT TERM
rm -rf "$download_dir"
download_dir=
say "Linuxemu $version installed in $prefix"
say "Reconnect through SSH or run: . $profile"
say "Then enter Alpine with: alpinx"
[ ! -d "$previous" ] || say "The previous installation is available through: linuxemu-rollback"
