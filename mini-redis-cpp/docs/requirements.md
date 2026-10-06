# Project Requirements & Documentation Index

Tài liệu yêu cầu kỹ thuật và chỉ mục tài liệu hệ thống của **mini-redis-cpp**.

## 1. Documentation Index (Tài Liệu Kỹ Thuật Dự Án)

Toàn bộ các tài liệu phân tích kỹ thuật chuyên sâu và luyện thi phỏng vấn được lưu trữ tại thư mục `docs/`:

- 📘 **[Technical Deep Dive (Chuyên Khảo Kiến Trúc & Kỹ Thuật Hệ Thống)](technical_deep_dive.md)**:
  Phân tích chi tiết toàn diện 8 tầng kiến trúc của dự án:
  1. Kiến trúc tổng thể & Triết lý Single-Threaded Reactor + BIO Workers (Zero dependencies).
  2. Tầng mạng & POSIX Non-blocking I/O (`epoll` Level-Triggered, On-Demand `EPOLLOUT`, `TCP_NODELAY`, `SO_REUSEADDR`).
  3. Giao thức Wire Protocol RESP2 & Máy trạng thái hữu hạn tiền định (DFA Streaming Parser, Pipelining, Elastic Compaction).
  4. Tầng lưu trữ bảng băm `std::unordered_map` Heterogeneous Lookup C++20 (`is_transparent`), Node Extraction $\mathcal{O}(1)$ (`store_.extract()`), Background I/O thread pool (`std::jthread`, `std::condition_variable`).
  5. Động cơ Expiry kép (Dual TTL Engine: $\mathcal{O}(1)$ Lazy eviction + 10Hz Active probabilistic sweep qua POSIX `timerfd`, ngưỡng 25%, Circuit Breaker 25ms).
  6. Tầng lưu trữ Snapshot RDB (Header magic `REDIS0009`, Checksum FNV-1a 64-bit, Linux `fork()` Copy-On-Write COW, `waitpid(WNOHANG)`, Atomic Rename).
  7. Hệ thống cấu hình tự phục hồi Self-Healing Configuration (4 trạng thái vòng đời, tự sinh file, hot repair, backup `.bak`).
  8. Production Hardening, An toàn bộ nhớ (RAII, zero leaks, ASan, UBSan, Valgrind, Benchmark đối đầu Redis 7.4.9).

- 🎯 **[Technical Interview Guide (Cẩm Nang Phỏng Vấn Kỹ Thuật Cấp Cao)](technical_interview_guide.md)**:
  Bộ 36 câu hỏi và câu trả lời chuyên sâu phân tích tận gốc tầng kernel, CPU cache, memory management và system design cho các vị trí Senior / Staff Systems Engineer.

- 🚀 **[Trade-offs, Edge Cases & Extension Blueprints (Báo Cáo Chuyên Khảo Staff Engineer)](tradeoffs_edgecases_extensions.md)**:
  Báo cáo chuyên khảo Staff/Principal giải quyết 3 trục trọng yếu:
  1. Đánh đổi kiến trúc & giải pháp thay thế (`epoll` vs `io_uring`, C++20 vs Rust/Go/C99, Reactor vs Thread-per-core, `timerfd` vs Timing Wheel, COW `fork()` vs Thread versioning...).
  2. Bẫy góc cạnh & hành vi bất thường của HĐH Linux (NTP Clock Slew/Jump, BGSAVE child crash/OOM killer, Exception `std::bad_alloc`, TCP `RST` & `SIGPIPE`, Slowloris / Memory Bomb, Pipelined `UNLINK` concurrency, POSIX `fork()` multithreaded semantics).
  3. Bản thiết kế kỹ thuật tiến hóa tính năng (AOF & `BGREWRITEAOF`, Threaded I/O Redis 6.0, Pub/Sub, Transactions `MULTI/EXEC/WATCH`, `maxmemory` + Approximated LRU/LFU, Master-Replica Replication `PSYNC`, Linux `io_uring` Zero-Syscall).

---

## 2. Core Functional Requirements

- **Network Layer**:
  - Socket TCP non-blocking (`O_NONBLOCK`).
  - Event loop hướng sự kiện dùng Linux `epoll` Level-Triggered.
  - On-Demand `EPOLLOUT` registration chống busy loop.
  - Tích hợp POSIX `timerfd` (100ms nhịp đập) cho background maintenance.
  - Idle Client Connection Timeout (300s mặc định).
- **Wire Protocol**:
  - Giao thức chuẩn RESP2 (REdis Serialization Protocol).
  - Streaming DFA State Machine: an toàn nhị phân (binary-safe), hỗ trợ stream phân mảnh TCP và pipelining.
  - Bộ đệm co giãn Elastic Buffer với Amortized Compaction và Auto Shrink RAM.
  - DoS buffer limit (1MB) và protocol limits.
- **Storage & In-Memory Data Structures**:
  - Bảng băm key-value với C++20 Heterogeneous Lookup (`is_transparent`).
  - Hỗ trợ các lệnh: `PING`, `ECHO`, `SET`, `GET`, `DEL`, `UNLINK`, `EXISTS`, `INCR`, `TYPE`, `FLUSHALL`.
  - Asynchronous Lazy Free (`UNLINK`) trích xuất node $\mathcal{O}(1)$ chuyển giao cho Background Threads (`BioManager`).
- **TTL & Expiry**:
  - Các lệnh: `EXPIRE`, `TTL`, `PERSIST`.
  - Cơ chế Dual Eviction: Lazy Eviction khi truy cập + Active Probabilistic Sweep 10Hz lấy mẫu ngẫu nhiên (20 keys, ngưỡng 25%, circuit breaker 25ms).
- **Persistence Layer**:
  - Snapshot đồng bộ (`SAVE`) và bất đồng bộ (`BGSAVE`).
  - Linux `fork()` với cơ chế Copy-On-Write (COW).
  - Thu hoạch tiến trình con không block qua `waitpid(WNOHANG)`.
  - Checksum toàn vẹn FNV-1a 64-bit và ghi đĩa nguyên tử (Atomic file rename).
- **Configuration & Resilience**:
  - Quản lý cấu hình tự động nhận diện và tự sửa lỗi (Self-Healing).
  - Tắt server an toàn (Graceful Shutdown) tuân thủ async-signal-safe.
