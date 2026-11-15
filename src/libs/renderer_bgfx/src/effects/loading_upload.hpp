#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "storm/render/loading_budget.hpp"

namespace storm::effects
{
// A value captured on the game thread after Present committed an actual fully opaque,
// generation-matching frame on a stable surface. It is not an inference/GPU-idle lease.
struct LoadingUploadScope
{
    uint64_t generation = 0;
    bool opaque_preparing = false;
    static LoadingUploadScope for_frame(uint64_t generation, bool preparing, const render::LoadingOpaque &opaque)
    {
        return {generation, preparing && opaque.presented(generation)};
    }
    bool active() const { return generation != 0 && opaque_preparing; }
    bool accepts(uint64_t entry_generation, bool selected) const
    {
        return active() && selected && entry_generation == generation;
    }
};

class UploadBatch
{
  public:
    static constexpr unsigned kGameplayUploads = 2, kLoadingUploads = 16;
    static constexpr uint64_t kLoadingBytes = 16u << 20;
    static constexpr double kLoadingMilliseconds = 4.0;

    explicit UploadBatch(bool loading) : loading_(loading) {}
    bool allows(uint64_t bytes, double elapsed_ms) const
    {
        if (!loading_)
            return uploads_ < kGameplayUploads;
        if (uploads_ >= kLoadingUploads)
            return false;
        // A submit cannot be preempted. One oversized/slow texture still advances the
        // existing validation/upload path; it is a reported singleton, never a stuck queue.
        if (uploads_ == 0)
            return true;
        return elapsed_ms < kLoadingMilliseconds && bytes_ <= kLoadingBytes && bytes <= kLoadingBytes - bytes_;
    }
    void submitted(uint64_t bytes)
    {
        ++uploads_;
        bytes_ = bytes > std::numeric_limits<uint64_t>::max() - bytes_
                     ? std::numeric_limits<uint64_t>::max() : bytes_ + bytes;
    }
    unsigned uploads() const { return uploads_; }
    uint64_t bytes() const { return bytes_; }

  private:
    bool loading_;
    unsigned uploads_ = 0;
    uint64_t bytes_ = 0;
};
} // namespace storm::effects
