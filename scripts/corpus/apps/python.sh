# Corpus app: Python 3.14.
corpus_packages="python3"

stage_startup()
{
    out=$(guest 60 /usr/bin/python3 -c \
        'import sys; print(sys.version_info[:2])' 2>&1)
    [ "$out" = "(3, 14)" ] || { echo "unexpected: $out"; return 1; }
}

stage_basic()
{
    # The application suite also makes one HTTPS connection.
    echo "nameserver ${DNS_SERVER:-192.168.0.1}" > \
        "$(rootfs_path /etc/resolv.conf)"
    cp "$project_dir/guest-tests/python-runtime-smoke.py" \
        "$(rootfs_path /tmp/python-runtime-smoke.py)"
    guest 300 /usr/bin/python3 /tmp/python-runtime-smoke.py
}

stage_concurrent()
{
    mkdir -p "$(rootfs_path /tmp/corpus-py)"
    echo hello > "$(rootfs_path /tmp/corpus-py/index.html)"
    guest_bg "$corpus_app_out/server.pid" /usr/bin/python3 -m http.server \
        18080 --bind 127.0.0.1 --directory /tmp/corpus-py
    wait_http 18080 60 || { echo "http.server did not answer"; return 1; }
    out=$(guest_sh 120 'rm -f /tmp/corpus-py-ok*; for i in 1 2 3 4 5 6 7 8 9 10; do
            (wget -q -O /tmp/corpus-py-get$i http://127.0.0.1:18080/index.html &&
                echo ok > /tmp/corpus-py-ok$i) &
        done; wait; ls /tmp/corpus-py-ok* | wc -l')
    [ "$out" = 10 ] || { echo "parallel GETs: $out of 10 ok"; return 1; }
}

stage_shutdown()
{
    kill -INT "$(cat "$corpus_app_out/server.pid")"
    wait_gone "$corpus_app_out/server.pid" 10 ||
        { echo "http.server still running 10s after SIGINT"; return 1; }
    # python3 -m http.server catches KeyboardInterrupt, prints this line and
    # exits 0 (on Linux too).
    status=$(cat "$corpus_app_out/server.pid.status")
    [ "$status" = 0 ] || { echo "http.server exit $status, want 0"; return 1; }
    grep -q 'Keyboard interrupt received, exiting' \
        "$corpus_app_out/background.log" ||
        { echo "no clean KeyboardInterrupt exit message"; return 1; }
    http_refused 18080 || { echo "port 18080 still answers"; return 1; }
}
