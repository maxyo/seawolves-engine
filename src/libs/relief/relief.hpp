// Relief maps from PBRify's normal, height and roughness models (fx/relief_model.hpp) through ncnn on the GPU or the CPU,
// for the game's stone,
// wood, building, plaster and ground textures, made from the player's game files into the relief directory (fx/relief_store.hpp) the game reads.
// The game makes the maps of the textures its current scene draws with them, while it runs (renderer_bgfx
// effects/relief_generator.hpp: Session); the host tool makes a whole game at once (Prepare: List, every texture, Prune).
#pragma once

#include "device_choice.hpp"
#include "storm/fx/relief_store.hpp"
#include "storm/fx/upscale.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace storm::relief
{
struct LoadingRetry;
struct ComputeResult;
struct FinalizeResult;

// The relief textures of a game (its RESOURCE/Textures), by name, without the characters' atlases (made only on request). Only names are read (a scan of the models for the
// most used ones took minutes on a phone's storage, in the way of the textures on screen).
std::vector<std::filesystem::path> List(const std::filesystem::path &textures);

enum class Outcome
{
    Prepared,
    Existing, // its map is there already
    Skipped,  // no prepared map for it: unknown format, too small, a grey height map (the game derives it), no rule
    Failed,   // logged with the reason
    Yielded,  // stopped on the GPU because the player sees the game's scene (Session::SetGame): to be made again, on the CPU
};

// The model files: ncnn graphs, the same on the GPU and the CPU. All are required.
struct Models
{
    std::span<const uint8_t> normal_param, normal_bin;       // pbrify-normal.ncnn.param, .bin
    std::span<const uint8_t> height_param, height_bin;       // pbrify-height.ncnn.param, .bin
    std::span<const uint8_t> roughness_param, roughness_bin; // pbrify-roughness.ncnn.param, .bin
    std::span<const uint8_t> upscale_param, upscale_bin;     // pbrify-upscale.ncnn.param, .bin (fx/upscale.hpp)
};

// Where the game's ×2 textures go (fx/upscale.hpp: the upscale directory) and the GPU tier they are encoded for.
struct UpscaleOutput
{
    std::filesystem::path directory;
    bool bc = false; // BC1/BC3 (GPUs with S3TC), else ETC2
};
// The model files' names, in Models' order.
inline constexpr const char *kModelFiles[] = {"pbrify-normal.ncnn.param",    "pbrify-normal.ncnn.bin",
                                              "pbrify-height.ncnn.param",    "pbrify-height.ncnn.bin",
                                              "pbrify-roughness.ncnn.param", "pbrify-roughness.ncnn.bin",
                                              "pbrify-upscale.ncnn.param",   "pbrify-upscale.ncnn.bin"};

// The models loaded once for many textures, ncnn on the GPU or the CPU: each texture's device follows the game's state
// (SetGame, device_choice.hpp); the host tool's, the device measured for this machine (backend.hpp ChooseDevice;
// logged). One thread at a time (Cancel aside).
class Session
{
  public:
    struct Stats
    {
        int prepared = 0;
        double infer_seconds = 0.0, max_texture_ms = 0.0;
        std::string provider;
        bool gpu = false;      // the last texture's networks ran on the GPU (else on the CPU)
        DeviceReason device_why = DeviceReason::None; // why the last texture took its device
        bool gpu_possible = false; // a GPU the session may use (its graphs, no crash recorded)
        std::string gpu_crash; // the GPU (and driver) whose inference crashed a previous run, so the CPU is used; else empty
        size_t cpu_memory_peak = 0; // the last texture's CPU networks' memory peak, bytes (backend.hpp kCpuNetworkBytes)
    };
    // `threads`: the CPU's inference threads; 0 = every core. `cache`: where the device choice is kept (the cache
    // root's relief directory). Null and `error` on failure.
    static std::unique_ptr<Session> Open(const Models &models, int threads, const std::filesystem::path &cache,
                                         std::string &error, std::function<bool()> cancelled = {});
    ~Session();
    // One texture file under `textures` (the game's RESOURCE/Textures; the key does not depend on where the game
    // lies), unless its map is there already. A texture up to the map's size is upscaled first and its maps are made
    // from its ×2 (fx::ReliefMapsFromUpscale); with `display` that ×2 is also kept for the game (fx/upscale.hpp).
    Outcome PrepareFile(const std::filesystem::path &textures, const std::filesystem::path &file,
                        const std::filesystem::path &output, int map_size, std::string &failure,
                        const UpscaleOutput *display = nullptr);
    // The game's ×2 of one texture file (fx::ClassifyUpscale by its path: Skipped for a texture that gets none), made
    // from its level whose larger side is `source_size` (the texture as the game loaded it; 0 its top level), unless it is
    // there already.
    Outcome Upscale(const std::filesystem::path &textures, const std::filesystem::path &file, const UpscaleOutput &display,
                    int source_size, std::string &failure);
    // Compute owner only. A non-null product is pending CPU finalization, never Stored/Prepared.
    // Owns every buffer crossing the handoff; the finalizer never accesses Session or its networks.
    ComputeResult ComputeFile(const std::filesystem::path &textures, const std::filesystem::path &file,
                              const std::filesystem::path &output, int map_size, std::string &failure,
                              const UpscaleOutput *display = nullptr);
    ComputeResult ComputeUpscale(const std::filesystem::path &textures, const std::filesystem::path &file,
                                 const UpscaleOutput &display, int source_size, std::string &failure);
    void AccountFinalized(const FinalizeResult &result); // compute owner, originating session only
    // Worker only; before decode/model work, with a conservative workspace estimate (the networks' own memory aside).
    // False cancels this job. The callback holds no network mutex or admission permits.
    void SetComputeAdmission(std::function<bool(size_t)> admission);
    // Any thread (the game's shutdown): the texture being prepared fails at once, and every later one.
    void Cancel();
    // Worker only, before the first texture: the game's state, which decides each texture's device
    // (device_choice.hpp). `scene_shown`: the player sees the game's scene (no texture starts on the GPU, and one on it
    // stops at its next submission: Outcome::Yielded); `gpu_free`: the game leaves the GPU free. Both are called on the
    // worker thread. Without them (the host tool) every texture runs on the device measured for this machine.
    void SetGame(std::function<bool()> gpu_free, std::function<bool()> scene_shown);
    // Worker only: one scoped job, the place's loading window (loading_retry.hpp).
    void SetLoadingRetry(std::shared_ptr<LoadingRetry> retry);
    // Worker only, between textures: the CPU networks' thread count from their next tile; nothing reloaded.
    void SetCpuBudget(unsigned threads);
    const Stats &stats() const;

  private:
    struct Impl;
    explicit Session(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Removes the maps of textures this game no longer has and every map of another method (another game, an older build,
// model or options; fx::ReliefMethod), and the gloss maps (fx/gloss_map.hpp) of textures it no longer has; the
// game's own height maps stay while they are current. `files`: List(textures), the textures Prepare makes maps for.
// Also discovers on-demand textures (characters) for cache validity, without adding them to that queue.
// Reads each source's header and probe level only. Returns the removal count, or -1 on incomplete/unreadable
// source inventory (no maps removed) or output I/O failure (some stale maps may already be removed).
// Current keys are preserved. A caller must retry later after -1.
int Prune(const std::filesystem::path &textures, const std::vector<std::filesystem::path> &files,
          const std::filesystem::path &output, int map_size);

// The whole job at once (host tool and checks): List, every texture, Prune.
struct Job
{
    std::filesystem::path textures; // the game's RESOURCE/Textures
    std::filesystem::path output;   // the relief directory
    Models models;
    int map_size = fx::kReliefMapSize;
    int threads = 0;
    std::optional<UpscaleOutput> display; // the ×2 the maps of small textures are made from, kept for the game too
};
struct Progress
{
    int done = 0, total = 0;
    std::string current;
};
struct Result
{
    int status = 0; // 0 complete, 1 cancelled, 2 error (`error` says why), including a partial pass with failed textures
    std::string error;
    int total = 0, prepared = 0, existing = 0, failed = 0, skipped = 0, removed = 0;
    double seconds = 0.0, infer_seconds = 0.0, max_texture_ms = 0.0;
    long long peak_rss_kb = 0;
    std::string provider;
    std::string Json() const;
};
// `progress` after each texture (and once with done = 0); returning true cancels.
Result Prepare(const Job &job, const std::function<bool(const Progress &)> &progress);

// The first level of the game texture `file` (.tx) whose larger side is at most `max_size`, decoded to RGBA8 (R first)
// into `rgba`. False when the file is no texture the relief reads (palette, cube map, unknown format, damaged).
bool ReadTextureLevel(const std::filesystem::path &file, int max_size, std::vector<uint8_t> &rgba, int &width, int &height);

// The textures of a game that get a ×2 (fx::ClassifyUpscale: their class by name, an alpha that cuts holes as the
// game decides it, effects texture_pixels), with their sizes: the host tool's list for `Session::Upscale`.
struct UpscaleCandidate
{
    std::filesystem::path file;
    std::string name;
    fx::UpscaleTarget target;
    int width = 0, height = 0;
};
std::vector<UpscaleCandidate> UpscaleCandidates(const std::filesystem::path &textures);

long long PeakRssKb();
// Logging of the library: Android log tag "storm-relief" and stderr.
void Log(const char *format, ...);

} // namespace storm::relief
