#include "tile_recovery.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

using namespace storm::relief;
void require(bool value, const char *message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main()
{
    TileRecovery recovery;
    auto lease = std::make_shared<LoadingRetry>(1000, 42);
    const auto plan = [&](double fixed = 1.0, double slope = 0.0001, int current = 32, int ceiling = 256,
                          int w = 512, int h = 512) {
        return recovery.plan(lease, 100, false, current, ceiling, 24, w, h, fixed, slope);
    };
    const auto probe = plan();
    require(probe.tile == 64 && probe.generation == 42, "one bounded step from guarded tile");
    require(!plan(-1) && !plan(std::numeric_limits<double>::infinity()) &&
        !plan(1, 0) && !plan(1, std::numeric_limits<double>::quiet_NaN()), "invalid calibration cannot authorize probe");
    require(!plan(1, .0001, 64, 64) && !plan(1, .0001, 32, 513) && !plan(1, .0001, 33),
        "ceiling, maximum and alignment are hard bounds");
    require(!plan(1, .0001, 32, 256, 63, 512) && !plan(1, .0001, 32, 256, 512, 63),
        "candidate requires a full first real tile in both dimensions");
    require(!recovery.plan(lease, 999, false, 32, 256, 24, 512, 512, 1, .0001),
        "predicted submission plus reserve must fit deadline");
    require(!recovery.begin(probe, lease, 999, false), "allocation time cannot bypass deadline preflight");
    lease->active = false;
    require(!recovery.begin(probe, lease, 100, false), "revocation immediately before submission prevents probe");
    // Production calls this boundary after ncnn input allocation/copy and extractor preparation.
    // Simulate revocation/elapsed budget during that preparation, without consuming the network's attempt.
    const auto fake_prepare_then_submit = [&](const std::shared_ptr<LoadingRetry> &permission,
                                              uint64_t after_preparation, bool revoke) {
        std::vector<float> input(3 * 112 * 112, .5f);
        if (revoke) permission->active = false;
        return !input.empty() && recovery.begin(probe, permission, after_preparation, false);
    };
    lease = std::make_shared<LoadingRetry>(1000, 42);
    require(!fake_prepare_then_submit(lease, 101, true), "revoke during input preparation prevents actual probe");
    lease = std::make_shared<LoadingRetry>(1000, 42);
    require(!fake_prepare_then_submit(lease, 1000, false), "expiry during input preparation prevents actual probe");
    lease = std::make_shared<LoadingRetry>(1000, 42);
    require(recovery.begin(probe, lease, 100, false), "no attempt was consumed by preflight refusals");
    const auto decision = [&](uint64_t now, bool cancelled, bool shape, double wall, double fixed) {
        return std::string(TileRecovery::decision(probe, lease, now, cancelled, shape, wall, fixed));
    };
    require(decision(110, false, true, 7, 1) == "accepted", "6ms adjusted boundary accepted");
    require(decision(110, false, true, 7.001, 1) != "accepted", "over-budget measurement rejected");
    require(decision(110, false, true, .5, 1) != "accepted", "stale fixed estimate larger than wall rejected");
    require(decision(110, false, true, std::numeric_limits<double>::quiet_NaN(), 1) != "accepted" &&
        decision(110, false, true, 0, 0) != "accepted", "invalid measured time rejected");
    require(decision(1000, false, true, 2, 1) == "expired" &&
        decision(110, true, true, 2, 1) == "cancelled" &&
        decision(110, false, false, 2, 1) == "inference-or-shape-failed", "expired, cancelled and failed probes retain old tile");
    lease->active = false;
    require(decision(110, false, true, 2, 1) == "revoked", "revocation after GPU submission forbids adoption");
    lease = std::make_shared<LoadingRetry>(2000, 42);
    require(!plan() && !recovery.begin(probe, lease, 110, false), "renewed same-generation deadline cannot repeat even rejected probe");
    lease = std::make_shared<LoadingRetry>(2000, 43);
    require(plan().tile == 64, "new generation permits one new measured step");
    lease = std::make_shared<LoadingRetry>(2000);
    require(!plan(), "legacy device retry does not grant recovery epoch");

    // Same vector move/resize used by RunTiles. Contents and output pointer must survive, and only one callback
    // may bypass inference. The model's full output is retained, including halo and all three scaled planes.
    PrimedTile first;
    first.width = first.height = 112;
    first.wall_ms = 2.25;
    first.output.resize(3 * 112 * 112 * 4 * 4);
    for (size_t i = 0; i < first.output.size(); ++i) first.output[i] = static_cast<float>(i);
    first.expected_output = first.output.data();
    first.pending = true;
    std::vector<float> ordinary_output = std::move(first.output);
    ordinary_output.resize(3 * 112 * 112 * 4 * 4);
    double ms = 0;
    require(first.consume(112, 112, ordinary_output.data(), ms) && first.reused && ms == 2.25,
        "accepted exact first output reused with original measured wall");
    require(!first.consume(112, 112, ordinary_output.data(), ms), "cached first output consumed once only");
    for (size_t i = 0; i < ordinary_output.size(); ++i)
        require(ordinary_output[i] == static_cast<float>(i), "all output planes and halo survive without copy");
    first.pending = true;
    require(!first.consume(80, 112, ordinary_output.data(), ms) && !first.pending,
        "different shape falls back to inference without cropping");
    first.pending = true;
    require(!first.consume(112, 112, nullptr, ms), "different output pointer cannot reuse stale pixels");
    PrimedTile rejected;
    require(!rejected.consume(112, 112, ordinary_output.data(), ms), "rejected or expired probe publishes no primed output");
    std::puts("relief tile recovery policy: lease, epoch, budget, estimator and exact first-output reuse PASS (CPU only)");
}
