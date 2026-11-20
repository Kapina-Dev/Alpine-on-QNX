# Corpus app: nginx, run as the ordinary user on 127.0.0.1:18081.
corpus_packages="nginx"

nginx_dir=/tmp/corpus-nginx

nginx_args()
{
    echo "-e $nginx_dir/error.log -c $nginx_dir/nginx.conf"
}

# nginx_worker: the pid of the newest worker, from the notice-level
# "start worker process <pid>" lines in the error log (the guest has no
# /proc for ps). Guest pids are host pids, so the host can signal them.
nginx_worker()
{
    awk '/start worker process/ { pid = $NF } END { print pid }' \
        "$(rootfs_path $nginx_dir/error.log)"
}

nginx_workers_started()
{
    grep -c 'start worker process' "$(rootfs_path $nginx_dir/error.log)"
}

# nginx_reload: one "nginx -s reload", settled when a new worker has
# started and the old one has exited (at most 10 s).
nginx_reload()
{
    nr_old=$(nginx_worker)
    nr_started=$(nginx_workers_started)
    guest 30 /usr/sbin/nginx $(nginx_args) -s reload ||
        { echo "nginx -s reload failed"; return 1; }
    nr_waited=0
    while [ "$(nginx_workers_started)" -le "$nr_started" ] ||
            kill -0 "$nr_old" 2>/dev/null; do
        if [ "$nr_waited" -ge 10 ]; then
            echo "worker not replaced after reload (old $nr_old, newest $(nginx_worker))"
            return 1
        fi
        sleep 1
        nr_waited=$((nr_waited + 1))
    done
}

stage_startup()
{
    dir=$(rootfs_path $nginx_dir)
    mkdir -p "$dir/www"
    cat > "$dir/nginx.conf" <<CONF
worker_processes 1;
daemon off;
pid $nginx_dir/nginx.pid;
error_log $nginx_dir/error.log notice;
events { worker_connections 64; }
http {
    access_log off;
    client_header_timeout 2s;
    client_body_temp_path $nginx_dir/body;
    proxy_temp_path $nginx_dir/proxy;
    fastcgi_temp_path $nginx_dir/fastcgi;
    uwsgi_temp_path $nginx_dir/uwsgi;
    scgi_temp_path $nginx_dir/scgi;
    server { listen 127.0.0.1:18081; root $nginx_dir/www; }
}
CONF
    guest 60 /usr/sbin/nginx -t $(nginx_args)
}

stage_basic()
{
    dir=$(rootfs_path $nginx_dir)
    head -c 65536 /dev/urandom > "$dir/www/big"
    echo ok > "$dir/www/index.html"
    start_server "$corpus_app_out/server.pid" "http_answers 18081" 60 \
        /usr/sbin/nginx $(nginx_args) || return 1
    out=$(guest_sh 60 "cd $nginx_dir && wget -q -O got http://127.0.0.1:18081/big &&
        md5sum www/big got | awk '{ print \$1 }' | uniq | wc -l")
    [ "$out" = 1 ] || { echo "64 KiB GET did not match"; return 1; }
    # An idle client that sends nothing: nginx must close it after
    # client_header_timeout (2 s). nc hands the connected socket to dd as
    # its standard input, so the client never writes and ends exactly when
    # a read returns 0 (server closed) or fails (connection reset). (nc
    # itself would keep polling its own standard input after the server
    # closed, and busybox cat would first try sendfile from the socket.)
    out=$(guest_sh 30 "s=\$(date +%s)
        timeout 10 nc 127.0.0.1 18081 -e dd of=/dev/null 2>/dev/null
        echo \$((\$(date +%s) - s))")
    case $out in
        2|3|4|5|6) ;;
        *) echo "idle client not closed within 2-6s (took ${out:-?}s)"; return 1 ;;
    esac
}

stage_concurrent()
{
    out=$(guest_sh 120 "cd $nginx_dir && rm -f got*; for i in 1 2 3 4 5 6 7 8 9 10; do
            wget -q -O got\$i http://127.0.0.1:18081/big &
        done; wait; md5sum www/big got1 got2 got3 got4 got5 got6 got7 got8 got9 got10 |
            awk '{ print \$1 }' | sort | uniq -c | awk '{ print \$1 }'")
    [ "$out" = 11 ] || { echo "parallel 64 KiB GETs matching: $out of 11"; return 1; }
    first=$(nginx_worker)
    [ -n "$first" ] || { echo "no worker pid in the error log"; return 1; }
    nginx_reload || return 1
    nginx_reload || return 1
    now=$(nginx_worker)
    [ "$now" != "$first" ] && kill -0 "$now" 2>/dev/null ||
        { echo "worker not replaced after reload (first $first, now $now)"; return 1; }
    out=$(guest 10 /bin/busybox wget -q -T 3 -O - http://127.0.0.1:18081/index.html)
    [ "$out" = ok ] || { echo "GET after reloads: ${out:-nothing}"; return 1; }
}

stage_shutdown()
{
    guest 30 /usr/sbin/nginx $(nginx_args) -s quit ||
        { echo "nginx -s quit failed"; return 1; }
    wait_gone "$corpus_app_out/server.pid" 10 ||
        { echo "nginx still running 10s after quit"; return 1; }
    [ ! -e "$(rootfs_path $nginx_dir/nginx.pid)" ] ||
        { echo "pid file left behind"; return 1; }
    http_refused 18081 || { echo "port 18081 still answers"; return 1; }
}
