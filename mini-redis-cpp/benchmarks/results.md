# Performance Benchmark Results

This document presents benchmark results for **mini-redis-cpp**, an event-driven in-memory key-value store built with C++20 and POSIX `epoll`.

## Test Environment

- **OS**: Linux (Kernel 6.x, x86_64)
- **Compiler**: GCC with `-std=c++20 -O3` (Zero warnings under `-Wall -Wextra -Werror`)
- **Concurrency Model**: Single-threaded Event Loop using `epoll` level-triggered (`EPOLLIN` + on-demand `EPOLLOUT`)
- **Load Generator**: `mini_redis_benchmark` (native multi-threaded TCP load generator simulating 50 concurrent client connections with `TCP_NODELAY`)
- **Dataset / Payload**: 100,000 requests per test case across 10,000 randomized keys.

---

## Benchmark Summary

| Command | Total Requests | Elapsed (s) | Throughput (RPS) | Min Latency | Avg Latency | p50 (Median) | p95 Latency | p99 Latency |
|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **PING** | 100,000 | 0.800 s | **124,959.51** | 0.018 ms | 0.390 ms | 0.383 ms | 0.442 ms | **0.772 ms** |
| **SET**  | 100,000 | 0.940 s | **106,330.77** | 0.016 ms | 0.456 ms | 0.438 ms | 0.616 ms | **0.883 ms** |
| **GET**  | 100,000 | 0.968 s | **103,354.44** | 0.017 ms | 0.470 ms | 0.460 ms | 0.557 ms | **0.927 ms** |
| **INCR** | 100,000 | 0.936 s | **106,886.39** | 0.019 ms | 0.456 ms | 0.438 ms | 0.536 ms | **0.897 ms** |

---

## Detailed Percentile Breakdown

### 1. PING (RESP Inline/Bulk)
```text
====== PING (inline/RESP) ======
  100,000 requests completed in 0.800 seconds
  Throughput: 124,959.51 requests per second (RPS)
  Latency percentiles:
    min: 0.018 ms
    avg: 0.390 ms
    p50: 0.383 ms
    p95: 0.442 ms
    p99: 0.772 ms
    max: 2.724 ms
```

### 2. SET (Key-Value Writes)
```text
====== SET (key-value store) ======
  100,000 requests completed in 0.940 seconds
  Throughput: 106,330.77 requests per second (RPS)
  Latency percentiles:
    min: 0.016 ms
    avg: 0.456 ms
    p50: 0.438 ms
    p95: 0.616 ms
    p99: 0.883 ms
    max: 3.769 ms
```

### 3. GET (Key-Value Reads)
```text
====== GET (cache hit/read) ======
  100,000 requests completed in 0.968 seconds
  Throughput: 103,354.44 requests per second (RPS)
  Latency percentiles:
    min: 0.017 ms
    avg: 0.470 ms
    p50: 0.460 ms
    p95: 0.557 ms
    p99: 0.927 ms
    max: 1.481 ms
```

### 4. INCR (Atomic Integer Counter)
```text
====== INCR (atomic counter) ======
  100,000 requests completed in 0.936 seconds
  Throughput: 106,886.39 requests per second (RPS)
  Latency percentiles:
    min: 0.019 ms
    avg: 0.456 ms
    p50: 0.438 ms
    p95: 0.536 ms
    p99: 0.897 ms
    max: 3.131 ms
```

---

## Architectural Key Takeaways

1. **Sub-millisecond Latency**: 99% of all requests complete under **1.0 ms** across all command types.
2. **Zero Context Switching**: Single-threaded event loop eliminates thread synchronization primitives, locks, mutex contention, and false sharing.
3. **Optimized I/O Memory Management**: Amortized buffer compaction prevents repeated reallocations, while `TCP_NODELAY` avoids Nagle delay penalties.
4. **Resilient Under Load**: Maintained stable throughput with zero connection drops across 50 concurrent client connections.
