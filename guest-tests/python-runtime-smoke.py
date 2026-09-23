#!/usr/bin/env python3
import bz2
import ctypes
import decimal
import errno
import hashlib
import json
import lzma
import os
from pathlib import Path
import select
import signal
import socket
import sqlite3
import ssl
import subprocess
import tempfile
import threading
import time


def check(name, function):
    function()
    print(f"python_{name}=ok", flush=True)


def test_imports_and_files():
    assert json.loads('{"value": 7}')["value"] == 7
    assert hashlib.sha256(b"linuxemu").hexdigest().startswith("44a2")
    assert bz2.decompress(bz2.compress(b"bz2")) == b"bz2"
    assert lzma.decompress(lzma.compress(b"lzma")) == b"lzma"
    assert decimal.Decimal("1.25") * 2 == decimal.Decimal("2.50")
    assert ctypes.sizeof(ctypes.c_void_p) == 4
    with tempfile.TemporaryDirectory(prefix="linuxemu-python-") as directory:
        path = Path(directory) / "data.json"
        path.write_text('{"roundtrip": true}\n', encoding="utf-8")
        assert json.loads(path.read_text(encoding="utf-8"))["roundtrip"]


def test_sqlite():
    with tempfile.TemporaryDirectory(prefix="linuxemu-sqlite-") as directory:
        database = Path(directory) / "runtime.db"
        with sqlite3.connect(database) as connection:
            connection.execute("create table values_under_test (value integer)")
            connection.executemany(
                "insert into values_under_test values (?)", [(2,), (3,), (5,)])
            result = connection.execute(
                "select sum(value) from values_under_test").fetchone()[0]
        assert result == 10


def test_clocks():
    wall_before = time.time()
    monotonic_before = time.monotonic()
    time.sleep(0.02)
    assert time.time() >= wall_before
    assert time.monotonic() > monotonic_before


def test_sockets():
    tcp_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp_server.bind(("127.0.0.1", 0))
    tcp_server.listen(1)
    port = tcp_server.getsockname()[1]

    def serve_tcp():
        connection, _ = tcp_server.accept()
        with connection:
            assert connection.recv(4) == b"ping"
            connection.sendall(b"pong")

    server_thread = threading.Thread(target=serve_tcp)
    server_thread.start()
    with socket.create_connection(("127.0.0.1", port), timeout=2) as client:
        client.sendall(b"ping")
        assert client.recv(4) == b"pong"
    server_thread.join(2)
    tcp_server.close()
    assert not server_thread.is_alive()

    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind(("127.0.0.1", 0))
    receiver.settimeout(2)
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sender.sendto(b"udp", receiver.getsockname())
    assert receiver.recvfrom(16)[0] == b"udp"
    sender.close()
    receiver.close()


def test_ssl():
    context = ssl.create_default_context()
    with socket.create_connection(("dl-cdn.alpinelinux.org", 443), timeout=10) as raw:
        with context.wrap_socket(raw,
                server_hostname="dl-cdn.alpinelinux.org") as secured:
            assert secured.version()
            assert secured.getpeercert()["subject"]


def test_subprocesses():
    echo = subprocess.run(
        ["/bin/echo", "python-subprocess"], check=True,
        capture_output=True, text=True)
    assert echo.stdout == "python-subprocess\n"
    child = subprocess.run(
        ["/usr/bin/python3", "-c", "print(6 * 7)"], check=True,
        capture_output=True, text=True)
    assert child.stdout == "42\n"


def test_signals():
    received = []

    def handler(number, _frame):
        received.append(number)

    previous = signal.signal(signal.SIGUSR1, handler)
    try:
        os.kill(os.getpid(), signal.SIGUSR1)
        deadline = time.monotonic() + 2
        while not received and time.monotonic() < deadline:
            time.sleep(0.01)
        assert received == [signal.SIGUSR1]
    finally:
        signal.signal(signal.SIGUSR1, previous)


def test_threads():
    condition = threading.Condition()
    ready = 0
    total = 0

    def worker():
        nonlocal ready, total
        with condition:
            ready += 1
            condition.notify_all()
            condition.wait_for(lambda: ready == 4)
        for _ in range(250):
            with condition:
                total += 1

    threads = [threading.Thread(target=worker) for _ in range(4)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(5)
    assert all(not thread.is_alive() for thread in threads)
    assert total == 1000
    assert all(0 < thread.native_id <= 0x3fffffff for thread in threads)


def test_unsupported_errors():
    if not hasattr(select, "epoll"):
        return
    try:
        poller = select.epoll()
    except OSError as error:
        assert error.errno == errno.ENOSYS
    else:
        poller.close()
        raise AssertionError("unsupported epoll unexpectedly succeeded")


check("imports_files", test_imports_and_files)
check("sqlite", test_sqlite)
check("clocks", test_clocks)
check("sockets", test_sockets)
check("ssl", test_ssl)
check("subprocesses", test_subprocesses)
check("signals", test_signals)
check("threads", test_threads)
check("unsupported_errors", test_unsupported_errors)
print("python_runtime_smoke_failures=0", flush=True)
