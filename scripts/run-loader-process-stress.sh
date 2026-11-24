#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
fixture_dir="$project_dir/guest-tests/fixtures"
timeout_command=${TIMEOUT_COMMAND:-timeout}
spawn_count=${SPAWN_COUNT:-100}
failures=0
module=/tmp/libphase9.so
thumb_module=/tmp/libthumb.so
stripped_dir=/tmp/linuxemu-stripped-fixtures

for fixture in phase9-module-armhf.so dlopen-stress-armhf \
    spawn-thread-stress-armhf thumb-module-armhf.so thumb-dlopen-armhf \
    stripped-mixed-armhf.so stripped-mixed-armhf.stripped.so \
    stripped-unknown-armhf.so stripped-compare-armhf; do
    if [ ! -f "$fixture_dir/$fixture" ]; then
        echo "missing fixture: $fixture_dir/$fixture" >&2
        exit 2
    fi
done

cp "$fixture_dir/phase9-module-armhf.so" "$alpine_root$module" || exit 2
cp "$fixture_dir/thumb-module-armhf.so" "$alpine_root$thumb_module" || exit 2
mkdir -p "$alpine_root$stripped_dir" || exit 2
for fixture in stripped-mixed-armhf.so stripped-mixed-armhf.stripped.so \
    stripped-unknown-armhf.so; do
    cp "$fixture_dir/$fixture" "$alpine_root$stripped_dir/$fixture" || exit 2
done
trap 'rm -f "$alpine_root$module" "$alpine_root$thumb_module"; \
    rm -rf "$alpine_root$stripped_dir"' EXIT

echo "=== dlopen executable mapping stress ==="
"$timeout_command" 30 "$linuxemu" --linuxemu-exec "$alpine_root" / \
    "$fixture_dir/dlopen-stress-armhf" /tmp/dlopen-stress "$module"
status=$?
echo "dlopen-stress exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

echo "=== Thumb library through dlopen ==="
"$timeout_command" 30 "$linuxemu" --linuxemu-exec "$alpine_root" / \
    "$fixture_dir/thumb-dlopen-armhf" /tmp/thumb-dlopen "$thumb_module"
status=$?
echo "thumb-dlopen exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

echo "=== stripped mixed ARM/Thumb library: patched as the unstripped one ==="
"$timeout_command" 30 "$linuxemu" --linuxemu-exec "$alpine_root" / \
    "$fixture_dir/stripped-compare-armhf" /tmp/stripped-compare \
    "$stripped_dir/stripped-mixed-armhf.so" \
    "$stripped_dir/stripped-mixed-armhf.stripped.so" mixed
status=$?
echo "stripped-compare exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

echo "=== stripped library: patch sites reported per mapping ==="
# LINUXEMU_VERBOSE prints "linuxemu: patch-sites ino=... sites=N" per
# patched file mapping. The stripped copy's total must equal the laptop's
# count for the same file: build/code-map-survey guest-tests/fixtures
# (sites= on stripped-mixed-armhf.stripped.so; rerun it when the fixture
# is rebuilt).
expected_stripped_sites=10
sites_log=$(mktemp "${TMPDIR:-/tmp}/linuxemu-sites.XXXXXX") || exit 2
LINUXEMU_VERBOSE=1 "$timeout_command" 30 "$linuxemu" \
    --linuxemu-exec "$alpine_root" / \
    "$fixture_dir/stripped-compare-armhf" /tmp/stripped-compare \
    "$stripped_dir/stripped-mixed-armhf.so" \
    "$stripped_dir/stripped-mixed-armhf.stripped.so" mixed \
    >/dev/null 2>"$sites_log"
stripped_ino=$(ls -i "$alpine_root$stripped_dir/stripped-mixed-armhf.stripped.so" |
    awk '{print $1}')
stripped_sites=$(awk -v ino="ino=$stripped_ino" '
    $1 == "linuxemu:" && $2 == "patch-sites" && $3 == ino {
        sub("sites=", "", $6); total += $6; seen = 1 }
    END { print seen ? total : "none" }' "$sites_log")
echo "stripped-sites reported=$stripped_sites expected=$expected_stripped_sites"
if [ "$stripped_sites" != "$expected_stripped_sites" ]; then
    failures=$((failures + 1))
fi
rm -f "$sites_log"

echo "=== stripped library with an undecodable svc: refused ==="
refusal_report=$(mktemp "${TMPDIR:-/tmp}/linuxemu-refusal.XXXXXX") || exit 2
LINUXEMU_DIAGNOSTICS="$refusal_report" "$timeout_command" 30 "$linuxemu" \
    --linuxemu-exec "$alpine_root" / \
    "$fixture_dir/stripped-compare-armhf" /tmp/stripped-compare \
    --refused "$stripped_dir/stripped-unknown-armhf.so"
status=$?
echo "stripped-refused exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi
if grep -q "detail=undecodable-code" "$refusal_report"; then
    echo "stripped-refused diagnostic=present expected=present"
else
    echo "stripped-refused diagnostic=missing expected=present"
    failures=$((failures + 1))
fi
rm -f "$refusal_report"

# SQLite built Thumb with unwind tables, when it has been built
# (scripts/build-sqlite-thumb-fixture.sh): the same comparison, then SQL
# through the stripped copy.
sqlite_dir="$project_dir/build/sqlite-thumb"
if [ -f "$sqlite_dir/libsqlite3.so" ] &&
    [ -f "$sqlite_dir/libsqlite3.stripped.so" ]; then
    echo "=== stripped SQLite: patched as the unstripped one ==="
    cp "$sqlite_dir/libsqlite3.so" "$sqlite_dir/libsqlite3.stripped.so" \
        "$alpine_root$stripped_dir/" || exit 2
    "$timeout_command" 120 "$linuxemu" --linuxemu-exec "$alpine_root" / \
        "$fixture_dir/stripped-compare-armhf" /tmp/stripped-compare \
        "$stripped_dir/libsqlite3.so" "$stripped_dir/libsqlite3.stripped.so" \
        sqlite
    status=$?
    echo "stripped-sqlite exit=$status expected=0"
    if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi
else
    echo "=== stripped SQLite: not built, skipped ==="
fi

echo "=== multithreaded posix_spawn stress: $spawn_count ==="
"$timeout_command" 120 "$linuxemu" --linuxemu-exec "$alpine_root" / \
    "$fixture_dir/spawn-thread-stress-armhf" /tmp/spawn-thread-stress \
    "$spawn_count"
status=$?
echo "spawn-thread-stress exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

echo "linuxemu_loader_process_stress_failures=$failures"
test "$failures" -eq 0
