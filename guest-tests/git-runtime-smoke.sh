#!/bin/sh
set -eu

git=/usr/bin/git
busybox=/bin/busybox
work=/tmp/linuxemu-git-smoke
remote_url=https://github.com/octocat/Hello-World.git
master_commit=7fd1a60b01f91b314f59955a4e4d4e80d8edf11d
test_commit=b3cbd5bbd7e81436d2eee04537ea2b4c0cad4cdf

cleanup()
{
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM

fail()
{
    echo "git runtime smoke: $*" >&2
    exit 1
}

rm -rf "$work"
mkdir -p "$work/home" "$work/local"
export HOME="$work/home"
export GIT_CONFIG_NOSYSTEM=1

version=$($git --version)
test "$version" = "git version 2.54.0" || fail "unexpected version: $version"
echo "git_version=ok"

cd "$work/local"
$git init -b main >/dev/null
$git config user.name "Linuxemu Test"
$git config user.email "linuxemu@example.invalid"
printf '%s\n' "first revision" >README
$git add README
$git commit -m initial >/dev/null
first=$($git rev-parse HEAD)
test "$($git cat-file -t "$first")" = commit || fail "commit object missing"
test -z "$($git status --porcelain)" || fail "initial worktree is dirty"
$git fsck --strict
echo "git_local_objects=ok"

$busybox cat /bin/busybox /bin/busybox /bin/busybox /bin/busybox >payload.bin
payload_size=$($busybox wc -c <payload.bin)
test "$payload_size" -ge 2097152 || fail "large payload is short: $payload_size"
$git add payload.bin
$git commit -m payload >/dev/null
payload_commit=$($git rev-parse HEAD)
$git gc --prune=now
$git fsck --strict

cd "$work"
$git clone --bare local origin.git >/dev/null
$git clone origin.git local-clone >/dev/null
test "$(cd local-clone && $git rev-parse HEAD)" = "$payload_commit" ||
    fail "local clone has the wrong commit"
test -z "$(cd local-clone && $git status --porcelain)" ||
    fail "local clone worktree is dirty"
cd local
printf '%s\n' "incremental revision" >>README
$git add README
$git commit -m incremental >/dev/null
incremental_commit=$($git rev-parse HEAD)
$git push ../origin.git main >/dev/null
cd "$work/local-clone"
$git fetch origin >/dev/null
test "$($git rev-parse origin/main)" = "$incremental_commit" ||
    fail "local incremental fetch has the wrong commit"
$git fsck --strict
echo "git_large_local_transfer=ok"

cd "$work"
$git clone --single-branch --branch master "$remote_url" https-clone >/dev/null
cd https-clone
test "$($git rev-parse HEAD)" = "$master_commit" ||
    fail "HTTPS clone master ref changed"
$git fetch origin refs/heads/test:refs/remotes/origin/pinned-test >/dev/null
test "$($git rev-parse refs/remotes/origin/pinned-test)" = "$test_commit" ||
    fail "HTTPS incremental fetch ref changed"
$git fsck --strict
echo "git_https_clone_fetch=ok"

cd "$work"
: >empty-ca.pem
if GIT_SSL_CAINFO="$work/empty-ca.pem" $git ls-remote "$remote_url" \
        >invalid-cert.out 2>&1; then
    fail "invalid CA file unexpectedly succeeded"
fi
test -s invalid-cert.out || fail "invalid CA failure produced no diagnostic"
echo "git_invalid_certificate=ok"

if GIT_TERMINAL_PROMPT=0 $git ls-remote \
        https://github.com/octocat/linuxemu-phase11-auth-required.git \
        >authentication.out 2>&1; then
    fail "unavailable authenticated repository unexpectedly succeeded"
fi
test -s authentication.out || fail "authentication failure produced no diagnostic"
echo "git_authentication_failure=ok"

if GIT_TERMINAL_PROMPT=0 $git -c http.connectTimeout=2 ls-remote \
        https://127.0.0.1:1/linuxemu.git >unreachable.out 2>&1; then
    fail "unreachable endpoint unexpectedly succeeded"
fi
test -s unreachable.out || fail "unreachable failure produced no diagnostic"
echo "git_unreachable_failure=ok"

echo "git_runtime_smoke_failures=0"
