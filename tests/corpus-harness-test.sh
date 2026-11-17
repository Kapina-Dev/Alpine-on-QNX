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

echo "corpus_harness_test_failures=$failures"
test "$failures" -eq 0
