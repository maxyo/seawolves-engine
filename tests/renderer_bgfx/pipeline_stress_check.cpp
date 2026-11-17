// Vulkan's asynchronous pipelines under load (patches/bgfx-vulkan-pipeline-cache.patch, src/shim/pipeline_sets.hpp),
// through the shim in an SDL window: the paths where a pipeline job's inputs can go while a pipeline thread uses them.
// Each frame draws six 3D scene draws, every one with a state no frame before had (120 blend x cull x depth-test
// combinations), so their pipelines are created on the pipeline threads while the draws are left out; between them:
//   * read-back frames (GetRenderTargetData: a flush frame takes queued pipelines back to the render thread and waits
//     for the ones a thread is creating);
//   * scene changes (BeginScenePipelines): the recorded pipelines of the other scene are prewarmed behind the draws;
//   * in four more starts, prewarms with programs the check made itself from the same shaders (registered for the
//     recorded shader names), destroyed while their prewarms are queued or being created (the drain of a program);
//   * a device shut down with pipelines still pending (the drain of every job).
// Every start ends with nothing pending and the draws of its last frames all drawn: on the pipeline threads' progress,
// never a frame or time budget (run.py's deadlock guard reports a hang with the step it printed last). Also worth
// running under AddressSanitizer and ThreadSanitizer (a tree with -fsanitize=... for every library, bgfx included), on
// the GPU's driver and on lavapipe (VK_ICD_FILENAMES=.../lvp_icd.json).
#include "device.hpp"

#include "check_backend.hpp"

#include <SDL2/SDL.h>
#include "storm/render/effects.hpp"
#include "storm/render/frame_owner.hpp"
#include "storm/render/pipelines.hpp"
#include "storm/render/program_cache.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

// patches/bgfx-vulkan-pipeline-cache.patch.
namespace bgfx
{
uint32_t getVkDrawsLeftOut();
uint32_t getVkPipelinesPending();
} // namespace bgfx

using namespace storm;
using shim::Device;
namespace fs = std::filesystem;

namespace
{

constexpr uint32_t kWidth = 96, kHeight = 64;
constexpr int kDrawsPerFrame = 6;
constexpr int kCombos = 120; // 10 blends x 3 cull modes x 4 depth tests

void check(HRESULT result, const char *what)
{
    if (FAILED(result))
    {
        std::fprintf(stderr, "FAIL: %s returned 0x%08x\n", what, static_cast<unsigned>(result));
        std::exit(1);
    }
}

bool expect(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

void step(const char *what)
{
    std::printf("step: %s\n", what);
    std::fflush(stdout);
}

struct SceneVertex
{
    float x, y, z;
    uint32_t color;
};
struct UiVertex
{
    float x, y, z, rhw;
    uint32_t color;
};

constexpr DWORD kBlends[][2] = {{D3DBLEND_ONE, D3DBLEND_ZERO},          {D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA},
                                {D3DBLEND_ONE, D3DBLEND_ONE},           {D3DBLEND_DESTCOLOR, D3DBLEND_ZERO},
                                {D3DBLEND_SRCALPHA, D3DBLEND_ONE},      {D3DBLEND_ZERO, D3DBLEND_SRCCOLOR},
                                {D3DBLEND_INVDESTCOLOR, D3DBLEND_ONE},  {D3DBLEND_ONE, D3DBLEND_INVSRCALPHA},
                                {D3DBLEND_SRCCOLOR, D3DBLEND_DESTCOLOR}, {D3DBLEND_INVSRCCOLOR, D3DBLEND_ZERO}};
constexpr DWORD kCulls[] = {D3DCULL_NONE, D3DCULL_CW, D3DCULL_CCW};
constexpr DWORD kDepthTests[] = {D3DCMP_LESSEQUAL, D3DCMP_ALWAYS, D3DCMP_GREATER, D3DCMP_EQUAL};

void combo(Device &d, int which)
{
    which %= kCombos;
    d.SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d.SetRenderState(D3DRS_SRCBLEND, kBlends[which % 10][0]);
    d.SetRenderState(D3DRS_DESTBLEND, kBlends[which % 10][1]);
    d.SetRenderState(D3DRS_CULLMODE, kCulls[which / 10 % 3]);
    d.SetRenderState(D3DRS_ZFUNC, kDepthTests[which / 30 % 4]);
}

struct Renderer
{
    Device *device = nullptr;
    shim::Surface *back = nullptr, *readback = nullptr;
    explicit Renderer(SDL_Window *window)
    {
        if (!shim::Context::instance().init(window, kWidth, kHeight, false))
        {
            std::fprintf(stderr, "FAIL: no renderer\n");
            std::exit(1);
        }
        D3DPRESENT_PARAMETERS params{};
        params.BackBufferWidth = kWidth;
        params.BackBufferHeight = kHeight;
        device = new Device(window, params);
        check(device->CreateOffscreenPlainSurface(kWidth, kHeight, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr),
              "CreateOffscreenPlainSurface");
        check(device->GetRenderTarget(0, &back), "GetRenderTarget");
    }
    ~Renderer()
    {
        readback->Release();
        back->Release();
        device->Release(); // shuts the context down with the device
        shim::Context::instance().shutdown();
    }

    // One frame: kDrawsPerFrame scene draws with the combinations from `first` on, then an interface quad; with
    // `read_back` the frame is read back (a flush frame on Vulkan).
    void frame(int first, bool read_back = false)
    {
        Device &d = *device;
        check(d.BeginScene(), "BeginScene");
        check(d.Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF2050A0, 1.0f, 0), "Clear");
        D3DMATRIX identity{};
        identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = identity.m[3][3] = 1.0f;
        D3DMATRIX world = identity;
        world.m[3][2] = 5.0f;
        D3DMATRIX projection{};
        projection.m[0][0] = static_cast<float>(kHeight) / kWidth;
        projection.m[1][1] = 1.0f;
        projection.m[2][2] = 100.0f / 99.0f;
        projection.m[2][3] = 1.0f;
        projection.m[3][2] = -100.0f / 99.0f;
        d.SetTransform(D3DTS_WORLD, &world);
        d.SetTransform(D3DTS_VIEW, &identity);
        d.SetTransform(D3DTS_PROJECTION, &projection);
        d.SetRenderState(D3DRS_LIGHTING, FALSE);
        d.SetRenderState(D3DRS_ZENABLE, TRUE);
        d.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        d.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        d.SetTexture(0, nullptr);
        const SceneVertex tri[3] = {{-4, -4, 0, 0xFFC08040}, {4, -4, 0, 0xFF40C080}, {0, 4, 0, 0xFF8040C0}};
        d.SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        for (int i = 0; i < kDrawsPerFrame; ++i)
        {
            combo(d, first + i);
            check(d.DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(SceneVertex)), "draw (scene)");
        }
        render::EffectsSceneComplete();
        d.SetRenderState(D3DRS_ZENABLE, FALSE);
        d.SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d.SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        const UiVertex ui[3] = {{0, 0, 0, 1, 0xFFE0E0E0}, {24, 0, 0, 1, 0xFFE0E0E0}, {0, 16, 0, 1, 0xFFE0E0E0}};
        d.SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        check(d.DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, ui, sizeof(UiVertex)), "draw (interface quad)");
        check(d.EndScene(), "EndScene");
        if (read_back)
            check(d.GetRenderTargetData(back, readback), "GetRenderTargetData");
        check(d.Present(), "Present");
    }

    // Frames of combinations `first`.. until nothing is pending, then four more: how many of their draws were left out.
    uint32_t settle(int first)
    {
        while (bgfx::getVkPipelinesPending() != 0)
            frame(first);
        const uint32_t before = bgfx::getVkDrawsLeftOut();
        for (int i = 0; i < 4; ++i)
            frame(first);
        return bgfx::getVkDrawsLeftOut() - before;
    }
};

// The shader names of the pipelines recorded in pipelines.bin (src/shim/pipeline_sets.cpp's format: magic, version,
// count, then per pipeline its hash, vertex and fragment shader names, recipe).
std::set<std::pair<std::string, std::string>> recorded_programs(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    std::set<std::pair<std::string, std::string>> names;
    size_t at = 8 + 4;
    const auto u32 = [&](uint32_t &value) {
        if (at + 4 > bytes.size())
            return false;
        std::memcpy(&value, bytes.data() + at, 4);
        at += 4;
        return true;
    };
    const auto text = [&](std::string &value) {
        uint16_t n = 0;
        if (at + 2 > bytes.size())
            return false;
        std::memcpy(&n, bytes.data() + at, 2);
        at += 2;
        if (at + n > bytes.size())
            return false;
        value.assign(bytes.data() + at, n);
        at += n;
        return true;
    };
    uint32_t count = 0;
    if (!u32(count))
        return names;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t hash = 0, size = 0;
        std::string vs, fs;
        if (!u32(hash) || !text(vs) || !text(fs) || !u32(size) || at + size > bytes.size())
            break;
        at += size;
        names.emplace(vs, fs);
    }
    return names;
}

fs::path find_sets(const fs::path &root)
{
    for (const auto &item : fs::recursive_directory_iterator(root))
        if (item.path().filename() == "pipelines.bin")
            return item.path();
    return {};
}

bool vulkan(SDL_Window *window, const fs::path &root)
{
    bool ok = true;
    {
        // 1. Two scenes record their pipelines: every frame's draws new, read-backs and scene changes in between.
        step("first start: new pipelines every frame, read-backs, scene changes");
        Renderer r(window);
        int next = 0;
        for (int f = 0; next < kCombos; ++f)
        {
            if (f % 5 == 0)
                render::BeginScenePipelines(f % 10 == 0 ? "stress/a" : "stress/b", 0);
            r.frame(next, f % 3 == 2);
            next += kDrawsPerFrame;
        }
        step("first start: settling");
        ok &= expect(r.settle(0) == 0, "first start: every draw appears once its pipeline exists");
    }
    const fs::path sets = find_sets(root);
    ok &= expect(!sets.empty(), "the scenes' pipelines are recorded");
    const auto programs = recorded_programs(sets);
    std::printf("     recorded programs: %zu\n", programs.size());
    ok &= expect(!programs.empty(), "the recorded pipelines name their shaders");
    // 2. Prewarms with the check's own programs, destroyed while their pipelines are queued or being created: a new
    // device each round (a created pipeline is found by its shaders and state, whatever the program, so a second round
    // on the same device prewarms nothing), destroyed after one to three frames.
    uint32_t destroyed_pending = 0;
    for (int round = 0; round < 4; ++round)
    {
        step("prewarmed programs destroyed while their pipelines are pending");
        Renderer r(window);
        render::FrameOwner &owner = shim::Context::instance();
        std::vector<bgfx::ProgramHandle> made;
        for (const auto &[vs, fs] : programs)
        {
            const uint32_t v = shim::shader_id_by_name(vs.c_str()), p = shim::shader_id_by_name(fs.c_str());
            if (v == UINT32_MAX || p == UINT32_MAX)
                continue; // not the shim's (the fx package's passes)
            const bgfx::ProgramHandle program =
                bgfx::createProgram(shim::create_embedded_shader(v), shim::create_embedded_shader(p), true);
            owner.RegisterPipelineProgram(vs.c_str(), fs.c_str(), program.idx);
            made.push_back(program);
        }
        if (round == 0)
            ok &= expect(!made.empty(), "the recorded shaders are the shim's embedded ones");
        render::BeginScenePipelines(round % 2 ? "stress/b" : "stress/a", 0);
        // The prewarms are asked for at the next frame and queued by the render thread a frame later; draws of new
        // combinations go in front of them.
        for (int f = 0; f < 1 + round % 3; ++f)
            r.frame(round * 7 + f, round == 1 && f == 1);
        const uint32_t pending = bgfx::getVkPipelinesPending();
        std::printf("     round %d: %zu programs destroyed with %u pipelines pending\n", round, made.size(), pending);
        destroyed_pending += pending;
        for (const bgfx::ProgramHandle program : made)
        {
            owner.UnregisterPipelineProgram(program.idx);
            bgfx::destroy(program);
        }
        r.frame(round * 7 + 5);
        ok &= expect(r.settle(0) == 0, "prewarmed programs destroyed: every draw appears once its pipeline exists");
    }
    ok &= expect(destroyed_pending > 0, "programs were destroyed while pipelines were pending");
    {
        // 3. A device shut down while prewarms and waiting draws are pending.
        step("shut down with pipelines pending");
        Renderer r(window);
        render::BeginScenePipelines("stress/a", 0);
        r.frame(3);
        r.frame(60);
        std::printf("     pending at shutdown: %u\n", bgfx::getVkPipelinesPending());
    }
    {
        // 4. And once more: nothing left of the earlier starts gets in the way.
        step("a start after them");
        Renderer r(window);
        render::BeginScenePipelines("stress/b", 0);
        for (int f = 0; f < 10; ++f)
            r.frame(f * 11, f % 4 == 3);
        ok &= expect(r.settle(1) == 0, "a start after them: every draw appears once its pipeline exists");
    }
    return ok;
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
    // NVIDIA's driver keeps compiled pipelines of its own across runs: every run must start without them.
    setenv("__GL_SHADER_DISK_CACHE", "0", 1);
    SDL_Window *window = SDL_CreateWindow("pipeline_stress_check", 0, 0, kWidth, kHeight, 0);
    if (!window)
    {
        std::fprintf(stderr, "FAIL: no window: %s\n", SDL_GetError());
        return 1;
    }
    const fs::path root = fs::temp_directory_path() / ("storm-pipeline-stress-check-" + std::to_string(getpid()));
    fs::remove_all(root);
    render::ConfigureProgramCache({root, 32ull << 20});
    bool ok = true;
    if (host_check::VulkanAsked())
        ok = vulkan(window, root);
    else
    {
        // OpenGL ES has no pipelines: the same frames draw and nothing is pending.
        Renderer r(window);
        for (int f = 0; f < 4; ++f)
            r.frame(f * kDrawsPerFrame, f == 2);
        ok &= expect(bgfx::getVkPipelinesPending() == 0, "OpenGL ES: nothing pending");
    }
    fs::remove_all(root);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf("%s: pipeline_stress_check\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
