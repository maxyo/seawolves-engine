#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace storm::fx {
// Shared CPU work admission. Exact inference permits reflect its immutable ORT
// pool; bake pieces use one permit each. FIFO, cancellation polled while parked.
class CpuAdmission {
 public:
    explicit CpuAdmission(unsigned cores = std::thread::hardware_concurrency()) : cores_(std::max(1u, cores)) {}
    struct Stats { unsigned phase_cap, helper_ceiling, reserved, reserved_peak; };
    Stats stats() const { std::lock_guard lock(mutex_); return {opaque_ ? cores_ : gameplay_threads(), cores_, used_, peak_}; }
    unsigned maximum_threads() const { return cores_; }
    size_t queued() const { std::lock_guard lock(mutex_); return queue_.size(); }
    unsigned gameplay_threads() const { return cores_ > 2 ? cores_ - 2 : 1; }
    unsigned threads() const {
        std::lock_guard lock(mutex_);
        return opaque_ ? cores_ : gameplay_threads();
    }
    // Generation changes invalidate queued pre-admission selection. No caller
    // waits here; only the relief worker waits for the first real stock Present.
    void begin(uint64_t generation, bool active) {
        { std::lock_guard lock(mutex_); generation_ = generation; phase_ = active; opaque_ = false; ++revision_; }
        wake_.notify_all();
    }
    void suspend() {
        { std::lock_guard lock(mutex_); opaque_ = false; }
        wake_.notify_all();
    }
    void suspend(uint64_t generation) {
        { std::lock_guard lock(mutex_); if (generation_ == generation) opaque_ = false; }
        wake_.notify_all();
    }
    void end(uint64_t generation) {
        { std::lock_guard lock(mutex_); if (generation_ != generation) return;
          phase_ = opaque_ = false; ++revision_; }
        wake_.notify_all();
    }
    void opaque(uint64_t generation, bool valid) {
        { std::lock_guard lock(mutex_); if (phase_ && generation_ == generation) opaque_ = valid; }
        wake_.notify_all();
    }
    bool wait_for_opaque(const std::function<bool()> &cancelled) {
        std::unique_lock lock(mutex_); const auto revision = revision_;
        while (phase_ && !opaque_) {
            if ((cancelled && cancelled()) || revision_ != revision) return false;
            wake_.wait_for(lock, std::chrono::milliseconds(10));
        }
        return revision_ == revision && !(cancelled && cancelled());
    }
    std::shared_ptr<void> acquire(unsigned count, const std::function<bool()> &cancelled) {
        if (!count || count > cores_) return {};
        auto token = std::make_unique<unsigned>(count);
        std::unique_lock lock(mutex_);
        const auto id = ++serial_;
        queue_.push_back(id);
        for (;;) {
            if (cancelled && cancelled()) {
                queue_.erase(std::find(queue_.begin(), queue_.end(), id));
                lock.unlock(); wake_.notify_all(); return {};
            }
            // A job admitted during loading may finish its existing immutable
            // pool after reveal. New jobs use threads(), never a smaller lease.
            const unsigned limit = std::max(count, opaque_ ? cores_ : gameplay_threads());
            if (queue_.front() == id && used_ + count <= limit) {
                queue_.pop_front(); used_ += count; peak_ = std::max(peak_, used_);
                lock.unlock(); wake_.notify_all();
                return std::shared_ptr<void>(token.release(), [this](void *p) {
                    const unsigned n = *static_cast<unsigned *>(p); delete static_cast<unsigned *>(p);
                    { std::lock_guard lock(mutex_); used_ -= n; }
                    wake_.notify_all();
                });
            }
            wake_.wait_for(lock, std::chrono::milliseconds(10));
        }
    }
 private:
    const unsigned cores_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    unsigned used_ = 0, peak_ = 0;
    uint64_t serial_ = 0;
    std::deque<uint64_t> queue_;
    bool opaque_ = false, phase_ = false;
    uint64_t generation_ = 0, revision_ = 0;
};
// Defined once in bakes.cpp, linked into the engine, never a header-local singleton.
CpuAdmission &SharedCpuAdmission();
}
