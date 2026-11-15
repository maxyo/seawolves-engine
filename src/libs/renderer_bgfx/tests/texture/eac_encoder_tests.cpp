#include "eac_encoder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace storm::texture
{
// Fault injection stays private to this test: normal callers can neither replace the codec nor the thread factory.
struct EacEncoderTestAccess
{
    static void Codec(EacEncoder &encoder, decltype(&encode_eac) codec) { encoder.encode_level_ = codec; }
    static void Threads(EacEncoder &encoder, EacEncoder::StartThread start) { encoder.start_thread_ = start; }
    static bool Started(const EacEncoder &encoder) { return encoder.started_; }
};
} // namespace storm::texture

using namespace storm::texture;
namespace
{
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); ++failures; } } while (0)

uint32_t Levels(uint32_t width, uint32_t height)
{
    uint32_t levels = 1;
    while (width > 1 || height > 1)
    {
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        ++levels;
    }
    return levels;
}

std::vector<uint8_t> Input(EacFormat format, uint32_t width, uint32_t height, unsigned pattern)
{
    std::vector<uint8_t> texels;
    uint32_t random = 0x95ab38c1;
    for (uint32_t level = 0, count = Levels(width, height); level < count; ++level)
    {
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                for (uint32_t channel = 0; channel < eac_channels(format); ++channel)
                {
                    random ^= random << 13;
                    random ^= random >> 17;
                    random ^= random << 5;
                    const uint8_t value = pattern == 0 ? 73 : pattern == 1 ? uint8_t(x * 7 + y * 17 + channel * 113) :
                                          pattern == 2 ? uint8_t(channel ? 255 - (x & 255) : y) : uint8_t(random);
                    texels.push_back(value);
                }
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    return texels;
}

std::atomic<bool> cancel{false};
std::atomic<unsigned> calls{0}, active{0};
void CancelAfterStripe(EacFormat format, const uint8_t *texels, uint32_t width, uint32_t height, uint8_t *output)
{
    ++active;
    encode_eac(format, texels, width, height, output);
    ++calls;
    cancel = true; // deterministically after actual work, with other helpers possibly still encoding
    --active;
}
void ThrowAfterStripe(EacFormat format, const uint8_t *texels, uint32_t width, uint32_t height, uint8_t *output)
{
    ++active;
    encode_eac(format, texels, width, height, output);
    ++calls;
    --active;
    throw std::runtime_error("injected encoder failure");
}

unsigned started = 0, fail_after = 0;
std::thread FailThreadStart(void (*entry)(void *), void *context)
{
    if (started == fail_after)
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    ++started;
    return std::thread(entry, context);
}

void Equality()
{
    std::atomic<bool> never_cancel{false};
    EacEncoder single(1), two(2), four(4);
    // Odd widths/last stripes exercise bimg's edge padding; thin/small and >512 exercise the serial fallback.
    for (const auto &[width, height] : std::array<std::pair<uint32_t, uint32_t>, 14>{
             {{1, 1}, {1, 17}, {17, 1}, {3, 5}, {4, 4}, {7, 9}, {31, 257}, {65, 129},
              {255, 257}, {256, 512}, {511, 509}, {512, 512}, {513, 9}, {9, 513}}})
        for (const EacFormat format : {EacFormat::R11, EacFormat::Rg11})
            for (unsigned pattern = 0; pattern < 4; ++pattern)
            {
                const auto input = Input(format, width, height, pattern);
                const auto reference = encode_eac_chain(format, input.data(), width, height, Levels(width, height));
                for (EacEncoder *encoder : {&single, &two, &four})
                {
                    std::vector<uint8_t> result{0xff};
                    CHECK(encoder->Encode(format, input.data(), width, height, Levels(width, height), result, never_cancel));
                    CHECK(result == reference);
                    CHECK(encoder->participants() >= 1 && encoder->participants() <= 4);
                }
            }
    CHECK(!EacEncoderTestAccess::Started(single));
    CHECK(EacEncoderTestAccess::Started(two) && EacEncoderTestAccess::Started(four));
}

void CancellationAndFailure()
{
    const auto input = Input(EacFormat::Rg11, 256, 512, 3);
    const auto reference = encode_eac_chain(EacFormat::Rg11, input.data(), 256, 512, 10);
    std::vector<uint8_t> out{0xff};
    EacEncoder encoder(4);
    cancel = true;
    CHECK(!encoder.Encode(EacFormat::Rg11, input.data(), 256, 512, 10, out, cancel));
    CHECK(out.empty() && !EacEncoderTestAccess::Started(encoder));
    cancel = false;
    EacEncoderTestAccess::Codec(encoder, &CancelAfterStripe);
    CHECK(!encoder.Encode(EacFormat::Rg11, input.data(), 256, 512, 10, out, cancel));
    CHECK(out.empty() && calls > 0 && active == 0);
    cancel = false;
    EacEncoderTestAccess::Codec(encoder, &ThrowAfterStripe);
    bool caught = false;
    try { encoder.Encode(EacFormat::Rg11, input.data(), 256, 512, 10, out, cancel); }
    catch (const std::runtime_error &) { caught = true; }
    CHECK(caught && out.empty() && active == 0);
    EacEncoderTestAccess::Codec(encoder, &encode_eac);
    CHECK(encoder.Encode(EacFormat::Rg11, input.data(), 256, 512, 10, out, cancel));
    CHECK(out == reference); // persistent helpers still work after both exceptional exits
    const auto small = Input(EacFormat::R11, 1, 17, 1);
    CHECK(encoder.Encode(EacFormat::R11, small.data(), 1, 17, Levels(1, 17), out, cancel));
    CHECK(out == encode_eac_chain(EacFormat::R11, small.data(), 1, 17, Levels(1, 17)) && encoder.participants() == 1);
    cancel = true; // an early cancellation must not publish an epoch to the already sleeping helpers
    CHECK(!encoder.Encode(EacFormat::Rg11, input.data(), 256, 512, 10, out, cancel) && out.empty());
    cancel = false;
    CHECK(encoder.Encode(EacFormat::Rg11, input.data(), 256, 512, 10, out, cancel) && out == reference);
    CHECK(!encoder.Encode(EacFormat::Rg11, input.data(), 0, 512, 10, out, cancel) && out.empty());
    for (const unsigned allowed : {0u, 1u})
    {
        started = 0;
        fail_after = allowed;
        EacEncoder limited(4);
        EacEncoderTestAccess::Threads(limited, &FailThreadStart);
        CHECK(limited.Encode(EacFormat::Rg11, input.data(), 256, 512, 10, out, cancel));
        CHECK(out == reference && limited.participants() == allowed + 1);
    } // destruction joins even a partially started team
}

void Benchmark()
{
    // Optional codec-only timing: no models, textures from the game, GPU or disk. Whole-preparation and frame
    // latency acceptance still need the game's stage timings under its normal workload.
    std::atomic<bool> never_cancel{false};
    EacEncoder one(1), two(2), four(4);
    const std::array<EacEncoder *, 3> encoders{&one, &two, &four};
    for (const bool fresh : {false, true})
    for (const uint32_t width : {256u, 512u})
    {
        const auto input = Input(EacFormat::Rg11, width, 512, 3);
        const auto reference = encode_eac_chain(EacFormat::Rg11, input.data(), width, 512, 10);
        std::array<std::vector<double>, 3> wall, cpu;
        for (unsigned round = 0; round < 7; ++round)
            for (unsigned order = 0; order < 3; ++order)
            {
                const unsigned team = (round + order) % 3;
                std::vector<uint8_t> out;
                const auto begin = std::chrono::steady_clock::now();
                const auto cpu_begin = std::clock();
                std::unique_ptr<EacEncoder> transient;
                if (fresh)
                    transient = std::make_unique<EacEncoder>(1u << team);
                EacEncoder *encoder = fresh ? transient.get() : encoders[team];
                CHECK(encoder->Encode(EacFormat::Rg11, input.data(), width, 512, 10, out, never_cancel));
                CHECK(encoder->participants() == (1u << team)); // do not label a degraded team as a P=4 sample
                transient.reset(); // fresh timing includes thread start and final join, as a short-lived Session does
                const double cpu_ms = 1000.0 * (std::clock() - cpu_begin) / CLOCKS_PER_SEC;
                const double wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                CHECK(out == reference);
                if (round) // one warmup per team; rotate the measured order
                {
                    wall[team].push_back(wall_ms);
                    cpu[team].push_back(cpu_ms);
                }
            }
        for (unsigned team = 0; team < 3; ++team)
        {
            std::sort(wall[team].begin(), wall[team].end());
            std::sort(cpu[team].begin(), cpu[team].end());
            std::printf("EAC-only synthetic RG11 %ux512 %s P=%u n=6 wall median/max %.3f/%.3f ms; process CPU median %.3f ms\n",
                        width, fresh ? "fresh" : "reused", 1u << team, (wall[team][2] + wall[team][3]) * .5, wall[team].back(),
                        (cpu[team][2] + cpu[team][3]) * .5);
        }
    }
}
} // namespace

int main(int argc, char **argv)
{
    const auto start = std::chrono::steady_clock::now();
    const bool benchmark = argc == 2 && std::string_view(argv[1]) == "--bench";
    if (benchmark)
        Benchmark();
    else
    {
        Equality();
        CancellationAndFailure();
    }
    std::printf("EAC %s: %s (%.3f s)\n", benchmark ? "codec-only benchmark" : "equality/cancellation/failure controls",
                failures ? "FAIL" : "PASS",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return failures ? 1 : 0;
}
