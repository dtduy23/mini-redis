#!/usr/bin/env python3
"""Regression tests for EOF handling, bounded buffers, bind and timer settings."""

import argparse
import contextlib
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest


def encode(*args):
    args = [arg.encode() if isinstance(arg, str) else arg for arg in args]
    return b"*" + str(len(args)).encode() + b"\r\n" + b"".join(
        b"$" + str(len(arg)).encode() + b"\r\n" + arg + b"\r\n" for arg in args
    )


def recv_exact(sock, size):
    result = bytearray()
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise EOFError("Connection closed before the response completed")
        result.extend(chunk)
    return bytes(result)


def recv_line(sock):
    result = bytearray()
    while not result.endswith(b"\r\n"):
        result.extend(recv_exact(sock, 1))
    return bytes(result)


def reply(sock):
    line = recv_line(sock)
    if line.startswith(b"$"):
        size = int(line[1:-2])
        if size == -1:
            return None
        payload = recv_exact(sock, size)
        if recv_exact(sock, 2) != b"\r\n":
            raise AssertionError("Invalid bulk response terminator")
        return payload
    return line


def command(sock, *args):
    sock.sendall(encode(*args))
    return reply(sock)


def rss_kib(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    raise AssertionError("Server RSS unavailable")


@contextlib.contextmanager
def paused(proc):
    # Queue both request bytes and FIN before the reactor can read either.
    os.kill(proc.pid, signal.SIGSTOP)
    try:
        deadline = time.monotonic() + 2
        while "\nState:\tT" not in Path(f"/proc/{proc.pid}/status").read_text():
            if time.monotonic() > deadline:
                raise AssertionError("Server did not pause")
            time.sleep(0.005)
        yield
    finally:
        os.kill(proc.pid, signal.SIGCONT)


class Server:
    def __init__(self, **overrides):
        self.folder = tempfile.TemporaryDirectory(prefix="mini-redis-regression-")
        self.path = Path(self.folder.name)
        self.log = tempfile.TemporaryFile()
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        config = {
            "bind": "127.0.0.1", "port": self.port, "timeout": 0, "loglevel": "error",
            "max-buffer-size": 1048576, "active-expire-interval-ms": 100,
            "compact-threshold": 32768, "shrink-threshold": 65536,
        }
        config.update(overrides)
        self.config = self.path / "mini-redis.conf"
        self.config.write_text("".join(f"{key} {value}\n" for key, value in config.items()))
        self.proc = None

    def start(self, wait=True):
        self.proc = subprocess.Popen(
            [SERVER_BIN, "--config", str(self.config)], cwd=self.path,
            stdout=self.log, stderr=self.log,
        )
        if wait:
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if self.proc.poll() is not None:
                    raise AssertionError("Server failed to start: " + self.logs())
                try:
                    with self.connect():
                        return self
                except OSError:
                    time.sleep(0.01)
            raise AssertionError("Server startup timeout: " + self.logs())
        return self

    def connect(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=5)

    def logs(self):
        self.log.seek(0)
        return self.log.read().decode(errors="replace")

    def close(self):
        if self.proc is not None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.log.close()
        self.folder.cleanup()


class ServerRegressions(unittest.TestCase):
    def server(self, **overrides):
        server = Server(**overrides)
        self.addCleanup(server.close)
        return server

    def test_half_close_executes_all_complete_commands(self):
        server = self.server().start()
        with server.connect() as sock:
            with paused(server.proc):
                sock.sendall(encode("SET", "k", "value") + encode("GET", "k"))
                sock.shutdown(socket.SHUT_WR)
            self.assertEqual(reply(sock), b"+OK\r\n")
            self.assertEqual(reply(sock), b"value")
            self.assertEqual(sock.recv(1), b"")
        with server.connect() as sock:
            self.assertEqual(command(sock, "GET", "k"), b"value")

    def test_half_close_drains_pending_output(self):
        server = self.server().start()
        value = b"x" * (256 * 1024)
        with server.connect() as sock:
            self.assertEqual(command(sock, "SET", "large", value), b"+OK\r\n")
            with paused(server.proc):
                sock.sendall(encode("GET", "large") * 32)
                sock.shutdown(socket.SHUT_WR)
            time.sleep(0.1)
            for _ in range(32):
                self.assertEqual(reply(sock), value)
            self.assertEqual(sock.recv(1), b"")

    def test_slow_reader_has_bounded_memory_and_keeps_pipeline(self):
        server = self.server().start()
        value = b"x" * (256 * 1024)
        with server.connect() as sock:
            self.assertEqual(command(sock, "SET", "large", value), b"+OK\r\n")
            before = rss_kib(server.proc.pid)
            sock.sendall(encode("GET", "large") * 128)
            time.sleep(0.2)
            growth = rss_kib(server.proc.pid) - before
            self.assertLess(growth, 16 * 1024, f"Slow reader retained {growth} KiB")
            with server.connect() as other:
                self.assertEqual(command(other, "PING"), b"+PONG\r\n")
            for _ in range(128):
                self.assertEqual(reply(sock), value)
            self.assertEqual(command(sock, "PING"), b"+PONG\r\n")

    def test_fragmented_arguments_cannot_bypass_input_limit(self):
        server = self.server(**{"max-buffer-size": 4096}).start()
        with server.connect() as sock:
            sock.sendall(b"*64\r\n$4\r\nECHO\r\n")
            # Each fragment is below 4 KiB; vector slots + retained payload exceed it.
            part = b"$1024\r\n" + b"x" * 1024 + b"\r\n"
            for _ in range(2):
                try:
                    sock.sendall(part)
                except (BrokenPipeError, ConnectionResetError):
                    break
                time.sleep(0.02)
            result = bytearray()
            try:
                while chunk := sock.recv(4096):
                    result.extend(chunk)
            except ConnectionResetError:
                pass
            self.assertTrue(not result or result.startswith(b"-ERR "))
        with server.connect() as sock:
            self.assertEqual(command(sock, "PING"), b"+PONG\r\n")

    def test_large_array_header_is_rejected(self):
        server = self.server().start()
        with server.connect() as sock:
            sock.sendall(b"*1048576\r\n")
            self.assertIn(b"command exceeds memory limit", recv_line(sock))
            self.assertEqual(sock.recv(1), b"")

    def test_invalid_bind_fails_without_opening_listener(self):
        server = self.server(bind="127.0.0.1_typo").start(wait=False)
        self.assertEqual(server.proc.wait(timeout=3), 1)
        self.assertIn("Invalid IPv4 bind address", server.logs())
        with self.assertRaises(OSError):
            with socket.create_connection(("127.0.0.2", server.port), timeout=0.2):
                pass

    def test_timer_uses_configured_seconds_and_milliseconds(self):
        server = self.server(**{"active-expire-interval-ms": 1234}).start()
        for fd in Path(f"/proc/{server.proc.pid}/fd").iterdir():
            if os.readlink(fd) == "anon_inode:[timerfd]":
                info = Path(f"/proc/{server.proc.pid}/fdinfo/{fd.name}").read_text()
                self.assertIn("it_interval: (1, 234000000)", info)
                break
        else:
            self.fail("No timerfd found")

    def test_corrupt_snapshot_is_not_served(self):
        server = self.server()
        data = b"REDIS0009\x00" + struct.pack("=I", 3) + b"foo"
        data += struct.pack("=I", 3) + b"bar\xff"
        checksum = 14695981039346656037
        for byte in data:
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
        (server.path / "dump.rdb").write_bytes(data + struct.pack("=Q", checksum ^ 1))
        server.start()
        with server.connect() as sock:
            self.assertEqual(command(sock, "GET", "foo"), None)
            self.assertEqual(command(sock, "PING"), b"+PONG\r\n")

    def test_snapshot_value_larger_than_output_limit_is_rejected(self):
        server = self.server(**{"max-buffer-size": 4096})
        value = b"x" * 65536
        data = b"REDIS0009\x00" + struct.pack("=I", 3) + b"foo"
        data += struct.pack("=I", len(value)) + value + b"\xff"
        checksum = 14695981039346656037
        for byte in data:
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
        (server.path / "dump.rdb").write_bytes(data + struct.pack("=Q", checksum))
        server.start()
        with server.connect() as sock:
            self.assertEqual(command(sock, "GET", "foo"), b"-ERR response exceeds output buffer limit\r\n")
            self.assertEqual(command(sock, "PING"), b"+PONG\r\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    args, remaining = parser.parse_known_args()
    SERVER_BIN = str(Path(args.server).resolve())
    unittest.main(argv=[__file__, *remaining], verbosity=2)
