// The in-play texture realize budget (src/shim/realize_budget.hpp).
//
// The budget itself: a frame of play makes textures while it has spent less than kFrameMs on them and always at least
// one; past that, draws are left out (swaps postponed) and counted; frames that prepare a scene have no budget and are
// not frames of play; every frame starts over and the window keeps the totals and the worst frame.
//
// Through the device (bgfx Noop backend): kTextures new textures appear in one frame of play, one 3D scene draw each.
// The frame spends about the budget on them and leaves the other draws out, counted and marked incomplete
// (storm::render::LastIncompleteFrame); later frames make the rest, all are drawn within a few frames. New textures
// while a scene is prepared (storm::loading::active) are all made in their first frame. Prints the game-thread time of
// the worst frame both ways.
#include "device.hpp"
#include "loading_transaction.hpp"
#include "realize_budget.hpp"
#include "storm/render/frame_owner.hpp"
#include "storm/render/perf_stats.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace storm;
using shim::RealizeBudget;

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

void check(HRESULT result, const char *what)
{
    if (FAILED(result))
    {
        std::fprintf(stderr, "FAIL: %s returned 0x%08x\n", what, static_cast<unsigned>(result));
        std::exit(1);
    }
}

void budget_alone()
{
    constexpr double kOver = RealizeBudget::kFrameMs + 1.0;
    RealizeBudget budget;
    RealizeBudget::Window window;

    require(budget.admit(false), "a frame of play makes its first texture");
    budget.made(kOver);
    require(!budget.admit(false), "past the budget the next texture waits");
    budget.left_out();
    budget.left_out();
    budget.postponed();
    RealizeBudget::Frame frame = budget.end_frame();
    require(frame.made == 1 && frame.left_out == 2 && frame.postponed == 1 && frame.ms == kOver && !frame.preparing,
            "the frame's totals");
    window.add(frame);

    // A scene prepared behind its loading screen: no budget, and not a frame of play.
    budget.made(kOver * 10);
    require(budget.admit(true), "a scene being prepared has no budget");
    budget.made(kOver * 10);
    frame = budget.end_frame();
    require(frame.preparing && frame.made == 2, "the preparing frame says so");
    window.add(frame);
    require(window.made == 1, "the window leaves preparing frames out");

    require(budget.admit(false), "the next frame starts over");
    budget.made(RealizeBudget::kFrameMs / 4);
    require(budget.admit(false), "under the budget another one goes");
    budget.made(RealizeBudget::kFrameMs / 4);
    budget.made(RealizeBudget::kFrameMs);
    require(!budget.admit(false), "until the frame has spent the budget");
    frame = budget.end_frame();
    require(frame.made == 3 && frame.left_out == 0 && frame.postponed == 0, "three made, none waited");
    window.add(frame);

    // A single texture over the budget, in every frame, still progresses one per frame.
    for (int i = 0; i < 3; ++i)
    {
        require(budget.admit(false), "one texture per frame at least");
        budget.made(kOver * 3);
        window.add(budget.end_frame());
    }
    window.add(budget.end_frame()); // a frame that made nothing
    require(window.made == 7 && window.left_out == 2 && window.postponed == 1 && window.frames_made == 5 &&
                window.frames_left_out == 1,
            "the window counts textures, draws left out, swaps postponed and their frames");
    require(window.worst_ms == kOver * 3, "the window keeps its worst frame");
    require(!window.empty() && RealizeBudget::Window{}.empty(), "an empty window says so");
}

// 32 uncompressed 1024x1024 textures: about 4 MB each to put into its DDS container, far over the budget together.
constexpr uint32_t kTextures = 32, kSize = 1024;

struct Vertex
{
    float x, y, z, u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_TEX1;

std::vector<shim::Texture2D *> make_textures(shim::Device &device)
{
    std::vector<shim::Texture2D *> textures;
    for (uint32_t i = 0; i < kTextures; ++i)
    {
        shim::Texture2D *texture = nullptr;
        check(device.CreateTexture(kSize, kSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr),
              "CreateTexture");
        D3DLOCKED_RECT rect;
        check(texture->LockRect(0, &rect, nullptr, 0), "LockRect");
        for (uint32_t y = 0; y < kSize; ++y)
            std::memset(static_cast<uint8_t *>(rect.pBits) + y * rect.Pitch, static_cast<int>(i + y), kSize * 4);
        texture->UnlockRect(0);
        textures.push_back(texture);
    }
    return textures;
}

// One frame: a perspective (3D scene) draw into the game picture per texture.
void frame(shim::Device &device, const std::vector<shim::Texture2D *> &textures)
{
    check(device.BeginScene(), "BeginScene");
    check(device.Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF102030, 1.0f, 0), "Clear");
    D3DMATRIX identity{}, proj{};
    identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = identity.m[3][3] = 1.0f;
    proj.m[0][0] = 1.0f, proj.m[1][1] = 1.777f, proj.m[2][2] = 1.0001f, proj.m[2][3] = 1.0f, proj.m[3][2] = -0.1f;
    device.SetTransform(D3DTS_WORLD, &identity);
    device.SetTransform(D3DTS_VIEW, &identity);
    device.SetTransform(D3DTS_PROJECTION, &proj);
    device.SetRenderState(D3DRS_LIGHTING, FALSE);
    device.SetRenderState(D3DRS_ZENABLE, TRUE);
    device.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device.SetVertexShader(nullptr);
    device.SetPixelShader(nullptr);
    device.SetFVF(kFvf);
    const Vertex quad[4] = {{-1, -1, 5, 0, 1}, {1, -1, 5, 1, 1}, {-1, 1, 5, 0, 0}, {1, 1, 5, 1, 0}};
    for (shim::Texture2D *texture : textures)
    {
        device.SetTexture(0, texture);
        check(device.DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex)), "draw");
    }
    device.SetTexture(0, nullptr);
    check(device.EndScene(), "EndScene");
    check(device.Present(), "Present");
}

struct Pass
{
    uint32_t frames = 0;     // until every texture was made
    double worst_ms = 0;     // the game-thread time of its worst frame making textures
    uint64_t left_out = 0;   // draws left out in all
    bool incomplete = false; // LastIncompleteFrame changed
};

void fail(uint32_t frame, const char *what, double a, double b)
{
    std::fprintf(stderr, "FAIL: frame %u: %s (%.2f, %.2f)\n", frame, what, a, b);
    std::exit(1);
}

Pass appear(shim::Device &device, bool preparing)
{
    std::vector<shim::Texture2D *> textures = make_textures(device);
    storm::loading::active = preparing;
    Pass pass;
    const uint32_t incomplete_at = render::LastIncompleteFrame();
    uint64_t made = 0;
    while (made < kTextures)
    {
        const render::PerfCounters start = render::PerfTotals();
        frame(device, textures);
        const render::PerfCounters f = render::PerfTotals().Since(start);
        ++pass.frames;
        made += f.texture_realizes;
        pass.left_out += f.texture_realize_waits;
        pass.worst_ms = std::max(pass.worst_ms, f.texture_realize_ms);
        if (f.draws + f.texture_realize_waits != kTextures)
            fail(pass.frames, "a draw neither submitted nor counted as left out", double(f.draws),
                 double(f.texture_realize_waits));
        if (f.texture_realizes == 0 || pass.frames > kTextures)
            fail(pass.frames, "no texture made", double(made), kTextures);
        // A draw is left out only once the frame has spent the budget.
        if (f.texture_realize_waits && f.texture_realize_ms < RealizeBudget::kFrameMs)
            fail(pass.frames, "draws left out under the budget", f.texture_realize_ms, RealizeBudget::kFrameMs);
    }
    // Every draw goes in once all are made.
    const render::PerfCounters start = render::PerfTotals();
    frame(device, textures);
    const render::PerfCounters f = render::PerfTotals().Since(start);
    require(f.draws == kTextures && f.texture_realize_waits == 0 && f.texture_realizes == 0,
            preparing ? "preparing: then every draw, nothing more to make" : "play: then every draw, nothing more to make");
    pass.incomplete = render::LastIncompleteFrame() != incomplete_at;
    storm::loading::active = false;
    for (shim::Texture2D *texture : textures)
        texture->Release();
    return pass;
}
} // namespace

int main()
{
    budget_alone();
    if (!shim::Context::instance().init_headless(1280, 720))
    {
        std::fprintf(stderr, "FAIL: headless bgfx did not start\n");
        return 1;
    }
    {
        D3DPRESENT_PARAMETERS params{};
        params.BackBufferWidth = 1280;
        params.BackBufferHeight = 720;
        auto *device = new shim::Device(nullptr, params);
        const Pass play = appear(*device, false);
        const Pass preparing = appear(*device, true);
        std::printf("%u new %ux%u textures in view at once: in play %u frames, worst frame %.1f ms making textures, "
                    "%llu draws left out; while a scene is prepared %u frame, %.1f ms\n",
                    kTextures, kSize, kSize, play.frames, play.worst_ms, static_cast<unsigned long long>(play.left_out),
                    preparing.frames, preparing.worst_ms);
        require(preparing.frames == 1 && preparing.left_out == 0 && !preparing.incomplete,
                "while a scene is prepared every texture is made in its first frame, no draw left out");
        require(play.frames > 1 && play.left_out > 0, "in play the textures are spread over frames");
        require(play.incomplete, "a frame that left a draw out is marked incomplete");
        require(play.worst_ms < preparing.worst_ms, "in play the worst frame spends less on textures");
        device->Release();
    }
    return 0;
}
