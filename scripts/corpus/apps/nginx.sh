# Corpus app: nginx, run as the ordinary user on 127.0.0.1:18081.
corpus_packages="nginx"

nginx_dir=/tmp/corpus-nginx

nginx_args()
{
    echo "-e $nginx_dir/error.log -c $nginx_dir/nginx.conf"
}

stage_startup()
{
    dir=$(rootfs_path $nginx_dir)
    mkdir -p "$dir/www"
    cat > "$dir/nginx.conf" <<CONF
worker_processes 1;
daemon off;
pid $nginx_dir/nginx.pid;
error_log $nginx_dir/error.log;
events { worker_connections 64; }
http {
    access_log off;
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
}

stage_concurrent()
{
    out=$(guest_sh 120 "cd $nginx_dir && rm -f got*; for i in 1 2 3 4 5 6 7 8 9 10; do
            wget -q -O got\$i http://127.0.0.1:18081/big &
        done; wait; md5sum www/big got1 got2 got3 got4 got5 got6 got7 got8 got9 got10 |
            awk '{ print \$1 }' | sort | uniq -c | awk '{ print \$1 }'")
    [ "$out" = 11 ] || { echo "parallel 64 KiB GETs matching: $out of 11"; return 1; }
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
