// Raster check of the coast foam's vertex shader (shaders/ports/vs_port_coastfoam.sc): the sea's height under a
// strand from the packed height frames (foam_gpu.hpp) must be the height SEA::WaveXZ computes on the CPU. A strand
// lies across the target; the projection moves it up by the height, so the band's rows give the height back.
#include "device.hpp"
#include "ports.hpp"
#include "check_backend.hpp"
#include "../../src/libs/island/src/foam_gpu.hpp"
#include <SDL2/SDL.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace storm;
namespace
{
constexpr unsigned size = 256;
constexpr float kPixelsPerMetre = 1.28f; // clip y per metre 0.01, 128 pixels per clip unit
void check(HRESULT value)
{
    if (FAILED(value))
    {
        std::fprintf(stderr, "D3D failure %x\n", unsigned(value));
        std::exit(2);
    }
}

// The frame texels: frame f is f * 3 everywhere, except frame 7, whose texel x is 2 * x (a ramp along x).
uint8_t texel(int frame, int x)
{
    return static_cast<uint8_t>(frame == 7 ? 2 * x : frame * 3);
}

// SEA::WaveXZ of one wave on these frames (the CPU reference).
float wave_xz(float x, float frame, float scale, float move, float amplitude)
{
    const float x1 = (x + move) * scale;
    const int ix = static_cast<int>(std::floor(x1));
    const float fx = x1 - ix;
    const int f1 = static_cast<int>(frame) % 64, f2 = (f1 + 1) % 64;
    const float d = frame - std::floor(frame);
    const auto at = [&](int f, int i) { return static_cast<float>(texel(f, i & 127)) / 255.0f; };
    const auto blended = [&](int i) { return at(f1, i) + (at(f2, i) - at(f1, i)) * d; };
    return amplitude * (blended(ix) + fx * (blended(ix + 1) - blended(ix)));
}
} // namespace

int main()
{
    if (SDL_Init(SDL_INIT_VIDEO))
        return 2;
    host_check::UseBackendFromEnvironment();
    auto *window = SDL_CreateWindow("coast_foam_vertex_check", 0, 0, size, size, 0);
    if (!window || !shim::Context::instance().init(window, size, size, false))
        return 2;
    bool ok = true;
    {
        D3DPRESENT_PARAMETERS params{};
        params.BackBufferWidth = params.BackBufferHeight = size;
        auto *d = new shim::Device(window, params);
        shim::Surface *target = nullptr, *readback = nullptr, *back = nullptr;
        check(d->CreateRenderTarget(size, size, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr));
        check(d->CreateOffscreenPlainSurface(size, size, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
        check(d->GetRenderTarget(0, &back));
        auto *vs = new shim::VertexShader;
        vs->port = shim::find_port("coastfoam_foam_vertex_shader", true);
        auto *ps = new shim::PixelShader;
        ps->port = shim::find_port("coastfoam_foam_pixel_shader", false);
        if (!vs->port || !ps->port)
        {
            std::puts("FAIL coast foam ports not found");
            return 1;
        }

        // The height frames as CoastFoam uploads them
        std::vector<std::vector<uint8_t>> frames(foam_gpu::kFrames, std::vector<uint8_t>(128 * 128));
        std::vector<const uint8_t *> pointers;
        for (int f = 0; f < foam_gpu::kFrames; ++f)
        {
            for (int i = 0; i < 128 * 128; ++i)
                frames[f][i] = texel(f, i % 128);
            pointers.push_back(frames[f].data());
        }
        const std::vector<uint32_t> packed = foam_gpu::pack_frames(pointers.data());
        shim::VolumeTexture *heights = nullptr;
        check(d->CreateVolumeTexture(128, 128, foam_gpu::kSlices, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &heights,
                                     nullptr));
        D3DLOCKED_BOX box{};
        check(heights->LockBox(0, &box, nullptr, 0));
        for (int s = 0; s < foam_gpu::kSlices; ++s)
            for (int y = 0; y < 128; ++y)
                std::memcpy(static_cast<uint8_t *>(box.pBits) + s * box.SlicePitch + y * box.RowPitch,
                            packed.data() + (s * 128 + y) * 128, 128 * 4);
        check(heights->UnlockBox(0));
        shim::Texture2D *white = nullptr;
        check(d->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &white, nullptr));
        D3DLOCKED_RECT rect{};
        check(white->LockRect(0, &rect, nullptr, 0));
        *static_cast<uint32_t *>(rect.pBits) = 0xff00ff00;
        check(white->UnlockRect(0));

        // One strand of two columns at x = -0.5 and 0.5, rows from z = 0 to z = 1 (move 1)
        std::vector<foam_gpu::Vertex> vertices;
        for (int row = 0; row < foam_gpu::kRows; ++row)
            for (int x = 0; x < 2; ++x)
                vertices.push_back({x - 0.5f, 0.0f, x - 0.5f, 1.0f, 0.0f, static_cast<uint8_t>(row), 4, 0, 0});
        std::vector<uint32_t> indices;
        for (uint32_t y = 0; y + 1 < foam_gpu::kRows; ++y)
            for (uint32_t i : {0u, 2u, 1u, 2u, 3u, 1u})
                indices.push_back(y * 2 + i);
        const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            {0, 20, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
            D3DDECL_END()};
        shim::VertexDeclaration *decl = nullptr;
        check(d->CreateVertexDeclaration(elements, &decl));
        shim::VertexBuffer *vb = nullptr;
        shim::IndexBuffer *ib = nullptr;
        check(d->CreateVertexBuffer(vertices.size() * sizeof(foam_gpu::Vertex), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
                                    &vb, nullptr));
        check(d->CreateIndexBuffer(indices.size() * 4, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT, &ib,
                                   nullptr));
        void *data = nullptr;
        check(vb->Lock(0, 0, &data, 0));
        std::memcpy(data, vertices.data(), vertices.size() * sizeof(foam_gpu::Vertex));
        check(vb->Unlock());
        check(ib->Lock(0, 0, &data, 0));
        std::memcpy(data, indices.data(), indices.size() * 4);
        check(ib->Unlock());

        struct Case
        {
            const char *name;
            float frame1, frame2, amp1, amp2, move1, cameraX;
        };
        // move1 places x = 0 at texel 10.25 of frame 7's ramp; the camera far away flattens the sea (WaveXZ's reach).
        const Case cases[] = {
            {"flat (beyond the sea's reach)", 5.5f, 0.0f, 100.0f, 0.0f, 0.0f, 1.0e4f},
            {"frames 5 and 6 blended", 5.5f, 0.0f, 100.0f, 0.0f, 0.0f, 0.0f},
            {"frames 63 and 0 (wrap)", 63.5f, 0.0f, 100.0f, 0.0f, 0.0f, 0.0f},
            {"two waves", 2.25f, 9.0f, 60.0f, 40.0f, 0.0f, 0.0f},
            {"ramp texel 10.25", 7.0f, 0.0f, 100.0f, 0.0f, 10.25f, 0.0f},
        };
        float flatRow = 0.0f;
        for (const Case &test : cases)
        {
            std::array<std::array<float, 4>, foam_gpu::kRegisters> c{};
            // clip x = 2 x, clip y = (z - 0.5) * 0.5 + 0.01 * y (columns of view * projection)
            c[0] = {2, 0, 0, 0};
            c[1] = {0, 0.01f, 0, 0};
            c[2] = {0, 0.5f, 0, 0};
            c[3] = {0, -0.25f, 0.5f, 1};
            const float scale = 1.0f;
            foam_gpu::wave_register(scale, test.move1, 0.0f, test.amp1, c[4].data());
            foam_gpu::wave_register(scale, 0.0f, 0.0f, test.amp2, c[5].data());
            c[6] = {test.frame1, test.frame2, 0.0f, 0.0f};
            c[7] = {test.cameraX, 0.0f, 1000.0f * 1000.0f, 0.0f};
            c[8] = {1.0f, 1.0f, 0.0f, 1.0f};
            check(d->SetVertexShaderConstantF(0, c[0].data(), foam_gpu::kRegisters));
            check(d->BeginScene());
            check(d->SetRenderTarget(0, target));
            check(d->SetDepthStencilSurface(nullptr));
            check(d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1, 0));
            d->SetRenderState(D3DRS_ZENABLE, FALSE);
            d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            d->SetRenderState(D3DRS_LIGHTING, FALSE);
            d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            d->SetRenderState(D3DRS_FOGENABLE, FALSE);
            d->SetTexture(0, white);
            d->SetTexture(1, heights);
            for (DWORD stage : {0u, 1u})
            {
                d->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                d->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                d->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            }
            d->SetVertexShader(vs);
            d->SetPixelShader(ps);
            d->SetVertexDeclaration(decl);
            d->SetStreamSource(0, vb, 0, sizeof(foam_gpu::Vertex));
            d->SetIndices(ib);
            check(d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, vertices.size(), 0, indices.size() / 3));
            check(d->EndScene());
            check(d->GetRenderTargetData(target, readback));
            D3DLOCKED_RECT locked{};
            check(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
            // The band's rows at the centre column (x = 0)
            int first = -1, last = -1;
            for (unsigned y = 0; y < size; ++y)
            {
                const auto *p = static_cast<const unsigned char *>(locked.pBits) + y * locked.Pitch + 4 * (size / 2);
                if (p[1] > 180 && p[0] < 100 && p[2] < 100)
                {
                    if (first < 0)
                        first = static_cast<int>(y);
                    last = static_cast<int>(y);
                }
            }
            check(readback->UnlockRect());
            check(d->SetRenderTarget(0, back));
            check(d->Present());
            const float centre = 0.5f * (first + last);
            if (test.cameraX > 1000.0f)
                flatRow = centre;
            const float expected = test.cameraX > 1000.0f
                                       ? 0.0f
                                       : wave_xz(0.0f, test.frame1, scale, test.move1, test.amp1) +
                                             wave_xz(0.0f, test.frame2, scale, 0.0f, test.amp2);
            const float measured = std::fabs(centre - flatRow) / kPixelsPerMetre;
            const bool pass = first >= 0 && last - first >= 50 && std::fabs(measured - expected) <= 1.5f;
            std::printf("%s coast foam height, %s: band rows %d..%d, %.2f m (expected %.2f m)\n",
                        pass ? "PASS" : "FAIL", test.name, first, last, measured, expected);
            ok &= pass;
        }
        d->SetVertexShader(nullptr);
        d->SetPixelShader(nullptr);
        d->SetStreamSource(0, nullptr, 0, 0);
        d->SetIndices(nullptr);
        d->SetVertexDeclaration(nullptr);
        d->SetTexture(0, nullptr);
        d->SetTexture(1, nullptr);
        vs->Release();
        ps->Release();
        decl->Release();
        vb->Release();
        ib->Release();
        heights->Release();
        white->Release();
        target->Release();
        readback->Release();
        back->Release();
        d->Release();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
}
