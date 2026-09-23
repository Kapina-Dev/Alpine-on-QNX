#!/bin/sh
set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
busybox="$alpine_root/bin/busybox"
server="$project_dir/build/native-probes/http-loopback-server"
dns_server=${DNS_SERVER:-192.168.0.1}
dns_name=${DNS_NAME:-example.com}
timeout_command=${TIMEOUT_COMMAND:-timeout}
output_dir="$project_dir/build/network-smoke"
failures=0
server_pid=

mkdir -p "$output_dir"

stop_server()
{
    if [ -n "$server_pid" ]; then
        kill -9 "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        server_pid=
    fi
}
trap stop_server EXIT HUP INT TERM

run_http()
{
    name=$1
    host=$2
    family=${3:-4}
    server_log="$output_dir/$name-server.out"
    guest_log="$output_dir/$name-guest.out"
    "$server" "$family" >"$server_log" 2>&1 &
    server_pid=$!
    sleep 1
    LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$busybox" wget -T 5 -qO- \
        "http://$host:18080/" >"$guest_log" 2>&1
    guest_status=$?
    if [ "$guest_status" -eq 0 ]; then
        wait "$server_pid"
        server_status=$?
        server_pid=
    else
        stop_server
        server_status=1
    fi
    cat "$guest_log"
    echo "$name guest_exit=$guest_status server_exit=$server_status expected=0"
    if [ "$guest_status" -ne 0 ] || [ "$server_status" -ne 0 ] ||
        ! grep -q '^phase5_http_ok$' "$guest_log"; then
        failures=$((failures + 1))
    fi
}

run_connect_probe()
{
    server_log="$output_dir/connect-server.out"
    "$server" 4 >"$server_log" 2>&1 &
    server_pid=$!
    sleep 1
    LINUXEMU_ROOT="$alpine_root" "$linuxemu" \
        "$project_dir/build/guest-tests/network-connect-syscalls"
    guest_status=$?
    if [ "$guest_status" -eq 0 ]; then
        wait "$server_pid"
        server_status=$?
        server_pid=
    else
        stop_server
        server_status=1
    fi
    echo "nonblocking-connect guest_exit=$guest_status server_exit=$server_status expected=0"
    if [ "$guest_status" -ne 0 ] || [ "$server_status" -ne 0 ]; then
        failures=$((failures + 1))
    fi
}

run_dns_query()
{
    LINUXEMU_ROOT="$alpine_root" "$timeout_command" -s 9 10 \
        "$linuxemu" "$busybox" nslookup \
        "$dns_name" "$dns_server" >"$output_dir/dns-guest.out" 2>&1
    status=$?
    cat "$output_dir/dns-guest.out"
    echo "dns-query exit=$status expected=0 server=$dns_server name=$dns_name"
    if [ "$status" -ne 0 ] ||
        ! grep -q "Name:.*$dns_name" "$output_dir/dns-guest.out"; then
        failures=$((failures + 1))
    fi
}

echo "=== network guest: syscall-probe ==="
LINUXEMU_ROOT="$alpine_root" "$linuxemu" \
    "$project_dir/build/guest-tests/socket-syscalls"
status=$?
echo "syscall-probe exit=$status expected=0"
if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi

run_connect_probe
run_http numeric-host 127.0.0.1 4
run_http hosts-name localhost 6
run_dns_query

LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$busybox" wget -T 1 -qO- \
    http://127.0.0.1:18080/ >"$output_dir/refused.out" 2>&1
status=$?
cat "$output_dir/refused.out"
echo "connection-refused exit=$status expected=1"
if [ "$status" -ne 1 ] ||
    ! grep -q 'Connection refused' "$output_dir/refused.out"; then
    failures=$((failures + 1))
fi

echo "linuxemu_network_smoke_failures=$failures"
test "$failures" -eq 0
