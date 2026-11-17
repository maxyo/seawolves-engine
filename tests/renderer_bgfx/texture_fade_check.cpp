// The texture swap fade (src/shim/texture_fade.hpp): when a store's draws start binding another handle (a finished
// transcode, its ×2) the previous one fades out over kTextureFadeMs; a store that was off screen
// swaps at once; a swap back mid-fade turns round without a jump; a retired handle outlives every fade from it.
#include "texture_fade.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace storm::shim;

namespace
{
void require(bool good, const char *what)
{
    if (!good)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
    std::printf("PASS: %s\n", what);
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
} // namespace

int main()
{
    constexpr uint32_t epoch = 1;
    {
        TextureFade fade;
        TextureFade::Sample s = fade.observe(10, TextureSource::Own, 1000, epoch);
        require(s.from == kNoTexture && !s.started, "the first draw of a store fades from nothing");
        s = fade.observe(10, TextureSource::Own, 1016, epoch);
        require(s.from == kNoTexture, "the same handle again: no fade");
        // Its transcode finished: the next draw binds the new handle.
        s = fade.observe(11, TextureSource::Own, 1032, epoch);
        require(s.started && s.from == 10 && near(s.share, 1.0f) && s.kind == TextureFadeKind::Transcode,
                "a swap on screen starts at the previous texture's full share (transcode)");
        float last = 1.0f;
        bool falling = true;
        for (uint64_t t = 16; t < kTextureFadeMs; t += 16)
        {
            s = fade.observe(11, TextureSource::Own, 1032 + t, epoch);
            falling = falling && s.from == 10 && !s.started && s.share < last && s.share > 0.0f;
            last = s.share;
        }
        require(falling, "the previous texture's share falls every frame of the fade");
        s = fade.observe(11, TextureSource::Own, 1032 + kTextureFadeMs / 2, epoch);
        s = fade.observe(11, TextureSource::Own, 1032 + kTextureFadeMs, epoch);
        require(s.from == kNoTexture, "after kTextureFadeMs only the new texture is bound");
        s = fade.observe(11, TextureSource::Own, 1032 + kTextureFadeMs + 16, epoch);
        require(s.from == kNoTexture, "and stays so");
    }
    require(near(TextureFade::Share(0), 1.0f) && near(TextureFade::Share(kTextureFadeMs / 2), 0.5f) &&
                near(TextureFade::Share(kTextureFadeMs), 0.0f) &&
                near(TextureFade::Share(kTextureFadeMs / 4) + TextureFade::Share(kTextureFadeMs * 3 / 4), 1.0f),
            "the share eases 1 -> 0, symmetric about the middle");
    {
        TextureFade fade;
        fade.observe(20, TextureSource::Own, 0, epoch);
        // Off screen for a second (the texture's draws were culled), then its ×2 is bound: nothing to fade from.
        TextureFade::Sample s = fade.observe(21, TextureSource::Upscale, 1000, epoch);
        require(s.from == kNoTexture && !s.started, "a store not drawn within kTextureFadeRecentMs swaps at once");
        s = fade.observe(20, TextureSource::Own, 1016, epoch);
        require(s.started && s.from == 21 && s.kind == TextureFadeKind::Upscale, "the ×2 evicted on screen fades out (x2)");
        // Half way it comes back: the fade turns round from what the player sees.
        s = fade.observe(20, TextureSource::Own, 1016 + 100, epoch);
        const float shown_x2 = s.share; // the ×2's share
        s = fade.observe(21, TextureSource::Upscale, 1016 + 100, epoch);
        require(s.started && s.reversed && s.from == 20 && near(1.0f - s.share, shown_x2),
                "a swap back mid-fade keeps the share the player sees (no jump)");
        s = fade.observe(21, TextureSource::Upscale, 1016 + 100 + kTextureFadeMs, epoch);
        require(s.from == kNoTexture, "the turned fade ends within kTextureFadeMs");
    }
    {
        // A ×2 evicted (the texture's own for one frame) and another resident at once: the new one fades in over the
        // one the player saw (the old ×2), never showing the texture's own whole.
        TextureFade fade;
        fade.observe(50, TextureSource::Upscale, 0, epoch);
        TextureFade::Sample s = fade.observe(51, TextureSource::Own, 16, epoch);
        require(s.started && s.from == 50, "the old x2 fades out to the texture's own");
        s = fade.observe(52, TextureSource::Upscale, 19, epoch);
        require(s.started && s.from == 50 && s.share > 0.95f, "a second swap early in the fade keeps fading from the old one");
        s = fade.observe(52, TextureSource::Upscale, 16 + kTextureFadeMs, epoch);
        require(s.from == kNoTexture, "and ends kTextureFadeMs after the first swap");
        // Late in a fade a new swap fades from the one fading in.
        fade.observe(53, TextureSource::Upscale, 400, epoch);
        s = fade.observe(54, TextureSource::Upscale, 400 + kTextureFadeMs * 2 / 3, epoch);
        require(s.started && s.from == 53 && near(s.share, 1.0f), "a swap late in a fade fades from the texture fading in");
        // The worst case for the retirement: the carried fade still ends before kTextureRetireMs after the retire.
        require(kTextureFadeRecentMs + kTextureFadeMs < kTextureRetireMs, "a carried fade ends within the retirement");
    }
    {
        TextureFade fade;
        fade.observe(30, TextureSource::Own, 0, epoch);
        TextureFade::Sample s = fade.observe(31, TextureSource::Own, 16, epoch);
        require(s.started && s.kind == TextureFadeKind::Transcode, "the store's own texture replaced fades as a transcode");
        fade.reset();
        s = fade.observe(32, TextureSource::Own, 32, epoch);
        require(s.from == kNoTexture, "a re-locked texture made again has nothing to fade from");
        fade.observe(32, TextureSource::Own, 48, epoch);
        s = fade.observe(33, TextureSource::Own, 64, epoch + 1);
        require(s.from == kNoTexture, "a new device (epoch) forgets the handles of the old one");
    }
    {
        // The worst case: the store was drawn kTextureFadeRecentMs before the swap's first draw; the handle was retired
        // just after that last draw. The fade must end before the retirement destroys it.
        TextureRetirement retirement;
        std::vector<uint16_t> destroyed;
        const auto destroy = [&](uint16_t h) { destroyed.push_back(h); };
        TextureFade fade;
        fade.observe(40, TextureSource::Own, 0, epoch);
        retirement.retire(40, 0);
        TextureFade::Sample s = fade.observe(41, TextureSource::Own, kTextureFadeRecentMs, epoch);
        require(s.started, "a store drawn kTextureFadeRecentMs ago still fades");
        bool alive = true;
        for (uint64_t t = kTextureFadeRecentMs; t <= kTextureFadeRecentMs + kTextureFadeMs; t += 16)
        {
            retirement.collect(t, destroy);
            s = fade.observe(41, TextureSource::Own, t, epoch);
            alive = alive && (s.from == kNoTexture || destroyed.empty());
        }
        require(alive, "no draw reads a retired handle after it was destroyed");
        retirement.collect(kTextureRetireMs, destroy);
        require(destroyed.size() == 1 && destroyed[0] == 40 && !retirement.size(), "the retired handle is destroyed after kTextureRetireMs");
        retirement.retire(42, 0);
        retirement.retire(kNoTexture, 0);
        retirement.flush(destroy);
        require(destroyed.size() == 2 && !retirement.size(), "flush destroys every retired handle (shutdown)");
    }
    std::puts("PASS: texture swap fade state");
    return 0;
}
