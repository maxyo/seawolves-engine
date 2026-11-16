#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace storm::effects
{
// Observe existing draws, never scan geometry. A 500 ms window merges all instances of a shared texture;
// observations not renewed for 2 s, or belonging to another scene, lose their spatial priority.
struct GenerationSnapshot
{
    uint16_t scene = 0;
    uint32_t window = 0;
};
inline constexpr uint32_t kGenerationWindowMask = 0xffffff;
inline uint32_t generation_window()
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return static_cast<uint32_t>(ms / 500) & kGenerationWindowMask;
}

class GenerationPriority
{
  public:
    void observe(float distance, GenerationSnapshot at) // game thread only
    {
        if (!std::isfinite(distance) || distance < 0.0f)
            return;
        const uint64_t prefix = (uint64_t(at.scene) << 48) | (uint64_t(at.window) << 24);
        const auto cm = static_cast<uint32_t>(std::min(double(distance) * 100.0, double(kGenerationWindowMask - 1)));
        const uint64_t old = sample_.load(std::memory_order_relaxed);
        if ((old & ~uint64_t(kGenerationWindowMask)) != prefix || cm < (old & kGenerationWindowMask))
            sample_.store(prefix | cm, std::memory_order_relaxed);
    }
    float distance(GenerationSnapshot at) const // worker; one atomic snapshot, no torn scene/distance pair
    {
        const uint64_t sample = sample_.load(std::memory_order_relaxed);
        const uint32_t window = static_cast<uint32_t>(sample >> 24) & kGenerationWindowMask;
        if (sample == UINT64_MAX || uint16_t(sample >> 48) != at.scene ||
            ((at.window - window) & kGenerationWindowMask) >= 4)
            return std::numeric_limits<float>::infinity();
        return float(sample & kGenerationWindowMask) * 0.01f;
    }

  private:
    std::atomic<uint64_t> sample_{UINT64_MAX};
};

// A literal projected sphere size, used only to break equal fresh player-distance ranks of normal-map requests.
// One atomic scene/window/score snapshot; shared texture instances keep the largest in the same 500 ms window.
class GenerationScreenSize
{
  public:
    void observe(float radius, float camera_distance, bool character, GenerationSnapshot at) // game thread only
    {
        if (!std::isfinite(radius) || radius < 0.0f || !std::isfinite(camera_distance) || camera_distance < 0.0f)
            return;
        const double ratio = double(radius) / std::max(double(camera_distance), 0.5) * (character ? 3.0 : 1.0);
        const auto score = static_cast<uint32_t>(std::round(std::min(ratio * 65536.0, double(kGenerationWindowMask - 1))));
        const uint64_t prefix = (uint64_t(at.scene) << 48) | (uint64_t(at.window) << 24);
        const uint64_t old = sample_.load(std::memory_order_relaxed);
        if ((old & ~uint64_t(kGenerationWindowMask)) != prefix || score > (old & kGenerationWindowMask))
            sample_.store(prefix | score, std::memory_order_relaxed);
    }
    uint32_t score(GenerationSnapshot at) const
    {
        const uint64_t sample = sample_.load(std::memory_order_relaxed);
        const uint32_t window = static_cast<uint32_t>(sample >> 24) & kGenerationWindowMask;
        if (sample == UINT64_MAX || uint16_t(sample >> 48) != at.scene ||
            ((at.window - window) & kGenerationWindowMask) >= 4)
            return 0;
        return static_cast<uint32_t>(sample) & kGenerationWindowMask;
    }
  private:
    std::atomic<uint64_t> sample_{UINT64_MAX};
};

// Fairness applies within 10 m bands, so a distant upscale never wins merely because it is that lane's turn.
// After 16 other admitted jobs, still-observed live work gets an oldest-first turn; thermal waiting does not age it.
inline std::pair<uint32_t, float> generation_rank(const GenerationPriority &priority, GenerationSnapshot at,
                                                 uint64_t admitted_since_queued)
{
    const float distance = priority.distance(at);
    if (!std::isfinite(distance))
        return {UINT32_MAX, distance};
    if (admitted_since_queued >= 16)
        return {0, -1.0f};
    return {static_cast<uint32_t>(distance / 10.0f), distance};
}

// Who claims the player's position. On land the controlled character and the main character's ship both exist (the
// ship moored in the town's port runs its Execute too): the character outranks the ship, whichever updates last.
enum class PlayerRole : uint8_t
{
    Ship,      // the main character's ship: the player at sea
    Character, // the controlled character in a location
};

class GenerationFocus
{
  public:
    GenerationSnapshot snapshot() const { return {scene_.load(std::memory_order_relaxed), generation_window()}; }
    // False when a valid claim of a higher role holds the anchor: the position is not taken.
    bool set_player(const std::array<float, 3> &position, const void *owner = nullptr,
                    PlayerRole role = PlayerRole::Character)
    {
        if (valid_ && owner != owner_ && role < role_)
            return false;
        if (owner != owner_)
            scene_.fetch_add(1, std::memory_order_relaxed); // a different controlled entity starts fresh observations
        owner_ = owner;
        role_ = role;
        player_ = position;
        valid_ = std::all_of(position.begin(), position.end(), [](float p) { return std::isfinite(p); });
        return valid_;
    }
    void clear_player(const void *owner = nullptr)
    {
        if (owner != owner_) return; // an old entity cannot clear its replacement's anchor
        valid_ = false;
        owner_ = nullptr;
        scene_.fetch_add(1, std::memory_order_relaxed);
    }
    void set_origin(const std::array<float, 3> &origin) { origin_ = origin; }
    // The controlled character's or main ship's position (absolute world); false: none is known.
    bool player(std::array<float, 3> &position) const
    {
        if (valid_)
            position = player_;
        return valid_;
    }
    // The role and identity (never dereferenced) of the current claim; meaningful while player() is true.
    PlayerRole player_role() const { return role_; }
    const void *player_owner() const { return owner_; }
    void set_scene(std::string_view name)
    {
        if (name != name_ || name.empty())
        {
            name_ = name;
            scene_.fetch_add(1, std::memory_order_relaxed);
            valid_ = false;
            owner_ = nullptr;
        }
    }
    float distance(const std::array<float, 3> &device_center, float radius) const
    {
        if (!valid_ || !std::isfinite(radius) ||
            !std::all_of(device_center.begin(), device_center.end(), [](float p) { return std::isfinite(p); }) ||
            !std::all_of(origin_.begin(), origin_.end(), [](float p) { return std::isfinite(p); }))
            return -1.0f; // no camera substitution: queued jobs keep ordinary load-order priority
        float squared = 0.0f;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float delta = device_center[axis] + (origin_[axis] - player_[axis]);
            squared += delta * delta;
        }
        return std::max(0.0f, std::sqrt(squared) - std::max(0.0f, radius));
    }

  private:
    std::atomic<uint16_t> scene_{1};
    std::string name_;
    std::array<float, 3> player_{}, origin_{};
    bool valid_ = false;
    PlayerRole role_ = PlayerRole::Character;
    const void *owner_ = nullptr; // identity only; never dereferenced
};
inline GenerationFocus &generation_focus()
{
    static GenerationFocus focus;
    return focus;
}
} // namespace storm::effects
