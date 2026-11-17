// Game-thread scene reveal state. Workers publish resource states; they never release the fader.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
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
    // relief: the location's relief/gloss maps still being made (the loading screen shows the count); they hold the
    // reveal only through `products` (SceneProductWait), never as a failure.
    uint32_t textures = 0, relief = 0, shadows = 0, failed = 0;
    bool switching = false;
    // The quality products still being made hold the reveal: within kSceneProductBudgetMs of mandatory-ready only.
    bool products = false;
    // The mandatory part: what the scene cannot be drawn without (GPU textures, this frame's shadow maps, a switch).
    bool mandatory_pending() const { return textures || shadows || switching; }
    bool pending() const { return mandatory_pending() || products; }
};
// When a location loads, the reveal waits for the quality products the active preset wants for the scene and that
// are still being made, for at most this long, counted from the moment its mandatory resources
// (SceneResources::mandatory_pending) are ready. What finishes within it is there at the reveal; the rest continues in
// the background and fades in after it (relief maps over kReliefFade, the AO and visibility volumes over lighting.cpp
// kVolumeFade). One budget per load, never extended.
inline constexpr uint64_t kSceneProductBudgetMs = 5000;
// The quality products a load waits for. A product the preset does not want is never counted (its owner makes
// nothing: «Низкая» has none of them), so it adds no wait.
enum class SceneProduct : uint8_t {
    Transcode, // a texture drawing its fallback while its full transcode runs (a cache read included)
    Relief,    // relief/gloss maps of the scene's loaded textures (cache read, generation, upload and fade)
    Upscale,   // neural x2 of the textures the residency plan chose (near the camera)
    Place,     // the place's probes: its visibility and AO volumes (cache load or bake)
    TownLight, // the town light: the lightmap set (cache load or bake) and its first composition
    Count
};
struct SceneProducts {
    std::array<uint32_t, static_cast<size_t>(SceneProduct::Count)> count{};
    void add(SceneProduct p, uint32_t n = 1) { count[static_cast<size_t>(p)] += n; }
    uint32_t operator[](SceneProduct p) const { return count[static_cast<size_t>(p)]; }
    bool any() const { for (const uint32_t n : count) if (n) return true; return false; }
    // "relief/gloss maps 12, town light 1", or "nothing".
    std::string describe() const {
        static constexpr const char *kNames[] = {"texture transcodes", "relief/gloss maps", "x2 textures",
                                                 "place probes (visibility/AO volume)", "town light"};
        std::string text;
        for (size_t i = 0; i < count.size(); ++i)
            if (count[i]) text += (text.empty() ? "" : ", ") + std::string(kNames[i]) + " " + std::to_string(count[i]);
        return text.empty() ? "nothing" : text;
    }
};
// The bounded wait for the quality products of one load (game thread; no clock or engine call of its own).
class SceneProductWait {
public:
    void begin() { *this = {}; }
    // One sample of the products in progress. `mandatory_ready`: a scene frame with no mandatory resource pending;
    // the first one starts the budget (later mandatory work does not restart it). True while the products hold.
    bool hold(bool mandatory_ready, const SceneProducts &products, uint64_t now) {
        last_ = products;
        if (finished_) return false;
        if (!started_) {
            if (!mandatory_ready) return false; // the mandatory resources hold the reveal; the budget has not begun
            started_ = true; ready_at_ = now;
        }
        if (!products.any()) return false;
        for (size_t i = 0; i < products.count.size(); ++i) waited_.count[i] = std::max(waited_.count[i], products.count[i]);
        if (now >= ready_at_ + kSceneProductBudgetMs) { expired_ = true; finished_ = true; waited_ms_ = kSceneProductBudgetMs; return false; }
        waited_ms_ = now - ready_at_;
        return true;
    }
    // The location was revealed: nothing holds it again in this load (a product starting later fills in).
    void revealed() { finished_ = true; }
    bool started() const { return started_; }
    bool expired() const { return expired_; }
    uint64_t ready_at() const { return ready_at_; }
    uint64_t waited_ms() const { return waited_ms_; }
    // The most of each product in progress while the wait ran.
    const SceneProducts &waited() const { return waited_; }
    // The products in progress at the last sample (at the reveal: what continues after it).
    const SceneProducts &last() const { return last_; }
private:
    bool started_ = false, finished_ = false, expired_ = false;
    uint64_t ready_at_ = 0, waited_ms_ = 0;
    SceneProducts waited_{}, last_{};
};
// Only producer failures with an implemented retry transition offer Retry. Allocation,
// mandatory missing materials and unsupported scene resources require a fresh scene load.
struct SceneFailures {
    uint32_t retryable = 0, reload_required = 0;
    bool can_retry() const { return retryable > 0 && reload_required == 0; }
};
// One scene texture's states for the reveal. A texture still in flight (its own upload, `holds`) holds the reveal as
// a mandatory resource, within SceneTextureStall's budget. A texture that failed never holds it: it will never get a
// GPU copy, so the caller logs an ERROR naming it and the scene draws the engine's unbound-texture fallback. Its
// relief/gloss maps (`relief_pending`: NormalMaps::prepare_scene_texture said 1) are quality products: the caller
// counts them in SceneProducts, which hold the reveal only within kSceneProductBudgetMs; their owners log failures.
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
void BeginSceneResources(const char *scene, uint64_t owner);
void ObserveSceneTexture(const std::shared_ptr<shim::TexStore> &texture);
// A location or sea scene is being prepared behind its loading screen (from BeginSceneResources to the reveal).
bool ScenePreparing();
bool HoldSceneResources();
void ScenePipelinesPending(bool pending);
} // namespace storm::effects
