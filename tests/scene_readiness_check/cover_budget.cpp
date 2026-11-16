// A location load whose opaque loading cover is never presented (a script path without loading artwork: the new game
// without its tutorial loads through reload.c LoadMainCharacterInFirstLocationGroup, no fader) still reaches ready:
// LoadingCoverBudget waives the cover after kBudgetFrames transaction frames, as effects.cpp BeginSceneWork does, and
// never earlier; time spent before the transaction (synchronous script work, no frames) does not count.
#include "storm/render/loading_budget.hpp"
#include "storm/render/loading_scene_work.hpp"
#include "effects/scene_readiness.hpp"
#include <cassert>
using storm::effects::SceneReadiness;
using storm::render::LoadingCoverBudget;
using storm::render::LoadingSceneWork;

// One loading frame as BeginSceneWork/SceneOpaquePresented run it; returns the readiness state after it.
static SceneReadiness::State frame(SceneReadiness &gate, LoadingSceneWork &work, LoadingCoverBudget &cover, uint64_t gen,
                                   unsigned id, bool cover_presented, bool budget, int *waivers) {
    cover.transaction_frame(gen); // Renderer::RunEnd
    bool opaque = cover_presented;
    if (cover_presented) cover.presented(gen);
    if (!opaque && budget) {
        bool first = false;
        opaque = cover.waived(gen, &first);
        *waivers += first;
    }
    work.begin(id, gen, 5, 8, true, opaque, false, true);
    if (!work.hidden(gen, 5, 8)) work.beauty_submitted();
    work.presented(true, true);
    gate.frame(gen, id, work.complete(gen), {});
    work.end();
    return gate.state();
}

int main() {
    // No cover, no budget (the 1651 behaviour): never ready, the stock transaction loops for ever.
    {
        SceneReadiness gate; LoadingSceneWork work; LoadingCoverBudget cover; int waivers = 0;
        const auto gen = gate.begin(0); cover.begin(gen);
        for (unsigned id = 1; id <= 600; ++id)
            assert(frame(gate, work, cover, gen, id, false, false, &waivers) != SceneReadiness::State::Ready);
    }
    // No cover, with the budget: not ready before kBudgetFrames, ready right after it; the waiver is reported once.
    {
        SceneReadiness gate; LoadingSceneWork work; LoadingCoverBudget cover; int waivers = 0;
        const auto gen = gate.begin(0); cover.begin(gen);
        assert(!cover.waived(gen)); // however long the scripts ran before the transaction
        unsigned id = 1;
        for (; id < LoadingCoverBudget::kBudgetFrames; ++id)
            assert(frame(gate, work, cover, gen, id, false, true, &waivers) != SceneReadiness::State::Ready);
        assert(waivers == 0);
        bool ready = false;
        for (unsigned end = id + 10; id < end && !ready; ++id)
            ready = frame(gate, work, cover, gen, id, false, true, &waivers) == SceneReadiness::State::Ready;
        assert(ready && waivers == 1);
    }
    // A presented cover (the stock artwork) is never waived, even after a slow first frame.
    {
        SceneReadiness gate; LoadingSceneWork work; LoadingCoverBudget cover; int waivers = 0;
        const auto gen = gate.begin(0); cover.begin(gen);
        bool ready = false;
        for (unsigned id = 1; id <= 10 && !ready; ++id)
            ready = frame(gate, work, cover, gen, id, true, true, &waivers) == SceneReadiness::State::Ready;
        assert(ready && waivers == 0);
        for (unsigned i = 0; i < 100; ++i) cover.transaction_frame(gen);
        assert(!cover.waived(gen));
    }
    // A new generation starts its own budget.
    {
        LoadingCoverBudget cover;
        cover.begin(1);
        for (unsigned i = 0; i < LoadingCoverBudget::kBudgetFrames; ++i) cover.transaction_frame(1);
        assert(cover.waived(1));
        cover.begin(2); assert(!cover.waived(2));
        for (unsigned i = 0; i < 100; ++i) cover.transaction_frame(1); // a stale generation's frames
        assert(!cover.waived(2) && !cover.waived(1));
    }
}
