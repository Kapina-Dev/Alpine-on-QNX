# Corpus app: inotify-tools (file watching). Linuxemu has no inotify yet
# (roadmap Phase 3), so this app is expected to fail at basic for now.
corpus_packages="inotify-tools"

stage_startup()
{
    out=$(guest 30 /usr/bin/inotifywait --help 2>&1)
    echo "$out" | grep -q inotifywait ||
        { echo "unexpected: $(echo "$out" | first_line)"; return 1; }
}

stage_basic()
{
    out=$(guest_sh 60 'rm -rf /tmp/corpus-watch; mkdir -p /tmp/corpus-watch
        inotifywait -q -e create -t 20 --format %f /tmp/corpus-watch \
            > /tmp/corpus-watch.out 2>&1 & p=$!
        sleep 2; touch /tmp/corpus-watch/newfile; wait $p
        cat /tmp/corpus-watch.out')
    [ "$out" = newfile ] || { echo "watcher saw: $(echo "$out" | first_line)"; return 1; }
}

stage_concurrent()
{
    out=$(guest_sh 60 'rm -rf /tmp/corpus-watch; mkdir -p /tmp/corpus-watch
        inotifywait -q -m -e create --format %f /tmp/corpus-watch \
            > /tmp/corpus-watch.out 2>&1 & p=$!
        sleep 2; t=""; for i in 1 2 3 4; do
            touch /tmp/corpus-watch/f$i & t="$t $!"
        done; wait $t; sleep 2; kill $p; wait $p
        sort /tmp/corpus-watch.out | tr "\n" " "')
    [ "$out" = "f1 f2 f3 f4 " ] || { echo "events seen: $out"; return 1; }
}

stage_shutdown()
{
    out=$(guest_sh 30 'mkdir -p /tmp/corpus-watch
        inotifywait -q -m /tmp/corpus-watch >/dev/null 2>&1 & p=$!
        sleep 1; kill $p; wait $p; kill -0 $p 2>/dev/null && echo alive || echo gone')
    [ "$out" = gone ] || { echo "watcher after kill: $out"; return 1; }
}
