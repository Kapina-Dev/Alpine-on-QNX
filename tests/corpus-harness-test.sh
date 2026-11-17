#!/bin/sh
# Host tests for the application-corpus harness (scripts/corpus/lib.sh and
# scripts/run-corpus.sh). Fake mode (CORPUS_FAKE=1) needs neither Linuxemu
# nor the Alpine rootfs.
#   sh tests/corpus-harness-test.sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
# The Passport has no /tmp: keep the scratch area under build/.
mkdir -p "$project_dir/build"
work=$(mktemp -d "$project_dir/build/corpus-test.XXXXXX") || exit 2
[ -n "${CORPUS_TEST_KEEP:-}" ] || trap 'rm -rf "$work"' EXIT
failures=0

# Seconds since the epoch: QNX's date has no %s, but -t prints it.
now()
{
    seconds=$(date +%s 2>/dev/null)
    case $seconds in
        ''|*[!0-9]*) date -t ;;
        *) echo "$seconds" ;;
    esac
}

check()
{
    if [ "$1" -eq 0 ]; then
        echo "ok: $2"
    else
        echo "FAIL: $2"
        failures=$((failures + 1))
    fi
}

# run_corpus <case> <app...>: fresh output, fake world and installed set.
run_corpus()
{
    case_name=$1
    shift
    out="$work/$case_name/out"
    rm -rf "$work/$case_name/out" "$work/$case_name/installed"
    mkdir -p "$out" "$work/$case_name/installed"
    printf 'alpine-baselayout\nbusybox\n' > "$work/$case_name/world"
    CORPUS_FAKE=1 CORPUS_APPS_DIR="$work/$case_name/apps" \
        CORPUS_OUT="$out" CORPUS_FAKE_WORLD="$work/$case_name/world" \
        CORPUS_FAKE_INSTALLED="$work/$case_name/installed" \
        CORPUS_STAGE_TIMEOUT="${CORPUS_STAGE_TIMEOUT:-20}" \
        sh "$project_dir/scripts/run-corpus.sh" ${1+"$@"} \
        > "$work/$case_name/runner.log" 2>&1
    runner_status=$?
}

# result <case> <app> <stage>: "<result>\t<note>" for that stage.
result()
{
    awk -F '\t' -v a="$2" -v s="$3" '$1 == a && $2 == s { print $3 "\t" $5 }' \
        "$work/$1/out/results.tsv" 2>/dev/null
}

new_app()
{
    mkdir -p "$work/$1/apps"
    cat > "$work/$1/apps/$2.sh"
}

tab=$(printf '\t')

# all-pass
new_app all-pass demo <<'EOF'
corpus_packages=demo
stage_install() { return 0; }
stage_startup() { return 0; }
stage_basic() { return 0; }
stage_concurrent() { return 0; }
stage_shutdown() { return 0; }
EOF
run_corpus all-pass demo
stages=$(awk -F '\t' '$1 == "demo" { printf "%s:%s ", $2, $3 }' \
    "$work/all-pass/out/results.tsv" 2>/dev/null)
[ "$stages" = "install:pass startup:pass basic:pass concurrent:pass shutdown:pass " ]
check $? "all-pass: five pass lines in stage order"
grep -q '^demo install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=0$' \
    "$work/all-pass/out/summary.txt" 2>/dev/null
check $? "all-pass: summary line compatible=yes"
grep -q '^corpus apps=1 compatible=1$' "$work/all-pass/out/summary.txt" \
    2>/dev/null
check $? "all-pass: totals line"
[ "$runner_status" -eq 0 ]
check $? "all-pass: runner exit 0"

# fail-skips
new_app fail-skips demo <<'EOF'
corpus_packages=demo
stage_install() { return 0; }
stage_startup() { return 0; }
stage_basic() { echo "wrong answer"; return 1; }
stage_concurrent() { return 0; }
stage_shutdown() { return 0; }
EOF
run_corpus fail-skips demo
[ "$(result fail-skips demo basic)" = "fail${tab}wrong answer" ]
check $? "fail-skips: basic fail with its first output line"
[ "$(result fail-skips demo concurrent)" = "skip${tab}after basic failed" ] &&
    [ "$(result fail-skips demo shutdown)" = "skip${tab}after basic failed" ]
check $? "fail-skips: later stages skipped"
grep -q '^demo .* compatible=no ' "$work/fail-skips/out/summary.txt" \
    2>/dev/null
check $? "fail-skips: compatible=no"
[ "$runner_status" -eq 0 ]
check $? "fail-skips: an app failure is a result, runner exit 0"

# fail-note: first line only
new_app fail-note demo <<'EOF'
corpus_packages=demo
stage_install() { printf 'boom\nsecond\n'; return 1; }
EOF
run_corpus fail-note demo
[ "$(result fail-note demo install)" = "fail${tab}boom" ]
check $? "fail-note: note is the first output line"

# fail-cleanup
new_app fail-cleanup demo <<EOF
corpus_packages=demo
stage_install() { return 0; }
stage_startup() { return 0; }
stage_basic() { return 1; }
app_cleanup() { touch "$work/fail-cleanup/cleaned"; }
EOF
run_corpus fail-cleanup demo
[ -f "$work/fail-cleanup/cleaned" ]
check $? "fail-cleanup: app_cleanup ran after a failure"
! grep -q '^world_restore ' "$work/fail-cleanup/out/summary.txt" 2>/dev/null
check $? "fail-cleanup: world restored"

# hang-cleanup
new_app hang-cleanup demo <<EOF
corpus_packages=demo
stage_install() { return 0; }
stage_startup() { sleep 30; return 0; }
app_cleanup() { touch "$work/hang-cleanup/cleaned"; }
EOF
started=$(now)
# A subshell keeps the override local: ksh keeps an assignment made in front
# of a function call.
( CORPUS_STAGE_TIMEOUT=2; export CORPUS_STAGE_TIMEOUT
  run_corpus hang-cleanup demo )
elapsed=$(( $(now) - started ))
[ "$(result hang-cleanup demo startup)" = "fail${tab}timeout after 2s" ]
check $? "hang-cleanup: startup fails with timeout note"
[ -f "$work/hang-cleanup/cleaned" ] && [ "$elapsed" -lt 20 ]
check $? "hang-cleanup: cleanup ran and the hang was cut short"

# dirty-rootfs: the virtual package of an earlier run is still installed
new_app dirty-rootfs demo <<'EOF'
corpus_packages=demo
stage_install() { return 0; }
stage_startup() { return 0; }
EOF
mkdir -p "$work/dirty-rootfs/apps"
(
    out="$work/dirty-rootfs/out"
    rm -rf "$out" "$work/dirty-rootfs/installed"
    mkdir -p "$out" "$work/dirty-rootfs/installed"
    touch "$work/dirty-rootfs/installed/.linuxemu-corpus-demo"
    printf 'busybox\n' > "$work/dirty-rootfs/world"
    CORPUS_FAKE=1 CORPUS_APPS_DIR="$work/dirty-rootfs/apps" \
        CORPUS_OUT="$out" CORPUS_FAKE_WORLD="$work/dirty-rootfs/world" \
        CORPUS_FAKE_INSTALLED="$work/dirty-rootfs/installed" \
        sh "$project_dir/scripts/run-corpus.sh" demo \
        > "$work/dirty-rootfs/runner.log" 2>&1
)
[ "$(result dirty-rootfs demo install)" = "fail${tab}rootfs not clean" ] &&
    [ "$(result dirty-rootfs demo startup)" = "skip${tab}after install failed" ]
check $? "dirty-rootfs: refused, later stages skipped"
[ -f "$work/dirty-rootfs/installed/.linuxemu-corpus-demo" ]
check $? "dirty-rootfs: the earlier run's package is left alone"

# undefined-stage
new_app undefined-stage demo <<'EOF'
corpus_packages=demo
stage_install() { return 0; }
stage_startup() { return 0; }
stage_basic() { return 0; }
stage_shutdown() { return 0; }
EOF
run_corpus undefined-stage demo
[ "$(result undefined-stage demo concurrent)" = "fail${tab}stage not defined" ] &&
    [ "$(result undefined-stage demo shutdown)" = "skip${tab}after concurrent failed" ]
check $? "undefined-stage: fail with note, then skip"

# continues-after-crash
new_app continues-after-crash alpha <<'EOF'
corpus_packages=alpha
stage_install() { exit 3; }
EOF
new_app continues-after-crash beta <<'EOF'
corpus_packages=beta
stage_install() { return 0; }
stage_startup() { return 0; }
stage_basic() { return 0; }
stage_concurrent() { return 0; }
stage_shutdown() { return 0; }
EOF
run_corpus continues-after-crash
[ "$(result continues-after-crash alpha install | cut -f1)" = fail ] &&
    grep -q '^beta .* compatible=yes ' \
        "$work/continues-after-crash/out/summary.txt" 2>/dev/null
check $? "continues-after-crash: next app still scored"
grep -q '^corpus apps=2 compatible=1$' \
    "$work/continues-after-crash/out/summary.txt" 2>/dev/null
check $? "continues-after-crash: totals count both apps"

# install-alone
new_app install-alone demo <<'EOF'
corpus_packages=demo
stage_install() { return 0; }
stage_startup() { return 1; }
EOF
run_corpus install-alone demo
grep -q '^demo install=pass startup=fail .* compatible=no ' \
    "$work/install-alone/out/summary.txt" 2>/dev/null
check $? "install-alone: installation alone is not compatibility"

# Default install: the pinned closure is checked against its manifest
# before apk runs. CORPUS_MANIFEST_DIR / CORPUS_ARCHIVE_DIR point at a
# temporary closure here.
pinned_case()
{
    case_name=$1
    mkdir -p "$work/$case_name/archive/demo" "$work/$case_name/manifests"
    printf 'pinned package bytes\n' > "$work/$case_name/archive/demo/demo-1.0-r0.apk"
    ( cd "$work/$case_name" &&
        sha256sum archive/demo/demo-1.0-r0.apk ) \
        > "$work/$case_name/manifests/demo-apks.sha256"
    new_app "$case_name" demo <<'APP'
corpus_packages=demo
stage_startup() { return 0; }
APP
}

pinned_case pinned-install
( CORPUS_MANIFEST_DIR="$work/pinned-install/manifests"
  CORPUS_ARCHIVE_DIR="$work/pinned-install/archive"
  CORPUS_PROJECT_RELATIVE="$work/pinned-install"
  export CORPUS_MANIFEST_DIR CORPUS_ARCHIVE_DIR CORPUS_PROJECT_RELATIVE
  run_corpus pinned-install demo )
[ "$(result pinned-install demo install | cut -f1)" = pass ]
check $? "pinned-install: verified closure installs"

pinned_case tampered-archive
printf 'tampered\n' >> "$work/tampered-archive/archive/demo/demo-1.0-r0.apk"
( CORPUS_MANIFEST_DIR="$work/tampered-archive/manifests"
  CORPUS_ARCHIVE_DIR="$work/tampered-archive/archive"
  CORPUS_PROJECT_RELATIVE="$work/tampered-archive"
  export CORPUS_MANIFEST_DIR CORPUS_ARCHIVE_DIR CORPUS_PROJECT_RELATIVE
  run_corpus tampered-archive demo )
[ "$(result tampered-archive demo install)" = "fail${tab}pinned archive check failed" ]
check $? "tampered-archive: install fails before apk runs"
[ ! -e "$work/tampered-archive/installed/.linuxemu-corpus-demo" ]
check $? "tampered-archive: apk was never called"

# corpus_timeout: the harness's own watchdog. GNU timeout 8.29 from
# BerryCore never fires on QNX, so the harness cannot rely on it.
(
    CORPUS_FAKE=1
    . "$project_dir/scripts/corpus/lib.sh"
    started=$(now)
    out=$(corpus_timeout 2 sleep 20; echo "status=$?")
    elapsed=$(( $(now) - started ))
    status=${out#status=}
    [ "$status" -ge 128 ] 2>/dev/null && [ "$elapsed" -ge 2 ] &&
        [ "$elapsed" -lt 10 ]
)
check $? "corpus_timeout: kills a command that overruns, even inside \$( )"
(
    CORPUS_FAKE=1
    . "$project_dir/scripts/corpus/lib.sh"
    started=$(now)
    out=$(corpus_timeout 10 sh -c 'echo done; exit 3'; echo "status=$?")
    elapsed=$(( $(now) - started ))
    [ "$out" = "done
status=3" ] && [ "$elapsed" -lt 5 ]
)
check $? "corpus_timeout: passes output and status through, returns promptly"

# track-quiet: tracking a guest never adds to the guest's captured output.
# QNX ksh does not retry an append open interrupted by the SIGCHLD of the
# watchdog it just killed ("cannot create ...: Interrupted function call").
(
    CORPUS_FAKE=1
    . "$project_dir/scripts/corpus/lib.sh"
    corpus_app_out="$work/track-quiet"
    mkdir -p "$corpus_app_out"
    i=0
    while [ "$i" -lt 60 ]; do
        out=$(corpus_timeout 5 echo ok 2>&1)
        [ "$out" = ok ] || { echo "track-quiet: got: $out"; exit 1; }
        i=$((i + 1))
    done
    [ "$(grep -c '^start ' "$corpus_app_out/guest.pids")" = 120 ] &&
        [ "$(grep -c '^done ' "$corpus_app_out/guest.pids")" = 120 ]
)
check $? "track-quiet: 60 tracked guests, clean output, every pid recorded"

# ---- real background guests ---------------------------------------------
# A fake root of links to host programs, and a "linuxemu" that just runs the
# program it is given, let these cases start real processes the way guests
# are started, and check that the harness kills them.
fake_root="$work/root"
mkdir -p "$fake_root/bin" "$fake_root/usr/bin"
# host_program <name>: its path in /bin or /usr/bin (command -v would name
# shell builtins such as false).
host_program()
{
    for dir in /bin /usr/bin; do
        [ -x "$dir/$1" ] && { echo "$dir/$1"; return 0; }
    done
    return 1
}
for program in sleep sh; do
    path=$(host_program "$program") && ln -s "$path" "$fake_root/bin/$program"
done
# With a host python3: the guest python3, and a busybox that only does the
# harness's "wget -q -T <s> -O <file> <url>" (some host busybox builds crash
# in wget).
if host_python=$(host_program python3); then
    ln -s "$host_python" "$fake_root/usr/bin/python3"
    cat > "$fake_root/bin/busybox" <<EOF
#!/bin/sh
[ "\$1" = wget ] || exit 127
for url; do :; done
exec "$host_python" -c 'import sys, urllib.request
urllib.request.urlopen(sys.argv[1], timeout=3).read()' "\$url" 2>/dev/null
EOF
    chmod +x "$fake_root/bin/busybox"
fi
printf '#!/bin/sh\nexec "$@"\n' > "$work/linuxemu-shim"
chmod +x "$work/linuxemu-shim"
CORPUS_LINUXEMU="$work/linuxemu-shim"
ALPINE_ROOT="$fake_root"
export CORPUS_LINUXEMU ALPINE_ROOT

# gone <pidfile>: 0 when the process in <pidfile> has ended (within 6 s).
gone()
{
    [ -s "$1" ] || return 1
    gone_pid=$(cat "$1")
    gone_waited=0
    while kill -0 "$gone_pid" 2>/dev/null; do
        [ "$gone_waited" -ge 6 ] && { kill -KILL "$gone_pid" 2>/dev/null; return 1; }
        sleep 1
        gone_waited=$((gone_waited + 1))
    done
    return 0
}

# kill-bg: a background guest is killed at cleanup after a failure.
new_app kill-bg demo <<EOF
corpus_packages=demo
stage_install() { return 0; }
stage_startup() {
    guest_bg "\$corpus_app_out/server.pid" /bin/sleep 60
    kill -0 "\$(cat "\$corpus_app_out/server.pid")" || return 1
    cp "\$corpus_app_out/server.pid" "$work/kill-bg/server.pid"
}
stage_basic() { return 1; }
EOF
run_corpus kill-bg demo
[ "$(result kill-bg demo startup | cut -f1)" = pass ] &&
    gone "$work/kill-bg/server.pid"
check $? "kill-bg: background guest killed at cleanup"

# stage-timeout-guests: a foreground guest still running when its stage
# times out is killed with the stage, not left to its own (longer) timeout.
new_app stage-timeout-guests demo <<EOF
corpus_packages=demo
stage_install() { return 0; }
stage_startup() {
    out=\$(corpus_timeout 60 sh -c 'echo \$\$ > "$work/stage-timeout-guests/guest.pid"; exec sleep 60')
}
EOF
( CORPUS_STAGE_TIMEOUT=3; export CORPUS_STAGE_TIMEOUT
  run_corpus stage-timeout-guests demo )
[ "$(result stage-timeout-guests demo startup)" = "fail${tab}timeout after 3s" ] &&
    gone "$work/stage-timeout-guests/guest.pid"
check $? "stage-timeout-guests: guest of a timed-out stage killed"

# world-restore: a world the cleanup can't restore is reported, exit 1.
new_app world-restore demo <<'EOF'
corpus_packages=demo
stage_install() { return 0; }
stage_startup() { echo extra >> "$CORPUS_FAKE_WORLD"; return 0; }
EOF
run_corpus world-restore demo
grep -q '^world_restore demo failed$' "$work/world-restore/out/summary.txt" \
    2>/dev/null && [ "$runner_status" -eq 1 ]
check $? "world-restore: reported in summary, runner exit 1"

# virtual-removed: the app's virtual package is removed after a failing
# stage and after a timed-out stage.
for case_name in virtual-removed-fail virtual-removed-timeout; do
    new_app "$case_name" demo <<'EOF'
corpus_packages=demo
stage_install() { touch "$CORPUS_FAKE_INSTALLED/$corpus_virtual"; }
stage_startup() { [ -z "${CORPUS_DEMO_HANG:-}" ] || sleep 30; return 1; }
EOF
done
run_corpus virtual-removed-fail demo
[ "$(result virtual-removed-fail demo startup | cut -f1)" = fail ] &&
    [ ! -e "$work/virtual-removed-fail/installed/.linuxemu-corpus-demo" ]
check $? "virtual-removed: removed after a failing stage"
( CORPUS_STAGE_TIMEOUT=2 CORPUS_DEMO_HANG=1
  export CORPUS_STAGE_TIMEOUT CORPUS_DEMO_HANG
  run_corpus virtual-removed-timeout demo )
[ "$(result virtual-removed-timeout demo startup)" = "fail${tab}timeout after 2s" ] &&
    [ ! -e "$work/virtual-removed-timeout/installed/.linuxemu-corpus-demo" ]
check $? "virtual-removed: removed after a timed-out stage"

# runner-interrupt: TERM to the runner (as from Ctrl-C) kills the app's
# background guests instead of leaving a server behind for the next run.
new_app runner-interrupt demo <<EOF
corpus_packages=demo
stage_install() { return 0; }
stage_startup() {
    guest_bg "\$corpus_app_out/server.pid" /bin/sleep 60
    kill -0 "\$(cat "\$corpus_app_out/server.pid")" || return 1
    cp "\$corpus_app_out/server.pid" "$work/runner-interrupt/server.pid"
    sleep 60
}
EOF
mkdir -p "$work/runner-interrupt/out" "$work/runner-interrupt/installed"
printf 'busybox\n' > "$work/runner-interrupt/world"
CORPUS_FAKE=1 CORPUS_APPS_DIR="$work/runner-interrupt/apps" \
    CORPUS_OUT="$work/runner-interrupt/out" \
    CORPUS_FAKE_WORLD="$work/runner-interrupt/world" \
    CORPUS_FAKE_INSTALLED="$work/runner-interrupt/installed" \
    sh "$project_dir/scripts/run-corpus.sh" demo \
    > "$work/runner-interrupt/runner.log" 2>&1 &
runner_pid=$!
waited=0
while [ ! -s "$work/runner-interrupt/server.pid" ] && [ "$waited" -lt 20 ]; do
    sleep 1
    waited=$((waited + 1))
done
kill -TERM "$runner_pid"
wait "$runner_pid"
gone "$work/runner-interrupt/server.pid"
check $? "runner-interrupt: background guest killed when the runner is stopped"

# start_server: refuses to start when something already answers (a server
# left by an earlier run would otherwise be scored), and fails when the
# server exits before it answers.
(
    CORPUS_FAKE=1
    . "$project_dir/scripts/corpus/lib.sh"
    corpus_app_out="$work/start-server"
    mkdir -p "$corpus_app_out"
    touch "$corpus_app_out/ready"
    out=$(start_server "$corpus_app_out/s.pid" "[ -f $corpus_app_out/ready ]" 5 \
        /bin/sleep 60)
    status=$?
    [ "$status" -ne 0 ] && [ "$out" = "port in use before start" ] &&
        [ ! -e "$corpus_app_out/s.pid" ]
)
check $? "start_server: refuses when the port already answers"
(
    CORPUS_FAKE=1
    . "$project_dir/scripts/corpus/lib.sh"
    corpus_app_out="$work/start-server"
    mkdir -p "$corpus_app_out"
    rm -f "$corpus_app_out/ready"
    started=$(now)
    out=$(start_server "$corpus_app_out/s.pid" "[ -f $corpus_app_out/ready ]" 20 \
        /bin/sh -c 'exit 1')
    status=$?
    elapsed=$(( $(now) - started ))
    [ "$status" -ne 0 ] && [ "$out" = "server exited (status 1) before answering" ] &&
        [ "$elapsed" -lt 10 ]
)
check $? "start_server: fails promptly when the server exits first"
(
    CORPUS_FAKE=1
    . "$project_dir/scripts/corpus/lib.sh"
    corpus_app_out="$work/start-server"
    mkdir -p "$corpus_app_out"
    rm -f "$corpus_app_out/ready"
    start_server "$corpus_app_out/s.pid" "[ -f $corpus_app_out/ready ]" 20 \
        /bin/sh -c "touch $corpus_app_out/ready; exec sleep 60" >/dev/null &&
        [ ! -f "$corpus_app_out/s.pid.status" ]
    status=$?
    corpus_kill_bg
    exit "$status"
)
check $? "start_server: returns once the server answers"

# python-sigint: the Python app's server shuts down on SIGINT under Linux
# semantics too. A background job of a non-interactive shell starts with
# SIGINT ignored and that survives exec on Linux, so the stage must not rely
# on Linuxemu dropping the inherited disposition. Runs the real python3 and
# a wget stand-in when the host has python3.
if [ -x "$fake_root/usr/bin/python3" ] && [ -x "$fake_root/bin/busybox" ]; then
    # The shim runs the host's python3, so the server's --directory is the
    # host's /tmp/corpus-py.
    made_py_dir=
    [ -d /tmp/corpus-py ] || { mkdir /tmp/corpus-py && made_py_dir=1; }
    echo hello > /tmp/corpus-py/index.html
    (
        CORPUS_FAKE=1
        . "$project_dir/scripts/corpus/lib.sh"
        corpus_app_out="$work/python-sigint"
        mkdir -p "$corpus_app_out"
        . "$project_dir/scripts/corpus/apps/python.sh"
        python_server_start && stage_shutdown
        status=$?
        corpus_kill_bg
        exit "$status"
    ) > "$work/python-sigint.log" 2>&1
    check $? "python-sigint: http.server exits cleanly on SIGINT (Linux semantics)"
    [ -z "$made_py_dir" ] || rm -rf /tmp/corpus-py
else
    echo "skip: python-sigint (no host python3 or busybox)"
fi

echo "corpus_harness_test_failures=$failures"
test "$failures" -eq 0
