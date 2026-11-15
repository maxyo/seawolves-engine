#include "eac_encoder.hpp"

#include <algorithm>
#include <system_error>

namespace storm::texture
{
EacEncoder::EacEncoder(unsigned participants) : limit_(std::clamp(participants, 1u, 4u)) {}

EacEncoder::~EacEncoder()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (std::thread &helper : helpers_)
        helper.join();
}

std::thread EacEncoder::Start(void (*entry)(void *), void *context)
{
    return std::thread(entry, context);
}

void EacEncoder::StartHelpers()
{
    if (started_)
        return;
    helpers_.reserve(limit_ - 1); // no vector allocation after a thread has been started
    started_ = true;
    for (unsigned i = 1; i < limit_; ++i)
    {
        try
        {
            // Created on the relief worker: inherit its low scheduling priority. No other subsystem starts these.
            helpers_.push_back(start_thread_(&WorkerEntry, this));
        }
        catch (const std::system_error &)
        {
            break; // resource exhaustion: the caller and any helpers already started can finish the same work
        }
    }
}

void EacEncoder::WorkerEntry(void *context)
{
    static_cast<EacEncoder *>(context)->Worker();
}

void EacEncoder::Worker()
{
    uint64_t seen = 0;
    std::unique_lock lock(mutex_);
    for (;;)
    {
        wake_.wait(lock, [&] { return stopping_ || epoch_ != seen; });
        if (stopping_)
            return;
        seen = epoch_;
        lock.unlock();
        RunStripes();
        lock.lock();
        ++completed_;
        done_.notify_one();
    }
}

void EacEncoder::RunStripes()
{
    while (!cancelled_->load(std::memory_order_relaxed) && !failed_.load(std::memory_order_relaxed))
    {
        const size_t index = next_.fetch_add(1, std::memory_order_relaxed);
        if (index >= stripes_.size())
            break;
        const Stripe &stripe = stripes_[index];
        try
        {
            encode_level_(format_, stripe.input, stripe.width, stripe.height, stripe.output);
        }
        catch (...)
        {
            std::lock_guard lock(mutex_);
            if (!failure_)
                failure_ = std::current_exception();
            failed_.store(true, std::memory_order_relaxed);
        }
    }
}

bool EacEncoder::Encode(EacFormat format, const uint8_t *texels, uint32_t width, uint32_t height, uint32_t levels,
                        std::vector<uint8_t> &out, const std::atomic<bool> &cancelled)
{
    out.clear();
    participants_ = 1;
    if (!texels || !width || !height || !levels || levels > 32 || cancelled.load(std::memory_order_relaxed))
        return false;
    // Production relief <=512, gloss <=256. Keep larger CLI maps on the original path rather than multiplying
    // scratch memory. Small chains cannot amortize dispatch; they also do not need a helper allocation.
    if (limit_ == 1 || width > 512 || height > 512 || size_t(blocks_x(width)) * blocks_x(height) < 256)
    {
        out = encode_eac_chain(format, texels, width, height, levels);
        if (cancelled.load(std::memory_order_relaxed))
            out.clear();
        return !out.empty();
    }
    size_t total = 0;
    for (uint32_t level = 0, w = width, h = height; level < levels; ++level, w = std::max(w / 2, 1u), h = std::max(h / 2, 1u))
        total += size_t(blocks_x(w)) * blocks_x(h) * eac_block_bytes(format);
    out.resize(total);
    stripes_.clear();
    size_t input = 0, output = 0;
    for (uint32_t level = 0, w = width, h = height; level < levels; ++level, w = std::max(w / 2, 1u), h = std::max(h / 2, 1u))
    {
        const uint32_t block_width = blocks_x(w);
        const uint32_t rows = std::max(1u, 1024u / block_width) * 4;
        for (uint32_t y = 0; y < h; y += rows)
            stripes_.push_back({texels + input + size_t(y) * w * eac_channels(format),
                                out.data() + output + size_t(y / 4) * block_width * eac_block_bytes(format),
                                w, std::min(rows, h - y)});
        input += size_t(w) * h * eac_channels(format);
        output += size_t(block_width) * blocks_x(h) * eac_block_bytes(format);
    }
    StartHelpers();
    participants_ = static_cast<unsigned>(helpers_.size()) + 1;
    {
        std::lock_guard lock(mutex_);
        format_ = format;
        cancelled_ = &cancelled;
        next_ = 0;
        failed_ = false;
        failure_ = nullptr;
        completed_ = 0;
        ++epoch_;
    }
    wake_.notify_all();
    RunStripes(); // the owning relief thread participates; it does not wait while an extra fourth helper runs
    std::exception_ptr failure;
    {
        std::unique_lock lock(mutex_);
        done_.wait(lock, [&] { return completed_ == helpers_.size(); });
        failure = failure_;
        cancelled_ = nullptr;
    }
    stripes_.clear(); // no borrowed input/output pointer remains reachable by a helper
    if (failure || cancelled.load(std::memory_order_relaxed))
    {
        out.clear();
        if (failure)
            std::rethrow_exception(failure);
        return false;
    }
    return true;
}
} // namespace storm::texture
