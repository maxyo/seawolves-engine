#pragma once

#include "loading_retry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace storm::relief
{
// Wall-time calibration is an estimate, not a GPU timestamp or a preemptible submission limit.
struct TileRecoveryPlan
{
    int tile = 0;
    uint64_t generation = 0;
    double predicted_wall_ms = 0.0;
    explicit operator bool() const { return tile != 0; }
};

class TileRecovery
{
  public:
    TileRecoveryPlan plan(const std::shared_ptr<LoadingRetry> &lease, uint64_t now, bool cancelled,
                          int current, int ceiling, int halo, int inner_w, int inner_h,
                          double fixed, double slope) const
    {
        if (!lease || !lease->generation || lease->generation == last_attempt_generation_ || cancelled ||
            !lease->active.load() || now >= lease->deadline_ms || current < 32 || current > 512 ||
            current % 32 || ceiling <= current || ceiling > 512 || ceiling % 32 || halo < 0 ||
            !std::isfinite(fixed) || fixed < 0 || !std::isfinite(slope) || slope <= 0)
            return {};
        const int candidate = std::min(current * 2, ceiling);
        if (inner_w < candidate || inner_h < candidate)
            return {};
        const double padded = static_cast<double>(candidate) + 2.0 * halo;
        const double predicted = fixed + slope * padded * padded;
        if (!std::isfinite(predicted) || predicted <= 0 ||
            predicted + kReserveMs >= static_cast<double>(lease->deadline_ms - now))
            return {};
        return {candidate, lease->generation, predicted};
    }

    // Called immediately before submitting, after input preparation. A renewed deadline never resets this count.
    bool begin(const TileRecoveryPlan &plan, const std::shared_ptr<LoadingRetry> &lease,
               uint64_t now, bool cancelled)
    {
        if (!plan || !lease || cancelled || !lease->active.load() || lease->generation != plan.generation ||
            !plan.generation || plan.generation == last_attempt_generation_ || now >= lease->deadline_ms ||
            plan.predicted_wall_ms + kReserveMs >= static_cast<double>(lease->deadline_ms - now))
            return false;
        last_attempt_generation_ = plan.generation;
        return true;
    }

    static const char *decision(const TileRecoveryPlan &plan, const std::shared_ptr<LoadingRetry> &lease,
                                uint64_t now, bool cancelled, bool shape_valid, double wall, double fixed)
    {
        if (cancelled) return "cancelled";
        if (!lease || !lease->active.load() || lease->generation != plan.generation) return "revoked";
        if (now >= lease->deadline_ms) return "expired";
        if (!shape_valid) return "inference-or-shape-failed";
        if (!std::isfinite(wall) || wall <= 0 || !std::isfinite(fixed) || fixed < 0 || wall < fixed)
            return "invalid-wall-estimate";
        if (wall - fixed > 6.0) return "over-estimated-work-budget";
        return "accepted";
    }

  private:
    static constexpr double kReserveMs = 1.0;
    uint64_t last_attempt_generation_ = 0;
};

// Accepted exact first tile only: RunTiles moves output into its ordinary scratch vector before the first resize.
// The pointer and exact dimensions prevent stale or differently shaped output from ever bypassing inference.
struct PrimedTile
{
    std::vector<float> output;
    float *expected_output = nullptr;
    int width = 0, height = 0;
    double wall_ms = 0.0;
    bool pending = false, reused = false;

    bool consume(int iw, int ih, float *out, double &ms)
    {
        if (!pending) return false;
        pending = false;
        if (iw != width || ih != height || out != expected_output) return false;
        ms = wall_ms;
        reused = true;
        return true;
    }
};
} // namespace storm::relief
