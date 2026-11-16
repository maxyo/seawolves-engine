#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace storm::render {
// Generation-bound receipt: arming/calling a void draw never proves a submission.
class LoadingOpaque {
public:
    void arm(uint64_t generation, bool full_screen, bool solid, uint8_t alpha) {
        armed_ = generation && full_screen && solid && alpha == 255 ? generation : 0;
    }
    void submitted(uint64_t generation, bool full_coverage = true, bool additive_blend = true) { if (full_coverage && additive_blend && armed_ && armed_ == generation) submitted_ = generation; }
    void frame(uint64_t generation, bool advanced, bool stable) {
        presented_ = advanced && stable && submitted_ == generation ? generation : 0;
        armed_ = submitted_ = 0;
    }
    void invalidate() { armed_ = submitted_ = presented_ = 0; }
    bool presented(uint64_t generation) const { return generation && presented_ == generation; }
private:
    uint64_t armed_ = 0, submitted_ = 0, presented_ = 0;
};
// The readiness counts only frames presented under the opaque loading cover (LoadingSceneWork). A load whose cover is
// never presented (a script path without loading artwork, a cover draw refused) must not wait for it forever: after
// kBudgetFrames frames of the stock loading transaction without one, the frames count without it (the caller logs the
// waiver as an ERROR, once per generation). Frames, not time: the synchronous script work of a load before its
// transaction draws nothing and must not use up the budget.
class LoadingCoverBudget {
public:
    static constexpr uint32_t kBudgetFrames = 30;
    void begin(uint64_t generation) { generation_ = generation; frames_ = 0; seen_ = waived_ = false; }
    void presented(uint64_t generation) { if (generation && generation == generation_) seen_ = true; }
    // One frame of the stock loading transaction (Renderer::RunEnd), before its Present.
    void transaction_frame(uint64_t generation) { if (generation && generation == generation_ && frames_ < kBudgetFrames) ++frames_; }
    // Whether this generation's frames count without a presented cover; `first` is set on the call that waives it.
    bool waived(uint64_t generation, bool *first = nullptr) {
        if (first) *first = false;
        if (!generation || generation != generation_ || seen_) return false;
        if (!waived_ && frames_ >= kBudgetFrames) {
            waived_ = true;
            if (first) *first = true;
        }
        return waived_;
    }
private:
    uint64_t generation_ = 0;
    uint32_t frames_ = 0;
    bool seen_ = false, waived_ = false;
};
// Milliseconds on steady_clock. This budgets complete frame STARTs, including prior CPU work.
// No timer manipulation, false scene frames, or GPU-idle assertion.
class LoadingBudget {
public:
    void frame_started(uint64_t now) { start_ = now; have_start_ = true; }
    uint64_t deadline(uint64_t now, bool eligible, double gpu_ms, uint64_t measured_at) const {
        if (!eligible || !have_start_ || now < start_ || now - start_ >= 80 ||
            !measured_at || now < measured_at || now - measured_at > 250 ||
            !std::isfinite(gpu_ms) || gpu_ms <= 0) return now;
        // Aim for a frame interval twice the measured GPU cost; the 80ms cap takes priority.
        const uint64_t interval = static_cast<uint64_t>(std::clamp(2.0 * gpu_ms, 16.0, 80.0));
        return std::max(now, start_ + interval);
    }
private:
    uint64_t start_ = 0;
    bool have_start_ = false;
};
}
