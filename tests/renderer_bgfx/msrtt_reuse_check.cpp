// A render target made in the texture slot of a freed multisampled one (patches/bgfx-gl-msaa-render-to-texture.patch).
//
// bgfx keeps its TextureGL objects and hands a freed texture handle to the next texture. The patch once kept the
// EXT_multisampled_render_to_texture sample count in the slot: after «Высокая» (4x MSAA scene at a render scale) ->
// «Низкая» (single-sample scene) the new single-sample scene colour took the old one's slot and was attached as a 4x
// MSRTT colour next to its single-sample depth texture. The framebuffer was incomplete and the 3D scene stayed black
// under the interface (Pixel 10 Pro, PowerVR D-Series, OpenGL ES).
//
// Through OpenGL ES in an SDL window, with bgfx directly: a 4x MSAA colour target is drawn and freed, then a
// single-sample colour target that takes its handle, with a single-sample sampled D24S8 depth (the scaled scene's
// stores, Device::create_scene_stores), is cleared green and read back. Needs a GL with
// EXT_multisampled_render_to_texture: run.py runs it on the host GPU through VirtualGL when STORM_VIRTUALGL is set.
// Desktop drivers fail the patch's mixed-renderbuffer probe, so the check asks for MSRTT without it
// (g_stormMsrttWithoutMixedProbe, a logged test override); without the extension it says SKIP and why.
#include "device.hpp"
#include "storm/render/frame_owner.hpp"

#include <SDL2/SDL.h>

#include <bgfx/bgfx.h>

#include <atomic>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace bgfx::gl
{
extern std::atomic<bool> g_stormMsrttActive;
extern std::atomic<bool> g_stormMsrttWithoutMixedProbe;
// bgfx's EGL import (glcontext_egl.cpp) on Linux looks every function up in libGLESv2 with dlsym, which has no
// extension entry points: the check takes this one from eglGetProcAddress (the import keeps a non-null pointer).
using FramebufferTexture2DMultisample = void (*)(unsigned, unsigned, unsigned, unsigned, int, int);
extern FramebufferTexture2DMultisample glFramebufferTexture2DMultisampleEXT;
} // namespace bgfx::gl

using namespace storm;

namespace
{
constexpr uint16_t kSize = 64;
constexpr uint32_t kGreen = 0x20C040FF; // RGBA

void frame(shim::Context &context, uint32_t *number = nullptr)
{
    if (!context.run_frame(BGFX_FRAME_FLUSH, number))
    {
        std::printf("FAIL: no frame ran\n");
        std::exit(1);
    }
}
} // namespace

int main()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::fprintf(stderr, "FAIL: SDL video: %s (an X display is needed)\n", SDL_GetError());
        return 1;
    }
    bgfx::gl::g_stormMsrttWithoutMixedProbe.store(true);
    if (void *egl = dlopen("libEGL.so.1", RTLD_NOW))
        if (auto get = reinterpret_cast<void *(*)(const char *)>(dlsym(egl, "eglGetProcAddress")))
            bgfx::gl::glFramebufferTexture2DMultisampleEXT =
                reinterpret_cast<bgfx::gl::FramebufferTexture2DMultisample>(get("glFramebufferTexture2DMultisampleEXT"));
    render::SetRenderBackend(render::Backend::OpenGLES, {});
    SDL_Window *window = SDL_CreateWindow("msrtt_reuse_check", 0, 0, kSize, kSize, 0);
    shim::Context &context = shim::Context::instance();
    if (!window || !context.init(window, kSize, kSize, false))
    {
        std::fprintf(stderr, "FAIL: no OpenGL ES window: %s\n", SDL_GetError());
        return 1;
    }
    frame(context);
    bool ok = true;
    if (!bgfx::gl::g_stormMsrttActive.load())
        std::printf("SKIP: this OpenGL ES (%s) has no usable EXT_multisampled_render_to_texture: nothing to check "
                    "(run.py runs it on the host GPU when STORM_VIRTUALGL is set)\n",
                    context.device_info().renderer.c_str());
    else
    {
        // The 4x scene colour of «Высокая»: sampled, single level, so attached with MSRTT. Drawn once, then freed.
        const bgfx::TextureHandle msaa =
            bgfx::createTexture2D(kSize, kSize, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT_MSAA_X4);
        const bgfx::FrameBufferHandle msaa_fb = bgfx::createFrameBuffer(1, &msaa, false);
        bgfx::setViewFrameBuffer(0, msaa_fb);
        bgfx::setViewRect(0, 0, 0, kSize, kSize);
        bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0xFF0000FF);
        bgfx::touch(0);
        frame(context);
        bgfx::destroy(msaa_fb);
        bgfx::destroy(msaa);
        frame(context);
        frame(context);

        // The single-sample scene of «Низкая»: colour and a sampled depth, like the scaled scene stores.
        const bgfx::TextureHandle color = bgfx::createTexture2D(kSize, kSize, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
        const bgfx::TextureHandle depth = bgfx::createTexture2D(kSize, kSize, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT);
        if (color.idx != msaa.idx)
        {
            std::printf("FAIL: the new colour target got texture handle %u, not the freed multisampled one's %u: the "
                        "check no longer reproduces the reuse\n",
                        color.idx, msaa.idx);
            ok = false;
        }
        const bgfx::TextureHandle attachments[] = {color, depth};
        const bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(2, attachments, false);
        const bgfx::TextureHandle readback = bgfx::createTexture2D(kSize, kSize, false, 1, bgfx::TextureFormat::RGBA8,
                                                                   BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        bgfx::setViewFrameBuffer(1, fb);
        bgfx::setViewRect(1, 0, 0, kSize, kSize);
        bgfx::setViewClear(1, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, kGreen, 1.0f, 0);
        bgfx::touch(1);
        bgfx::TextureRegion source, destination;
        source.init(color);
        destination.init(readback);
        bgfx::blit(2, destination, source);
        std::vector<uint8_t> pixels(size_t(kSize) * kSize * 4, 0);
        bgfx::TextureRegion region;
        region.init(readback);
        const uint32_t ready = bgfx::read(region, pixels.data());
        for (uint32_t number = 0; number < ready;)
            frame(context, &number);
        const uint8_t *p = &pixels[(size_t(kSize / 2) * kSize + kSize / 2) * 4];
        const uint8_t expected[4] = {0x20, 0xC0, 0x40, 0xFF};
        bool green = true;
        for (int c = 0; c < 4; ++c)
            green &= std::abs(int(p[c]) - int(expected[c])) <= 2;
        std::printf("%s: the single-sample target in the freed MSRTT target's slot shows %02x%02x%02x%02x, expected "
                    "%02x%02x%02x%02x%s\n",
                    green ? "PASS" : "FAIL", p[0], p[1], p[2], p[3], expected[0], expected[1], expected[2], expected[3],
                    green ? "" : " (attached as a multisampled colour next to a single-sample depth: incomplete)");
        ok &= green;
        bgfx::destroy(readback);
        bgfx::destroy(fb);
        bgfx::destroy(color);
        bgfx::destroy(depth);
        frame(context);
    }
    context.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
}
