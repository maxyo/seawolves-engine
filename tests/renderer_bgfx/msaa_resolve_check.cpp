// Vulkan: multisampled colour targets resolved by their render pass, and a view's colour discard honoured for the
// whole pass it ends (patches/bgfx-vulkan-resolve-in-pass.patch).
//
// bgfx stored every multisampled colour attachment and resolved it after the pass with vkCmdResolveImage: on a tile
// GPU the whole 4x scene and the UI's 2x display-size layers were written to memory and read back each frame, and a
// discard (BGFX_CLEAR_DISCARD_COLOR_0, which the shim and the UI set on a target's last view) could not be honoured.
// bgfx also took a render pass's store ops from the view that began it, though the pass runs on through the next
// views into the same frame buffer: the discard of the last scene view never reached the store op.
//
// Through bgfx directly, in an SDL window on Vulkan (STORM_CHECK_BACKEND=vulkan; on OpenGL ES it says SKIP):
// - one view clears a 4x target red and discards it: the resolved texture is red, resolved in the pass, no copy;
// - a pass through two views (clear green, then a view without a clear that discards): green, one pass begun with
//   the discard (the lookahead), resolved in the pass;
// - the same frame buffer with another target's view between (two passes): the first pass keeps its samples (no
//   discard: the second pass loads them), the second discards; the picture is the first pass's blue.
// The counters are the patch's own (bgfx::vk::g_storm*).
#include "check_backend.hpp"
#include "device.hpp"

#include <SDL2/SDL.h>

#include <bgfx/bgfx.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace bgfx::vk
{
extern std::atomic<uint32_t> g_stormResolvesInPass;
extern std::atomic<uint32_t> g_stormResolveCopies;
extern std::atomic<uint32_t> g_stormColourDiscardPasses;
} // namespace bgfx::vk

using namespace storm;

namespace
{
constexpr uint16_t kSize = 64;

void frame(shim::Context &context, uint32_t *number = nullptr)
{
    if (!context.run_frame(BGFX_FRAME_FLUSH, number))
    {
        std::printf("FAIL: no frame ran\n");
        std::exit(1);
    }
}

struct Counters
{
    uint32_t in_pass, copies, discards;
    static Counters Now()
    {
        return {bgfx::vk::g_stormResolvesInPass.load(), bgfx::vk::g_stormResolveCopies.load(),
                bgfx::vk::g_stormColourDiscardPasses.load()};
    }
};

// The resolved picture's centre pixel, RGBA, after the frame's views (blitted from the multisampled texture).
uint32_t ReadCentre(shim::Context &context, bgfx::TextureHandle msaa, bgfx::ViewId blit_view)
{
    const bgfx::TextureHandle readback =
        bgfx::createTexture2D(kSize, kSize, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
    bgfx::TextureRegion source, destination;
    source.init(msaa);
    destination.init(readback);
    bgfx::blit(blit_view, destination, source);
    std::vector<uint8_t> pixels(size_t(kSize) * kSize * 4, 0);
    bgfx::TextureRegion region;
    region.init(readback);
    const uint32_t ready = bgfx::read(region, pixels.data());
    for (uint32_t number = 0; number < ready;)
        frame(context, &number);
    bgfx::destroy(readback);
    frame(context);
    const uint8_t *p = &pixels[(size_t(kSize / 2) * kSize + kSize / 2) * 4];
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

bool Near(uint32_t got, uint32_t expected)
{
    for (int shift = 0; shift < 32; shift += 8)
        if (std::abs(int((got >> shift) & 0xFF) - int((expected >> shift) & 0xFF)) > 2)
            return false;
    return true;
}

void Touch(bgfx::ViewId view, bgfx::FrameBufferHandle fb, uint16_t clear, uint32_t rgba)
{
    bgfx::setViewFrameBuffer(view, fb);
    bgfx::setViewRect(view, 0, 0, kSize, kSize);
    bgfx::setViewClear(view, clear, rgba);
    bgfx::touch(view);
}
} // namespace

int main()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::fprintf(stderr, "FAIL: SDL video: %s (an X display is needed)\n", SDL_GetError());
        return 1;
    }
    host_check::UseBackendFromEnvironment();
    SDL_Window *window = SDL_CreateWindow("msaa_resolve_check", 0, 0, kSize, kSize, 0);
    shim::Context &context = shim::Context::instance();
    if (!window || !context.init(window, kSize, kSize, false))
    {
        std::fprintf(stderr, "FAIL: no render window: %s\n", SDL_GetError());
        return 1;
    }
    frame(context);
    bool ok = true;
    const auto expect = [&ok](bool passed, const char *what, uint32_t got, uint32_t want, Counters before, Counters after) {
        std::printf("%s: %s: picture %08x (expected %08x); resolves in the pass +%u, copies +%u, passes begun with a colour "
                    "discard +%u\n",
                    passed ? "PASS" : "FAIL", what, got, want, after.in_pass - before.in_pass, after.copies - before.copies,
                    after.discards - before.discards);
        ok &= passed;
    };
    if (bgfx::getRendererType() != bgfx::RendererType::Vulkan)
        std::printf("SKIP: the render backend is not Vulkan: the check is about bgfx's Vulkan backend\n");
    else
    {
        // The 4x scene colour (sampled: a single-sample image beside it) and another target.
        const bgfx::TextureHandle msaa =
            bgfx::createTexture2D(kSize, kSize, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT_MSAA_X4);
        const bgfx::FrameBufferHandle msaa_fb = bgfx::createFrameBuffer(1, &msaa, false);
        const bgfx::TextureHandle other = bgfx::createTexture2D(kSize, kSize, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
        const bgfx::FrameBufferHandle other_fb = bgfx::createFrameBuffer(1, &other, false);
        frame(context);
        constexpr uint32_t kRed = 0xE02010FF, kGreen = 0x20C040FF, kBlue = 0x1030D0FF, kGrey = 0x808080FF;

        // One view that clears and discards.
        Counters before = Counters::Now();
        Touch(0, msaa_fb, BGFX_CLEAR_COLOR | BGFX_CLEAR_DISCARD_COLOR_0, kRed);
        uint32_t got = ReadCentre(context, msaa, 1);
        Counters after = Counters::Now();
        expect(Near(got, kRed) && after.copies == before.copies && after.in_pass > before.in_pass && after.discards == before.discards + 1,
               "one view clears the 4x target and discards it", got, kRed, before, after);

        // One pass through two views: the second one's discard is the pass's.
        before = Counters::Now();
        Touch(0, msaa_fb, BGFX_CLEAR_COLOR, kGreen);
        Touch(1, msaa_fb, BGFX_CLEAR_DISCARD_COLOR_0, 0);
        got = ReadCentre(context, msaa, 2);
        after = Counters::Now();
        expect(Near(got, kGreen) && after.copies == before.copies && after.in_pass > before.in_pass && after.discards == before.discards + 1,
               "a pass through two views takes the last one's discard", got, kGreen, before, after);

        // Another target's view between: two passes; the first keeps its samples for the second, which loads them.
        before = Counters::Now();
        Touch(0, msaa_fb, BGFX_CLEAR_COLOR, kBlue);
        Touch(1, other_fb, BGFX_CLEAR_COLOR, kGrey);
        Touch(2, msaa_fb, BGFX_CLEAR_DISCARD_COLOR_0, 0);
        got = ReadCentre(context, msaa, 3);
        after = Counters::Now();
        expect(Near(got, kBlue) && after.copies == before.copies && after.discards == before.discards + 1,
               "a pass cut by another target's view keeps its samples; the last pass discards", got, kBlue, before, after);

        for (bgfx::ViewId view = 0; view < 4; ++view)
            bgfx::setViewClear(view, BGFX_CLEAR_NONE);
        bgfx::destroy(other_fb);
        bgfx::destroy(other);
        bgfx::destroy(msaa_fb);
        bgfx::destroy(msaa);
        frame(context);
    }
    context.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
}
