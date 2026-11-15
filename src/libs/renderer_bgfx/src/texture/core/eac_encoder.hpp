#pragma once

#include "formats.hpp"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace storm::texture
{
// One owner/caller, with cancellation from another thread. Helpers exist only after a large encode, sleep between
// calls, and never outlive an encode's borrowed buffers. The relief Session owns this; ordinary transcode workers
// keep using encode_eac_chain so their own worker count is not multiplied.
class EacEncoder
{
  public:
    explicit EacEncoder(unsigned participants);
    ~EacEncoder();
    EacEncoder(const EacEncoder &) = delete;
    EacEncoder &operator=(const EacEncoder &) = delete;

    // Same bytes as encode_eac_chain. False (empty out) on cancellation or invalid dimensions. Worker allocation
    // exceptions are rethrown on the caller only after every helper has finished. A thread-start failure merely
    // reduces parallelism. The caller must not concurrently call Encode or destroy this object.
    bool Encode(EacFormat format, const uint8_t *texels, uint32_t width, uint32_t height, uint32_t levels,
                std::vector<uint8_t> &out, const std::atomic<bool> &cancelled);
    unsigned participants() const { return participants_; } // the last call's actual team, caller included

  private:
    friend struct EacEncoderTestAccess;
    struct Stripe
    {
        const uint8_t *input;
        uint8_t *output;
        uint32_t width, height;
    };
    using StartThread = std::thread (*)(void (*)(void *), void *);
    using EncodeLevel = decltype(&encode_eac);
    static std::thread Start(void (*entry)(void *), void *context);
    static void WorkerEntry(void *context);
    void Worker();
    void RunStripes();
    void StartHelpers();

    const unsigned limit_;
    unsigned participants_ = 1;
    bool started_ = false;
    std::vector<std::thread> helpers_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    uint64_t epoch_ = 0;
    size_t completed_ = 0;
    bool stopping_ = false;
    std::vector<Stripe> stripes_;
    std::atomic<size_t> next_{0};
    std::atomic<bool> failed_{false};
    std::exception_ptr failure_;
    const std::atomic<bool> *cancelled_ = nullptr;
    EacFormat format_ = EacFormat::Rg11;
    // Private injection seams for deterministic cancellation, exception and partial thread-start controls.
    StartThread start_thread_ = &Start;
    EncodeLevel encode_level_ = &encode_eac;
};
} // namespace storm::texture
