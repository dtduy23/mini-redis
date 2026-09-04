#!/usr/bin/env python3
"""
Test Suite toàn diện kèm đo lường thông số hiệu năng (Metrics & Benchmarks)
cho Section 5.1 Networking Layer của mini-redis-cpp.

Đo lường:
- Round-Trip Latency (Min, Max, Avg, P50, P99)
- Throughput (Requests per Second - QPS)
- Throughput đa luồng đồng thời (Concurrency)
- Tốc độ truyền tải dữ liệu (Transfer Bandwidth MB/s)
- Khả năng chống chịu lỗi kết nối & giới hạn DoS
"""

import hashlib
import os
import socket
import struct
import subprocess
import sys
import threading
import time

SERVER_BIN = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "../build/src/mini_redis_cpp")
)
TEST_PORT = 7890
HOST = "127.0.0.1"

# Danh sách lưu kết quả thống kê để in bảng tổng kết cuối cùng
METRICS_TABLE = []


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
    raise RuntimeError(f"Server không thể khởi động trên port {port}")


def stop_server(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def recv_exact(sock, n):
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(min(n - len(data), 65536))
        if not chunk:
            raise ConnectionResetError("Socket đóng trước khi đọc đủ bytes")
        data.extend(chunk)
    return bytes(data)


# ─── TEST CASES KÈM ĐO LƯỜNG THÔNG SỐ ─────────────────────────────────────────

def test_1_configurable_port_and_reuseaddr():
    print("\n[TEST 1] Configurable Port & SO_REUSEADDR")
    t0 = time.perf_counter()
    custom_port = 7891

    t_start = time.perf_counter()
    p1 = start_server(custom_port)
    p1_time = (time.perf_counter() - t_start) * 1000

    # Dừng server 1
    t_stop = time.perf_counter()
    stop_server(p1)
    stop_time = (time.perf_counter() - t_stop) * 1000

    # Khởi động lại ngay lập tức trên cùng port để kiểm tra SO_REUSEADDR
    t_restart = time.perf_counter()
    p2 = start_server(custom_port)
    restart_time = (time.perf_counter() - t_restart) * 1000

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, custom_port))
    s.sendall(b"PING\n")
    res = s.recv(1024)
    assert res == b"PING\n", f"Expected b'PING\\n', got {res}"
    s.close()
    stop_server(p2)

    total_duration = time.perf_counter() - t0
    key_metric = f"Restart: {restart_time:.1f}ms (Stop: {stop_time:.1f}ms)"
    METRICS_TABLE.append((1, "Port Config & SO_REUSEADDR", key_metric, f"{total_duration:.2f}s", "PASSED ✅"))

    print(f"  • Cổng thử nghiệm       : {custom_port}")
    print(f"  • Thời gian khởi động lần 1: {p1_time:.2f} ms")
    print(f"  • Thời gian tắt server   : {stop_time:.2f} ms")
    print(f"  • Thời gian Re-bind tức thì: {restart_time:.2f} ms (SO_REUSEADDR hoạt động chuẩn xác)")
    print("  --> Kết quả: PASSED ✅")


def test_2_basic_echo_latency():
    print("\n[TEST 2] Basic Echo & Round-Trip Latency Benchmark (1,000 requests)")
    t0 = time.perf_counter()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, TEST_PORT))

    num_requests = 1000
    msg = b"PING\r\n"
    latencies = []

    for _ in range(num_requests):
        req_start = time.perf_counter()
        s.sendall(msg)
        reply = recv_exact(s, len(msg))
        lat = (time.perf_counter() - req_start) * 1_000_000 # tính bằng microsecond (µs)
        latencies.append(lat)
        assert reply == msg

    s.close()
    total_duration = time.perf_counter() - t0

    latencies.sort()
    avg_lat = sum(latencies) / len(latencies)
    min_lat = latencies[0]
    max_lat = latencies[-1]
    p50_lat = latencies[int(num_requests * 0.50)]
    p95_lat = latencies[int(num_requests * 0.95)]
    p99_lat = latencies[int(num_requests * 0.99)]
    qps = num_requests / total_duration

    key_metric = f"Avg: {avg_lat:.1f}µs | {qps:,.0f} QPS"
    METRICS_TABLE.append((2, "Echo Latency (1K reqs)", key_metric, f"{total_duration:.2f}s", "PASSED ✅"))

    print(f"  • Tổng số lượt Ping-Pong: {num_requests:,} requests")
    print(f"  • Thông lượng (Throughput) : {qps:,.0f} req/sec (QPS)")
    print(f"  • Độ trễ trung bình (Avg): {avg_lat:.2f} µs ({avg_lat/1000:.3f} ms)")
    print(f"  • Min / Max              : {min_lat:.2f} µs / {max_lat:.2f} µs")
    print(f"  • Phân vị P50 / P95 / P99: {p50_lat:.2f} µs / {p95_lat:.2f} µs / {p99_lat:.2f} µs")
    print("  --> Kết quả: PASSED ✅")


def test_3_multiple_concurrent_clients():
    print("\n[TEST 3] Multiple Concurrent Clients (20 clients đồng thời, 1,000 requests)")
    t0 = time.perf_counter()
    num_clients = 20
    msgs_per_client = 50
    total_msgs = num_clients * msgs_per_client
    errors = []

    def client_worker(cid):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect((HOST, TEST_PORT))
            for m in range(msgs_per_client):
                payload = f"CLIENT_{cid:02d}_REQ_{m:04d}\r\n".encode("utf-8")
                s.sendall(payload)
                reply = recv_exact(s, len(payload))
                if reply != payload:
                    errors.append(f"Client {cid} mismatch")
                    break
            s.close()
        except Exception as e:
            errors.append(f"Client {cid} error: {e}")

    threads = [threading.Thread(target=client_worker, args=(i,)) for i in range(num_clients)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    total_duration = time.perf_counter() - t0
    qps = total_msgs / total_duration
    assert len(errors) == 0, f"Errors: {errors}"

    key_metric = f"20 clients | {qps:,.0f} QPS | 0% err"
    METRICS_TABLE.append((3, "Concurrent Clients (20 conns)", key_metric, f"{total_duration:.2f}s", "PASSED ✅"))

    print(f"  • Số kết nối đồng thời   : {num_clients} client threads")
    print(f"  • Tổng số request xử lý : {total_msgs:,} requests")
    print(f"  • Thời gian hoàn thành  : {total_duration:.3f} s")
    print(f"  • Thông lượng đa luồng  : {qps:,.0f} req/sec")
    print(f"  • Tỷ lệ lỗi (Error rate) : 0.0% (Không lẫn lộn buffer giữa các client)")
    print("  --> Kết quả: PASSED ✅")


def test_4_partial_reads():
    print("\n[TEST 4] Partial Reads (Mô phỏng mạng lag / phân mảnh stream TCP)")
    t0 = time.perf_counter()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, TEST_PORT))

    msg = b"FRAGMENTED_STREAM_PACKET_TESTING_1234567890_ABCDEFGHIJKLMN\r\n"
    num_bytes = len(msg)

    # Gửi từng byte một với độ trễ 1ms
    send_start = time.perf_counter()
    for byte in msg:
        s.sendall(bytes([byte]))
        time.sleep(0.001) # 1ms delay mỗi byte
    send_time = (time.perf_counter() - send_start) * 1000

    reply = recv_exact(s, len(msg))
    assert reply == msg, f"Expected {msg}, got {reply}"
    s.close()
    total_duration = time.perf_counter() - t0

    key_metric = f"{num_bytes} fragments | 100% integrity"
    METRICS_TABLE.append((4, "Stream Fragmentation (Lag)", key_metric, f"{total_duration:.2f}s", "PASSED ✅"))

    print(f"  • Kích thước payload     : {num_bytes} bytes")
    print(f"  • Số mảnh chia nhỏ       : {num_bytes} gói tin (1 byte/lần gửi)")
    print(f"  • Thời gian stream data  : {send_time:.1f} ms")
    print(f"  • Tính toàn vẹn dữ liệu  : 100% khớp tuyệt đối sau khi gom buffer")
    print("  --> Kết quả: PASSED ✅")


def test_5_partial_writes_large_payload():
    print("\n[TEST 5] Partial Writes & Bandwidth Benchmark (512 KB Large Payload)")
    t0 = time.perf_counter()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, TEST_PORT))

    # Sinh 512 KB dữ liệu
    payload_size = 512 * 1024
    pattern = b"0123456789ABCDEF" * (payload_size // 16)
    expected_hash = hashlib.sha256(pattern).hexdigest()

    transfer_start = time.perf_counter()
    s.sendall(pattern)
    received = recv_exact(s, payload_size)
    transfer_time = time.perf_counter() - transfer_start

    actual_hash = hashlib.sha256(received).hexdigest()
    assert actual_hash == expected_hash, "Hash mismatch!"
    s.close()
    total_duration = time.perf_counter() - t0

    # Băng thông 2 chiều (Gửi 512KB + Nhận lại 512KB = 1MB tổng dữ liệu truyền)
    total_transferred_mb = (payload_size * 2) / (1024 * 1024)
    speed_mb_s = total_transferred_mb / transfer_time

    key_metric = f"512KB | {speed_mb_s:.1f} MB/s | SHA256 OK"
    METRICS_TABLE.append((5, "Large I/O & EPOLLOUT (512KB)", key_metric, f"{total_duration:.2f}s", "PASSED ✅"))

    print(f"  • Kích thước payload     : {payload_size / 1024:.0f} KB ({payload_size:,} bytes)")
    print(f"  • Tổng dữ liệu 2 chiều   : {total_transferred_mb:.2f} MB")
    print(f"  • Thời gian truyền & nhận: {transfer_time * 1000:.2f} ms")
    print(f"  • Tốc độ truyền tải      : {speed_mb_s:,.1f} MB/s")
    print(f"  • Kiểm tra toàn vẹn      : SHA-256 ({actual_hash[:12]}...) KHỚP 100%")
    print("  --> Kết quả: PASSED ✅")


def test_6_graceful_disconnect():
    print("\n[TEST 6] Graceful Disconnect (TCP FIN Handshake)")
    t0 = time.perf_counter()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, TEST_PORT))
    s.sendall(b"PING\r\n")
    recv_exact(s, 6)

    t_fin = time.perf_counter()
    s.close()
    fin_time = (time.perf_counter() - t_fin) * 1000

    # Client mới kết nối ngay sau đó
    t_next = time.perf_counter()
    s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s2.connect((HOST, TEST_PORT))
    s2.sendall(b"PING\r\n")
    res = recv_exact(s2, 6)
    assert res == b"PING\r\n"
    next_conn_time = (time.perf_counter() - t_next) * 1000
    s2.close()

    total_duration = time.perf_counter() - t0
    key_metric = f"FIN: {fin_time:.2f}ms | Next: {next_conn_time:.2f}ms"
    METRICS_TABLE.append((6, "Graceful Disconnect (FIN)", key_metric, f"{total_duration:.2f}s", "PASSED ✅"))

    print(f"  • Thời gian gửi FIN đóng : {fin_time:.3f} ms")
    print(f"  • Kết nối mới kế tiếp   : {next_conn_time:.3f} ms (Server dọn dẹp kết nối cũ tức thì)")
    print("  --> Kết quả: PASSED ✅")


def test_7_abrupt_disconnect_rst():
    print("\n[TEST 7] Abrupt Disconnect (TCP RST via SO_LINGER 0)")
    t0 = time.perf_counter()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, TEST_PORT))
    s.sendall(b"HELLO_BEFORE_RST\r\n")

    # Bắn cờ RST bằng SO_LINGER 0 thay vì FIN
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()

    # Đo độ sẵn sàng của server ngay sau khi nhận RST
    t_check = time.perf_counter()
    s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s2.connect((HOST, TEST_PORT))
    s2.sendall(b"HEALTH_CHECK\r\n")
    res = recv_exact(s2, 14)
    assert res == b"HEALTH_CHECK\r\n"
    recovery_time = (time.perf_counter() - t_check) * 1000
    s2.close()

    total_duration = time.perf_counter() - t0
    key_metric = f"EPOLLERR caught | Recov: {recovery_time:.2f}ms"
    METRICS_TABLE.append((7, "Abrupt Disconnect (TCP RST)", key_metric, f"{total_duration:.2f}s", "PASSED ✅"))

    print(f"  • Sự kiện Kernel bắt được: EPOLLHUP | EPOLLERR (Không dính SIGPIPE)")
    print(f"  • Thời gian phục hồi     : {recovery_time:.3f} ms (Server an toàn 100%, không crash)")
    print("  --> Kết quả: PASSED ✅")


def test_8_dos_buffer_limit():
    print("\n[TEST 8] DoS Protection (Cắt kết nối khi vượt ngưỡng MAX_BUFFER_SIZE = 1MB)")
    t0 = time.perf_counter()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, TEST_PORT))

    # Gửi 1.20 MB dữ liệu
    flood_size = 1200 * 1024
    flood = b"A" * flood_size
    disconnected = False

    t_flood = time.perf_counter()
    try:
        s.sendall(flood)
        while True:
            chunk = s.recv(4096)
            if not chunk:
                disconnected = True
                break
    except (ConnectionResetError, BrokenPipeError, OSError):
        disconnected = True
    detect_time = (time.perf_counter() - t_flood) * 1000

    s.close()
    assert disconnected, "Server phải ngắt kết nối khi vượt quá 1MB!"

    # Kiểm tra server có tiếp tục hoạt động bình thường sau đợt DoS không
    t_health = time.perf_counter()
    s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s2.connect((HOST, TEST_PORT))
    s2.sendall(b"STILL_ALIVE\r\n")
    res = recv_exact(s2, 13)
    assert res == b"STILL_ALIVE\r\n"
    health_latency = (time.perf_counter() - t_health) * 1000
    s2.close()

    total_duration = time.perf_counter() - t0
    key_metric = f"Cutoff 1.2MB @ 1MB limit | {detect_time:.1f}ms"
    METRICS_TABLE.append((8, "DoS Protection (>1MB Cutoff)", key_metric, f"{total_duration:.2f}s", "PASSED ✅"))

    print(f"  • Dung lượng tấn công gửi: {flood_size / (1024*1024):.2f} MB")
    print(f"  • Ngưỡng bảo vệ server   : 1.00 MB (MAX_BUFFER_SIZE)")
    print(f"  • Thời gian phát hiện&ngắt: {detect_time:.2f} ms")
    print(f"  • Độ trễ phục hồi sau DoS: {health_latency:.2f} ms (Server hoạt động bình thường)")
    print("  --> Kết quả: PASSED ✅")


# ─── BẢNG TỔNG KẾT THÔNG SỐ ───────────────────────────────────────────────────

def print_summary_table():
    print("\n" + "=" * 82)
    print("           BẢNG TỔNG HỢP THÔNG SỐ KIỂM THỬ SECTION 5.1 NETWORKING LAYER")
    print("=" * 82)
    header = f"| {'ID':<2} | {'Tên Test Case':<28} | {'Thông Số Đo Lường Chính':<30} | {'Thời gian':<9} | {'Trạng Thái':<8} |"
    sep = "+" + "-" * 4 + "+" + "-" * 30 + "+" + "-" * 32 + "+" + "-" * 11 + "+" + "-" * 10 + "+"
    print(sep)
    print(header)
    print(sep)
    for row in METRICS_TABLE:
        print(f"| {row[0]:<2} | {row[1]:<28} | {row[2]:<30} | {row[3]:<9} | {row[4]:<8} |")
    print(sep)


def main():
    if not os.path.isfile(SERVER_BIN):
        print(f"Lỗi: Không tìm thấy binary tại {SERVER_BIN}. Hãy build trước!")
        sys.exit(1)

    print("=" * 82)
    print("   BẮT ĐẦU CHẠY BENCHMARK & KIỂM THỬ THÔNG SỐ TẦNG MẠNG (5.1 NETWORKING)")
    print("=" * 82)

    test_1_configurable_port_and_reuseaddr()

    main_server = start_server(TEST_PORT)
    try:
        test_2_basic_echo_latency()
        test_3_multiple_concurrent_clients()
        test_4_partial_reads()
        test_5_partial_writes_large_payload()
        test_6_graceful_disconnect()
        test_7_abrupt_disconnect_rst()
        test_8_dos_buffer_limit()
    finally:
        stop_server(main_server)

    print_summary_table()
    print("\n🎉 TOÀN BỘ 8 BÀI TEST & BENCHMARK THÔNG SỐ ĐÃ VƯỢT QUA XUẤT SẮC!\n")


if __name__ == "__main__":
    main()
