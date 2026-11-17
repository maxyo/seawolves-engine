// Host check of relief::Finalizer (finalizer.hpp): the real finalization (box filter, gloss, material, EAC RG11 encoder
// team, fx writers, the ×2 KTX encoder) on synthetic computed planes, published to a real directory and read back. No
// models, no GPU: the finalizer only encodes planes the networks already computed. The CPU permits are the process's
// real fx::SharedCpuAdmission. Argument: a scratch directory (wiped).
#include "finalizer.hpp"

#include "storm/fx/cpu_admission.hpp"
#include "storm/fx/relief_store.hpp"
#include "storm/fx/upscale.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <thread>

namespace
{
using namespace storm;
using namespace storm::relief;
using Clock = std::chrono::steady_clock;

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

constexpr uint64_t kReliefKey = 11, kGlossKey = 12, kUpscaleKey = 77;

enum class Kind { NormalGloss, ModelScale2, GlossOnly, ReliefOnly, Parallel };

int FullChain(int width, int height)
{
    int levels = 1;
    for (; width > 1 || height > 1; width = std::max(width / 2, 1), height = std::max(height / 2, 1))
        ++levels;
    return levels;
}

// What ComputeFile hands over for a stone texture: colour, the models' planes (at twice the map's size on a ×2 run),
// the roughness and gloss height at the gloss map's size.
std::unique_ptr<ComputedTexture> Fixture(Kind kind, const std::filesystem::path &output)
{
    auto p = std::make_unique<ComputedTexture>();
    p->cancelled = std::make_shared<std::atomic<bool>>(false);
    p->output = output;
    p->name = "synthetic stone";
    p->session = 42;
    p->provider = "synthetic";
    p->started = p->computed_at = Clock::now();
    p->width = p->height = 32;
    p->fw = p->fh = 16;
    p->gw = p->gh = 8;
    p->mw = p->mh = kind == Kind::ModelScale2 ? 32 : 16;
    p->model_scale = kind == Kind::ModelScale2 ? 2 : 1;
    if (kind == Kind::Parallel)
    {
        // A 64x64 map is 256 EAC blocks: the size the encoder takes a team for.
        p->width = p->height = 128;
        p->mw = p->mh = p->fw = p->fh = 64;
        p->gw = p->gh = 32;
    }
    p->key = kReliefKey;
    p->gloss_key = kGlossKey;
    p->gloss_wanted = p->height_class = true;
    p->relief = fx::ReliefClass::Stone;
    p->gloss_class = fx::GlossClass::Stone;
    p->rgba.resize(size_t(p->width) * p->height * 4);
    for (size_t i = 0; i < p->rgba.size(); ++i)
        p->rgba[i] = uint8_t((i * 37 + 11) % 256);
    const auto fill = [](std::vector<float> &v, size_t n, int seed) {
        v.resize(n);
        for (size_t i = 0; i < n; ++i)
            v[i] = float((i * 17 + seed) % 251) / 250.f;
    };
    fill(p->normal_planes, size_t(p->mw) * p->mh * 3, 3);
    fill(p->height_planes, size_t(p->mw) * p->mh * 3, 9);
    fill(p->roughness_planes, size_t(p->gw) * p->gh * 3, 21);
    fill(p->gloss_height, size_t(p->gw) * p->gh, 39);
    if (kind == Kind::GlossOnly)
    {
        p->key.reset();
        p->normal_planes.clear();
        p->height_planes.clear();
        p->height_class = false;
    }
    if (kind == Kind::ReliefOnly)
    {
        p->gloss_key.reset();
        p->height_class = p->gloss_wanted = false;
        p->gloss_height.clear();
        p->roughness_planes.clear();
    }
    return p;
}

void CheckMap(const std::optional<fx::NormalMap> &map, int size, bool material, const char *what)
{
    CHECK(map.has_value());
    if (!map)
    {
        std::printf("  (%s not readable)\n", what);
        return;
    }
    CHECK(map->format == fx::NormalMapFormat::EacRg11);
    CHECK(map->width == size && map->height == size);
    CHECK(map->mip_count == FullChain(size, size));
    CHECK(!map->rg.empty());
    CHECK(map->material.empty() == !material);
}

// Prepared, the expected files and only them, each valid.
void CheckPrepared(Finalizer &finalizer, Kind kind, const std::filesystem::path &dir, const char *what)
{
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto p = Fixture(kind, dir);
    const bool relief = p->key.has_value(), gloss = p->gloss_key.has_value(), material = p->height_class;
    const int map = p->fw, gloss_size = p->gw;
    const auto result = finalizer.Run(std::move(p));
    std::printf("%s: %s%s\n", what, result.outcome == Outcome::Prepared ? "prepared" : "failed: ", result.failure.c_str());
    CHECK(result.outcome == Outcome::Prepared);
    CHECK(result.session == 42 && !result.upscale && !result.upscale_written);
    CHECK(result.relief_written == (relief ? std::optional<uint64_t>(kReliefKey) : std::nullopt));
    CHECK(result.gloss_written == (gloss ? std::optional<uint64_t>(kGlossKey) : std::nullopt));
    CHECK(fx::HasReliefMap(dir, kReliefKey) == relief);
    CHECK(fx::HasGlossMap(dir, kGlossKey) == gloss);
    if (relief)
        CheckMap(fx::ReadReliefMap(dir, kReliefKey), map, material, what);
    if (gloss)
        CheckMap(fx::ReadGlossMap(dir, kGlossKey), gloss_size, false, what);
    CHECK(std::distance(std::filesystem::directory_iterator(dir), {}) == int(relief) + int(gloss));
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: storm_relief_finalizer_check <scratch directory>\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    std::filesystem::remove_all(root);
    auto &admission = fx::SharedCpuAdmission();
    Finalizer finalizer;

    CheckPrepared(finalizer, Kind::NormalGloss, root / "normal-gloss", "normal + gloss");
    CheckPrepared(finalizer, Kind::ModelScale2, root / "scale2", "models on the ×2");
    CheckPrepared(finalizer, Kind::GlossOnly, root / "gloss-only", "gloss only");
    CheckPrepared(finalizer, Kind::ReliefOnly, root / "relief-only", "relief only");
    // Small maps encode on one thread, and the material permit is released before the encoder's: never two at once.
    CHECK(admission.stats().reserved == 0 && admission.stats().reserved_peak == 1);

    // A map of 256 blocks takes an encoder team, admitted as a whole through the shared admission (the loading
    // screen's full core budget here) and released afterwards.
    admission.begin(1, true);
    admission.opaque(1, true);
    const unsigned team = std::min(4u, admission.threads());
    CheckPrepared(finalizer, Kind::Parallel, root / "parallel", "encoder team");
    admission.end(1);
    std::printf("encoder team: %u threads admitted (peak %u)\n", team, admission.stats().reserved_peak);
    if (team < 2)
        std::printf("NOTE: a host with one core admits no team: the team admission is not exercised\n");
    CHECK(admission.stats().reserved_peak == team);
    CHECK(admission.stats().reserved == 0);

    // Every permit taken elsewhere: the finalizer waits for its first one; cancelling it ends the wait promptly,
    // Failed, without touching the permits it never had.
    {
        const unsigned capacity = admission.threads();
        auto held = admission.acquire(capacity, [] { return false; });
        CHECK(held && admission.stats().reserved == capacity);
        const auto dir = root / "cancelled";
        auto p = Fixture(Kind::GlossOnly, dir);
        const auto cancel = p->cancelled;
        auto waiting = std::async(std::launch::async, [&finalizer, p = std::move(p)]() mutable {
            return finalizer.Run(std::move(p));
        });
        // On progress, no deadline: a finalizer that never queues, or never ends after its cancel, hangs here and
        // run.py's deadlock guard reports the last step printed.
        while (admission.queued() != 1)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(waiting.wait_for(std::chrono::seconds(0)) == std::future_status::timeout); // waiting for a permit
        *cancel = true;
        std::printf("step: the cancelled finalizer ends its wait for a permit\n");
        std::fflush(stdout);
        const auto result = waiting.get();
        CHECK(result.outcome == Outcome::Failed && result.failure == "cancelled");
        CHECK(!result.relief_written && !result.gloss_written);
        CHECK(!std::filesystem::exists(dir));
        CHECK(admission.queued() == 0 && admission.stats().reserved == capacity);
        // The ×2's encoder runs on the finalizer's first permit alone: it waits for it, and runs once one is free.
        const auto x2 = root / "x2-waiting";
        auto display = Fixture(Kind::NormalGloss, root / "unused");
        display->kind = ComputedTexture::Kind::Upscale;
        display->width = display->height = 16;
        display->display = {x2, false};
        auto queued = std::async(std::launch::async, [&finalizer, p = std::move(display)]() mutable {
            return finalizer.Run(std::move(p));
        });
        while (admission.queued() != 1)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(queued.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
        CHECK(!std::filesystem::exists(x2));
        held.reset();
        std::printf("step: the x2 runs once every permit is free\n");
        std::fflush(stdout);
        CHECK(queued.get().outcome == Outcome::Prepared && fx::HasUpscaled(x2, kReliefKey));
        CHECK(admission.stats().reserved == 0);
    }

    // The relief map published, the gloss map's publication failing (a directory where its file goes): Failed,
    // with the relief map reported written and the gloss map not.
    {
        const auto dir = root / "partial";
        std::filesystem::create_directories(fx::GlossFile(dir, kGlossKey));
        std::ofstream(fx::GlossFile(dir, kGlossKey) / "occupied") << "x";
        const auto result = finalizer.Run(Fixture(Kind::NormalGloss, dir));
        std::printf("partial write: %s\n", result.failure.c_str());
        CHECK(result.outcome == Outcome::Failed);
        CHECK(result.relief_written == std::optional<uint64_t>(kReliefKey));
        CHECK(!result.gloss_written);
        CHECK(result.failure == "cannot write " + fx::GlossFile(dir, kGlossKey).string());
        CheckMap(fx::ReadReliefMap(dir, kReliefKey), 16, true, "partial relief");
        CHECK(std::filesystem::is_directory(fx::GlossFile(dir, kGlossKey)));
    }

    // The ×2 for the game: the 32x32 ×2 of a 16x16 texture as a KTX with its whole chain.
    {
        const auto dir = root / "x2";
        auto p = Fixture(Kind::NormalGloss, root / "unused");
        p->kind = ComputedTexture::Kind::Upscale;
        p->key = kUpscaleKey;
        p->gloss_key.reset();
        p->width = p->height = 16;
        p->display = {dir, false};
        const auto result = finalizer.Run(std::move(p));
        CHECK(result.outcome == Outcome::Prepared && result.upscale);
        CHECK(result.upscale_written == std::optional<uint64_t>(kUpscaleKey));
        CHECK(!result.relief_written && !result.gloss_written);
        CHECK(!std::filesystem::exists(root / "unused"));
        const auto ktx = fx::ReadUpscaled(dir, kUpscaleKey);
        CHECK(ktx.has_value());
        if (ktx)
        {
            static const uint8_t kKtx1[12] = {0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n'};
            uint32_t field[16] = {};
            CHECK(ktx->size() > 64 && std::memcmp(ktx->data(), kKtx1, sizeof(kKtx1)) == 0);
            if (ktx->size() > 64)
                std::memcpy(field, ktx->data(), 64);
            // KTX 1.1 header: pixelWidth, pixelHeight, numberOfMipmapLevels at words 9, 10, 14.
            CHECK(field[9] == 32 && field[10] == 32 && field[14] == uint32_t(FullChain(32, 32)));
        }
    }

    CHECK(admission.stats().reserved == 0 && admission.queued() == 0);
    std::filesystem::remove_all(root);
    if (failures)
    {
        std::printf("FAIL: %d checks failed\n", failures);
        return 1;
    }
    std::puts("PASS: relief finalizer (normal + gloss, ×2 models, gloss only, relief only, encoder team, cancelled wait, "
              "partial write, ×2 display, ×2 permit wait)");
    return 0;
}
