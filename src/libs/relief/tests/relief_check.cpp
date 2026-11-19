// Host check of the relief preparation with the real PBRify models (storm_relief_core): a small synthetic game,
// prepared (a stone texture's relief with its material, a window glass's own gloss map), marked complete, keyed as the game
// keys it, resumed without work, stale maps pruned, cancelled; the neural ×2 (the upscaler, the maps of a small texture
// made from its ×2 and the ×2 kept for the game, a character's ×2 alone). With --memory: the CPU networks' memory bound.
#include "relief.hpp"

#include "backend.hpp"
#include "upscale.hpp"
#include "storm/fx/upscale.hpp"
#include "storm/fx/gloss_map.hpp"
#include "storm/fx/relief_model.hpp"
#include "storm/fx/height_map.hpp"
#include "storm/fx/relief_store.hpp"

#include <malloc.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
int failures = 0;
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);                                        \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

// An A8R8G8B8 .tx with all mips: bricks (light faces, dark mortar).
std::vector<uint8_t> WriteTx(const std::filesystem::path &path, int size)
{
    std::vector<uint8_t> file(24);
    const int32_t levels = static_cast<int32_t>(std::log2(size)) + 1;
    const int32_t header[6] = {0, size, size, levels, 21, size * size * 4};
    std::memcpy(file.data(), header, sizeof(header));
    std::vector<uint8_t> probe;
    for (int s = size, level = 0; level < levels; s /= 2, ++level)
    {
        const size_t start = file.size();
        for (int y = 0; y < s; ++y)
            for (int x = 0; x < s; ++x)
            {
                const int fx = x * size / s, fy = y * size / s;
                const bool mortar = fy % 16 < 2 || (fx + (fy / 16 % 2) * 16) % 32 < 2;
                const uint8_t v = mortar ? 50 : 190;
                file.insert(file.end(), {static_cast<uint8_t>(v * 0.7f), static_cast<uint8_t>(v * 0.8f), v, 255});
            }
        if (s <= storm::fx::kReliefProbeSize && probe.empty())
            probe.assign(file.begin() + static_cast<std::ptrdiff_t>(start), file.end());
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
    return probe;
}

long RssAnonKb()
{
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);)
        if (line.rfind("RssAnon:", 0) == 0)
            return std::atol(line.c_str() + 8);
    return -1;
}

// The CPU networks' memory bound (backend.hpp kCpuNetworkBytes) on the game's largest jobs, each a session's first on the
// CPU (the game's device while the player sees the scene): a 1024 x 1024 character's ×2 alone, a 1024 x 1024 stone
// texture's relief (three networks) and a 256 x 256 one's (its maps made from its ×2). Before the bound one ×2 took 440 MB
// and one relief texture 1.3..1.7 GB (ncnn's per-network pools, kept until the session ended). Each job: the networks'
// peak (the allocator's) within the bound, the process's anonymous memory within the bound and the job's own buffers
// (images, planes, the finalization), and back down after it. glibc returns large blocks at once here, as Android's
// allocator does (its dynamic threshold would otherwise keep freed blocks resident).
int CheckMemory(const std::filesystem::path &root, const storm::relief::Models &models)
{
    using namespace storm;
    mallopt(M_MMAP_THRESHOLD, 1 << 20);
    constexpr long kJobBuffersKb = 64 << 10;
    const std::filesystem::path textures = root / "RESOURCE/Textures";
    WriteTx(textures / "Characters/Large.tga.tx", 1024);
    WriteTx(textures / "Locations/StoneLarge.tga.tx", 1024);
    WriteTx(textures / "Locations/StoneSmall.tga.tx", 256);
    struct Case { const char *file; bool x2_only; };
    for (const Case &c : {Case{"Characters/Large.tga.tx", true}, Case{"Locations/StoneLarge.tga.tx", false},
                          Case{"Locations/StoneSmall.tga.tx", false}})
    {
        std::string error;
        const std::unique_ptr<relief::Session> session = relief::Session::Open(models, 4, root / "device", error);
        CHECK(session);
        if (!session)
            continue;
        session->SetGame([] { return false; }, [] { return true; });
        const relief::UpscaleOutput display{root / "upscale", false};
        const long before = RssAnonKb();
        std::atomic<bool> done{false};
        std::atomic<long> peak{before};
        std::thread sampler([&] {
            while (!done)
            {
                peak = std::max(peak.load(), RssAnonKb());
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
        const auto start = std::chrono::steady_clock::now();
        std::string failure;
        const relief::Outcome outcome =
            c.x2_only ? session->Upscale(textures, textures / c.file, display, 0, failure)
                      : session->PrepareFile(textures, textures / c.file, root / "relief", fx::kReliefMapSize, failure,
                                             &display);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        done = true;
        sampler.join();
        const long after = RssAnonKb();
        const size_t networks = session->stats().cpu_memory_peak;
        std::printf("memory: %s %s on %s: networks' peak %.0f MB of %zu, anonymous memory +%ld MB at most (bound %zu + %ld "
                    "MB), +%ld MB after, %.0f ms\n",
                    c.file, c.x2_only ? "×2" : "relief", session->stats().provider.c_str(), double(networks) / 1048576.0,
                    relief::kCpuNetworkBytes >> 20, (peak - before) >> 10, relief::kCpuNetworkBytes >> 20, kJobBuffersKb >> 10,
                    (after - before) >> 10, ms);
        CHECK(outcome == relief::Outcome::Prepared && !session->stats().gpu);
        CHECK(networks > 0 && networks <= relief::kCpuNetworkBytes);
        CHECK(before > 0 && peak - before <= static_cast<long>(relief::kCpuNetworkBytes >> 10) + kJobBuffersKb);
        CHECK(after - before <= 16 << 10);
    }
    std::printf("memory checks: %d failures\n", failures);
    return failures ? 1 : 0;
}

// Pruning validates cache ownership without inference or loading model files.
void CheckDemandPrune(const std::filesystem::path &root)
{
    using namespace storm;
    const auto textures = root / "textures", output = root / "maps";
    const auto character = textures / "Characters/Sharl1.tga.tx";
    const auto deleted = textures / "Characters/Removed.tga.tx";
    const auto probe = WriteTx(character, 128);
    WriteTx(deleted, 128);
    WriteTx(textures / "StoneWall.tga.tx", 64);
    std::filesystem::create_directories(output);
    const auto method = fx::ReliefMethod(fx::ReliefClass::Character);
    const auto key = fx::ReliefKey("characters/sharl1", probe, 64, 64, method, fx::kReliefMapSize);
    const auto old = fx::ReliefKey("characters/sharl1", probe, 64, 64, method ^ 1, fx::kReliefMapSize);
    const auto gone = fx::ReliefKey("characters/removed", probe, 64, 64, method, fx::kReliefMapSize);
    for (const auto k : {key, old, gone})
        std::ofstream(fx::ReliefFile(output, k), std::ios::binary) << "existing demand map";
    std::filesystem::remove(deleted);
    const auto background = relief::List(textures);
    CHECK(background.size() == 1 && background.front().filename() == "StoneWall.tga.tx");
    // Real permission failures, not a missing source: never delete any map on uncertainty.
    const auto permissions = std::filesystem::status(character).permissions();
    std::filesystem::permissions(character, std::filesystem::perms::none);
    if (geteuid() != 0)
    {
        CHECK(!std::ifstream(character, std::ios::binary).good());
        CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == -1);
        CHECK(std::filesystem::exists(fx::ReliefFile(output, key)));
        CHECK(std::filesystem::exists(fx::ReliefFile(output, old)));
    }
    std::filesystem::permissions(character, permissions);
    const auto directory_permissions = std::filesystem::status(character.parent_path()).permissions();
    std::filesystem::permissions(character.parent_path(), std::filesystem::perms::none);
    if (geteuid() != 0)
    {
        CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == -1);
        CHECK(std::filesystem::exists(fx::ReliefFile(output, key)));
    }
    std::filesystem::permissions(character.parent_path(), directory_permissions);
    std::filesystem::resize_file(character, 24 + 128 * 128 / 2 + 1); // incomplete 64px probe
    CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == -1);
    CHECK(std::filesystem::exists(fx::ReliefFile(output, key)));
    WriteTx(character, 128);
    CHECK(relief::Prune(root / "missing-texture-root", background, output, fx::kReliefMapSize) == -1);
    CHECK(std::filesystem::exists(fx::ReliefFile(output, key)));
    const auto output_permissions = std::filesystem::status(output).permissions();
    if (geteuid() != 0)
    {
        std::filesystem::permissions(output, std::filesystem::perms::none);
        std::error_code denied;
        std::filesystem::directory_iterator unreadable(output, denied);
        CHECK(bool(denied));
        CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == -1);
        std::filesystem::permissions(output, output_permissions);
        CHECK(std::filesystem::exists(fx::ReliefFile(output, key)));
        std::filesystem::permissions(output, std::filesystem::perms::owner_read |
                                                std::filesystem::perms::owner_exec);
        CHECK(!std::filesystem::remove(fx::ReliefFile(output, old), denied) && bool(denied));
        CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == -1);
        std::filesystem::permissions(output, output_permissions);
        CHECK(std::filesystem::exists(fx::ReliefFile(output, key)));
        CHECK(std::filesystem::exists(fx::ReliefFile(output, old)));
    }
    CHECK(relief::Prune(textures, background, root / "missing-output", fx::kReliefMapSize) == -1);
    CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == 2);
    CHECK(std::filesystem::exists(fx::ReliefFile(output, key)));
    CHECK(!std::filesystem::exists(fx::ReliefFile(output, old)));
    CHECK(!std::filesystem::exists(fx::ReliefFile(output, gone)));
    std::ifstream kept(fx::ReliefFile(output, key), std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(kept), {}) == "existing demand map");
    const auto stone_probe = WriteTx(textures / "StoneWall.tga.tx", 64);
    const auto stone_key = fx::ReliefKey("stonewall", stone_probe, 64, 64,
                                       fx::ReliefMethod(fx::ReliefClass::Stone), fx::kReliefMapSize);
    std::ofstream(fx::ReliefFile(output, stone_key), std::ios::binary) << "background map";
    CHECK(relief::Prune(textures, {}, output, fx::kReliefMapSize) == 0);
    CHECK(std::filesystem::exists(fx::ReliefFile(output, stone_key)));
    CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == 0);
    // Deterministic removal failure even under root; no production fault-injection hook.
    const auto blocked = output / "blocked.relief";
    std::filesystem::create_directory(blocked);
    std::ofstream(blocked / "child") << "not an empty stale entry";
    CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == -1);
    CHECK(std::filesystem::exists(fx::ReliefFile(output, key)));
    CHECK(std::filesystem::exists(fx::ReliefFile(output, stone_key)));
    std::filesystem::remove_all(blocked);
    CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == 0);
    // Stock Ammo/EnvAmmo: a readable DXT1 cube is unsupported, not an I/O failure.
    const auto cube = textures / "Ammo/EnvAmmo.tga.tx";
    std::filesystem::create_directories(cube.parent_path());
    const uint32_t cube_header[6] = {2, 32, 32, 4, 0x31545844, 512};
    std::vector<uint8_t> cube_bytes(24 + 6 * (512 + 128 + 32 + 8), 0);
    std::memcpy(cube_bytes.data(), cube_header, sizeof(cube_header));
    std::ofstream(cube, std::ios::binary).write(reinterpret_cast<const char *>(cube_bytes.data()), cube_bytes.size());
    CHECK(relief::List(textures).size() == background.size() + 1);
    std::ofstream(fx::ReliefFile(output, old)) << "stale";
    CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == 1);
    CHECK(std::filesystem::exists(fx::ReliefFile(output, key)));
    CHECK(std::filesystem::exists(fx::ReliefFile(output, stone_key)));
    CHECK(!std::filesystem::exists(fx::ReliefFile(output, old)));
    // Unsupported must still be structurally complete: a truncated cube is uncertainty.
    std::ofstream(fx::ReliefFile(output, old)) << "stale";
    std::filesystem::resize_file(cube, cube_bytes.size() - 1);
    CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == -1);
    CHECK(std::filesystem::exists(fx::ReliefFile(output, old)));
    CHECK(std::filesystem::exists(fx::ReliefFile(output, key)));
    // Compressed 2x2/1x1 mips still need a whole DXT1 block each, not quarter-sized bytes.
    const uint32_t tiny_header[6] = {2, 4, 4, 3, 0x31545844, 8};
    std::vector<uint8_t> tiny_cube(24 + 6 * (8 + 2), 0);
    std::memcpy(tiny_cube.data(), tiny_header, sizeof(tiny_header));
    std::ofstream(cube, std::ios::binary).write(reinterpret_cast<const char *>(tiny_cube.data()), tiny_cube.size());
    CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == -1);
    CHECK(std::filesystem::exists(fx::ReliefFile(output, old)));
    std::filesystem::remove(cube);
    CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == 1);
    std::filesystem::remove(character);
    CHECK(relief::Prune(textures, background, output, fx::kReliefMapSize) == 1);
    CHECK(!std::filesystem::exists(fx::ReliefFile(output, key)));
    CHECK(relief::List(textures) == background);
}
} // namespace

int main(int argc, char **argv)
{
    using namespace storm;
    if (argc == 2 && std::strcmp(argv[1], "--prune-only") == 0)
    {
        const auto root = std::filesystem::temp_directory_path() /
                          ("storm_relief_prune_check-" + std::to_string(getpid()));
        std::filesystem::remove_all(root);
        CheckDemandPrune(root);
        std::filesystem::remove_all(root);
        std::printf("demand prune checks: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    if (argc < 2)
    {
        std::printf("usage: %s <model dir> [--memory]\n", argv[0]);
        return 2;
    }
    const auto read = [&](const char *name) {
        std::ifstream in(std::string(argv[1]) + "/" + name, std::ios::binary);
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(in), {}};
    };
    std::vector<std::vector<uint8_t>> files;
    for (const char *name : relief::kModelFiles)
        files.push_back(read(name));
    const relief::Models all{files[0], files[1], files[2], files[3], files[4], files[5], files[6], files[7]};
    // Its own folder: gates of several worktrees run this check at once.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("storm_relief_check-" + std::to_string(getpid()));
    std::filesystem::remove_all(root);
    if (argc == 3 && std::strcmp(argv[2], "--memory") == 0)
    {
        const int status = CheckMemory(root, all);
        std::filesystem::remove_all(root);
        relief::ReleaseGpu();
        return status;
    }
    CheckDemandPrune(root / "demand-prune");
    const std::filesystem::path textures = root / "RESOURCE/Textures", output = root / "relief";
    const std::vector<uint8_t> probe = WriteTx(textures / "Locations/StoneWall.tga.tx", 128);
    const std::vector<uint8_t> bottle_probe = WriteTx(textures / "Locations/WindowGlass.tga.tx", 64); // glass: gloss only, no relief
    WriteTx(textures / "Characters/Face.tga.tx", 64); // no relief, no gloss by the rules

    relief::Job job;
    job.textures = textures;
    job.output = output;
    // The device the session measures (the GPU where the host has a Vulkan compute device); both are checked below.
    job.models = all;
    job.map_size = 128;
    job.threads = 2;
    if (argc == 3 && std::strcmp(argv[2], "--bulk-only") == 0)
    {
        // Obstruct only the first write, then restore the directory before Prune.
        // This is independent of uid/permissions and uses the real CPU Session/Prepare.
        int visited = 0;
        const auto failed = relief::Prepare(job, [&](const relief::Progress &state) {
            if (state.done == 0)
            {
                std::filesystem::remove_all(output);
                std::ofstream(output) << "temporary output obstruction";
            }
            else if (state.done == 1)
            {
                std::filesystem::remove(output);
                std::filesystem::create_directory(output);
            }
            visited = state.done;
            return false;
        });
        CHECK(failed.failed == 1 && failed.prepared == 1 && visited == 2);
        CHECK(failed.status == 2 && !failed.error.empty());
        const auto retry = relief::Prepare(job, {});
        CHECK(retry.status == 0 && retry.failed == 0 && retry.prepared == 1 && retry.existing == 1);
        std::filesystem::remove_all(root);
        std::printf("bulk completion checks: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    CHECK(relief::List(textures).size() == 2);
    int calls = 0;
    relief::Result result = relief::Prepare(job, [&](const relief::Progress &) { return ++calls, false; });
    CHECK(result.status == 0 && result.prepared == 2 && result.failed == 0 && result.total == 2 && calls == 3);
    CHECK(result.provider.rfind("CPU (ncnn)", 0) == 0 || result.provider.rfind("GPU", 0) == 0);
    // By name, only relief and gloss textures.
    {
        const std::filesystem::path game = root / "ordered";
        WriteTx(game / "RESOURCE/Textures/StoneB.tga.tx", 64);
        WriteTx(game / "RESOURCE/Textures/StoneA.tga.tx", 64);
        WriteTx(game / "RESOURCE/Textures/Characters/Face.tga.tx", 64);
        const std::vector<std::filesystem::path> order = relief::List(game / "RESOURCE/Textures");
        CHECK(order.size() == 2 && order[0].filename() == "StoneA.tga.tx" && order[1].filename() == "StoneB.tga.tx");
    }
    // The game's key: path, probe level (64x64 here), the stone class's method, size.
    const uint64_t key = fx::ReliefKey("locations/stonewall", probe, 64, 64, fx::ReliefMethod(fx::ReliefClass::Stone), job.map_size);
    const std::optional<fx::NormalMap> map = fx::ReadReliefMap(output, key);
    CHECK(map && map->width == 128 && map->mip_count == 8 && map->format == fx::NormalMapFormat::EacRg11);
    // A stone texture's map holds its material texture (EAC RG11: height, gloss; the same mips) and the stone's depth.
    CHECK(map && map->material.size() == map->rg.size() && map->depth == fx::ReliefDepthTexels(fx::ReliefClass::Stone) * 0.25f);
    // The window glass: no relief, its own gloss map (EAC RG11: metal mask, gloss) at its size, all mips.
    const uint64_t gloss_key = fx::ReliefKey("locations/windowglass", bottle_probe, 64, 64, fx::GlossMethod(fx::GlossClass::Glass),
                                             fx::kGlossMapSize);
    const std::optional<fx::NormalMap> gloss = fx::ReadGlossMap(output, gloss_key);
    CHECK(gloss && gloss->width == 64 && gloss->mip_count == 7 && gloss->format == fx::NormalMapFormat::EacRg11);
    // Resumed: nothing to do; a stale map from another game is pruned after the complete run.
    fx::WriteReliefMap(output, 42, *map);
    result = relief::Prepare(job, nullptr);
    CHECK(result.status == 0 && result.prepared == 0 && result.existing == 2 && result.removed == 1 && !fx::HasReliefMap(output, 42));
    // The system evicts part of the cache: the next pass makes the missing map again.
    std::filesystem::remove(fx::ReliefFile(output, key));
    result = relief::Prepare(job, nullptr);
    CHECK(result.status == 0 && result.prepared == 1 && result.existing == 1);
    // Cancelled at the start: nothing prepared, nothing pruned.
    std::filesystem::remove(fx::ReliefFile(output, key));
    fx::WriteReliefMap(output, 42, *map);
    result = relief::Prepare(job, [](const relief::Progress &) { return true; });
    CHECK(result.status == 1 && result.prepared == 0 && fx::HasReliefMap(output, 42));
    // The neural ×2 (fx/upscale.hpp). The upscaler itself on the bricks: twice the size, the same colours when boxed back
    // (the network sharpens, it does not shift), and no seam where the wrapped texture tiles.
    {
        std::string error;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        CHECK(relief::ReadTextureLevel(textures / "Locations/StoneWall.tga.tx", 1 << 16, rgba, w, h) && w == 128);
        std::vector<uint8_t> rolled(rgba.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                std::memcpy(&rolled[(size_t(y) * w + (x + 37) % w) * 4], &rgba[(size_t(y) * w + x) * 4], 4);
        const auto cpu_runtime = relief::CpuRuntime::Open(2, error);
        std::vector<uint8_t> cpu_x2;
        // On the CPU, and on the GPU where the host has a Vulkan compute device (ncnn, the same graph).
        for (int device = 0; device < (relief::Gpu().available ? 2 : 1); ++device)
        {
            std::unique_ptr<relief::Network> network =
                device == 0 ? (cpu_runtime ? cpu_runtime->Load(files[6], files[7], fx::kUpscalePad, 4, error) : nullptr)
                            : relief::LoadGpuNetwork(files[6], files[7], fx::kUpscalePad, 4, error);
            CHECK(network);
            if (!network)
                continue;
            const std::string describe = network->Describe();
            relief::Upscaler upscaler(std::move(network));
            std::vector<uint8_t> x2, x2_rolled;
            CHECK(upscaler.Run(rgba, w, h, true, x2, error) && x2.size() == size_t(4 * w * h * 4));
            if (x2.size() != size_t(4 * w * h * 4))
                continue;
            double diff = 0.0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 3; ++c)
                    {
                        int sum = 0;
                        for (int d = 0; d < 4; ++d)
                            sum += x2[((size_t(2 * y + d / 2) * 2 * w) + 2 * x + d % 2) * 4 + c];
                        diff += std::abs(sum / 4.0 - rgba[(size_t(y) * w + x) * 4 + c]);
                    }
            diff /= double(w) * h * 3;
            // No seam: the ×2 of the texture shifted by 37 texels is the ×2 shifted by 74 (wrapped padding beyond the
            // network's reach makes every texel see the same neighbours; on the GPU its own tiles must join too).
            int worst = 0;
            CHECK(upscaler.Run(rolled, w, h, true, x2_rolled, error) && x2_rolled.size() == x2.size());
            for (int y = 0; y < 2 * h && x2_rolled.size() == x2.size(); ++y)
                for (int x = 0; x < 2 * w; ++x)
                    for (int c = 0; c < 3; ++c)
                        worst = std::max(worst, std::abs(int(x2_rolled[(size_t(y) * 2 * w + (x + 74) % (2 * w)) * 4 + c]) -
                                                         int(x2[(size_t(y) * 2 * w + x) * 4 + c])));
            // The GPU against the CPU, the same graph: only their arithmetic differs (fp16 on the GPU; 57..59 dB on game
            // textures).
            double psnr = 0.0;
            if (device == 0)
                cpu_x2 = x2;
            else
            {
                double squared = 0.0;
                for (size_t i = 0; i < x2.size(); ++i)
                    squared += (i % 4 == 3) ? 0.0 : double(x2[i] - cpu_x2[i]) * double(x2[i] - cpu_x2[i]);
                psnr = 10.0 * std::log10(255.0 * 255.0 / std::max(squared / (x2.size() * 0.75), 1e-9));
            }
            std::printf("x2 on %s: boxed back %.2f/255 from the source; shifted by 37 texels: at most %d/255 off%s\n",
                        describe.c_str(), diff, worst,
                        device ? (", against the CPU: PSNR " + std::to_string(psnr) + " dB").c_str() : "");
            CHECK(diff < 8.0 && worst <= 3 && (device == 0 || psnr > 45.0));
        }
    }
    // With a display output the maps of a small texture come from its ×2, which is kept for the game under the game's key.
    {
        const std::filesystem::path x2_dir = root / "upscale";
        std::filesystem::remove(fx::ReliefFile(output, key));
        job.display = relief::UpscaleOutput{x2_dir, false};
        result = relief::Prepare(job, nullptr);
        const uint64_t x2_key = fx::ReliefKey("locations/stonewall", probe, 64, 64, fx::UpscaleMethod(false), 128);
        const std::optional<std::vector<uint8_t>> ktx = fx::ReadUpscaled(x2_dir, x2_key);
        CHECK(result.prepared == 1 && ktx && ktx->size() > 12 && std::memcmp(ktx->data() + 1, "KTX 11", 6) == 0);
        // KTX 1.1: pixelWidth at 36, mip levels at 56.
        uint32_t width = 0, levels = 0;
        if (ktx && ktx->size() > 60)
        {
            std::memcpy(&width, ktx->data() + 36, 4);
            std::memcpy(&levels, ktx->data() + 56, 4);
        }
        CHECK(width == 256 && levels == 9);
        job.display.reset();
        // The ×2 alone: a character's texture (no relief, no gloss) gets one, once; a texture without a target none.
        std::string error;
        const std::unique_ptr<relief::Session> session = relief::Session::Open(all, 2,
                                                                               root / "device", error);
        CHECK(session);
        const relief::UpscaleOutput display{x2_dir, true};
        WriteTx(textures / "Weather/Sky/Sky1.tga.tx", 64);
        std::string failure;
        CHECK(session && session->Upscale(textures, textures / "Characters/Face.tga.tx", display, 64, failure) == relief::Outcome::Prepared);
        CHECK(session && session->Upscale(textures, textures / "Characters/Face.tga.tx", display, 64, failure) == relief::Outcome::Existing);
        CHECK(session && session->Upscale(textures, textures / "Weather/Sky/Sky1.tga.tx", display, 0, failure) == relief::Outcome::Skipped);
    }
    // A broken model is an error, not a crash.
    const std::vector<uint8_t> junk(100, 7);
    job.models = all;
    job.models.roughness_bin = junk;
    CHECK(relief::Prepare(job, nullptr).status == 2);
    job.models = all;
    job.models.normal_param = {};
    CHECK(relief::Prepare(job, nullptr).status == 2);
    // The GPU against the CPU (ncnn, the same graphs), network by network, on a brick pattern with the game's padding:
    // the same maps within fp16's error, and the GPU's tiles join.
    if (relief::Gpu().available)
    {
        std::string error;
        const auto cpu_runtime = relief::CpuRuntime::Open(2, error);
        const int size = 200 + 2 * fx::kReliefModelPad; // two tiles across, the second one partial
        std::vector<float> image(static_cast<size_t>(size) * size * 3);
        for (int c = 0; c < 3; ++c)
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                {
                    const bool mortar = y % 16 < 2 || (x + (y / 16 % 2) * 16) % 32 < 2;
                    image[(static_cast<size_t>(c) * size + y) * size + x] = (mortar ? 0.2f : 0.7f) * (0.8f + 0.1f * c);
                }
        for (int n = 0; n < 3; ++n)
        {
            const auto cpu = cpu_runtime ? cpu_runtime->Load(files[2 * n], files[2 * n + 1], fx::kReliefModelPad, 1, error) : nullptr;
            const auto gpu = relief::LoadGpuNetwork(files[2 * n], files[2 * n + 1], fx::kReliefModelPad, 1, error);
            std::vector<float> a(image.size()), b(image.size());
            CHECK(cpu && gpu && cpu->Infer(image.data(), size, size, a.data(), error) &&
                  gpu->Infer(image.data(), size, size, b.data(), error));
            double squared = 0.0;
            size_t count = 0;
            for (int c = 0; c < 3; ++c)
                for (int y = fx::kReliefModelPad; y < size - fx::kReliefModelPad; ++y)
                    for (int x = fx::kReliefModelPad; x < size - fx::kReliefModelPad; ++x)
                    {
                        const size_t i = (static_cast<size_t>(c) * size + y) * size + x;
                        const double d = std::clamp(a[i], 0.0f, 1.0f) - std::clamp(b[i], 0.0f, 1.0f);
                        squared += d * d;
                        ++count;
                    }
            const double psnr = 10.0 * std::log10(1.0 / std::max(squared / static_cast<double>(count), 1e-12));
            std::printf("%s: %s against the CPU: PSNR %.1f dB\n", relief::kModelFiles[2 * n],
                        gpu ? gpu->Describe().c_str() : "-", psnr);
            CHECK(psnr > 45.0);
        }
    }
    else
        std::printf("no GPU check: %s\n", relief::Gpu().why.c_str());
    // The CPU in tiles (smaller tiles than the memory bound allows): the same output as the whole image (the halo covers
    // what the network sees); not to the bit: ncnn picks its convolution kernels by the blob's size, so only rounding
    // differs. 160 texels fit the bound whole (backend.hpp kCpuNetworkBytes; fp32 storage's bytes a texel).
    {
        std::string error;
        const int size = 160 + 2 * fx::kReliefModelPad;
        std::vector<float> image(static_cast<size_t>(size) * size * 3);
        for (size_t i = 0; i < image.size(); ++i)
            image[i] = static_cast<float>((i * 2654435761u) % 1000u) / 1000.0f;
        const auto whole = relief::CpuRuntime::Open(2, error, 0), tiled = relief::CpuRuntime::Open(2, error, 64);
        const auto a = whole ? whole->Load(files[0], files[1], fx::kReliefModelPad, 1, error) : nullptr;
        const auto b = tiled ? tiled->Load(files[0], files[1], fx::kReliefModelPad, 1, error) : nullptr;
        std::vector<float> pa(image.size()), pb(image.size());
        CHECK(a && b && a->Infer(image.data(), size, size, pa.data(), error) && b->Infer(image.data(), size, size, pb.data(), error));
        double worst = 0.0;
        for (int c = 0; c < 3; ++c)
            for (int y = fx::kReliefModelPad; y < size - fx::kReliefModelPad; ++y)
                for (int x = fx::kReliefModelPad; x < size - fx::kReliefModelPad; ++x)
                {
                    const size_t i = (static_cast<size_t>(c) * size + y) * size + x;
                    worst = std::max(worst, static_cast<double>(std::fabs(pa[i] - pb[i])));
                }
        std::printf("CPU tiles of 64 against the whole image: at most %.2g apart (%s; %s)\n", worst,
                    b ? b->Describe().c_str() : "-", a ? a->Describe().c_str() : "-");
        CHECK(a && a->TileCeiling() >= 160);
        CHECK(worst < 1.0 / 4096.0);
    }
    // The crash guard (backend.hpp MarkGpuRunning): a marker left behind by a run that died in the GPU's driver.
    {
        const std::filesystem::path cache = root / "crash";
        std::string error;
        std::string logged; // what the library logged (stderr) during open()
        const auto open = [&] {
            std::fflush(stderr);
            const int saved = dup(2);
            const std::filesystem::path log = root / "stderr.txt";
            std::freopen(log.string().c_str(), "w", stderr);
            const auto session = relief::Session::Open(all, 2, cache, error);
            std::string provider = session ? session->stats().provider : std::string("failed: ") + error;
            std::fflush(stderr);
            dup2(saved, 2);
            close(saved);
            std::ifstream in(log);
            logged.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return provider;
        };
        const auto running = [&] { return std::filesystem::exists(cache / "relief-gpu-running.txt"); };
        // Died while Vulkan opened: the CPU, Vulkan never touched again (until «Очистить кэш»).
        relief::MarkGpuRunning(cache, relief::kGpuOpening);
        std::string provider = open();
        CHECK(provider.rfind("CPU", 0) == 0 && relief::GpuCrash(cache) == relief::kGpuOpening && !running());
        CHECK(logged.find("ERROR: relief GPU inference crashed the previous run") != std::string::npos);
        CHECK(open().rfind("CPU", 0) == 0);
        std::filesystem::remove_all(cache);
        if (relief::Gpu().available)
        {
            const std::string gpu = relief::GpuLabel(relief::Gpu());
            // Died in this GPU's inference: the CPU for this GPU and driver, from then on.
            relief::MarkGpuRunning(cache, gpu);
            provider = open();
            std::printf("after a crash on %s: %s\n", gpu.c_str(), provider.c_str());
            CHECK(provider.rfind("CPU", 0) == 0 && relief::GpuCrash(cache) == gpu && !running());
            CHECK(logged.find("ERROR: relief GPU inference crashed the previous run on " + gpu + ": using the CPU") !=
                  std::string::npos);
            CHECK(open().rfind("CPU", 0) == 0);
            // Another driver since: measured again, and no marker is left by a clean run.
            std::ofstream(cache / "relief-gpu-crashed.txt", std::ios::trunc) << "Other GPU, driver 1.0.0\n";
            provider = open();
            CHECK(relief::GpuCrash(cache).empty() && !running());
            std::printf("after another GPU's crash: %s\n", provider.c_str());
            // A crash recorded by an earlier version of the GPU path (its label without kGpuPath): tried again.
            std::ofstream(cache / "relief-gpu-crashed.txt", std::ios::trunc)
                << relief::Gpu().name << ", driver " << relief::Gpu().driver << "\n";
            open();
            CHECK(relief::GpuCrash(cache).empty() && !running());
            // A marker left by a process the system killed for memory (the app's previous-exit.txt in the cache root):
            // no crash recorded, the GPU stays possible; a native crash is kept as one.
            const std::filesystem::path exit_file = cache.parent_path() / "previous-exit.txt";
            std::ofstream(exit_file, std::ios::trunc) << "low_memory 1763300000000 critical_mem_pressure\n";
            relief::MarkGpuRunning(cache, gpu);
            open();
            CHECK(relief::GpuCrash(cache).empty() && !running());
            CHECK(logged.find("the system ended the process (low_memory) while it ran: not a crash") != std::string::npos);
            std::ofstream(exit_file, std::ios::trunc) << "crash_native 1763300000000 signal 11\n";
            relief::MarkGpuRunning(cache, gpu);
            CHECK(open().rfind("CPU", 0) == 0 && relief::GpuCrash(cache) == gpu);
            std::filesystem::remove(exit_file);
        }
    }
    std::filesystem::remove_all(root);
    // Every network is gone: the Vulkan instance closes now, before the exit handlers (the driver's own run first).
    relief::ReleaseGpu();
    if (failures)
    {
        std::printf("%d relief check(s) failed\n", failures);
        return 1;
    }
    std::printf("relief checks passed\n");
    return 0;
}
