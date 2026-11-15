// Game-thread scene reveal state. Workers publish resource states; they never release the fader.
#pragma once
#include <cstdint>
#include <memory>
#include <string_view>
#include "effects/thermal.hpp"
namespace storm::shim { struct TexStore; }
namespace storm::effects {
inline bool SceneCooling(bool inference_pending, bool worker_running, std::string_view pause) {
    return inference_pending && !worker_running && pause == kThermalWaitReason;
}
struct SceneResources {
    uint32_t textures = 0, relief = 0, shadows = 0, failed = 0;
    bool switching = false;
    bool pending() const { return textures || relief || shadows || switching; }
};
// Only producer failures with an implemented retry transition offer Retry. Allocation,
// mandatory missing materials and unsupported scene resources require a fresh scene load.
struct SceneFailures {
    uint32_t retryable = 0, reload_required = 0;
    bool can_retry() const { return retryable > 0 && reload_required == 0; }
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
