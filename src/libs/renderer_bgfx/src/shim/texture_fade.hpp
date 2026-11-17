// A texture a store's draws bind that is swapped for another in front of the player fades over kTextureFadeMs
// instead of popping: a finished transcode in place of its interim copy (TexStore::finish_transcode), the neural ×2
// in place of the texture and back (effects/upscale.hpp). (The town light's ratio map fades by its own weight,
// effects/town_light.hpp; kTextureFadeMs and TextureFade::Share are its clock too.)
//
// The swap layer is Device::draw's texture binding: each store remembers the handle its draws bound last
// (TextureFade). When that changes while the store was on screen a moment before, the previous handle is bound
// beside the new one for kTextureFadeMs and the fixed-function fragment shader mixes the two by the stage's share
// (u_ffStage[3 i + 2].z above 1, ff_fragment.sh FF_STAGE): no extra pass, one more texture fetch only while it fades.
// The previous one goes to the FF unit the draw leaves free (stage 0's to unit 4, stage 1's to unit 3) unless a town's
// light ratio takes it (the ratio goes first, Device::draw); a draw that has none free, a game shader port and a cube or
// volume stage swap at once, counted in the fade line.
//
// A handle that is replaced and destroyed (the interim copy, an evicted ×2, a town's ratio map let go) is retired, not
// destroyed: TextureRetirement keeps it until no fade can read it (kTextureRetireMs). A fade starts only for a store
// drawn within kTextureFadeRecentMs before the swap, so it always ends before its previous handle is destroyed; a
// store that was off screen just shows the new texture.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace storm::shim
{

inline constexpr uint64_t kTextureFadeMs = 300;
// A store drawn no longer ago than this when its handle changed was on screen: it fades.
inline constexpr uint64_t kTextureFadeRecentMs = 250;
// A retired handle lives this long: a fade from it starts within kTextureFadeRecentMs and lasts kTextureFadeMs.
inline constexpr uint64_t kTextureRetireMs = kTextureFadeRecentMs + kTextureFadeMs + 250;
inline constexpr uint16_t kNoTexture = UINT16_MAX;

// What made the handle a store's draws bind: its own texture or its ×2.
enum class TextureSource : uint8_t
{
    Own,
    Upscale,
};

// Why a swap faded or did not (the fade line).
enum class TextureFadeKind : uint8_t
{
    Transcode, // the store's own texture changed: a finished transcode
    Upscale,   // its ×2 came or went
    Count
};

struct TextureFade
{
    uint16_t current = kNoTexture; // the handle the store's draws bound last
    uint16_t from = kNoTexture;    // the one before, while it fades out
    TextureSource source = TextureSource::Own;
    uint32_t epoch = 0;            // TextureFades::epoch() when `current` was bound (a new device forgets everything)
    uint64_t since = 0;            // when the fade began, ms
    uint64_t drawn = 0;            // when a draw last bound `current`, ms

    struct Sample
    {
        uint16_t from = kNoTexture; // bind it beside the new one, at `share`
        float share = 0.0f;         // of the previous texture in the stage's colour: 1 at the swap, 0 at the end
        bool started = false;       // this draw began the fade
        bool reversed = false;      // ... by swapping back to the texture that was fading out
        TextureFadeKind kind = TextureFadeKind::Transcode;
    };

    // The draw at `now` binds `handle` (from `source`); the fade it shows, if any.
    Sample observe(uint16_t handle, TextureSource handle_source, uint64_t now, uint32_t now_epoch)
    {
        Sample sample;
        if (epoch != now_epoch)
            current = from = kNoTexture;
        if (handle == current)
        {
            drawn = now;
            if (from == kNoTexture)
                return sample;
            const uint64_t t = now - since;
            if (t >= kTextureFadeMs)
            {
                from = kNoTexture;
                return sample;
            }
            sample.from = from;
            sample.share = Share(t);
            return sample;
        }
        if (from != kNoTexture && handle == from && now - since < kTextureFadeMs)
        {
            // Swapped back while fading (an evicted ×2 that comes back): the fade turns round from the share the
            // player sees now (Share(kTextureFadeMs - t) = 1 - Share(t)).
            const uint64_t t = now - since;
            sample.kind = Kind(source, handle_source);
            from = current;
            current = handle;
            source = handle_source;
            since = now - (kTextureFadeMs - t);
            drawn = now;
            sample.from = from;
            sample.share = Share(now - since);
            sample.started = sample.reversed = true;
            return sample;
        }
        sample.kind = Kind(source, handle_source);
        if (from != kNoTexture && now - since < kTextureFadeMs / 2)
        {
            // Swapped again in the first half of a fade (a ×2 evicted and another resident at once): the player still
            // sees mostly the texture fading out, so the new one fades in over it, at its share, and the one in
            // between is skipped. Its fade ends by since + kTextureFadeMs, within its
            // retirement.
            current = handle;
            source = handle_source;
            drawn = now;
            sample.from = from;
            sample.share = Share(now - since);
            sample.started = true;
            return sample;
        }
        const bool on_screen = current != kNoTexture && now - drawn <= kTextureFadeRecentMs;
        // Swapped again later in a fade: the new one fades in over the one the player sees most (the one fading in).
        from = on_screen ? current : kNoTexture;
        current = handle;
        source = handle_source;
        epoch = now_epoch;
        since = drawn = now;
        if (from == kNoTexture)
            return sample;
        sample.from = from;
        sample.share = 1.0f;
        sample.started = true;
        return sample;
    }
    // The store's texture was made again from new contents (a re-lock): nothing to fade from.
    void reset() { current = from = kNoTexture; }

    static TextureFadeKind Kind(TextureSource a, TextureSource b)
    {
        return a == TextureSource::Upscale || b == TextureSource::Upscale ? TextureFadeKind::Upscale : TextureFadeKind::Transcode;
    }
    // The previous texture's share `t` ms into the fade: 1 -> 0, eased at both ends.
    static float Share(uint64_t t)
    {
        const float x = std::min(1.0f, static_cast<float>(t) / static_cast<float>(kTextureFadeMs));
        return 1.0f - x * x * (3.0f - 2.0f * x);
    }
};

// Handles retired at a time, destroyed by `collect` once kTextureRetireMs passed (or all by `flush`).
class TextureRetirement
{
  public:
    void retire(uint16_t handle, uint64_t now)
    {
        if (handle != kNoTexture)
            retired_.push_back({handle, now});
    }
    template <class Destroy> size_t collect(uint64_t now, Destroy &&destroy)
    {
        size_t destroyed = 0;
        std::erase_if(retired_, [&](const Entry &entry) {
            if (now - entry.at < kTextureRetireMs)
                return false;
            destroy(entry.handle);
            ++destroyed;
            return true;
        });
        return destroyed;
    }
    template <class Destroy> void flush(Destroy &&destroy)
    {
        for (const Entry &entry : retired_)
            destroy(entry.handle);
        retired_.clear();
    }
    size_t size() const { return retired_.size(); }

  private:
    struct Entry
    {
        uint16_t handle;
        uint64_t at;
    };
    std::vector<Entry> retired_;
};

// The fades of a span of frames, for the log line.
struct TextureFadeCounts
{
    std::array<uint32_t, size_t(TextureFadeKind::Count)> started{};
    uint32_t no_unit = 0, port = 0, not_2d = 0; // swaps drawn at once: why
    uint32_t reversed = 0; // swaps back to the texture that was fading out (of `started`): many say a store's draws
                           // disagree about its texture
    uint32_t total_started() const { return started[0] + started[1]; }
    uint32_t total_skipped() const { return no_unit + port + not_2d; }
    bool empty() const { return !total_started() && !total_skipped(); }
};

} // namespace storm::shim
