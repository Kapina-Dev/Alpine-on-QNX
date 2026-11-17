#!/bin/sh
# Score the pinned application corpus (roadmap Phase 0): for each app,
# install, startup, basic, concurrent and shutdown. A failing app is a
# measurement, not a runner failure; the runner exits 1 only when an app's
# cleanup did not restore the Alpine package world.
#   sh scripts/run-corpus.sh [app ...]
# Results: build/corpus/results.tsv and build/corpus/summary.txt.
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
. "$project_dir/scripts/corpus/lib.sh"

if [ "$corpus_fake" != 1 ] &&
    { [ ! -x "$linuxemu" ] || [ ! -f "$alpine_root/sbin/apk" ]; }; then
    echo "Linuxemu or the Alpine test rootfs is missing" >&2
    exit 2
fi

mkdir -p "$corpus_out"
: > "$corpus_out/results.tsv"
: > "$corpus_out/apps.list"
rm -f "$corpus_out/world-failures"

if [ "$#" -eq 0 ]; then
    set -- $(cd "$corpus_apps_dir" && ls *.sh 2>/dev/null | sed 's/[.]sh$//' |
        sort)
fi
# Stopping the runner (Ctrl-C, or TERM) stops the current app, which kills
# its guests: a server left running would answer in the next run.
corpus_app_pid=
corpus_interrupt()
{
    trap '' INT TERM
    if [ -n "$corpus_app_pid" ]; then
        kill -TERM "$corpus_app_pid" 2>/dev/null
        wait "$corpus_app_pid"
    fi
    echo "corpus run interrupted" >&2
    exit 130
}
trap 'corpus_interrupt' INT TERM

for app in ${1+"$@"}; do
    definition="$corpus_apps_dir/$app.sh"
    if [ ! -f "$definition" ]; then
        echo "no app definition: $definition" >&2
        continue
    fi
    echo "$app" >> "$corpus_out/apps.list"
    echo "=== corpus: $app ==="
    ( corpus_run_app "$definition" ) &
    corpus_app_pid=$!
    wait "$corpus_app_pid"
    corpus_app_pid=
    awk -F '\t' -v a="$app" '$1 == a { print "  " $2 ": " $3 ($5 == "" ? "" : " (" $5 ")") }' \
        "$corpus_out/results.tsv"
done

corpus_summary
cat "$corpus_out/summary.txt"
[ ! -f "$corpus_out/world-failures" ]
