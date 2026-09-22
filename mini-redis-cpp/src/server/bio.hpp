#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

namespace mini_redis {

/// @brief Các loại công việc chạy nền (mô phỏng src/bio.c của Redis 4.0)
enum class BioType : size_t {
    CloseFile = 0,  ///< Đóng file descriptors ngầm
    AofFsync  = 1,  ///< fsync() ghi đĩa ngầm
    LazyFree  = 2,  ///< Giải phóng bộ nhớ ngầm (UNLINK, ASYNC FLUSH)
    NumOps    = 3   ///< Tổng số loại worker
};

/// @brief Quản lý hệ thống Background I/O luồng chuyên trách chuẩn Redis bio.c
class BioManager {
public:
    static BioManager& instance();

    BioManager(const BioManager&) = delete;
    BioManager& operator=(const BioManager&) = delete;
    BioManager(BioManager&&) = delete;
    BioManager& operator=(BioManager&&) = delete;

    /// @brief Khởi tạo các background threads (nếu chưa chạy hoặc sau stop)
    void init();

    /// @brief Đẩy một công việc vào hàng đợi của worker tương ứng
    void submit(BioType type, std::function<void()> job);

    /// @brief Lấy số lượng công việc đang chờ xử lý của một loại worker
    size_t pending(BioType type) const noexcept;

    /// @brief Chờ hoàn tất mọi công việc hiện có của một loại worker (dùng cho test/sync)
    void wait_empty(BioType type);

    /// @brief Dừng toàn bộ các worker threads một cách an toàn (Graceful Shutdown)
    void stop();

    ~BioManager();

private:
    BioManager();

    struct Worker {
        std::queue<std::function<void()>> jobs;
        mutable std::mutex mtx;
        std::condition_variable cv;
        std::condition_variable empty_cv;
        std::atomic<size_t> pending{0};
        std::atomic<bool> stopped{false};
        std::jthread thread;
    };

    void worker_loop(size_t type_idx, std::stop_token stop_token);

    std::vector<Worker> workers_;
    std::atomic<bool> initialized_{false};
};

}  // namespace mini_redis