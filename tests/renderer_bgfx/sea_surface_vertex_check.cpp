// Raster check of the sea's vertex shader (shaders/include/sea_surface.sh, here through vs_port_sea3): a grid
// quad displaced on the GPU from the packed height field (src/libs/sea/include/sea_surface_gpu.hpp) must land where
// SEA's CPU formulas put it: the height of SEA::WaveXZ and the shift of the vertex along the normal, from the
// normals SEA::BuildVolumeTexture makes of the same frames. The projection maps x to the screen's x and z plus the
// height to its y; the quad's edges, measured at the middle row and column, are compared with the edges of the quad
// the CPU reference displaces, within one pixel.
//
// Then the sea's self-check (storm/render/sea_surface_check.hpp: the same shader code for points of the field, read
// back) must return the reference's heights and normals.
#include "device.hpp"
#include "ports.hpp"
#include "check_backend.hpp"
#include "../../src/libs/sea/include/sea_surface_gpu.hpp"
#include "storm/render/sea_surface_check.hpp"
#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace storm;
namespace
{
constexpr unsigned size = 512;
void check(HRESULT value)
{
    if (FAILED(value))
    {
        std::fprintf(stderr, "D3D failure %x\n", unsigned(value));
        std::exit(2);
    }
}

// Deterministic frames: smooth waves plus noise, every texel and frame different.
std::vector<std::vector<uint8_t>> make_frames()
{
    std::vector<std::vector<uint8_t>> frames(sea_gpu::kFrames, std::vector<uint8_t>(sea_gpu::kSize * sea_gpu::kSize));
    uint32_t seed = 12345;
    for (int f = 0; f < sea_gpu::kFrames; ++f)
        for (int y = 0; y < sea_gpu::kSize; ++y)
            for (int x = 0; x < sea_gpu::kSize; ++x)
            {
                seed = seed * 1664525u + 1013904223u;
                const double phase = 2.0 * 3.14159265358979 * (x / 32.0 + y / 64.0 + f / 64.0);
                const double v = 128.0 + 90.0 * std::sin(phase) + 20.0 * std::cos(3.0 * phase + y * 0.1) +
                                 static_cast<double>(seed >> 28) - 8.0;
                frames[f][x + y * sea_gpu::kSize] = static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
            }
    return frames;
}

// The CPU side, as SEA computes it: the per-texel normals of SEA::BuildVolumeTexture (x and z as shorts), the frame
// blend of SEA::FrameBlend and the bilinear sample of SEA::WaveXZ.
struct Reference
{
    const std::vector<std::vector<uint8_t>> &frames;
    std::vector<std::vector<std::array<float, 2>>> normals;
    std::vector<std::vector<uint32_t>> packed; // as SEA::BuildVolumeTexture stores them (aNormals): x, z shorts

    explicit Reference(const std::vector<std::vector<uint8_t>> &f) : frames(f)
    {
        normals.resize(sea_gpu::kFrames, std::vector<std::array<float, 2>>(sea_gpu::kSize * sea_gpu::kSize));
        packed.resize(sea_gpu::kFrames, std::vector<uint32_t>(sea_gpu::kSize * sea_gpu::kSize));
        for (int i = 0; i < sea_gpu::kFrames; ++i)
            for (int y = 0; y < sea_gpu::kSize; ++y)
                for (int x = 0; x < sea_gpu::kSize; ++x)
                {
                    const auto at = [&](int dx, int dy) {
                        return static_cast<float>(frames[i][((x + dx) & 127) + ((y + dy) & 127) * 128]) / 255.0f;
                    };
                    const float c = at(0, 0), l = at(-1, 0) - c, r = at(1, 0) - c, t = at(0, -1) - c, b = at(0, 1) - c;
                    float nx = 2.0f * l - 2.0f * r, ny = 4.0f, nz = 2.0f * t - 2.0f * b;
                    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    nx /= len;
                    nz /= len;
                    const short sx = static_cast<short>(nx * 32767.5f), sz = static_cast<short>(nz * 32767.5f);
                    normals[i][x + y * 128] = {sx / 32767.5f, sz / 32767.5f};
                    packed[i][x + y * 128] = static_cast<uint16_t>(sx) | static_cast<uint32_t>(static_cast<uint16_t>(sz)) << 16;
                }
    }

    // One wave: height (metres) and the bilinear normal x, z.
    std::array<float, 3> wave(float x, float z, float frame, float scale, float moveX, float moveZ, float amp) const
    {
        const int f1 = static_cast<int>(frame) % 64, f2 = (f1 + 1) % 64;
        const float d = frame - static_cast<float>(static_cast<int>(frame));
        const float x1 = (x + moveX) * scale, z1 = (z + moveZ) * scale;
        const int ix = static_cast<int>(std::floor(x1)), iz = static_cast<int>(std::floor(z1));
        const float fx = x1 - ix, fz = z1 - iz;
        const auto texel = [&](int i, int j) {
            const int k = (i & 127) + (j & 127) * 128;
            const float h1 = frames[f1][k] / 255.0f, h2 = frames[f2][k] / 255.0f;
            const auto &n1 = normals[f1][k], &n2 = normals[f2][k];
            return std::array<float, 3>{h1 + (h2 - h1) * d, n1[0] + (n2[0] - n1[0]) * d, n1[1] + (n2[1] - n1[1]) * d};
        };
        const auto a1 = texel(ix, iz), a2 = texel(ix + 1, iz), a3 = texel(ix, iz + 1), a4 = texel(ix + 1, iz + 1);
        std::array<float, 3> s{};
        for (int c = 0; c < 3; ++c)
            s[c] = a1[c] + fx * (a2[c] - a1[c]) + fz * (a3[c] - a1[c]) + fx * fz * (a4[c] + a1[c] - a2[c] - a3[c]);
        s[0] *= amp;
        return s;
    }
};

struct Case
{
    const char *name;
    float frame1, frame2;
    float scale1, scale2, amp1, amp2;
    float move1x, move1z, move2x, move2z;
    double originX, originZ;
    float shift;
    float cameraDistance; // from the quad; beyond 1000 m the sea is flat
};
} // namespace

int main()
{
    if (SDL_Init(SDL_INIT_VIDEO))
        return 2;
    host_check::UseBackendFromEnvironment();
    auto *window = SDL_CreateWindow("sea_surface_vertex_check", 0, 0, size, size, 0);
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
        vs->port = shim::find_port("sea_sea3_vertex_shader", true);
        auto *ps = new shim::PixelShader;
        ps->port = shim::find_port("coastfoam_foam_pixel_shader", false);
        if (!vs->port || !ps->port || !vs->port->vertex_texture)
        {
            std::puts("FAIL sea surface port not found or without its vertex texture");
            return 1;
        }

        const auto frames = make_frames();
        const Reference reference(frames);
        std::vector<const uint8_t *> heights;
        std::vector<const uint32_t *> normals;
        for (int f = 0; f < sea_gpu::kFrames; ++f)
        {
            heights.push_back(frames[f].data());
            normals.push_back(reference.packed[f].data());
        }
        const std::vector<float> texels = sea_gpu::pack_field(heights.data(), normals.data());
        shim::Texture2D *field = nullptr;
        check(d->CreateTexture(sea_gpu::kAtlasSize, sea_gpu::kAtlasSize, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED,
                               &field, nullptr));
        D3DLOCKED_RECT fieldRect{};
        check(field->LockRect(0, &fieldRect, nullptr, 0));
        for (int y = 0; y < sea_gpu::kAtlasSize; ++y)
            std::memcpy(static_cast<uint8_t *>(fieldRect.pBits) + y * fieldRect.Pitch,
                        texels.data() + static_cast<size_t>(y) * sea_gpu::kAtlasSize * 4, sea_gpu::kAtlasSize * 16);
        check(field->UnlockRect(0));
        shim::Texture2D *green = nullptr;
        check(d->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &green, nullptr));
        D3DLOCKED_RECT rect{};
        check(green->LockRect(0, &rect, nullptr, 0));
        *static_cast<uint32_t *>(rect.pBits) = 0xff00ff00;
        check(green->UnlockRect(0));

        // One grid quad, metres from the mesh's origin: corners 0 (x0, z0), 1 (x1, z0), 2 (x0, z1), 3 (x1, z1); the
        // sea's vertex (position, normal, texture coordinates: the flat grid leaves the last two 0)
        constexpr float x0 = 1.3f, x1 = 3.3f, z0 = -0.7f, z1 = 1.3f;
        const float corners[4][8] = {{x0, 0, z0}, {x1, 0, z0}, {x0, 0, z1}, {x1, 0, z1}};
        const uint16_t indices[6] = {0, 2, 1, 2, 3, 1};
        const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
            {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
        shim::VertexDeclaration *decl = nullptr;
        check(d->CreateVertexDeclaration(elements, &decl));
        shim::VertexBuffer *vb = nullptr;
        shim::IndexBuffer *ib = nullptr;
        check(d->CreateVertexBuffer(sizeof(corners), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr));
        check(d->CreateIndexBuffer(sizeof(indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, nullptr));
        void *data = nullptr;
        check(vb->Lock(0, 0, &data, 0));
        std::memcpy(data, corners, sizeof(corners));
        check(vb->Unlock());
        check(ib->Lock(0, 0, &data, 0));
        std::memcpy(data, indices, sizeof(indices));
        check(ib->Unlock());

        const Case cases[] = {
            {"flat (beyond the sea's reach)", 5.5f, 9.0f, 0.5f, 2.0f, 2.0f, 1.0f, 0, 0, 0, 0, 0.0, 0.0, 3.6f, 5000.0f},
            {"wave 1, frames 5 and 6", 5.25f, 0.0f, 0.5f, 2.0f, 2.0f, 0.0f, 0, 0, 0, 0, 0.0, 0.0, 3.6f, 10.0f},
            {"wave 2, frames 63 and 0 (wrap)", 0.0f, 63.5f, 0.5f, 2.0f, 0.0f, 1.0f, 0, 0, 0, 0, 0.0, 0.0, 3.6f, 10.0f},
            {"two waves, far origin and moves", 17.75f, 40.125f, 0.5f, 2.0f, 2.0f, 1.0f, 5123.25f, -771.5f, 33.0f,
             8020.75f, 9175.04, -2293.76, 3.6f, 10.0f},
            {"two waves, no shift", 30.5f, 2.9f, 0.4f, 1.7f, 2.5f, 0.8f, 12.0f, 3.0f, -4.0f, 7.0f, -1234.5, 777.25,
             0.0f, 10.0f},
        };
        // clip x = (x - 2.3) * 0.75, clip y = z * 0.35 + 0.2 y (D3D rows of the matrix c24..c27)
        constexpr float sx = 0.75f, cx = 2.3f, sz = 0.35f, sy = 0.2f, cz = 0.3f;
        for (const Case &test : cases)
        {
            std::array<std::array<float, 4>, 39> c{};
            float surface[sea_gpu::kRegisters][4]{};
            sea_gpu::wave_register(test.originX, test.originZ, test.move1x, test.move1z, test.scale1, test.amp1,
                                   surface[0]);
            sea_gpu::wave_register(test.originX, test.originZ, test.move2x, test.move2z, test.scale2, test.amp2,
                                   surface[1]);
            surface[2][0] = test.frame1;
            surface[2][1] = test.frame2;
            surface[2][2] = test.shift;
            surface[2][3] = 0.1f;
            surface[4][0] = 2.3f + test.cameraDistance; // the camera minus the mesh's origin
            surface[4][1] = 0.3f;
            surface[4][2] = 1000.0f * 1000.0f;
            surface[4][3] = sea_gpu::kSurfaceGpu;
            std::memcpy(c[sea_gpu::kFirstRegister].data(), surface, sizeof(surface));

            // The CPU's displaced corners on the screen (pixels, D3D: pixel centres at whole numbers), the quad
            // centred vertically
            float clipX[4], clipY[4], px[4], py[4];
            for (int i = 0; i < 4; ++i)
            {
                const double wx = test.originX + corners[i][0], wz = test.originZ + corners[i][2];
                float h = 0.0f, nx = 0.0f, nz = 0.0f;
                if (test.cameraDistance < 1000.0f)
                {
                    // SEA::WaveXZ on world positions (single precision, as the CPU)
                    const auto w1 = reference.wave(static_cast<float>(wx), static_cast<float>(wz), test.frame1,
                                                   test.scale1, test.move1x, test.move1z, test.amp1);
                    const auto w2 = reference.wave(static_cast<float>(wx), static_cast<float>(wz), test.frame2,
                                                   test.scale2, test.move2x, test.move2z, test.amp2);
                    h = w1[0] + w2[0];
                    float vx = test.scale1 * test.amp1 * w1[1] + test.scale2 * test.amp2 * w2[1];
                    float vz = test.scale1 * test.amp1 * w1[2] + test.scale2 * test.amp2 * w2[2];
                    float vy = std::sqrt(1.0f - (w1[1] * w1[1] + w1[2] * w1[2])) +
                               std::sqrt(1.0f - (w2[1] * w2[1] + w2[2] * w2[2]));
                    const float len = std::sqrt(vx * vx + vy * vy + vz * vz);
                    nx = vx / len;
                    nz = vz / len;
                }
                const float x = corners[i][0] - nx * test.shift, z = corners[i][2] - nz * test.shift;
                clipX[i] = (x - cx) * sx;
                clipY[i] = (z - cz) * sz + h * sy;
            }
            const float centre = 0.25f * (clipY[0] + clipY[1] + clipY[2] + clipY[3]);
            for (int i = 0; i < 4; ++i)
            {
                px[i] = (clipX[i] + 1.0f) * 0.5f * size;
                py[i] = (1.0f - (clipY[i] - centre)) * 0.5f * size;
            }
            c[24] = {sx, 0, 0, -cx * sx};
            c[25] = {0, sy, sz, -cz * sz - centre};
            c[26] = {0, 0, 0, 0.5f};
            c[27] = {0, 0, 0, 1};
            check(d->SetVertexShaderConstantF(0, c[0].data(), static_cast<UINT>(c.size())));

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
            d->SetTexture(0, green);
            d->SetTexture(D3DVERTEXTEXTURESAMPLER0, field);
            d->SetVertexShader(vs);
            d->SetPixelShader(ps);
            d->SetVertexDeclaration(decl);
            d->SetStreamSource(0, vb, 0, 32);
            d->SetIndices(ib);
            check(d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
            check(d->EndScene());
            check(d->GetRenderTargetData(target, readback));
            D3DLOCKED_RECT locked{};
            check(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
            const auto drawn = [&](unsigned x, unsigned y) {
                const auto *p = static_cast<const unsigned char *>(locked.pBits) + y * locked.Pitch + 4 * x;
                return p[1] > 180 && p[0] < 100 && p[2] < 100;
            };
            // The middle row's left and right ends and the middle column's top and bottom ends
            const unsigned mid = size / 2;
            int left = -1, right = -1, top = -1, bottom = -1;
            for (unsigned x = 0; x < size; ++x)
                if (drawn(x, mid))
                {
                    if (left < 0)
                        left = static_cast<int>(x);
                    right = static_cast<int>(x);
                }
            for (unsigned y = 0; y < size; ++y)
                if (drawn(mid, y))
                {
                    if (top < 0)
                        top = static_cast<int>(y);
                    bottom = static_cast<int>(y);
                }
            check(readback->UnlockRect());
            check(d->SetRenderTarget(0, back));
            check(d->Present());
            // The CPU quad's edges there: left 0-2, right 1-3 at the row's centre y; top 2-3, bottom 0-1 at the
            // column's centre x
            const float at = static_cast<float>(mid);
            const auto along = [](float a0, float b0, float a1, float b1, float a) {
                return b0 + (b1 - b0) * (a - a0) / (a1 - a0);
            };
            const float eLeft = along(py[0], px[0], py[2], px[2], at), eRight = along(py[1], px[1], py[3], px[3], at);
            const float eTop = along(px[2], py[2], px[3], py[3], at), eBottom = along(px[0], py[0], px[1], py[1], at);
            // A covered pixel's centre lies inside: the first and last covered pixels are within a pixel of the edge
            const auto near = [](int measured, float edge) {
                return measured >= 0 && std::fabs(static_cast<float>(measured) - edge) <= 1.0f;
            };
            const bool pass = near(left, eLeft) && near(right, eRight) && near(top, eTop) && near(bottom, eBottom);
            std::printf("%s sea surface, %s: left %d (%.1f) right %d (%.1f) top %d (%.1f) bottom %d (%.1f)\n",
                        pass ? "PASS" : "FAIL", test.name, left, eLeft, right, eRight, top, eTop, bottom, eBottom);
            ok &= pass;
        }

        // The self-check: points of the field through the same shader code, read back (two waves, the second across
        // the frames' wrap; a far origin and moves), against the reference.
        {
            const Case &test = cases[3];
            float surface[sea_gpu::kRegisters][4]{};
            sea_gpu::wave_register(test.originX, test.originZ, test.move1x, test.move1z, test.scale1, test.amp1,
                                   surface[0]);
            sea_gpu::wave_register(test.originX, test.originZ, test.move2x, test.move2z, test.scale2, test.amp2,
                                   surface[1]);
            surface[2][0] = test.frame1;
            surface[2][1] = 63.6f;
            surface[4][2] = 1.0e8f;
            constexpr uint32_t kPoints = render::kSeaSurfaceCheckMaxPoints;
            float points[kPoints][2];
            for (uint32_t i = 0; i < kPoints; ++i)
            {
                const float angle = static_cast<float>(i) * 2.39996f, radius = 1.37f * static_cast<float>(i) + 0.21f;
                points[i][0] = radius * std::cos(angle);
                points[i][1] = radius * std::sin(angle);
            }
            std::string why;
            bool retry = false;
            uint32_t ticket = 0;
            std::vector<render::SeaSurfaceSample> samples;
            render::SeaSurfaceCheckState state = render::SeaSurfaceCheckState::Lost;
            for (int frame = 0; frame < 20 && state != render::SeaSurfaceCheckState::Done; ++frame)
            {
                check(d->BeginScene());
                if (!ticket)
                {
                    ticket = render::StartSeaSurfaceCheck(field, &surface[0][0], &points[0][0], kPoints, why, retry);
                    if (!ticket)
                        std::printf("     self-check not started: %s%s\n", why.c_str(), retry ? " (retry)" : "");
                }
                check(d->EndScene());
                check(d->Present());
                if (ticket)
                    state = render::PollSeaSurfaceCheck(ticket, samples);
            }
            float worstHeight = 0.0f, worstNormal = 0.0f;
            bool pass = state == render::SeaSurfaceCheckState::Done && samples.size() == kPoints;
            for (uint32_t i = 0; pass && i < kPoints; ++i)
            {
                const float wx = static_cast<float>(test.originX + points[i][0]);
                const float wz = static_cast<float>(test.originZ + points[i][1]);
                const auto w1 = reference.wave(wx, wz, test.frame1, test.scale1, test.move1x, test.move1z, test.amp1);
                const auto w2 = reference.wave(wx, wz, 63.6f, test.scale2, test.move2x, test.move2z, test.amp2);
                const float h = w1[0] + w2[0];
                const float vx = test.scale1 * test.amp1 * w1[1] + test.scale2 * test.amp2 * w2[1];
                const float vz = test.scale1 * test.amp1 * w1[2] + test.scale2 * test.amp2 * w2[2];
                const float vy = std::sqrt(1.0f - (w1[1] * w1[1] + w1[2] * w1[2])) +
                                 std::sqrt(1.0f - (w2[1] * w2[1] + w2[2] * w2[2]));
                const float len = std::sqrt(vx * vx + vy * vy + vz * vz);
                const auto &s = samples[i];
                worstHeight = std::max(worstHeight, std::fabs(s.height - h));
                worstNormal = std::max({worstNormal, std::fabs(s.nx - vx / len), std::fabs(s.ny - vy / len),
                                        std::fabs(s.nz - vz / len)});
            }
            pass = pass && worstHeight <= 1e-3f && worstNormal <= 1e-3f;
            std::printf("%s sea surface self-check (%s): %zu points, largest difference height %.6f m, normal %.6f\n",
                        pass ? "PASS" : "FAIL", render::SeaSurfaceCheckDevice().c_str(), samples.size(), worstHeight,
                        worstNormal);
            ok &= pass;
        }

        d->SetVertexShader(nullptr);
        d->SetPixelShader(nullptr);
        d->SetStreamSource(0, nullptr, 0, 0);
        d->SetIndices(nullptr);
        d->SetVertexDeclaration(nullptr);
        d->SetTexture(0, nullptr);
        d->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
        vs->Release();
        ps->Release();
        decl->Release();
        vb->Release();
        ib->Release();
        field->Release();
        green->Release();
        target->Release();
        readback->Release();
        back->Release();
        d->Release();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
}
