#!/bin/sh
# Build libuv's test runner, build/libuv/uv_run_tests, with the guest gcc.
# Needs the source from scripts/fetch-libuv-source.sh in artifacts/libuv/
# and the pinned gcc/make/musl-dev closure (docs/corpus/gcc-make-apks.sha256,
# artifacts/corpus/gcc-make/). Like the corpus, the tools are installed in a
# fresh copy of the test rootfs (the closure upgrades musl), so the shared
# test rootfs is never changed; the copy is deleted afterwards. The kernel
# headers libuv needs come from the pinned linux-headers package that
# scripts/fetch-libuv-source.sh puts in artifacts/libuv/.
#
# The sources are the unix and Linux lists of libuv's CMakeLists.txt, and
# the tests are its uv_run_tests list (test/*.c without the benchmarks and
# the Windows runner), linked statically into one binary. A rebuild is
# skipped while build/libuv/uv_run_tests is newer than the source archive.
#
# Environment: ALPINE_ROOT (default test-rootfs), LIBUV_BUILD_JOBS
# (make -j, default 4), LIBUV_BUILD_TIMEOUT (seconds, default 14400).
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=1.52.1
source_dir="$project_dir/artifacts/libuv/libuv-v$version"
archive="$project_dir/artifacts/libuv/libuv-v$version.tar.gz"
headers_apk="$project_dir/artifacts/libuv/linux-headers-7.0.0-r1.apk"
headers_sha256=aafb48ac40fd2b8066796bf992692436a5ab882f9e36e6c77fbcb7e349ec7f5d
out_dir="$project_dir/build/libuv"
binary="$out_dir/uv_run_tests"
jobs=${LIBUV_BUILD_JOBS:-4}
build_timeout=${LIBUV_BUILD_TIMEOUT:-14400}
guest_build=/tmp/linuxemu-libuv-build

if [ ! -f "$source_dir/include/uv.h" ] || [ ! -f "$archive" ] ||
        [ ! -f "$headers_apk" ]; then
    echo "libuv source missing: run scripts/fetch-libuv-source.sh and copy artifacts/libuv/" >&2
    exit 2
fi
if [ -f "$binary" ] && [ -d "$out_dir/fixtures" ] &&
        [ "$binary" -nt "$archive" ]; then
    echo "libuv_build=up-to-date $binary"
    exit 0
fi

. "$script_dir/corpus/lib.sh"
corpus_app=gcc-make
corpus_packages="gcc make musl-dev"
corpus_virtual=.linuxemu-libuv-build
headers_virtual=.linuxemu-libuv-headers
corpus_app_out="$project_dir/build/libuv-build"
alpine_root="$corpus_app_out/rootfs"
log="$corpus_app_out/build.log"

rm -rf "$corpus_app_out"
mkdir -p "$alpine_root"
( cd "$corpus_base_root" && tar cf - . ) | ( cd "$alpine_root" && tar xf - ) ||
    { echo "copying the base rootfs failed" >&2; exit 1; }
corpus_world=$(corpus_world_path)
cp "$corpus_world" "$corpus_app_out/world.before"

cleanup()
{
    trap - EXIT HUP INT TERM
    corpus_kill_guests
    corpus_apk del "$headers_virtual" >>"$log" 2>&1
    corpus_remove_virtual >>"$log" 2>&1
    if ! cmp -s "$corpus_world" "$corpus_app_out/world.before"; then
        echo "libuv_world_restore=failed"
    fi
    rm -rf "$alpine_root"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

echo "=== libuv build: install gcc make musl-dev linux-headers ==="
corpus_default_install >"$log" 2>&1 || {
    cat "$log"
    echo "libuv_build=failed (install)"
    exit 1
}
if [ "$(sha256sum "$headers_apk" | awk '{print $1}')" != "$headers_sha256" ]; then
    echo "libuv_build=failed (linux-headers checksum)"
    exit 1
fi
mkdir -p "$alpine_root/tmp/linuxemu-libuv-apks"
cp "$headers_apk" "$alpine_root/tmp/linuxemu-libuv-apks/"
( cd "$alpine_root" && corpus_apk add --no-network --no-chown \
    --allow-untrusted --virtual "$headers_virtual" \
    tmp/linuxemu-libuv-apks/linux-headers-7.0.0-r1.apk ) >>"$log" 2>&1 || {
    cat "$log"
    echo "libuv_build=failed (install linux-headers)"
    exit 1
}
rm -rf "$alpine_root/tmp/linuxemu-libuv-apks"

echo "=== libuv build: compile libuv $version ==="
work="$alpine_root$guest_build"
mkdir -p "$work"
( cd "$source_dir" && tar cf - include src test ) | ( cd "$work" && tar xf - ) ||
    { echo "copying the libuv source failed"; exit 1; }

lib_objects=
for file in fs-poll.c idna.c inet.c random.c strscpy.c strtok.c \
        thread-common.c threadpool.c timer.c uv-common.c \
        uv-data-getter-setters.c version.c; do
    lib_objects="$lib_objects src/${file%.c}.o"
done
for file in async.c core.c dl.c fs.c getaddrinfo.c getnameinfo.c \
        loop-watcher.c loop.c pipe.c poll.c process.c random-devurandom.c \
        signal.c stream.c tcp.c thread.c tty.c udp.c \
        linux.c procfs-exepath.c random-getrandom.c random-sysctl-linux.c \
        proctitle.c; do
    lib_objects="$lib_objects src/unix/${file%.c}.o"
done
test_objects=
for path in "$work"/test/*.c; do
    file=${path##*/}
    case $file in
        benchmark-*|run-benchmarks.c|runner-win.c) continue ;;
    esac
    test_objects="$test_objects test/${file%.c}.o"
done

# The defines are CMakeLists.txt's unix and Linux uv_defines. Like
# upstream's uv_run_tests_a, the tests link against the static library:
# some tests include a libuv source file, so linking the library objects
# directly would define those functions twice.
{
    echo 'CC = gcc'
    echo 'CFLAGS = -O1 -pthread -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_POSIX_C_SOURCE=200112 -Iinclude -Isrc'
    echo "LIB_OBJECTS =$lib_objects"
    echo "TEST_OBJECTS =$test_objects"
    echo 'uv_run_tests: $(TEST_OBJECTS) libuv.a'
    printf '\t$(CC) -static -pthread -o $@ $(TEST_OBJECTS) libuv.a -ldl -lrt\n'
    echo 'libuv.a: $(LIB_OBJECTS)'
    printf '\tar rcs $@ $(LIB_OBJECTS)\n'
    echo '%.o: %.c'
    printf '\t$(CC) $(CFLAGS) -c -o $@ $<\n'
} >"$work/Makefile"

# One retry, which carries on from what was built: under make -j a guest
# start occasionally fails with "linuxemu load: File exists". A real
# compile error fails the retry too.
started=$(corpus_now)
guest_sh "$build_timeout" \
    "cd $guest_build && make -j$jobs uv_run_tests" >>"$log" 2>&1
status=$?
if [ "$status" -ne 0 ]; then
    echo "libuv_build_retry=1 (make exit $status)"
    guest_sh "$build_timeout" \
        "cd $guest_build && make -j$jobs uv_run_tests" >>"$log" 2>&1
    status=$?
fi
seconds=$(( $(corpus_now) - started ))
if [ "$status" -ne 0 ] || [ ! -f "$work/uv_run_tests" ]; then
    tail -40 "$log"
    echo "libuv_build=failed (make exit $status after ${seconds}s; log $log)"
    exit 1
fi

mkdir -p "$out_dir"
rm -rf "$out_dir/fixtures"
cp "$work/uv_run_tests" "$binary.new" && mv "$binary.new" "$binary" &&
    cp -R "$source_dir/test/fixtures" "$out_dir/fixtures" ||
    { echo "libuv_build=failed (copying the result)"; exit 1; }
echo "libuv_build_seconds=$seconds"
echo "libuv_build=ok $binary"
