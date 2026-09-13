#!/usr/bin/env python3
"""
E2E Integration Test Suite for mini-redis-cpp (Section 5.3: Data Store & Commands)
Kiểm tra server thực tế chạy qua POSIX socket TCP với giao thức RESP2.
"""

import os
import socket
import subprocess
import sys
import threading
import time

SERVER_BIN = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "../build/src/mini_redis_cpp")
)
TEST_PORT = 7895
HOST = "127.0.0.1"


def encode_resp_cmd(*args):
    """Mã hóa danh sách các chuỗi thành RESP Array of Bulk Strings"""
    res = f"*{len(args)}\r\n"
    for arg in args:
        encoded = arg.encode("utf-8") if isinstance(arg, str) else arg
        res += f"${len(encoded)}\r\n"
        res = res.encode("utf-8") + encoded + b"\r\n"
        res = res.decode("latin1")
    return res.encode("latin1")


def recv_exact(sock, n):
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(min(n - len(data), 65536))
        if not chunk:
            raise ConnectionResetError("Socket closed prematurely")
        data.extend(chunk)
    return bytes(data)


def start_server(port=TEST_PORT):
    proc = subprocess.Popen(
        [SERVER_BIN, str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    for _ in range(30):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect((HOST, port))
            s.close()
            return proc
        except ConnectionRefusedError:
            time.sleep(0.02)
    raise RuntimeError(f"Server failed to start on port {port}")


def stop_server(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def main():
    if not os.path.isfile(SERVER_BIN):
        print(f"Error: binary {SERVER_BIN} not found. Run cmake --build first.")
        sys.exit(1)

    print("=" * 60)
    print("  RUNNING E2E NETWORK INTEGRATION TESTS (PORT 7895)  ")
    print("=" * 60)

    server_proc = start_server(TEST_PORT)
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, TEST_PORT))

        def send_and_expect(cmd_args, expected_bytes, desc):
            req = encode_resp_cmd(*cmd_args)
            s.sendall(req)
            resp = recv_exact(s, len(expected_bytes))
            assert resp == expected_bytes, f"[{desc}] Expected {expected_bytes!r}, got {resp!r}"
            print(f"  [OK] {desc}")

        # 1. PING không tham số
        send_and_expect(["PING"], b"+PONG\r\n", "PING -> +PONG")

        # 2. PING có message
        send_and_expect(["PING", "hello"], b"$5\r\nhello\r\n", "PING hello -> $5\\r\\nhello\\r\\n")

        # 3. ECHO
        send_and_expect(["ECHO", "mini-redis"], b"$10\r\nmini-redis\r\n", "ECHO mini-redis")

        # 4. SET & GET
        send_and_expect(["SET", "mykey", "myval"], b"+OK\r\n", "SET mykey myval -> +OK")
        send_and_expect(["GET", "mykey"], b"$5\r\nmyval\r\n", "GET mykey -> $5\\r\\nmyval\\r\n")

        # 5. GET missing key
        send_and_expect(["GET", "nonexistent"], b"$-1\r\n", "GET nonexistent -> $-1\\r\\n")

        # 6. EXISTS
        send_and_expect(["EXISTS", "mykey", "nonexistent", "mykey"], b":2\r\n", "EXISTS multiple -> :2")

        # 7. INCR
        send_and_expect(["INCR", "counter"], b":1\r\n", "INCR new key -> :1")
        send_and_expect(["INCR", "counter"], b":2\r\n", "INCR existing key -> :2")

        # 8. TYPE
        send_and_expect(["TYPE", "mykey"], b"+string\r\n", "TYPE mykey -> +string")
        send_and_expect(["TYPE", "ghost"], b"+none\r\n", "TYPE ghost -> +none")

        # 9. DEL
        send_and_expect(["DEL", "mykey", "counter"], b":2\r\n", "DEL 2 keys -> :2")
        send_and_expect(["GET", "mykey"], b"$-1\r\n", "GET deleted key -> $-1")

        # 10. FLUSHALL
        send_and_expect(["SET", "k1", "v1"], b"+OK\r\n", "SET k1 v1")
        send_and_expect(["FLUSHALL"], b"+OK\r\n", "FLUSHALL -> +OK")
        send_and_expect(["GET", "k1"], b"$-1\r\n", "GET k1 after FLUSHALL -> $-1")

        # 11. Errors
        send_and_expect(["UNKNOWN_CMD"], b"-ERR unknown command 'UNKNOWN_CMD'\r\n", "Unknown command error")
        send_and_expect(["SET", "only_one_arg"], b"-ERR wrong number of arguments for 'set' command\r\n", "Wrong arity error")

        send_and_expect(["SET", "str_key", "not_a_num"], b"+OK\r\n", "SET non-int")
        send_and_expect(["INCR", "str_key"], b"-ERR value is not an integer or out of range\r\n", "INCR non-int error")

        # 12. Pipelining (gửi nhiều lệnh 1 lúc)
        pipeline_data = (
            encode_resp_cmd("SET", "p1", "v1") +
            encode_resp_cmd("SET", "p2", "v2") +
            encode_resp_cmd("GET", "p1") +
            encode_resp_cmd("GET", "p2")
        )
        expected_pipeline = b"+OK\r\n+OK\r\n$2\r\nv1\r\n$2\r\nv2\r\n"
        s.sendall(pipeline_data)
        pipe_resp = recv_exact(s, len(expected_pipeline))
        assert pipe_resp == expected_pipeline, f"Pipeline mismatch: {pipe_resp!r}"
        print("  [OK] Pipelined batch execution")

        s.close()

        # 13. Multi-client concurrency test
        print("  Running concurrent client test (10 clients)...")
        errors = []

        def worker(cid):
            try:
                ws = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                ws.connect((HOST, TEST_PORT))
                for i in range(50):
                    k = f"c_{cid}_k_{i}"
                    v = f"v_{i}"
                    ws.sendall(encode_resp_cmd("SET", k, v))
                    ok_resp = recv_exact(ws, 5)
                    if ok_resp != b"+OK\r\n":
                        errors.append(f"Client {cid} SET failed: {ok_resp}")
                        break
                    ws.sendall(encode_resp_cmd("GET", k))
                    expected_get = f"${len(v)}\r\n{v}\r\n".encode("utf-8")
                    get_resp = recv_exact(ws, len(expected_get))
                    if get_resp != expected_get:
                        errors.append(f"Client {cid} GET failed: {get_resp}")
                        break
                ws.close()
            except Exception as ex:
                errors.append(f"Client {cid} exception: {ex}")

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(10)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        assert len(errors) == 0, f"Concurrency errors: {errors}"
        print("  [OK] Concurrency test (10 clients, 500 SET + 500 GET) passed")

    finally:
        stop_server(server_proc)

    print("=" * 60)
    print("  ALL E2E INTEGRATION TESTS PASSED SUCCESSFULLY!  ")
    print("=" * 60)


if __name__ == "__main__":
    main()

