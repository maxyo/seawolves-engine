#include "texture_fades.hpp"

#include "shim.hpp"

#include <chrono>

namespace storm::shim
{

namespace
{
uint64_t SteadyMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void Destroy(uint16_t idx)
{
    bgfx::destroy(bgfx::TextureHandle{idx});
}
} // namespace

TextureFades &texture_fades()
{
    static TextureFades instance;
    return instance;
}

TextureFades::TextureFades() : clock_(SteadyMs), now_(SteadyMs()), logged_at_(now_) {}

void TextureFades::retire(bgfx::TextureHandle handle)
{
    if (bgfx::isValid(handle))
        retirement_.retire(handle.idx, clock_());
}

void TextureFades::frame()
{
    now_ = clock_();
    retirement_.collect(now_, Destroy);
    if (counts_.empty() || now_ - logged_at_ < 1000)
        return;
    logf("texture fades: %u swaps faded over %llu ms (transcode %u, x2 %u); %u swapped at once "
         "(no free unit %u, game shader port %u, cube/volume %u); %u turned back mid-fade; %zu replaced handles kept for them",
         counts_.total_started(), static_cast<unsigned long long>(kTextureFadeMs),
         counts_.started[size_t(TextureFadeKind::Transcode)], counts_.started[size_t(TextureFadeKind::Upscale)],
         counts_.total_skipped(), counts_.no_unit, counts_.port, counts_.not_2d, counts_.reversed, retirement_.size());
    counts_ = {};
    logged_at_ = now_;
}

void TextureFades::shutdown()
{
    retirement_.flush(Destroy);
    ++epoch_; // every store's remembered handle went with the device
    counts_ = {};
}

void TextureFades::set_clock(uint64_t (*clock)())
{
    clock_ = clock ? clock : SteadyMs;
    now_ = clock_();
}

} // namespace storm::shim
