#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace mini_redis::benchmark {

struct BenchConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    int clients = 50;
    int total_requests = 100000;
};

struct BenchStats {
    std::string test_name;
    int total_requests = 0;
    double elapsed_sec = 0.0;
    double rps = 0.0;
    double lat_min_ms = 0.0;
    double lat_avg_ms = 0.0;
    double lat_p50_ms = 0.0;
    double lat_p95_ms = 0.0;
    double lat_p99_ms = 0.0;
    double lat_max_ms = 0.0;
};

// Đọc chính xác n bytes từ socket
bool read_exact(int fd, char* dest, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t s = ::recv(fd, dest + total, n - total, 0);
        if (s <= 0) return false;
        total += static_cast<size_t>(s);
    }
    return true;
}

// Đọc 1 dòng RESP kết thúc bởi \r\n
bool read_line(int fd, std::string& out_line) {
    out_line.clear();
    char c = 0;
    while (true) {
        ssize_t s = ::recv(fd, &c, 1, 0);
        if (s <= 0) return false;
        out_line.push_back(c);
        if (out_line.size() >= 2 && out_line[out_line.size() - 2] == '\r' && out_line[out_line.size() - 1] == '\n') {
            return true;
        }
    }
}

// Đọc 1 frame RESP hoàn chỉnh
bool read_resp_reply(int fd) {
    std::string line;
    if (!read_line(fd, line)) return false;
    if (line.empty()) return false;

    char prefix = line[0];
    if (prefix == '+' || prefix == '-' || prefix == ':') {
        return true;
    }
    if (prefix == '$') {
        int len = std::stoi(line.substr(1, line.size() - 3));
        if (len == -1) return true; // Null bulk string
        std::vector<char> buf(len + 2); // payload + \r\n
        return read_exact(fd, buf.data(), buf.size());
    }
    return true;
}

// Kết nối tới server
int connect_to_server(const std::string& host, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

// Chạy bài đo cho một lệnh cụ thể
BenchStats run_test(const BenchConfig& cfg,
                    std::string_view test_name,
                    auto generate_cmd) {
    int reqs_per_client = cfg.total_requests / cfg.clients;
    int actual_total_reqs = reqs_per_client * cfg.clients;

    std::vector<std::thread> workers;
    workers.reserve(cfg.clients);

    std::vector<std::vector<double>> client_latencies(cfg.clients);
    std::atomic<bool> start_flag{false};
    std::atomic<int> ready_count{0};
    std::atomic<int> failed_requests{0};

    for (int i = 0; i < cfg.clients; ++i) {
        workers.emplace_back([&, cid = i]() {
            client_latencies[cid].reserve(reqs_per_client);
            int fd = connect_to_server(cfg.host, cfg.port);
            if (fd < 0) {
                ready_count.fetch_add(1);
                failed_requests.fetch_add(reqs_per_client);
                return;
            }

            ready_count.fetch_add(1);
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::mt19937 rng(1337 + cid);

            for (int r = 0; r < reqs_per_client; ++r) {
                std::string cmd = generate_cmd(cid, r, rng);
                auto t0 = std::chrono::steady_clock::now();

                ssize_t sent = ::send(fd, cmd.data(), cmd.size(), 0);
                if (sent != static_cast<ssize_t>(cmd.size()) || !read_resp_reply(fd)) {
                    failed_requests.fetch_add(1);
                    break;
                }

                auto t1 = std::chrono::steady_clock::now();
                double lat_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                client_latencies[cid].push_back(lat_us);
            }

            ::close(fd);
        });
    }

    while (ready_count.load() < cfg.clients) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto start_time = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);

    for (auto& w : workers) {
        w.join();
    }
    auto end_time = std::chrono::steady_clock::now();

    double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();

    std::vector<double> all_latencies;
    all_latencies.reserve(actual_total_reqs);
    for (const auto& lats : client_latencies) {
        all_latencies.insert(all_latencies.end(), lats.begin(), lats.end());
    }

    BenchStats stats;
    stats.test_name = std::string(test_name);
    stats.total_requests = static_cast<int>(all_latencies.size());
    stats.elapsed_sec = elapsed_sec;
    stats.rps = (elapsed_sec > 0) ? (stats.total_requests / elapsed_sec) : 0;

    if (!all_latencies.empty()) {
        std::sort(all_latencies.begin(), all_latencies.end());
        double sum = std::accumulate(all_latencies.begin(), all_latencies.end(), 0.0);
        stats.lat_min_ms = all_latencies.front() / 1000.0;
        stats.lat_avg_ms = (sum / all_latencies.size()) / 1000.0;
        stats.lat_p50_ms = all_latencies[all_latencies.size() * 50 / 100] / 1000.0;
        stats.lat_p95_ms = all_latencies[all_latencies.size() * 95 / 100] / 1000.0;
        stats.lat_p99_ms = all_latencies[all_latencies.size() * 99 / 100] / 1000.0;
        stats.lat_max_ms = all_latencies.back() / 1000.0;
    }

    return stats;
}

void print_stats(const BenchStats& s) {
    std::cout << "====== " << s.test_name << " ======\n";
    std::cout << "  " << s.total_requests << " requests completed in "
              << std::fixed << std::setprecision(3) << s.elapsed_sec << " seconds\n";
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) << s.rps << " requests per second (RPS)\n";
    std::cout << "  Latency percentiles:\n";
    std::cout << "    min: " << std::setprecision(3) << s.lat_min_ms << " ms\n";
    std::cout << "    avg: " << std::setprecision(3) << s.lat_avg_ms << " ms\n";
    std::cout << "    p50: " << std::setprecision(3) << s.lat_p50_ms << " ms\n";
    std::cout << "    p95: " << std::setprecision(3) << s.lat_p95_ms << " ms\n";
    std::cout << "    p99: " << std::setprecision(3) << s.lat_p99_ms << " ms\n";
    std::cout << "    max: " << std::setprecision(3) << s.lat_max_ms << " ms\n\n";
}

}  // namespace mini_redis::benchmark

int main(int argc, char* argv[]) {
    mini_redis::benchmark::BenchConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if ((arg == "-h" || arg == "--host") && i + 1 < argc) {
            cfg.host = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            cfg.port = std::stoi(argv[++i]);
        } else if ((arg == "-c" || arg == "--clients") && i + 1 < argc) {
            cfg.clients = std::stoi(argv[++i]);
        } else if ((arg == "-n" || arg == "--requests") && i + 1 < argc) {
            cfg.total_requests = std::stoi(argv[++i]);
        }
    }

    std::cout << "========================================================\n";
    std::cout << "  mini-redis Benchmark Suite (epoll + RESP2)\n";
    std::cout << "  Target: " << cfg.host << ":" << cfg.port << "\n";
    std::cout << "  Clients: " << cfg.clients << ", Requests: " << cfg.total_requests << "\n";
    std::cout << "========================================================\n\n";

    // Test 1: PING
    auto s_ping = mini_redis::benchmark::run_test(cfg, "PING (inline/RESP)", [](int, int, std::mt19937&) {
        return "*1\r\n$4\r\nPING\r\n";
    });
    mini_redis::benchmark::print_stats(s_ping);

    // Test 2: SET
    auto s_set = mini_redis::benchmark::run_test(cfg, "SET (key-value store)", [](int, int r, std::mt19937& rng) {
        int key_id = static_cast<int>(rng() % 10000);
        std::string k = "bench_k_" + std::to_string(key_id);
        std::string v = "val_" + std::to_string(r);
        return "*3\r\n$3\r\nSET\r\n$" + std::to_string(k.size()) + "\r\n" + k + "\r\n$"
               + std::to_string(v.size()) + "\r\n" + v + "\r\n";
    });
    mini_redis::benchmark::print_stats(s_set);

    // Test 3: GET
    auto s_get = mini_redis::benchmark::run_test(cfg, "GET (cache hit/read)", [](int, int, std::mt19937& rng) {
        int key_id = static_cast<int>(rng() % 10000);
        std::string k = "bench_k_" + std::to_string(key_id);
        return "*2\r\n$3\r\nGET\r\n$" + std::to_string(k.size()) + "\r\n" + k + "\r\n";
    });
    mini_redis::benchmark::print_stats(s_get);

    // Test 4: INCR
    auto s_incr = mini_redis::benchmark::run_test(cfg, "INCR (atomic counter)", [](int cid, int, std::mt19937&) {
        std::string k = "counter_" + std::to_string(cid % 10);
        return "*2\r\n$4\r\nINCR\r\n$" + std::to_string(k.size()) + "\r\n" + k + "\r\n";
    });
    mini_redis::benchmark::print_stats(s_incr);

    return 0;
}

