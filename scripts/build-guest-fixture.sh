#!/bin/sh
# Build guest-tests/NAME.c with the guest's own gcc into the committed
# fixture guest-tests/fixtures/NAME-armhf.  ALPINE_ROOT must have Alpine
# build-base installed (the default test rootfs does not).
set -eu

[ "$#" -eq 1 ] || { echo "usage: $0 NAME" >&2; exit 2; }
name=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
gcc="$alpine_root/usr/bin/gcc"
strip="$alpine_root/usr/bin/strip"
guest_source=/tmp/linuxemu-fixture-$name.c
guest_binary=/tmp/linuxemu-fixture-$name
fixture="$project_dir/guest-tests/fixtures/$name-armhf"

[ -f "$project_dir/guest-tests/$name.c" ] ||
    { echo "no guest-tests/$name.c" >&2; exit 2; }
if [ ! -x "$gcc" ] || [ ! -x "$strip" ]; then
    echo "guest gcc and strip are required; install Alpine build-base first" >&2
    exit 2
fi
cp "$project_dir/guest-tests/$name.c" "$alpine_root$guest_source"
trap 'rm -f "$alpine_root$guest_source" "$alpine_root$guest_binary"' EXIT

attempt=1
while :; do
    if "$linuxemu" --linuxemu-exec "$alpine_root" / "$gcc" /usr/bin/gcc \
            -O2 -pthread -o "$guest_binary" "$guest_source"; then
        break
    fi
    if [ "$attempt" -ge 8 ]; then exit 1; fi
    attempt=$((attempt + 1))
done
LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$strip" "$guest_binary"
mkdir -p "$(dirname -- "$fixture")"
cp "$alpine_root$guest_binary" "$fixture"
echo "wrote $fixture"
