// Point sprites (D3DRS_POINTSPRITEENABLE): the texture lies across the point. The night sky's stars (weather/stars.cpp)
// are point sprites. This synthetic check proves their generated coordinates separately from actual sky visibility.
//
// Real GPU readback of a 32-pixel point with asymmetric four-quadrant texture: full, inset and off-axis
// viewports, disabled point sprites, and triangles while the point-sprite render state remains enabled.
// Run independently on GLES and Vulkan. This is renderer behavior coverage, not phone sky evidence.
#include "device.hpp"
#include "check_backend.hpp"

#include <SDL2/SDL.h>

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace storm;
using shim::Device;

namespace
{
constexpr uint32_t kSize = 64, kPoint = 32;
constexpr uint32_t kRed = 0xFFFF0000, kBlue = 0xFF0000FF, kGreen = 0xFF00FF00, kWhite = 0xFFFFFFFF;

void check(HRESULT result, const char *what)
{
    if (FAILED(result))
    {
        std::fprintf(stderr, "FAIL: %s returned 0x%08x\n", what, static_cast<unsigned>(result));
        std::exit(1);
    }
}

struct Vertex
{
    float x, y, z;
    uint32_t diffuse;
};

bool expect(const uint8_t *pixels, uint32_t x, uint32_t y, uint32_t argb, const char *what)
{
    const uint8_t *p = &pixels[(y * kSize + x) * 4]; // B, G, R, A
    const int expected[3] = {int(argb & 0xFF), int((argb >> 8) & 0xFF), int((argb >> 16) & 0xFF)};
    for (int k = 0; k < 3; ++k)
        if (std::abs(int(p[k]) - expected[k]) > 8)
        {
            std::printf("FAIL: %s: (%u, %u) shows %02x%02x%02x, expected %06x\n", what, x, y, p[2], p[1], p[0], argb & 0xFFFFFF);
            return false;
        }
    std::printf("ok: %s\n", what);
    return true;
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
    SDL_Window *window = SDL_CreateWindow("point_sprite_check", 0, 0, kSize, kSize, 0);
    if (!window || !shim::Context::instance().init(window, kSize, kSize, false))
    {
        std::fprintf(stderr, "FAIL: no selected backend window: %s\n", SDL_GetError());
        return 1;
    }
    bool ok = true;
    {
        D3DPRESENT_PARAMETERS params{};
        params.BackBufferWidth = kSize;
        params.BackBufferHeight = kSize;
        auto *device = new Device(window, params);
        Device &d = *device;
        shim::Surface *target = nullptr, *readback = nullptr, *back = nullptr;
        shim::Texture2D *texture = nullptr;
        check(d.CreateRenderTarget(kSize, kSize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr), "CreateRenderTarget");
        check(d.CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr), "CreateOffscreenPlainSurface");
        check(d.GetRenderTarget(0, &back), "GetRenderTarget");
        check(d.CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr), "CreateTexture");
        D3DLOCKED_RECT rect;
        check(texture->LockRect(0, &rect, nullptr, 0), "LockRect(texture)");
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x)
                *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(rect.pBits) + y * rect.Pitch + x * 4) = y < 8 ? (x < 8 ? kRed : kBlue) : (x < 8 ? kGreen : kWhite);
        texture->UnlockRect(0);

        struct Case {const char *name;D3DVIEWPORT9 viewport;float x,y;bool sprite,triangles;bool scaled=false;};
        const Case cases[] = {
            {"full", {0,0,64,64,0,1}, 0,0,true,false},
            {"inset", {4,8,40,40,0,1}, 0,0,true,false},
            {"off-axis", {0,0,64,64,0,1}, .25f,-.2f,true,false},
            {"distance-scaled", {0,0,64,64,0,1}, 0,0,true,false,true},
            {"disabled", {0,0,64,64,0,1}, 0,0,false,false},
            {"triangles", {0,0,64,64,0,1}, 0,0,true,true},
        };
        for (const Case &test : cases)
        // Several frames: the first ones may build the programs.
        for (int frame = 0; frame < 3; ++frame)
        {
            check(d.BeginScene(), "BeginScene");
            check(d.SetRenderTarget(0, target), "SetRenderTarget");
            check(d.SetViewport(&test.viewport), "SetViewport");
            check(d.SetDepthStencilSurface(nullptr), "SetDepthStencilSurface");
            check(d.Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0), "Clear");
            D3DMATRIX identity{};
            identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = identity.m[3][3] = 1.0f;
            d.SetTransform(D3DTS_WORLD, &identity);
            d.SetTransform(D3DTS_VIEW, &identity);
            d.SetTransform(D3DTS_PROJECTION, &identity);
            d.SetRenderState(D3DRS_ZENABLE, FALSE);
            d.SetRenderState(D3DRS_LIGHTING, FALSE);
            d.SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            d.SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            d.SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            d.SetRenderState(D3DRS_POINTSPRITEENABLE, test.sprite);
            d.SetRenderState(D3DRS_POINTSCALEENABLE, test.scaled);
            d.SetRenderState(D3DRS_POINTSCALE_A, std::bit_cast<DWORD>(0.0f));
            d.SetRenderState(D3DRS_POINTSCALE_B, std::bit_cast<DWORD>(0.0f));
            d.SetRenderState(D3DRS_POINTSCALE_C, std::bit_cast<DWORD>(1.0f));
            const float size = test.scaled ? static_cast<float>(kPoint) * .5f / test.viewport.Height : static_cast<float>(kPoint);
            d.SetRenderState(D3DRS_POINTSIZE, std::bit_cast<DWORD>(size));
            d.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            d.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            d.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            d.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            d.SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            d.SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            d.SetTexture(0, texture);
            if (test.triangles)
            {
                struct QuadVertex {float x,y,z;uint32_t diffuse;float u,v;};
                const QuadVertex quad[]={{-.75f,-.75f,.5f,0xFFFFFFFF,.25f,.25f},
                    {-.75f,.75f,.5f,0xFFFFFFFF,.25f,.25f},{.75f,.75f,.5f,0xFFFFFFFF,.25f,.25f},
                    {-.75f,-.75f,.5f,0xFFFFFFFF,.25f,.25f},{.75f,.75f,.5f,0xFFFFFFFF,.25f,.25f},
                    {.75f,-.75f,.5f,0xFFFFFFFF,.25f,.25f}};
                d.SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                check(d.DrawPrimitiveUP(D3DPT_TRIANGLELIST,2,quad,sizeof(QuadVertex)),"Draw triangles");
            }
            else
            {
                d.SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
                const Vertex centre = {test.x, test.y, 0.5f, 0xFFFFFFFF};
                check(d.DrawPrimitiveUP(D3DPT_POINTLIST, 1, &centre, sizeof(Vertex)), "Draw point");
            }
            check(d.EndScene(), "EndScene");
            check(d.GetRenderTargetData(target, readback), "GetRenderTargetData");
            if (frame == 2)
            {
                D3DLOCKED_RECT locked;
                check(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "LockRect(readback)");
                uint8_t pixels[kSize * kSize * 4];
                for (uint32_t y = 0; y < kSize; ++y)
                    std::memcpy(&pixels[y * kSize * 4], static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch, kSize * 4);
                readback->UnlockRect();
                const uint32_t cx=static_cast<uint32_t>(test.viewport.X+(test.x*.5f+.5f)*test.viewport.Width);
                const uint32_t cy=static_cast<uint32_t>(test.viewport.Y+(-test.y*.5f+.5f)*test.viewport.Height);
                std::printf("case: %s\n",test.name);
                const bool generated=test.sprite && !test.triangles;
                ok &= expect(pixels,cx-10,cy-6,kRed,"upper left");
                ok &= expect(pixels,cx+10,cy-6,generated?kBlue:kRed,"upper right");
                ok &= expect(pixels,cx-10,cy+6,generated?kGreen:kRed,"lower left");
                ok &= expect(pixels,cx+10,cy+6,generated?kWhite:kRed,"lower right");
                if(!test.triangles) {
                    ok &= expect(pixels,cx-18,cy,0xFF000000,"outside point left");
                    ok &= expect(pixels,cx+18,cy,0xFF000000,"outside point right");
                }
            }
            check(d.SetRenderTarget(0, back), "SetRenderTarget(back)");
            check(d.Present(), "Present");
        }
        texture->Release();
        target->Release();
        readback->Release();
        back->Release();
        device->Release();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf(ok ? "PASS: point sprites\n" : "FAIL: point sprites\n");
    return ok ? 0 : 1;
}
