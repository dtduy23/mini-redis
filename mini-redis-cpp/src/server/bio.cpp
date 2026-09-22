#include "bio.hpp"

namespace mini_redis {

BioManager& BioManager::instance() {
    static BioManager s_instance;
    return s_instance;
}

BioManager::BioManager() {
    init();
}

BioManager::~BioManager() {
    stop();
}

void BioManager::init() {
    if (initialized_.load()) {
        return;
    }

    size_t num_ops = static_cast<size_t>(BioType::NumOps);
    workers_ = std::vector<Worker>(num_ops);

    for (size_t i = 0; i < num_ops; ++i) {
        workers_[i].stopped.store(false);
        workers_[i].pending.store(0);
        workers_[i].thread = std::jthread([this, i](std::stop_token st) {
            worker_loop(i, st);
        });
    }

    initialized_.store(true);
}

void BioManager::submit(BioType type, std::function<void()> job) {
    size_t idx = static_cast<size_t>(type);
    if (idx >= workers_.size()) {
        return;
    }

    auto& w = workers_[idx];
    {
        std::lock_guard lock(w.mtx);
        w.jobs.push(std::move(job));
        w.pending.fetch_add(1, std::memory_order_relaxed);
    }
    w.cv.notify_one();
}

size_t BioManager::pending(BioType type) const noexcept {
    size_t idx = static_cast<size_t>(type);
    if (idx >= workers_.size()) {
        return 0;
    }
    return workers_[idx].pending.load(std::memory_order_relaxed);
}

void BioManager::wait_empty(BioType type) {
    size_t idx = static_cast<size_t>(type);
    if (idx >= workers_.size()) {
        return;
    }
    auto& w = workers_[idx];
    std::unique_lock lock(w.mtx);
    w.empty_cv.wait(lock, [&]() {
        return w.jobs.empty() && w.pending.load(std::memory_order_relaxed) == 0;
    });
}

void BioManager::stop() {
    if (!initialized_.load()) {
        return;
    }

    for (auto& w : workers_) {
        w.stopped.store(true);
        w.thread.request_stop();
        w.cv.notify_all();
    }

    // std::jthread destructor tự động join khi vector bị clear
    workers_.clear();
    initialized_.store(false);
}

void BioManager::worker_loop(size_t type_idx, std::stop_token stop_token) {
    auto& w = workers_[type_idx];

    while (true) {
        std::function<void()> task;

        {
            std::unique_lock lock(w.mtx);
            w.cv.wait(lock, [&]() {
                return stop_token.stop_requested() || w.stopped.load() || !w.jobs.empty();
            });

            // Nếu nhận tín hiệu dừng và không còn job nào trong hàng đợi
            if ((stop_token.stop_requested() || w.stopped.load()) && w.jobs.empty()) {
                break;
            }

            if (!w.jobs.empty()) {
                task = std::move(w.jobs.front());
                w.jobs.pop();
            }
        }

        if (task) {
            task();
            w.pending.fetch_sub(1, std::memory_order_relaxed);
        }

        {
            std::lock_guard lock(w.mtx);
            if (w.jobs.empty() && w.pending.load(std::memory_order_relaxed) == 0) {
                w.empty_cv.notify_all();
            }
        }
    }
}

}  // namespace mini_redis

