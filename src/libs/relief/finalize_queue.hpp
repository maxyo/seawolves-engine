#pragma once

#include "finalizer.hpp"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>

namespace storm::relief
{
// Exactly one slot, including work executing and a completion not yet consumed. No unbounded results queue.
// The compute owner may hold one next product while waiting here, without any CPU/GPU admission permit.
class FinalizeQueue
{
  public:
    struct Completion { uint64_t id = 0; FinalizeResult result; std::exception_ptr exception; };
    using Work = std::function<FinalizeResult(std::unique_ptr<ComputedTexture>)>;
    FinalizeQueue(Work work, std::function<void()> wake, std::function<void()> initialize = {})
        : work_(std::move(work)), wake_(std::move(wake)), initialize_(std::move(initialize)),
          worker_([this] { run(); }) {}
    ~FinalizeQueue() { stop(); }
    FinalizeQueue(const FinalizeQueue &) = delete;
    FinalizeQueue &operator=(const FinalizeQueue &) = delete;

    bool submit(uint64_t id, std::unique_ptr<ComputedTexture> &product)
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || occupied_ || !product) return false;
        occupied_ = true; id_ = id; cancelled_ = product->cancelled;
        product_ = std::move(product); available_.notify_one(); return true;
    }
    std::optional<Completion> take()
    {
        std::lock_guard lock(mutex_);
        if (!completion_) return {};
        auto value = std::move(completion_); completion_.reset();
        occupied_ = false; cancelled_.reset(); return value;
    }
    bool completed() const { std::lock_guard lock(mutex_); return completion_.has_value(); }
    bool occupied() const { std::lock_guard lock(mutex_); return occupied_; }
    void request_stop()
    {
        { std::lock_guard lock(mutex_); stopping_ = true; if (cancelled_) *cancelled_ = true; }
        available_.notify_all();
        // The owner wakes its own waiters when requesting stop. The completion callback may
        // acquire the owner's mutex to close the condition-variable check/wait race.
    }
    void stop()
    {
        request_stop();
        if (worker_.joinable()) worker_.join();
    }

  private:
    void run()
    {
        if (initialize_) initialize_();
        for (;;) {
            std::unique_ptr<ComputedTexture> product;
            Completion done;
            {
                std::unique_lock lock(mutex_);
                available_.wait(lock, [&] { return stopping_ || product_; });
                if (!product_) return;
                done.id = id_; product = std::move(product_);
                done.result.session = product->session;
            }
            try { done.result = work_(std::move(product)); }
            catch (...) { done.exception = std::current_exception(); }
            {
                std::lock_guard lock(mutex_);
                completion_ = std::move(done);
            }
            if (wake_) wake_(); // no queue lock held; never call a Session/renderer from this thread
        }
    }
    Work work_;
    std::function<void()> wake_, initialize_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    bool occupied_ = false, stopping_ = false;
    uint64_t id_ = 0;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::unique_ptr<ComputedTexture> product_;
    std::optional<Completion> completion_;
    std::thread worker_;
};
} // namespace storm::relief
