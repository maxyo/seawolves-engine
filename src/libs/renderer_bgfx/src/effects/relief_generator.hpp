// The game's relief maps made while it runs (the owner's choice): PBRify's models (src/libs/relief) on one worker thread in the
// game process, into the relief directory (fx/relief_store.hpp) the game reads.
//
//   * A loaded colour texture of a relief class without its map, or of a gloss class without its gloss map, is requested
//     (NormalMaps::on_texture_loaded); one preparation makes both (PBRify's normal, height and roughness networks). The
//     requests are reselected from recent player-distance observations before each job, alternating with standalone
//     upscales within the same 10 m band. Unobserved entries fall back to draw/load order; old-scene entries follow
//     current-scene work. Live work ages after 16 other admissions. When its map is written the texture's entry
//     becomes Stored: the draw reads and uploads it and the relief fades in (NormalMaps::handle).
//   * Only loaded normal/gloss and ×2 requests are made. The game never lists/prepares the whole corpus in
//     idle, loading or gameplay. Explicit standalone storm_relief_cli preparation remains independent.
//   * The worker runs at nice 10. CPU jobs use cores-2 in gameplay and all cores
//     after a presented opaque stock frame, sharing admission with ray-bake pieces.
//     The renderer's explicit hold (the automatic graphics preset's measurement) pauses work; device thermal
//     status/headroom is telemetry only. The models are loaded at the first job and released after 10 s with no work left.
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
    void set_display(bool on, std::filesystem::path directory, bool bc);
    // Stops the worker (the texture in progress is abandoned: onnxruntime stops its inference).
    void shutdown();
    // Game thread, every frame: whether a perspective 3D scene was drawn. A few frames without a scene
    // (a loading or an interface screen), or no frame for a second (the game in the background), leave the GPU free:
    // relief::Session can try the GPU again for the next required texture; this never triggers a corpus sweep.
    void frame(bool scene_drawn);
    // Game thread: exact scoped entries eligible for one bounded measured retry. Empty revokes.
    void loading_window(uint64_t generation, uint64_t deadline_ms,
                        std::vector<std::shared_ptr<NormalMapEntry>> normal,
                        std::vector<std::shared_ptr<UpscaleEntry>> upscale);
    bool gpu_free() const;
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
        std::string paused;        // why the work waits (the thermal status, the renderer's hold); empty: it may run
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
    void run();
    std::optional<Request> next_request(); // under mutex_
    std::shared_ptr<UpscaleEntry> next_upscale(); // under mutex_: nearest live candidate
    void reconcile_upscales(); // under mutex_: publish completed files; prune expired/stale requests
    bool open_session();
    void prepare_request(const Request &request);
    void release_models(const char *why); // worker, under mutex_
    bool must_wait(); // under mutex_: the background gate (thermal.hpp), changes logged
    void prepare_upscale(const std::shared_ptr<UpscaleEntry> &entry);

    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<int64_t> last_frame_us_{0};
    std::atomic<bool> scene_quiet_{false};
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
    std::filesystem::path display_directory_;
    uint64_t order_ = 0, admitted_ = 0;
    bool next_upscale_ = false; // a live lane never waits behind the entire other queue
    std::chrono::steady_clock::time_point idle_since_{};
    Stats stats_;
    // Worker only.
    std::unique_ptr<relief::Session> session_;
    std::array<std::vector<uint8_t>, 12> models_; // relief::kModelFiles; onnxruntime keeps no copy of the bytes it was given
    bool model_failed_ = false;
    relief::Session *running_ = nullptr; // under mutex_: the session shutdown() cancels
};

} // namespace storm::effects
