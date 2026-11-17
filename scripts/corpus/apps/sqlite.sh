# Corpus app: SQLite.
corpus_packages="sqlite"

stage_startup()
{
    out=$(guest 30 /usr/bin/sqlite3 --version 2>&1)
    case $out in 3.53*) ;; *) echo "unexpected: $out"; return 1 ;; esac
}

stage_basic()
{
    rm -f "$(rootfs_path /tmp/corpus.db)"
    out=$(guest 60 /usr/bin/sqlite3 /tmp/corpus.db 'create table t(x);
        with recursive c(i) as (select 1 union all select i + 1 from c
            where i < 100) insert into t select i from c;
        select count(*) from t;')
    [ "$out" = 100 ] || { echo "count: $out"; return 1; }
}

stage_concurrent()
{
    guest 30 /usr/bin/sqlite3 /tmp/corpus.db 'pragma journal_mode=wal;' \
        >/dev/null || { echo "could not enable WAL"; return 1; }
    out=$(guest_sh 300 'for i in 1 2 3 4; do
            sqlite3 -cmd ".timeout 5000" /tmp/corpus.db "with recursive
                c(i) as (select 1 union all select i + 1 from c where i < 250)
                insert into t select i from c;" &
        done; wait; sqlite3 /tmp/corpus.db "select count(*) from t;"')
    [ "$out" = 1100 ] || { echo "count after 4 writers: $out"; return 1; }
}

stage_shutdown()
{
    out=$(guest 60 /usr/bin/sqlite3 /tmp/corpus.db 'pragma integrity_check;')
    [ "$out" = ok ] || { echo "integrity_check: $out"; return 1; }
    [ ! -e "$(rootfs_path /tmp/corpus.db-wal)" ] ||
        { echo "WAL file left after the last connection closed"; return 1; }
}
