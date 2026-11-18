// The game's relief maps made while it runs: PBRify's models (src/libs/relief) on one worker thread in the
// game process, into the relief directory (fx/relief_store.hpp) the game reads.
//
//   * Only a texture the current scene draws with a draw that reads its map is requested (NormalMaps::upload: the
//     draws' own choice of relief, gloss and the characters' relief at the current settings); one preparation makes
//     both of its maps (PBRify's normal, height and roughness networks). Nothing is made ahead or for a texture that
//     is merely loaded. The requests are reselected from recent player-distance observations before each job,
//     alternating with standalone ×2 upscales (chosen by their residency plan) within the same 10 m band. Unobserved
//     entries fall back to draw order. Live work ages after 16 other admissions. When its map is written the
//     texture's entry becomes Stored: the draw reads and uploads it and the relief fades in (NormalMaps::handle).
//   * A new scene, or settings that change which draws read the maps, drop the queue (drop_queue): the entries go
//     back to Unqueued and the next draws that read them ask again. When nothing is requested the worker is idle.
//   * The device follows the game (relief device_choice.hpp): while the player sees the game's scene only the CPU; the
//     GPU only while the game draws no scene (a loading or an interface screen, the game in the background) or in a
//     place's loading window. A GPU texture stops at its next submission when the scene shows, and goes back to the
//     queue for the CPU. Each change of device is logged.
//   * The worker runs at nice 10. CPU jobs take fx::CpuAdmission's share, with the ray-bake pieces: all cores after a
//     presented opaque stock frame (a place loads), and while the game is played a quarter of the cores, at most two,
//     one when the device is hot and none at thermal status SEVERE (thermal.hpp). The renderer's explicit hold (the
//     automatic graphics preset's measurement) and SEVERE while the game is played pause the work. The models are
//     loaded at the first job and released after 10 s with no work left.
// The maps persist: a texture's map is made once per game, method and map size.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace storm::relief
{
class Session;
struct LoadingRetry;
struct ComputedTexture;
struct FinalizeResult;
class Finalizer;
class FinalizeQueue;
}

namespace storm::effects
{
struct NormalMapEntry;
struct UpscaleEntry;

struct ReliefSetup
{
    std::filesystem::path directory; // the relief directory
    std::filesystem::path textures;  // the game's RESOURCE/Textures
    // Reads a model file by its name (src/libs/relief/model/README.md): the APK assets on Android, engine.ini relief_models on the
    // host. Null: no maps are made (logged at the first request); the textures without a map get no relief.
    std::function<std::vector<uint8_t>(const char *file)> model;
};

class ReliefGenerator
{
  public:
    static ReliefGenerator &instance();
    ~ReliefGenerator();

    // Main thread, before the first texture is loaded. Stops the work of a previous setup.
    void configure(ReliefSetup setup);
    // Main thread: a loaded texture whose map is not in the directory; `file` is its game file.
    void request(const std::shared_ptr<NormalMapEntry> &entry, std::filesystem::path file);
    // Main thread: request a missing ×2 texture (effects/upscale.hpp). Alternates with loaded relief requests,
    // A completed cache file is published even while an explicit renderer hold pauses
    // inference. The caller sets Waiting before enqueueing. False when
    // kUpscaleQueue requests wait already (the entry asks again later). Done: the entry becomes Stored, or Failed.
    bool request_upscale(const std::shared_ptr<UpscaleEntry> &entry);
    // Main thread: the game shows the ×2 textures (Settings::upscale): the requested relief maps made from a ×2 also
    // keep it in `directory`, encoded for the tier (`bc`). No other game textures are discovered here.
    // Off also drops the queued ×2 requests (their entries Missing: the residency plan asks again when it is on) and
    // stops a ×2 in progress at its next stage.
    void set_display(bool on, std::filesystem::path directory, bool bc);
    // Main thread (NormalMaps::configure / configure_gloss): whether the settings read the relief / the gloss maps
    // (fx::BackgroundProducts). Off: no request of that product is taken, the queued ones are dropped (their entries
    // Unqueued: looked up again when a draw reads them) and the texture in progress for it stops at its next stage.
    void set_wanted(bool relief, bool gloss);
    // Main thread: every queued relief and gloss request is dropped (its entry Unqueued: the next draw that reads it
    // asks again); `why` is logged with the count. A new scene, or settings that change which draws read the maps.
    // The texture in progress ends (it was drawn a moment ago).
    void drop_queue(const char *why);
    // Stops the worker (the texture in progress is abandoned at its next inference tile).
    void shutdown();
    // Game thread, every frame: whether a perspective 3D scene was drawn, and whether a place loads (its scene is
    // drawn under the loading cover, unseen). A scene drawn while no place loads is shown to the player: the relief
    // runs on the CPU, and a GPU texture stops (scene_shown). A few frames without a scene (a loading or an interface
    // screen), or no frame for a second (the game in the background), leave the GPU free (gpu_free). Neither starts
    // any work by itself.
    void frame(bool scene_drawn, bool loading);
    // Game thread: exact scoped entries eligible for one bounded measured retry. Empty revokes.
    void loading_window(uint64_t generation, uint64_t deadline_ms,
                        std::vector<std::shared_ptr<NormalMapEntry>> normal,
                        std::vector<std::shared_ptr<UpscaleEntry>> upscale);
    bool gpu_free() const;
    bool scene_shown() const; // any thread: the last frame showed the player a scene, less than a second ago
    // Any thread (the system is low on memory: render::TrimRelief): the models are released after the texture in
    // progress, and the GPU instance closed; they load again when work comes. Logged.
    void trim();

    struct Stats
    {
        uint32_t requested = 0;    // loaded textures waiting for their map
        uint32_t made = 0;         // maps made since the start
        uint32_t upscaled = 0;     // ×2 textures made alone (effects/upscale.hpp) since the start
        double upscale_ms = 0.0;   // their worker time
        double made_ms = 0.0;      // their worker time
        double slowest_ms = 0.0;   // the slowest of them
        std::string paused;        // why the work waits (heat while the game is played, the renderer's hold); empty: it may run
        bool working = false;      // a loaded texture is being prepared now
        std::string device;        // where the networks run (relief::Session's provider), once loaded
    };
    Stats stats();

  private:
    struct Request
    {
        std::weak_ptr<NormalMapEntry> entry;
        std::filesystem::path file;
        uint64_t order = 0;
    };
    struct PendingFinalize
    {
        uint64_t id = 0, key = 0;
        std::optional<uint64_t> relief_key, gloss_key;
        std::optional<Request> request;
        std::weak_ptr<UpscaleEntry> upscale;
        bool is_upscale = false;
    };
    bool wait_finalizer(); // no admission/network lock held
    void stage(std::unique_ptr<relief::ComputedTexture> product, PendingFinalize pending);
    void drain_finalized(); // compute owner, never called with mutex_ held
    void finish(const PendingFinalize &pending, const relief::FinalizeResult &result);
    void run();
    std::optional<Request> next_request(); // under mutex_
    std::shared_ptr<UpscaleEntry> next_upscale(); // under mutex_: nearest live candidate
    void reconcile_upscales(std::unique_lock<std::mutex> &lock); // holding mutex_ (released for the file checks): publish completed files; prune expired/stale requests
    bool open_session();
    void prepare_request(const Request &request);
    void release_models(const char *why); // worker, under mutex_
    void note_device(); // worker: logs a change of the device the textures run on
    bool must_wait(); // under mutex_: the background gate (thermal.hpp), changes logged
    void prepare_upscale(const std::shared_ptr<UpscaleEntry> &entry);
    bool wanted(const NormalMapEntry &entry) const; // under mutex_: its product is switched on (set_wanted)
    bool running_wanted() const; // under mutex_: the job in progress is still wanted (set_wanted, set_display)

    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<int64_t> last_frame_us_{0};
    std::atomic<bool> scene_quiet_{false};
    std::atomic<bool> scene_shown_{false};
    bool scene_draws_[30] = {};
    uint32_t scene_frame_ = 0;
    uint64_t loading_generation_ = 0;
    std::shared_ptr<relief::LoadingRetry> loading_retry_;
    std::vector<std::shared_ptr<NormalMapEntry>> loading_normal_;
    std::vector<std::shared_ptr<UpscaleEntry>> loading_upscale_;
    bool trim_ = false;
    ReliefSetup setup_;
    std::deque<Request> requests_;
    std::deque<std::weak_ptr<UpscaleEntry>> upscales_;
    bool display_ = false, display_bc_ = false;
    bool want_relief_ = false, want_gloss_ = false;
    std::filesystem::path display_directory_;
    uint64_t order_ = 0, admitted_ = 0;
    bool next_upscale_ = false; // a live lane never waits behind the entire other queue
    std::chrono::steady_clock::time_point idle_since_{};
    Stats stats_;
    // Worker only.
    std::unique_ptr<relief::Session> session_;
    std::array<std::vector<uint8_t>, 8> models_; // relief::kModelFiles (checked in the .cpp); ncnn keeps no copy of the weights' bytes
    bool model_failed_ = false;
    bool device_logged_ = false, device_gpu_ = false; // the device last logged (note_device)
    std::unique_ptr<relief::FinalizeQueue> finalizer_queue_;
    std::unique_ptr<relief::Finalizer> inline_finalizer_; // oversized progress, only after the worker slot drains
    std::optional<PendingFinalize> finalizing_; // under mutex_: includes unconsumed completion
    uint64_t next_finalize_ = 0;
    bool computing_ = false, computing_normal_ = false, computing_gloss_ = false; // under mutex_

    relief::Session *running_ = nullptr; // under mutex_: the session shutdown() cancels
};

} // namespace storm::effects
