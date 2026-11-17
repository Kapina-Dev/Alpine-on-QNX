# Application-corpus harness (roadmap Phase 0). Sourced by
# scripts/run-corpus.sh, not executed.
#
# An app definition (scripts/corpus/apps/<app>.sh) sets corpus_packages and
# defines stage_startup, stage_basic, stage_concurrent and stage_shutdown,
# plus optionally stage_install (default: the pinned closure) and
# app_cleanup. A stage passes when it returns 0. Each stage runs in its own
# subshell and process group with a timeout, later stages are skipped after
# a failure, and cleanup always runs.
#
# Environment:
#   CORPUS_APPS_DIR       app definitions (default scripts/corpus/apps)
#   CORPUS_OUT            output directory (default build/corpus)
#   CORPUS_STAGE_TIMEOUT  seconds per stage (default 300)
#   CORPUS_FAKE=1         host test mode: no Linuxemu or rootfs; the world
#                         file is CORPUS_FAKE_WORLD and installed virtual
#                         packages are marker files in CORPUS_FAKE_INSTALLED
#   CORPUS_FRESH_ROOTFS   1 (default): each app runs in a fresh copy of the
#                         base rootfs, so pinned closures that upgrade base
#                         packages never change the shared test rootfs
#   CORPUS_MANIFEST_DIR   pinned manifests (default docs/corpus)
#   CORPUS_ARCHIVE_DIR    pinned closures (default artifacts/corpus)

# Portability: the Passport's /bin/sh is QNX ksh, where an empty "$@" under
# set -u is an error, so argument lists are passed as ${1+"$@"}.

corpus_stages="install startup basic concurrent shutdown"
corpus_apps_dir=${CORPUS_APPS_DIR:-"$project_dir/scripts/corpus/apps"}
corpus_out=${CORPUS_OUT:-"$project_dir/build/corpus"}
corpus_stage_timeout=${CORPUS_STAGE_TIMEOUT:-300}
corpus_fake=${CORPUS_FAKE:-0}
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
timeout_command=${TIMEOUT_COMMAND:-timeout}
corpus_base_root=$alpine_root
corpus_fresh_rootfs=${CORPUS_FRESH_ROOTFS:-1}
corpus_manifest_dir=${CORPUS_MANIFEST_DIR:-"$project_dir/docs/corpus"}
corpus_archive_dir=${CORPUS_ARCHIVE_DIR:-"$project_dir/artifacts/corpus"}
# Manifest paths are relative to this directory (the project, normally).
corpus_manifest_base=${CORPUS_PROJECT_RELATIVE:-"$project_dir"}

corpus_world_path()
{
    if [ "$corpus_fake" = 1 ]; then
        echo "$CORPUS_FAKE_WORLD"
    else
        echo "$alpine_root/etc/apk/world"
    fi
}
corpus_world=$(corpus_world_path)

# ---- helpers for app definitions -------------------------------------

rootfs_path()
{
    printf '%s%s\n' "$alpine_root" "$1"
}

# guest <timeout_seconds> <guest-path> [args...]
guest()
{
    guest_timeout=$1
    guest_program=$2
    shift 2
    LINUXEMU_ROOT="$alpine_root" \
        LINUXEMU_DIAGNOSTICS="$corpus_app_out/diagnostics.txt" \
        "$timeout_command" -s 9 "$guest_timeout" \
        "$linuxemu" "$alpine_root$guest_program" ${1+"$@"}
}

# guest_sh <timeout_seconds> '<busybox sh script>'
guest_sh()
{
    guest "$1" /bin/busybox sh -c "$2"
}

# guest_bg <pidfile> <guest-path> [args...]: background guest. Its pid is
# written to <pidfile>, and its exit status to <pidfile>.status when it
# ends, so a later stage can signal it and check how it exited. Killed at
# cleanup by corpus_kill_bg.
guest_bg()
{
    guest_pidfile=$1
    guest_program=$2
    shift 2
    rm -f "$guest_pidfile" "$guest_pidfile.status"
    (
        LINUXEMU_ROOT="$alpine_root" \
            LINUXEMU_DIAGNOSTICS="$corpus_app_out/diagnostics.txt" \
            "$linuxemu" "$alpine_root$guest_program" ${1+"$@"} \
            >>"$corpus_app_out/background.log" 2>&1 &
        guest_bg_pid=$!
        echo "$guest_bg_pid" > "$guest_pidfile"
        echo "$guest_bg_pid" >> "$corpus_app_out/background.pids"
        wait "$guest_bg_pid"
        echo "$?" > "$guest_pidfile.status"
    ) &
    while [ ! -s "$guest_pidfile" ]; do
        sleep 1
    done
}

# wait_gone <pidfile> <seconds>: 0 once the background guest has exited.
wait_gone()
{
    waited=0
    while [ ! -f "$1.status" ]; do
        [ "$waited" -ge "$2" ] && return 1
        sleep 1
        waited=$((waited + 1))
    done
    return 0
}

# wait_http <port> <seconds>: 0 once http://127.0.0.1:<port>/ answers.
wait_http()
{
    waited=0
    until guest 10 /bin/busybox wget -q -T 2 -O /dev/null \
            "http://127.0.0.1:$1/" 2>/dev/null; do
        [ "$waited" -ge "$2" ] && return 1
        sleep 1
        waited=$((waited + 1))
    done
    return 0
}

# http_refused <port>: 0 when nothing answers on 127.0.0.1:<port>.
http_refused()
{
    ! guest 10 /bin/busybox wget -q -T 3 -O /dev/null \
        "http://127.0.0.1:$1/" 2>/dev/null
}

corpus_kill_bg()
{
    pids_file="$corpus_app_out/background.pids"
    [ -f "$pids_file" ] || return 0
    while read -r pid; do
        kill -TERM "$pid" 2>/dev/null
    done < "$pids_file"
    sleep 3
    while read -r pid; do
        kill -KILL "$pid" 2>/dev/null
    done < "$pids_file"
    rm -f "$pids_file"
}

# ---- package state -----------------------------------------------------

corpus_apk()
{
    LINUXEMU_ROOT="$alpine_root" "$timeout_command" -s 9 300 \
        "$linuxemu" "$alpine_root/sbin/apk" ${1+"$@"}
}

corpus_virtual_installed()
{
    if [ "$corpus_fake" = 1 ]; then
        [ -e "$CORPUS_FAKE_INSTALLED/$corpus_virtual" ]
    else
        corpus_apk info -e "$corpus_virtual" >/dev/null 2>&1
    fi
}

corpus_remove_virtual()
{
    if [ "$corpus_fake" = 1 ]; then
        rm -f "$CORPUS_FAKE_INSTALLED/$corpus_virtual"
    elif corpus_virtual_installed; then
        corpus_apk del "$corpus_virtual"
    fi
}

# Default install: verify the pinned offline closure against its manifest,
# then install exactly the listed files under the app's virtual package.
corpus_default_install()
{
    manifest="$corpus_manifest_dir/$corpus_app-apks.sha256"
    if [ ! -s "$manifest" ] || ! ( cd "$corpus_manifest_base" &&
            sha256sum -c "$manifest" ) >/dev/null 2>&1; then
        echo "pinned archive check failed"
        return 1
    fi
    if [ "$corpus_fake" = 1 ]; then
        touch "$CORPUS_FAKE_INSTALLED/$corpus_virtual"
        return 0
    fi
    staging="$alpine_root/tmp/linuxemu-corpus-apks"
    rm -rf "$staging"
    mkdir -p "$staging"
    awk '{ print $2 }' "$manifest" | while read -r file; do
        cp "$corpus_manifest_base/$file" "$staging/" || exit 1
    done || { echo "copying the pinned closure failed"; return 1; }
    ( cd "$alpine_root" && corpus_apk add --no-network --no-chown \
        --allow-untrusted --virtual "$corpus_virtual" \
        tmp/linuxemu-corpus-apks/*.apk )
    status=$?
    rm -rf "$staging"
    [ "$status" -eq 0 ] || { echo "apk add failed: exit $status"; return 1; }
    for package in $corpus_packages; do
        corpus_apk info -e "$package" >/dev/null 2>&1 ||
            { echo "not installed after apk add: $package"; return 1; }
    done
    return 0
}

# ---- stage runner ------------------------------------------------------

# Seconds since the epoch: QNX's date has no %s, but -t prints it.
corpus_now()
{
    now=$(date +%s 2>/dev/null)
    case $now in
        ''|*[!0-9]*) date -t ;;
        *) echo "$now" ;;
    esac
}

corpus_has_function()
{
    type "$1" 2>/dev/null | grep -q 'function'
}

corpus_record()
{
    printf '%s\t%s\t%s\t%s\t%s\n' "$corpus_app" "$1" "$2" "$3" "$4" \
        >> "$corpus_out/results.tsv"
}

# corpus_run_stage <stage>: returns 0 when the stage passed.
corpus_run_stage()
{
    stage=$1
    function_name="stage_$stage"
    if ! corpus_has_function "$function_name"; then
        if [ "$stage" = install ]; then
            function_name=corpus_default_install
        else
            corpus_record "$stage" fail 0 "stage not defined"
            return 1
        fi
    fi
    log="$corpus_app_out/$stage.log"
    started=$(corpus_now)
    timed_out=0
    ( "$function_name" ) > "$log" 2>&1 &
    stage_pid=$!
    waited=0
    while kill -0 "$stage_pid" 2>/dev/null; do
        if [ "$waited" -ge "$corpus_stage_timeout" ]; then
            timed_out=1
            # Killing the stage subshell unblocks the runner. Guests the
            # stage started still end at their own guest timeout, and
            # background guests are killed at cleanup.
            kill -TERM "$stage_pid" 2>/dev/null
            sleep 1
            kill -KILL "$stage_pid" 2>/dev/null
            break
        fi
        sleep 1
        waited=$((waited + 1))
    done
    wait "$stage_pid"
    status=$?
    seconds=$(( $(corpus_now) - started ))
    if [ "$timed_out" -eq 1 ]; then
        corpus_record "$stage" fail "$seconds" \
            "timeout after ${corpus_stage_timeout}s"
        return 1
    fi
    if [ "$status" -eq 0 ]; then
        corpus_record "$stage" pass "$seconds" ""
        return 0
    fi
    note=$(head -n 1 "$log" | tr '\t\r' '  ' | cut -c1-120)
    [ -n "$note" ] || note="exit $status"
    corpus_record "$stage" fail "$seconds" "$note"
    return 1
}

# corpus_run_app <app definition file>: run in a subshell by the runner.
corpus_run_app()
{
    corpus_app=$(basename "$1" .sh)
    corpus_app_out="$corpus_out/$corpus_app"
    rm -rf "$corpus_app_out"
    mkdir -p "$corpus_app_out"
    corpus_packages=
    corpus_virtual=".linuxemu-corpus-$corpus_app"
    . "$1"
    if [ "$corpus_fake" != 1 ] && [ "$corpus_fresh_rootfs" = 1 ]; then
        alpine_root="$corpus_app_out/rootfs"
        mkdir -p "$alpine_root"
        ( cd "$corpus_base_root" && tar cf - . ) |
            ( cd "$alpine_root" && tar xf - ) ||
            { echo "copying the base rootfs failed" >&2; return 1; }
    fi
    corpus_world=$(corpus_world_path)
    cp "$corpus_world" "$corpus_app_out/world.before"

    failed=
    if corpus_virtual_installed; then
        corpus_record install fail 0 "rootfs not clean"
        failed=install
        for stage in $corpus_stages; do
            [ "$stage" = install ] && continue
            corpus_record "$stage" skip 0 "after install failed"
        done
        [ "$alpine_root" = "$corpus_base_root" ] || rm -rf "$alpine_root"
        return 0
    fi
    for stage in $corpus_stages; do
        if [ -n "$failed" ]; then
            corpus_record "$stage" skip 0 "after $failed failed"
        elif ! corpus_run_stage "$stage"; then
            failed=$stage
        fi
    done

    corpus_kill_bg
    if corpus_has_function app_cleanup; then
        app_cleanup > "$corpus_app_out/cleanup.log" 2>&1
    fi
    corpus_remove_virtual >> "$corpus_app_out/cleanup.log" 2>&1
    if ! cmp -s "$corpus_world" "$corpus_app_out/world.before"; then
        echo "world_restore $corpus_app failed" >> "$corpus_out/world-failures"
    fi
    if [ "$alpine_root" != "$corpus_base_root" ]; then
        rm -rf "$alpine_root"
    fi
    return 0
}

# corpus_summary: write summary.txt from results.tsv for the apps in
# apps.list, in run order.
corpus_summary()
{
    summary="$corpus_out/summary.txt"
    : > "$summary"
    apps=0
    compatible_apps=0
    while read -r app; do
        apps=$((apps + 1))
        line="$app"
        all_pass=yes
        for stage in $corpus_stages; do
            result=$(awk -F '\t' -v a="$app" -v s="$stage" \
                '$1 == a && $2 == s { print $3 }' "$corpus_out/results.tsv")
            [ -n "$result" ] || result=missing
            [ "$result" = pass ] || all_pass=no
            line="$line $stage=$result"
        done
        unsupported=0
        if [ -f "$corpus_out/$app/diagnostics.txt" ]; then
            unsupported=$(awk '$1 == "linuxemu-unsupported" {
                    key = ""
                    for (i = 2; i <= NF; i++)
                        if ($i ~ /^(nr|error|detail)=/) key = key " " $i
                    seen[key] = 1
                } END { n = 0; for (k in seen) n++; print n }' \
                "$corpus_out/$app/diagnostics.txt")
        fi
        [ "$all_pass" = yes ] && compatible_apps=$((compatible_apps + 1))
        echo "$line compatible=$all_pass unsupported=$unsupported" >> "$summary"
    done < "$corpus_out/apps.list"
    echo "corpus apps=$apps compatible=$compatible_apps" >> "$summary"
    if [ -f "$corpus_out/world-failures" ]; then
        cat "$corpus_out/world-failures" >> "$summary"
    fi
}
