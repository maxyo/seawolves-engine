// The height of the UI backend's texture-job buffers (backend_bgfx.cpp PlanJobs): the queued jobs shelf-packed
// in the order and the way PlaceJob packs them, rounded up to `step`, at least one step and at most the surface's
// height. A job covers its extent from the origin (its element's bottom-right corner on the surface).
#pragma once

#include <algorithm>
#include <span>
#include <utility>

namespace storm::ui
{
inline constexpr int kJobGap = 32; // between neighbours: their half-resolution blur passes stay apart

inline int JobBufferHeight(std::span<const std::pair<int, int>> extents, int width, int height, int step)
{
    int x = 0, y = 0, row = 0;
    for (const auto &[ex, ey] : extents)
    {
        const int w = ex + kJobGap;
        if (x + w > width && x > 0)
            y += row, x = 0, row = 0;
        x += w;
        row = std::max(row, ey + kJobGap);
    }
    const int need = (y + row + step - 1) / step * step;
    return std::clamp(need, std::min(step, height), height);
}
} // namespace storm::ui
