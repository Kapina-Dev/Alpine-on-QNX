#!/bin/sh
# A real JIT under Linuxemu (Phase 4 part 1): LuaJIT, installed from its
# pinned closure (docs/corpus/luajit-apks.sha256) and scored like a corpus
# app by scripts/run-corpus.sh, from scripts/jit-apps/ so the corpus itself
# stays at its pinned apps. Prints the app's stage line, then
# linuxemu_jit_failures=N; exits 0 only when N is 0.
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out="$project_dir/build/jit-smoke"
failures=0

CORPUS_APPS_DIR="$project_dir/scripts/jit-apps" CORPUS_OUT="$out" \
    sh "$script_dir/run-corpus.sh" luajit > "$out.log" 2>&1
status=$?
line=$(grep '^luajit ' "$out/summary.txt" 2>/dev/null)
echo "${line:-luajit no result (run-corpus exit $status, log $out.log)}"
case $line in
*compatible=yes*) ;;
*) failures=1; grep -v '^$' "$out/results.tsv" 2>/dev/null | grep -v pass ;;
esac
echo "linuxemu_jit_failures=$failures"
[ "$failures" -eq 0 ]
