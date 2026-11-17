# Corpus app: BusyBox. Install reinstalls the pinned closure over the base.
corpus_packages="busybox"

stage_startup()
{
    out=$(guest 30 /bin/busybox --help 2>&1)
    echo "$out" | grep -q 'BusyBox v1\.' ||
        { echo "unexpected: $(echo "$out" | first_line)"; return 1; }
}

stage_basic()
{
    top=$(guest_sh 60 'seq 1 1000 | sort -rn | head -n 1')
    [ "$top" = 1000 ] || { echo "seq|sort|head gave: $top"; return 1; }
    guest_sh 60 'set -e; rm -rf /tmp/corpus-bb; mkdir -p /tmp/corpus-bb/src
        cd /tmp/corpus-bb; echo one > src/a; echo two > src/b
        seq 1 500 > src/c; (cd src && md5sum a b c) > before
        tar czf t.tgz src; rm -rf src; tar xzf t.tgz
        (cd src && md5sum a b c) > after; cmp before after' ||
        { echo "tar+gzip round trip differs"; return 1; }
}

stage_concurrent()
{
    out=$(guest_sh 120 'for i in 1 2 3 4; do
            (seq 1 20000 | sort -n | tail -n 1 > /tmp/corpus-bb/p$i) &
        done; wait; cat /tmp/corpus-bb/p1 /tmp/corpus-bb/p2 \
            /tmp/corpus-bb/p3 /tmp/corpus-bb/p4' | tr '\n' ' ')
    [ "$out" = "20000 20000 20000 20000 " ] ||
        { echo "4 parallel pipelines gave: $out"; return 1; }
}

stage_shutdown()
{
    out=$(guest_sh 60 'sleep 30 & p=$!; kill -TERM $p; wait $p; s=$?
        kill -0 $p 2>/dev/null && echo still-alive; echo $s')
    [ "$out" = 143 ] || { echo "sleep after SIGTERM: $out"; return 1; }
}
