#!/usr/bin/env python3
"""
E2E Integration Test Suite for mini-redis-cpp (Section 5.3 & Section 5.4)
Kiểm tra server thực tế chạy qua POSIX socket TCP với giao thức RESP2:
- Core commands (PING, ECHO, SET, GET, DEL, EXISTS, INCR, TYPE, FLUSHALL)
- Expiry & TTL commands (EXPIRE, TTL, PERSIST)
- Active Expiry qua Linux timerfd (100ms cycle)
- Client Idle Timeout (ngắt kết nối client ngâm quá hạn)
- Pipeline & Concurrent clients
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
TIMEOUT_TEST_PORT = 7896
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


def start_server(port=TEST_PORT, idle_timeout=300):
    proc = subprocess.Popen(
        [SERVER_BIN, str(port), str(idle_timeout)],
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

        # 13. Section 5.4: EXPIRE & TTL
        print("\n--- Section 5.4: Expiry & TTL E2E Tests ---")
        send_and_expect(["SET", "ttl_key", "temp_val"], b"+OK\r\n", "SET ttl_key")
        send_and_expect(["TTL", "ttl_key"], b":-1\r\n", "TTL before EXPIRE -> :-1")

        send_and_expect(["EXPIRE", "ttl_key", "2"], b":1\r\n", "EXPIRE ttl_key 2 -> :1")
        # Kiểm tra TTL trả về số giây còn lại (1 hoặc 2)
        s.sendall(encode_resp_cmd("TTL", "ttl_key"))
        ttl_resp = recv_exact(s, 4)  # :1\r\n hoặc :2\r\n
        assert ttl_resp in (b":1\r\n", b":2\r\n"), f"Unexpected TTL response: {ttl_resp!r}"
        print(f"  [OK] TTL ttl_key returns {ttl_resp.decode().strip()}")

        print("  Waiting 2.1s for ttl_key to expire...")
        time.sleep(2.1)
        send_and_expect(["TTL", "ttl_key"], b":-2\r\n", "TTL after expiration -> :-2")
        send_and_expect(["GET", "ttl_key"], b"$-1\r\n", "GET after expiration -> $-1 (lazy eviction)")

        # 14. Section 5.4: PERSIST
        send_and_expect(["SET", "persist_key", "forever"], b"+OK\r\n", "SET persist_key")
        send_and_expect(["EXPIRE", "persist_key", "100"], b":1\r\n", "EXPIRE persist_key 100")
        send_and_expect(["PERSIST", "persist_key"], b":1\r\n", "PERSIST persist_key -> :1")
        send_and_expect(["TTL", "persist_key"], b":-1\r\n", "TTL after PERSIST -> :-1")
        send_and_expect(["PERSIST", "persist_key"], b":0\r\n", "PERSIST again -> :0")
        send_and_expect(["GET", "persist_key"], b"$7\r\nforever\r\n", "GET persist_key -> forever")

        # 15. Section 5.4: EXPIRE 0 / negative
        send_and_expect(["SET", "del_now", "bye"], b"+OK\r\n", "SET del_now")
        send_and_expect(["EXPIRE", "del_now", "0"], b":1\r\n", "EXPIRE 0 -> :1")
        send_and_expect(["GET", "del_now"], b"$-1\r\n", "GET del_now -> $-1")

        # 16. Section 5.4: Active Expiry via timerfd
        print("  Testing Active Expiry background sweep (timerfd 100ms)...")
        for i in range(20):
            send_and_expect(["SET", f"sweep_{i}", "x"], b"+OK\r\n", f"SET sweep_{i}")
            send_and_expect(["EXPIRE", f"sweep_{i}", "1"], b":1\r\n", f"EXPIRE sweep_{i} 1")

        print("  Waiting 1.3s for timerfd background sweep...")
        time.sleep(1.3)
        # Các key đã được dọn bởi active_expire_cycle trong background timerfd
        send_and_expect(["EXISTS"] + [f"sweep_{i}" for i in range(20)], b":0\r\n", "Active expiry swept all 20 keys (EXISTS -> 0)")

        s.close()

        # 17. Multi-client concurrency test
        print("\n--- Concurrency Test (10 clients) ---")
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

    # 18. Section 5.4: Client Idle Timeout Test (Port 7896, idle_timeout=1s)
    print("\n--- Testing Client Idle Timeout (idle_timeout = 1s on Port 7896) ---")
    timeout_server = start_server(TIMEOUT_TEST_PORT, idle_timeout=1)
    try:
        # Client 1: Ngâm kết nối (không gửi dữ liệu) -> Server phải tự động đá sau ~1s
        idle_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        idle_sock.connect((HOST, TIMEOUT_TEST_PORT))
        print("  Connected idle client, waiting 1.3s without sending data...")
        time.sleep(1.3)

        # Thử đọc từ socket: server đã close, recv() phải trả về b"" (EOF)
        idle_sock.settimeout(0.5)
        data = idle_sock.recv(1024)
        assert len(data) == 0, f"Expected EOF from idle client, got: {data!r}"
        idle_sock.close()
        print("  [OK] Idle client was disconnected by server after timeout")

        # Client 2: Client hoạt động liên tục (gửi PING đều đặn) -> Không bị đá!
        active_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        active_sock.connect((HOST, TIMEOUT_TEST_PORT))
        for p in range(4):
            time.sleep(0.3)
            active_sock.sendall(encode_resp_cmd("PING"))
            resp = recv_exact(active_sock, 7)
            assert resp == b"+PONG\r\n", f"Active ping {p} failed: {resp!r}"
        active_sock.close()
        print("  [OK] Active client kept alive without being disconnected")

    finally:
        stop_server(timeout_server)

    # 19. Configuration File System E2E Test
    print("\n--- Testing Configuration File System (Auto-generation & Custom Config) ---")
    conf_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "e2e_conf_tmp"))
    os.makedirs(conf_dir, exist_ok=True)
    auto_conf_path = os.path.join(conf_dir, "auto_gen.conf")
    if os.path.exists(auto_conf_path):
        os.remove(auto_conf_path)

    # Khởi động server với file config chưa tồn tại -> Server phải tự động sinh file
    print("  Testing first-run auto-generation of configuration file...")
    proc_auto = subprocess.Popen([SERVER_BIN, auto_conf_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.3)
    assert os.path.isfile(auto_conf_path), "Configuration file was auto-generated on first run"
    with open(auto_conf_path, "r") as f:
        conf_text = f.read()
    assert "port 6379" in conf_text, "Auto-generated config contains default port"
    assert "timeout 300" in conf_text, "Auto-generated config contains default timeout"
    stop_server(proc_auto)
    print("  [OK] First-run configuration file automatically created with complete directives")

    # Khởi động server với file config tùy biến cổng 7897
    custom_conf_path = os.path.join(conf_dir, "custom_e2e.conf")
    with open(custom_conf_path, "w") as f:
        f.write("bind 127.0.0.1\nport 7897\ntimeout 60\nloglevel debug\n")

    print("  Testing server startup with custom configuration file (Port 7897)...")
    proc_custom = subprocess.Popen([SERVER_BIN, custom_conf_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.3)
    s_conf = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s_conf.connect((HOST, 7897))
    s_conf.sendall(encode_resp_cmd("PING", "from_config"))
    conf_resp = recv_exact(s_conf, 18)
    assert conf_resp == b"$11\r\nfrom_config\r\n", f"Unexpected response from custom port: {conf_resp!r}"
    s_conf.close()
    stop_server(proc_custom)
    print("  [OK] Server loaded custom configuration successfully (Port 7897 responded)")

    # Dọn dẹp thư mục tạm
    for f in os.listdir(conf_dir):
        os.remove(os.path.join(conf_dir, f))
    os.rmdir(conf_dir)

    print("\n" + "=" * 60)
    print("  ALL E2E INTEGRATION TESTS PASSED SUCCESSFULLY!  ")
    print("=" * 60)


if __name__ == "__main__":
    main()
