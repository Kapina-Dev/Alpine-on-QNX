# Corpus app: Redis, on 127.0.0.1:18083.
corpus_packages="redis"

redis_ping()
{
    [ "$(guest 10 /usr/bin/redis-cli -p 18083 ping 2>/dev/null)" = PONG ]
}

stage_startup()
{
    out=$(guest 30 /usr/bin/redis-server --version 2>&1)
    case $out in *v=8.8*) ;; *) echo "unexpected: $out"; return 1 ;; esac
}

stage_basic()
{
    mkdir -p "$(rootfs_path /tmp/corpus-redis)"
    start_server "$corpus_app_out/server.pid" redis_ping 60 \
        /usr/bin/redis-server --port 18083 --bind 127.0.0.1 --save '' \
        --appendonly no --dir /tmp/corpus-redis || return 1
    guest 10 /usr/bin/redis-cli -p 18083 set corpus value >/dev/null
    out=$(guest 10 /usr/bin/redis-cli -p 18083 get corpus)
    [ "$out" = value ] || { echo "get: $out"; return 1; }
}

stage_concurrent()
{
    guest 300 /usr/bin/redis-benchmark -p 18083 -c 10 -n 1000 -t set,get -q
}

stage_shutdown()
{
    guest 30 /usr/bin/redis-cli -p 18083 shutdown nosave >/dev/null 2>&1
    wait_gone "$corpus_app_out/server.pid" 10 ||
        { echo "redis still running 10s after SHUTDOWN"; return 1; }
    ! redis_ping || { echo "port 18083 still answers"; return 1; }
}
