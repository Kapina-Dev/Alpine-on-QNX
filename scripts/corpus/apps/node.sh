# Corpus app: Node.js (libuv), on 127.0.0.1:18082.
corpus_packages="nodejs"

stage_startup()
{
    out=$(guest 60 /usr/bin/node -e 'console.log(process.versions.node)' 2>&1)
    case $out in 24.*) ;; *) echo "unexpected: $(echo "$out" | first_line)"; return 1 ;; esac
}

stage_basic()
{
    out=$(guest 60 /usr/bin/node -e "const fs = require('fs');
        fs.writeFileSync('/tmp/corpus-node.txt', 'round trip');
        console.log(fs.readFileSync('/tmp/corpus-node.txt', 'utf8'));" 2>&1)
    [ "$out" = "round trip" ] || { echo "unexpected: $(echo "$out" | first_line)"; return 1; }
}

stage_concurrent()
{
    mkdir -p "$(rootfs_path /tmp/corpus-node)"
    cat > "$(rootfs_path /tmp/corpus-node/server.js)" <<'JS'
const http = require('http');
const server = http.createServer((request, response) => response.end('ok'));
server.listen(18082, '127.0.0.1');
process.on('SIGTERM', () => server.close(() => process.exit(0)));
JS
    start_server "$corpus_app_out/server.pid" "http_answers 18082" 60 \
        /usr/bin/node /tmp/corpus-node/server.js || return 1
    out=$(guest_sh 120 'rm -f /tmp/corpus-node-ok*; for i in 1 2 3 4 5 6 7 8 9 10; do
            (wget -q -O /dev/null http://127.0.0.1:18082/ &&
                echo ok > /tmp/corpus-node-ok$i) &
        done; wait; ls /tmp/corpus-node-ok* | wc -l')
    [ "$out" = 10 ] || { echo "parallel GETs: $out of 10 ok"; return 1; }
}

stage_shutdown()
{
    kill -TERM "$(cat "$corpus_app_out/server.pid")"
    wait_gone "$corpus_app_out/server.pid" 10 ||
        { echo "node still running 10s after SIGTERM"; return 1; }
    status=$(cat "$corpus_app_out/server.pid.status")
    [ "$status" = 0 ] || { echo "node exit $status, want 0"; return 1; }
    http_refused 18082 || { echo "port 18082 still answers"; return 1; }
}
