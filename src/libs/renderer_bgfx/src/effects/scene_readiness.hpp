// Game-thread scene reveal state. Workers publish resource states; they never release the fader.
#pragma once
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_set>
#include <vector>
#include "effects/thermal.hpp"
namespace storm::shim { struct TexStore; }
namespace storm::effects {
inline bool SceneCooling(bool inference_pending, bool worker_running, std::string_view pause) {
    return inference_pending && !worker_running && pause == kThermalWaitReason;
}
struct SceneResources {
    // relief: the location's relief/gloss maps still being made. Reported only (the loading screen shows the
    // count); never pending() and never a failure: the reveal does not wait for quality products.
    uint32_t textures = 0, relief = 0, shadows = 0, failed = 0;
    bool switching = false;
    bool pending() const { return textures || shadows || switching; }
};
// Only producer failures with an implemented retry transition offer Retry. Allocation,
// mandatory missing materials and unsupported scene resources require a fresh scene load.
struct SceneFailures {
    uint32_t retryable = 0, reload_required = 0;
    bool can_retry() const { return retryable > 0 && reload_required == 0; }
};
// One scene texture's states for the reveal. Only a texture still in flight (its own upload, `holds`) holds the
// reveal, and only within SceneTextureStall's budget. A texture that failed never holds it: it will never get a GPU
// copy, so the caller logs an ERROR naming it and the scene draws the engine's unbound-texture fallback. Its
// relief/gloss maps and its x2 (`relief_pending`: NormalMaps::prepare_scene_texture said 1) are quality products
// that fill in after the reveal, flat or the original texture meanwhile; their owners log their failures.
inline void AccountSceneTexture(SceneResources &r, bool holds, bool relief_pending) {
    if (holds) ++r.textures;
    if (relief_pending) ++r.relief;
}
// The budget of the in-flight scene textures: when none of them changed stage for kBudgetMs, the ones still in flight
// stop holding the reveal (the caller logs each by name as an ERROR). A slow but progressing upload keeps holding.
class SceneTextureStall {
public:
    static constexpr uint64_t kBudgetMs = 60000;
    void begin(uint64_t now) { released_.clear(); pending_.clear(); signature_ = sample_ = 0; progress_at_ = now; }
    // Called for every texture in flight in a sample; false once its stall was released (it no longer holds).
    bool in_flight(uint64_t serial, uint64_t stage) {
        if (released_.contains(serial)) return false;
        pending_.push_back(serial);
        sample_ ^= mix(serial ^ mix(stage + 1));
        return true;
    }
    // Ends a sample: the textures released now, because none of those in flight progressed within the budget.
    std::vector<uint64_t> end(uint64_t now) {
        std::vector<uint64_t> released;
        if (pending_.empty() || sample_ != signature_) progress_at_ = now;
        else if (now >= progress_at_ + kBudgetMs) { released = pending_; released_.insert(pending_.begin(), pending_.end()); }
        signature_ = sample_; sample_ = 0; pending_.clear();
        return released;
    }
private:
    static uint64_t mix(uint64_t x) {
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27; x *= 0x94d049bb133111ebULL; return x ^ (x >> 31);
    }
    std::unordered_set<uint64_t> released_;
    std::vector<uint64_t> pending_;
    uint64_t signature_ = 0, sample_ = 0, progress_at_ = 0;
};
inline bool SceneTextureName(std::string_view normalized) {
    return !normalized.empty() && !normalized.starts_with("interfaces/") && !normalized.starts_with("loading/");
}
class SceneReadiness {
public:
    enum class State { Inactive, Warming, Error, Ready };
    static constexpr uint64_t kSlowNoticeMs = 120000;
    uint64_t begin(uint64_t now) {
        ++generation_; started_ = now; state_ = State::Warming;
        stable_ = 0; last_frame_ = 0; resources_ = {};
        progress_at_ = now; progress_signature_ = 0; have_progress_ = false; return generation_;
    }
    void invalidate() { stable_ = 0; if (state_ == State::Ready) state_ = State::Warming; }
    void refresh(SceneResources resources) {
        resources_ = resources;
        if (state_ == State::Inactive) return;
        if (resources.failed) { state_ = State::Error; stable_ = 0; }
        else if (resources.pending()) invalidate();
    }
    // A sample is taken after the ordinary scene frame/pump. Queue emptiness and elapsed time alone are not ready.
    void frame(uint64_t generation, uint64_t frame, bool scene, SceneResources resources) {
        if (generation != generation_ || state_ == State::Inactive || frame <= last_frame_) return;
        last_frame_ = frame; refresh(resources);
        if (resources.failed) return;
        if (!scene || resources.pending()) { invalidate(); return; }
        if (state_ == State::Ready) return;
        if (++stable_ >= 3) state_ = State::Ready;
    }
    void observe_progress(uint64_t signature, uint64_t now) {
        if (!have_progress_ || signature != progress_signature_) {
            have_progress_ = true; progress_signature_ = signature; progress_at_ = now;
        }
    }
    uint64_t elapsed(uint64_t now) const { return now >= started_ ? now - started_ : 0; }
    uint64_t unchanged(uint64_t now) const { return now >= progress_at_ ? now - progress_at_ : 0; }
    bool slow(uint64_t now) const { return state_ == State::Warming && unchanged(now) >= kSlowNoticeMs; }
    bool hold(uint64_t) const {
        // Inference duration is unbounded by this consumer. Unchanged observed stages are NOT
        // proof that a worker stalled. Report slow preparation, retain responsive Cancel, and
        // enter Error only for a producer's explicit failure; elapsed time never grants reveal.
        return state_ == State::Warming || state_ == State::Error;
    }
    uint64_t generation() const { return generation_; }
    State state() const { return state_; }
    SceneResources resources() const { return resources_; }
private:
    uint64_t generation_ = 0, started_ = 0, last_frame_ = 0;
    uint64_t progress_at_ = 0, progress_signature_ = 0;
    bool have_progress_ = false;
    uint32_t stable_ = 0;
    State state_ = State::Inactive;
    SceneResources resources_{};
};
// Main/game-thread glue; no bgfx or worker calls are made by the state machine above.
void BeginSceneResources(const char *scene);
void ObserveSceneTexture(const std::shared_ptr<shim::TexStore> &texture);
bool HoldSceneResources();
void ScenePipelinesPending(bool pending);
} // namespace storm::effects
