#include "storm/fx/cpu_admission.hpp"
#include "loading_retry.hpp"
#include "relief.hpp"
#include "finalizer.hpp"

#include "backend.hpp"
#include "device_choice.hpp"
#include "tx_file.hpp"

#include "formats.hpp"
#include "eac_encoder.hpp"
#include "storm/fx/relief_model.hpp"
#include "storm/fx/gloss_map.hpp"
#include "storm/fx/height_map.hpp"
#include "storm/fx/normal_map.hpp"
#include "storm/fx/relief_store.hpp"

#include "upscale.hpp"
#include "cache.hpp"
#include "storm/fx/upscale.hpp"
#include "storm/fx/cutout.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <sys/resource.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace storm::relief
{

void Log(const char *format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
#ifdef __ANDROID__
    __android_log_print(std::strncmp(buffer, "ERROR", 5) == 0 ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "storm-relief", "%s", buffer);
#endif
    std::fprintf(stderr, "[storm-relief] %s\n", buffer); // the shell tool on a device too (nowhere in the app)
}

namespace
{
using Clock = std::chrono::steady_clock;

double Seconds(Clock::time_point since)
{
    return std::chrono::duration<double>(Clock::now() - since).count();
}

struct Candidate
{
    std::filesystem::path file;
    std::string name; // normalized texture path
    fx::ReliefClass relief = fx::ReliefClass::None;
    fx::GlossClass gloss = fx::GlossClass::None;
    // Made by the background pass (List). The characters' atlases (956 in TEHO, ~0.7 MB of maps each) are made only
    // for the characters a location loads, on request.
    bool background = true;
};

std::string Lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A relief or gloss texture: the rules give it relief or gloss, or its name says "bump" (its pixels decide).
std::optional<Candidate> Classify(const std::filesystem::path &textures, const std::filesystem::path &file, std::error_code *failure = nullptr)
{
    std::error_code error;
    if (Lower(file.extension().string()) != ".tx")
        return std::nullopt;
    const std::string name = fx::NormalizeTexturePath(std::filesystem::relative(file, textures, error).generic_string());
    if (error)
    {
        if (failure) *failure = error;
        return std::nullopt;
    }
    const fx::ReliefVerdict verdict = fx::ClassifyTextureName(name);
    const bool bump = fx::NameSuggestsHeightMap(name) && !(verdict.by_folder && verdict.relief == fx::ReliefClass::None);
    const fx::GlossClass gloss = fx::ClassifyGlossName(name).gloss;
    if (verdict.relief == fx::ReliefClass::None && !bump && gloss == fx::GlossClass::None)
        return std::nullopt;
    return Candidate{file, name, verdict.relief, gloss, verdict.relief != fx::ReliefClass::Character};
}

std::vector<Candidate> Candidates(const std::filesystem::path &textures, std::error_code *failure = nullptr)
{
    std::vector<Candidate> list;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(textures, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
    {
        if (it->is_regular_file(error))
            if (std::optional<Candidate> candidate = Classify(textures, it->path(), &error))
                list.push_back(std::move(*candidate));
        if (error)
            break; // increment(error) must not clear a classification/status error.
    }
    if (failure) *failure = error;
    return list;
}

// The texture's key under `method`, or nothing when it gets no prepared map (unknown format, too small for a probe
// level).
std::optional<uint64_t> Key(const Candidate &candidate, const TxFile &tx, uint64_t method, int map_size)
{
    const int probe = fx::ReliefProbeLevel(static_cast<int>(tx.width), static_cast<int>(tx.height), static_cast<int>(tx.levels));
    if (probe < 0)
        return std::nullopt;
    const std::span<const uint8_t> bytes = tx.Level(probe);
    const int pw = static_cast<int>(tx.width >> probe), ph = static_cast<int>(tx.height >> probe);
    if (bytes.size() != texture::d3d_level_bytes(tx.format, pw, ph))
        return std::nullopt;
    return fx::ReliefKey(candidate.name, bytes, pw, ph, method, map_size);
}

std::optional<uint64_t> Key(const Candidate &candidate, const TxFile &tx, int map_size)
{
    return candidate.relief == fx::ReliefClass::None ? std::nullopt
                                                     : Key(candidate, tx, fx::ReliefMethod(candidate.relief), map_size);
}

// The key of the texture's own gloss map: a texture of a gloss class whose relief class has no height (the others
// keep their gloss in their relief's material texture).
std::optional<uint64_t> GlossKey(const Candidate &candidate, const TxFile &tx)
{
    if (candidate.gloss == fx::GlossClass::None || fx::ReliefHasHeight(candidate.relief))
        return std::nullopt;
    return Key(candidate, tx, fx::GlossMethod(candidate.gloss), fx::kGlossMapSize);
}

// Box halvings of an RGBA8 image down to `tw` x `th` (a size NormalMapSize gives: the image halved, or itself).
std::vector<uint8_t> HalveTo(std::vector<uint8_t> rgba, int &w, int &h, int tw, int th)
{
    while (w > tw || h > th)
    {
        const int nw = w > tw ? w / 2 : w, nh = h > th ? h / 2 : h, sx = w / nw, sy = h / nh;
        std::vector<uint8_t> out(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    int sum = 0;
                    for (int dy = 0; dy < sy; ++dy)
                        for (int dx = 0; dx < sx; ++dx)
                            sum += rgba[(static_cast<size_t>(y * sy + dy) * w + x * sx + dx) * 4 + c];
                    out[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + sx * sy / 2) / (sx * sy));
                }
        rgba = std::move(out);
        w = nw;
        h = nh;
    }
    return rgba;
}

// The same for one float plane.
std::vector<float> HalveTo(std::vector<float> v, int w, int h, int tw, int th)
{
    while (w > tw || h > th)
    {
        const int nw = w > tw ? w / 2 : w, nh = h > th ? h / 2 : h, sx = w / nw, sy = h / nh;
        std::vector<float> out(static_cast<size_t>(nw) * nh);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
            {
                float sum = 0.0f;
                for (int dy = 0; dy < sy; ++dy)
                    for (int dx = 0; dx < sx; ++dx)
                        sum += v[static_cast<size_t>(y * sy + dy) * w + x * sx + dx];
                out[static_cast<size_t>(y) * nw + x] = sum / static_cast<float>(sx * sy);
            }
        v = std::move(out);
        w = nw;
        h = nh;
    }
    return v;
}

// A plane of `w` x `h` bilinearly stretched (wrapping: the textures tile) over `tw` x `th`, texel centres aligned.
std::vector<uint8_t> Stretch(std::span<const float> v, int w, int h, int tw, int th)
{
    std::vector<uint8_t> out(static_cast<size_t>(tw) * th);
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x)
        {
            const float fx = (static_cast<float>(x) + 0.5f) * static_cast<float>(w) / static_cast<float>(tw) - 0.5f;
            const float fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(h) / static_cast<float>(th) - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
            const float ax = fx - static_cast<float>(x0), ay = fy - static_cast<float>(y0);
            const auto at = [&](int xi, int yi) {
                return v[static_cast<size_t>(((yi % h) + h) % h) * w + static_cast<size_t>(((xi % w) + w) % w)];
            };
            const float value = (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) +
                                (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
            out[static_cast<size_t>(y) * tw + x] = static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }
    return out;
}

// A grey "bump" texture: a height map the game derives itself (the runtime classifier decides from the same level).
bool GreyHeightMap(const Candidate &candidate, const TxFile &tx)
{
    if (!fx::NameSuggestsHeightMap(candidate.name))
        return false;
    const int probe = fx::ReliefProbeLevel(static_cast<int>(tx.width), static_cast<int>(tx.height), static_cast<int>(tx.levels));
    if (probe < 0)
        return false;
    const uint32_t pw = tx.width >> probe, ph = tx.height >> probe;
    std::vector<uint8_t> rgba(size_t(pw) * ph * 4);
    return texture::decode_rgba8(tx.format, tx.Level(probe).data(), pw, ph, rgba.data()) && fx::IsGrayscale(rgba);
}

// The key of a texture that gets a prepared map, with the class it is prepared as.
std::optional<uint64_t> PreparedKey(const Candidate &candidate, const TxFile &tx, int map_size, fx::ReliefClass &relief)
{
    const std::optional<uint64_t> key = Key(candidate, tx, map_size);
    relief = key && !GreyHeightMap(candidate, tx) ? candidate.relief : fx::ReliefClass::None;
    return relief == fx::ReliefClass::None ? std::nullopt : key;
}

} // namespace

long long PeakRssKb()
{
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss;
}

bool ReadTextureLevel(const std::filesystem::path &file, int max_size, std::vector<uint8_t> &rgba, int &width, int &height)
{
    TxFile tx;
    if (!tx.Load(file, [max_size](const TxFile &t) {
            int level = 0;
            while (level + 1 < static_cast<int>(t.levels) && std::max(t.width >> level, t.height >> level) > uint32_t(max_size))
                ++level;
            return level;
        }))
        return false;
    const uint32_t lw = std::max(1u, tx.width >> tx.first), lh = std::max(1u, tx.height >> tx.first);
    const std::span<const uint8_t> bytes = tx.Level(tx.first);
    if (bytes.size() < texture::d3d_level_bytes(tx.format, lw, lh))
        return false;
    rgba.assign(size_t(lw) * lh * 4, 0);
    if (!texture::decode_rgba8(tx.format, bytes.data(), lw, lh, rgba.data()))
        return false;
    width = static_cast<int>(lw);
    height = static_cast<int>(lh);
    return true;
}

std::vector<std::filesystem::path> List(const std::filesystem::path &textures)
{
    std::vector<Candidate> candidates = Candidates(textures);
    std::erase_if(candidates, [](const Candidate &candidate) { return !candidate.background; });
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.name < b.name; });
    std::vector<std::filesystem::path> files;
    for (const Candidate &candidate : candidates)
        files.push_back(candidate.file);
    return files;
}

namespace
{
// The game's ×2 of a texture (fx/upscale.hpp): its chain encoded for the device's tier, into the upscale directory.
bool WriteDisplay(const UpscaleOutput &display, uint64_t key, const std::vector<uint8_t> &x2, int width, int height,
                  const std::string &name)
{
    const Clock::time_point start = Clock::now();
    bool opaque = true;
    for (size_t i = 3; i < x2.size() && opaque; i += 4)
        opaque = x2[i] == 255;
    const texture::GpuFormat format = display.bc ? (opaque ? texture::GpuFormat::Bc1 : texture::GpuFormat::Bc3)
                                                 : (opaque ? texture::GpuFormat::Etc2Rgb8 : texture::GpuFormat::Etc2Rgba8);
    const texture::Blob ktx = texture::encode_rgba_to_ktx(x2.data(), static_cast<uint32_t>(width),
                                                          static_cast<uint32_t>(height), format);
    if (!fx::WriteUpscaled(display.directory, key, {ktx.data.get(), ktx.size}))
    {
        Log("ERROR: ×2 of %s: cannot write %s", name.c_str(), fx::UpscaleFile(display.directory, key).string().c_str());
        return false;
    }
    Log("×2 %s: %dx%d %s, %.1f MB, mips and encode %.0f ms", name.c_str(), width, height, opaque ? "opaque" : "with alpha",
        static_cast<double>(ktx.size) / 1048576.0, Seconds(start) * 1000.0);
    return true;
}

} // namespace

// Which device a texture runs on: device_choice.hpp. Each change is logged by the game (relief_generator.cpp); the
// crash record of the GPU work is kept across sessions (backend.hpp MarkGpuRunning).
struct Session::Impl
{
    Models models; // owned by the caller, alive with the session
    int threads = 1;
    std::filesystem::path cache;
    bool gpu_possible = false;   // a GPU, its graphs, no crash recorded for it
    bool measured_gpu = false;   // the host tool's device (ChooseDevice)
    bool on_gpu = false;         // the last texture's device
    std::function<bool()> gpu_free, scene_shown; // the game's state (SetGame); empty: the host tool
    std::shared_ptr<LoadingRetry> loading_retry;
    uint64_t loading_retry_at = 0;
    bool TakeLoadingRetry() {
        const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
        if (!loading_retry || !loading_retry->take(now, gpu_possible, loading_retry_at)) return false;
        loading_retry_at = now;
        return true;
    }
    bool Shown() const { return scene_shown && scene_shown(); }
    // The device of the next texture (device_choice.hpp), its networks loaded: false with `failure` when they cannot be.
    bool PickDevice(bool &use_gpu, std::string &failure)
    {
        const DeviceChoice choice = ChooseTextureDevice(gpu_possible, static_cast<bool>(scene_shown), measured_gpu, Shown(),
                                                        gpu_free && gpu_free(), [this] { return TakeLoadingRetry(); });
        use_gpu = choice.gpu;
        if (!EnsureDevice(use_gpu, failure))
            return false;
        on_gpu = use_gpu;
        stats.device_why = use_gpu == choice.gpu ? choice.why : DeviceReason::NoGpu; // its GPU networks failed to load
        Describe();
        return true;
    }
    // A GPU texture stopped because the player sees the scene: no crash, its work ended (the guard's marker goes; the
    // GPU stays unproven until a texture completes on it).
    bool Yielded(bool use_gpu, const std::string &name)
    {
        if (!use_gpu || cancelled || !Shown())
            return false;
        if (gpu_unproven)
            ClearGpuRunning(cache);
        Log("relief %s: stopped on the GPU (the player sees the game's scene): made again on the CPU", name.c_str());
        return true;
    }
    bool gpu_unproven = false; // the GPU's first texture: the crash guard's marker is written around it
    // Controlled GPU model/calibration failures in this session (EnsureDevice): the session gives up the GPU only
    // after kGpuInitAttempts of them; before that the next texture that may take the GPU tries again.
    static constexpr int kGpuInitAttempts = 3;
    int gpu_init_failures = 0;
    std::unique_ptr<CpuRuntime> runtime; // before the networks: released after them
    std::array<std::unique_ptr<Network>, 3> gpu, cpu; // normal, height, roughness
    std::unique_ptr<Upscaler> gpu_upscaler, cpu_upscaler; // the 4x upscaler (fx/upscale.hpp), on each device
    std::mutex networks_mutex;   // swaps against Cancel from another thread
    Stats stats;
    std::atomic<bool> cancelled{false};
    std::function<bool(size_t)> compute_admission;
    std::unique_ptr<Finalizer> synchronous_finalizer; // compute owner only, never used by finalizer thread
    std::shared_ptr<std::atomic<bool>> finalize_cancelled = std::make_shared<std::atomic<bool>>(false);
    inline static std::atomic<uint64_t> next_session{0};
    const uint64_t session = ++next_session;

    std::array<std::unique_ptr<Network>, 3> &Active() { return on_gpu ? gpu : cpu; }
    // Around each texture's computation, however it ends: the CPU networks' memory goes back to the system after it.
    struct CpuJob
    {
        Impl &impl;
        ~CpuJob()
        {
            if (!impl.runtime)
                return;
            impl.stats.cpu_memory_peak = impl.runtime->JobPeak();
            impl.runtime->EndJob();
        }
    };
    // The device's description for the texture's log line; on the CPU with the networks' memory peak of this texture.
    std::string JobProvider() const
    {
        if (on_gpu || !runtime)
            return stats.provider;
        char peak[64];
        std::snprintf(peak, sizeof(peak), "; networks' memory peak %.0f MB",
                      static_cast<double>(runtime->JobPeak()) / 1048576.0);
        return stats.provider + peak;
    }
    bool EnsureGpu(std::string &error)
    {
        if (gpu_upscaler)
            return true;
        std::lock_guard lock(networks_mutex);
        // The networks' pipelines are made on the GPU now: the crash guard's marker stays until its first texture.
        // A failed load may have closed Vulkan (EnsureDevice). Guard the lazy instance creation itself before
        // evaluating Gpu(), just as the initial ChooseDevice path does.
        MarkGpuRunning(cache, kGpuOpening);
        gpu_unproven = true;
        MarkGpuRunning(cache, GpuLabel(Gpu()));
        gpu[0] = LoadGpuNetwork(models.normal_param, models.normal_bin, fx::kReliefModelPad, 1, error);
        if (gpu[0])
            gpu[1] = LoadGpuNetwork(models.height_param, models.height_bin, fx::kReliefModelPad, 1, error);
        if (gpu[1])
            gpu[2] = LoadGpuNetwork(models.roughness_param, models.roughness_bin, fx::kReliefModelPad, 1, error);
        // The upscaler on the same device (its tiles padded by fx::kUpscalePad, beyond its reach).
        if (gpu[2])
            if (std::unique_ptr<Network> upscale = LoadGpuNetwork(models.upscale_param, models.upscale_bin, fx::kUpscalePad, 4, error))
                gpu_upscaler = std::make_unique<Upscaler>(std::move(upscale));
        // Lazy GPU reopen must receive the same exact job scope as already-open networks.
        const auto yield = [this] { return Shown(); };
        for (const auto &network : gpu)
            if (network) { network->SetLoadingRetry(loading_retry); network->SetYield(yield); }
        if (gpu_upscaler) { gpu_upscaler->SetLoadingRetry(loading_retry); gpu_upscaler->SetYield(yield); }
        return gpu_upscaler != nullptr;
    }
    bool EnsureCpu(std::string &error)
    {
        if (cpu_upscaler)
            return true;
        std::lock_guard lock(networks_mutex);
        // A failed load must leave no partial set of networks; a transient allocation/model error can be retried on
        // this Session.
        const auto reopen_started = Clock::now();
        auto next_runtime = CpuRuntime::Open(threads, error);
        if (!next_runtime)
            return false;
        std::array<std::unique_ptr<Network>, 3> next_cpu;
        next_cpu[0] = next_runtime->Load(models.normal_param, models.normal_bin, fx::kReliefModelPad, 1, error);
        if (next_cpu[0])
            next_cpu[1] = next_runtime->Load(models.height_param, models.height_bin, fx::kReliefModelPad, 1, error);
        if (next_cpu[1])
            next_cpu[2] = next_runtime->Load(models.roughness_param, models.roughness_bin, fx::kReliefModelPad, 1, error);
        if (!next_cpu[2])
            return false;
        auto upscale = next_runtime->Load(models.upscale_param, models.upscale_bin, fx::kUpscalePad, 4, error);
        if (!upscale)
            return false;
        // Publish only a complete set, destroying any old networks before replacing their runtime.
        cpu_upscaler.reset();
        cpu = {};
        runtime = std::move(next_runtime);
        cpu = std::move(next_cpu);
        cpu_upscaler = std::make_unique<Upscaler>(std::move(upscale));
        // As many permits as the networks' current threads (SetCpuBudget changes both on this worker thread).
        auto admission = [this] {
            return fx::SharedCpuAdmission().acquire(static_cast<unsigned>(threads), [this] { return cancelled.load(); });
        };
        for (auto &network : cpu) network->SetCpuAdmission(admission);
        cpu_upscaler->SetCpuAdmission(admission);
        Log("relief CPU runtime ready: %d threads, model reopen %.3f ms", threads, std::chrono::duration<double, std::milli>(Clock::now() - reopen_started).count());
        return cpu_upscaler != nullptr;
    }
    bool EnsureDevice(bool &use_gpu, std::string &error)
    {
        if (!use_gpu)
            return EnsureCpu(error);
        if (EnsureGpu(error))
        {
            gpu_init_failures = 0; // consecutive failures count
            return true;
        }
        if (cancelled)
            return false;
        // A controlled model-load/calibration error is not a driver crash. Preserve this texture on the CPU; a
        // transient failure (a lazy Vulkan reopen) leaves the GPU to the next texture that may take it, repeated
        // failures end GPU use for this session.
        ++gpu_init_failures;
        const bool give_up = gpu_init_failures >= kGpuInitAttempts;
        Log("ERROR: relief GPU model initialization failed (%s, attempt %d of %d); %s", error.c_str(), gpu_init_failures,
            kGpuInitAttempts, give_up ? "this session continues on the CPU" : "this texture on the CPU, the GPU again later");
        {
            std::lock_guard lock(networks_mutex);
            gpu_upscaler.reset();
            gpu = {};
        }
        ClearGpuRunning(cache);
        gpu_unproven = false;
        ReleaseGpu();
        if (give_up)
            gpu_possible = stats.gpu_possible = false;
        on_gpu = use_gpu = false;
        error.clear();
        const bool opened = EnsureCpu(error);
        Describe();
        return opened;
    }
    void Describe()
    {
        stats.gpu = on_gpu;
        stats.provider = Active()[0] ? Active()[0]->Describe() : "-";
    }
    ~Impl()
    {
        // Closed before the GPU's first texture (no work came): nothing crashed.
        if (gpu_unproven)
            ClearGpuRunning(cache);
    }
};

Session::Session(std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
}
Session::~Session() = default;

std::unique_ptr<Session> Session::Open(const Models &models, int threads, const std::filesystem::path &cache,
                                       std::string &error, std::function<bool()> cancelled)
{
    auto impl = std::make_unique<Impl>();
    const int cores = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    threads = threads > 0 ? std::min(threads, cores) : cores;
    impl->models = models;
    impl->threads = threads;
    impl->cache = cache;
    // The measured device (backend.hpp ChooseDevice: kept from before, or measured now with the normal network): the
    // host tool's; it also learns whether the GPU works.
    for (const auto &bytes : {models.normal_param, models.normal_bin, models.height_param, models.height_bin, models.roughness_param,
                              models.roughness_bin, models.upscale_param, models.upscale_bin})
        if (bytes.empty())
        {
            error = "relief: a network's ncnn graph is missing (relief.hpp kModelFiles)";
            return nullptr;
        }
    // Calibration may run CPU inference before persistent CPU networks exist.
    // Its fixed pool must participate in the same admission as normal tiles.
    auto probe_permit = fx::SharedCpuAdmission().acquire(static_cast<unsigned>(threads), cancelled);
    if (!probe_permit) { error = "CPU probe admission cancelled"; return nullptr; }
    const Device device = ChooseDevice(cache, models.normal_param, models.normal_bin, fx::kReliefModelPad, threads);
    probe_permit.reset();
    impl->stats.gpu_crash = GpuCrash(cache);
    impl->gpu_possible = impl->stats.gpu_crash.empty() && Gpu().available;
    impl->stats.gpu_possible = impl->gpu_possible;
    impl->measured_gpu = device == Device::Gpu && impl->gpu_possible;
    // The CPU's networks now (the game's device while it is played; a failure here leaves the session unopened), the
    // GPU's at the first texture that runs on it.
    if (!impl->EnsureCpu(error))
    {
        Log("ERROR: %s", error.c_str());
        return nullptr;
    }
    if (!impl->gpu_possible)
        ReleaseGpu(); // opened to learn the GPU, never used
    impl->Describe();
    return std::unique_ptr<Session>(new Session(std::move(impl)));
}

void Session::SetCpuBudget(unsigned threads)
{
    if (!threads || static_cast<unsigned>(impl_->threads) == threads) return;
    const int previous = impl_->threads;
    impl_->synchronous_finalizer.reset(); // compute owner only; async finalizer has its own lifetime
    impl_->threads = static_cast<int>(threads);
    {
        std::lock_guard lock(impl_->networks_mutex);
        if (impl_->runtime)
            impl_->runtime->SetThreads(impl_->threads); // the loaded networks run with it from their next tile
    }
    Log("relief CPU budget: %u threads (from %d)", threads, previous);
}

void Session::SetGame(std::function<bool()> gpu_free, std::function<bool()> scene_shown)
{
    impl_->gpu_free = std::move(gpu_free);
    impl_->scene_shown = std::move(scene_shown);
}

void Session::SetLoadingRetry(std::shared_ptr<LoadingRetry> retry)
{
    impl_->loading_retry = std::move(retry);
    for (const auto &network : impl_->gpu)
        if (network) network->SetLoadingRetry(impl_->loading_retry);
    if (impl_->gpu_upscaler) impl_->gpu_upscaler->SetLoadingRetry(impl_->loading_retry);
}

void Session::Cancel()
{
    impl_->cancelled = true;
    *impl_->finalize_cancelled = true;
    std::lock_guard lock(impl_->networks_mutex);
    for (auto *set : {&impl_->gpu, &impl_->cpu})
        for (const std::unique_ptr<Network> &network : *set)
            if (network)
                network->Cancel();
    for (Upscaler *upscaler : {impl_->gpu_upscaler.get(), impl_->cpu_upscaler.get()})
        if (upscaler)
            upscaler->Cancel();
}

const Session::Stats &Session::stats() const
{
    return impl_->stats;
}

ComputeResult Session::ComputeFile(const std::filesystem::path &textures, const std::filesystem::path &file,
                             const std::filesystem::path &output, int map_size, std::string &failure,
                             const UpscaleOutput *display)
{
    // One decode, then PBRify's networks: the normals and the height at the map size, the
    // roughness at the gloss map's. Outputs: the relief map (EAC RG11 normals, with the material texture of a class with
    // a height: R the height normalised per region, G the gloss) and, for a texture of a gloss class without a height,
    // its own gloss map (R the metal mask, G the gloss). Each stage is timed: the log line of the texture says where the
    // time went.
    const Impl::CpuJob job{*impl_};
    const Clock::time_point start = Clock::now();
    Clock::time_point mark = start;
    const auto lap = [&mark] {
        const Clock::time_point now = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - mark).count();
        mark = now;
        return ms;
    };
    const std::optional<Candidate> candidate = Classify(textures, file);
    TxFile tx;
    fx::ReliefClass relief = fx::ReliefClass::None;
    // From the level the models read (the first no larger than twice the map) on: it and the probe level.
    const auto source_level = [map_size](const TxFile &t) {
        int level = 0;
        while (level + 1 < static_cast<int>(t.levels) && std::max(t.width >> level, t.height >> level) > uint32_t(2 * map_size))
            ++level;
        return level;
    };
    // The keys from the probe level; the models' level only when a map is to be made.
    if (!candidate)
        return Outcome::Skipped;
    if (!tx.LoadProbe(file))
    {
        if (!tx.unreadable)
            return Outcome::Skipped; // not a texture the relief reads (TxFile::Error says why)
        failure = tx.Error();
        Log("ERROR: no relief map for %s: %s", candidate->name.c_str(), failure.c_str());
        return Outcome::Failed;
    }
    const std::optional<uint64_t> key = PreparedKey(*candidate, tx, map_size, relief);
    const std::optional<uint64_t> gloss_key = GlossKey(*candidate, tx);
    if (!key && !gloss_key)
        return Outcome::Skipped;
    if ((!key || fx::HasReliefMap(output, *key)) && (!gloss_key || fx::HasGlossMap(output, *gloss_key)))
        return Outcome::Existing;
    const int selected = source_level(tx);
    const size_t source_area = size_t(std::max(1u, tx.width >> selected)) * std::max(1u, tx.height >> selected);
    // Full-image vectors + maximum x4 tile I/O scratch. The networks' own activations are bounded by their tiles.
    if (impl_->compute_admission && !impl_->compute_admission(source_area * 256 + (size_t{32} << 20))) {
        failure = "cancelled"; return Outcome::Failed;
    }
    if (!tx.Load(file, source_level))
    {
        failure = tx.Error(); // its probe read a moment ago: the file changed or is damaged
        Log("ERROR: no relief map for %s: %s", candidate->name.c_str(), failure.c_str());
        return Outcome::Failed;
    }
    const double read_ms = lap();
    // The source: the first level no larger than twice the map, which the models' inputs are box-filtered down from.
    const int level = tx.first;
    const uint32_t lw = std::max(1u, tx.width >> level), lh = std::max(1u, tx.height >> level);
    int width = static_cast<int>(lw), height = static_cast<int>(lh);
    std::vector<uint8_t> rgba(size_t(lw) * lh * 4);
    const std::span<const uint8_t> bytes = tx.Level(level);
    if (bytes.size() < texture::d3d_level_bytes(tx.format, lw, lh) ||
        !texture::decode_rgba8(tx.format, bytes.data(), lw, lh, rgba.data()))
    {
        failure = "unreadable level " + std::to_string(level);
        Log("ERROR: no relief map for %s: %s", candidate->name.c_str(), failure.c_str());
        return Outcome::Failed;
    }
    const double decode_ms = lap();
    // The device of this texture: the game's state (device_choice.hpp).
    bool use_gpu = false;
    {
        std::string error;
        if (!impl_->PickDevice(use_gpu, error))
        {
            failure = error;
            Log("ERROR: no relief map for %s: %s", candidate->name.c_str(), failure.c_str());
            return Outcome::Failed;
        }
    }
    std::array<std::unique_ptr<Network>, 3> &nets = use_gpu ? impl_->gpu : impl_->cpu;
    Upscaler &upscaler = use_gpu ? *impl_->gpu_upscaler : *impl_->cpu_upscaler;
    // A texture up to the map's size: its ×2 first (fx::ReliefMapsFromUpscale), the models run on it at up to twice the
    // map's size and are box-filtered to the map (the ×2 is kept for the game's display too, `display`).
    double upscale_ms = 0.0;
    int model_scale = 1;
    if (fx::ReliefMapsFromUpscale(width, height))
    {
        std::vector<uint8_t> x2;
        const fx::UpscaleTarget target = fx::ClassifyUpscale(candidate->name, relief, false);
        // The crash guard around the GPU's first texture (backend.hpp MarkGpuRunning): the ×2 is its first network.
        if (use_gpu && impl_->gpu_unproven)
            MarkGpuRunning(impl_->cache, GpuLabel(Gpu()));
        if (!upscaler.Run(rgba, width, height, fx::UpscaleWraps(target, relief), x2, failure))
        {
            if (impl_->Yielded(use_gpu, candidate->name))
                return Outcome::Yielded;
            if (impl_->cancelled)
                failure = "cancelled";
            Log("ERROR: no relief map for %s: its ×2: %s", candidate->name.c_str(), failure.c_str());
            return Outcome::Failed;
        }
        if (display && level == 0 && target != fx::UpscaleTarget::None)
            if (const std::optional<uint64_t> x2_key = Key(*candidate, tx, fx::UpscaleMethod(display->bc), std::max(width, height)))
                if (!fx::HasUpscaled(display->directory, *x2_key)) {
                    auto permit = fx::SharedCpuAdmission().acquire(1, [&] { return impl_->cancelled.load(); });
                    if (!permit) { failure = "cancelled"; return Outcome::Failed; }
                    WriteDisplay(*display, *x2_key, x2, width * 2, height * 2, candidate->name);
                }
        rgba = std::move(x2);
        width *= 2;
        height *= 2;
        model_scale = 2;
        upscale_ms = lap();
    }
    // The sizes: the relief map's (map_size) and the gloss's (fx::kGlossMapSize), halvings of the texture itself; on a ×2
    // the normal and height networks run at twice the map's size and are box-filtered down to it.
    fx::NormalMapOptions options;
    options.max_size = map_size;
    int fw = 0, fh = 0;
    fx::NormalMapSize(width / model_scale, height / model_scale, options, fw, fh);
    options.max_size = map_size * model_scale;
    int mw = 0, mh = 0;
    fx::NormalMapSize(width, height, options, mw, mh);
    options.max_size = fx::kGlossMapSize;
    int gw = 0, gh = 0;
    fx::NormalMapSize(width / model_scale, height / model_scale, options, gw, gh);
    double infer_ms = 0.0;
    std::string per_model; // each inference: the model, its padded size, its time
    const auto run = [&](Network &model, const std::vector<float> &input, int w, int h) {
        return fx::ReliefModelRun(input, w, h, [&](const float *image, int pw, int ph, float *out) {
            const Clock::time_point infer_start = Clock::now();
            const bool ok = model.Infer(image, pw, ph, out, failure);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - infer_start).count();
            infer_ms += ms;
            char part[64];
            std::snprintf(part, sizeof(part), "%s%s %dx%d %.0f ms", per_model.empty() ? "" : ", ",
                          &model == nets[0].get() ? "normal" : &model == nets[1].get() ? "height" : "roughness", pw, ph, ms);
            per_model += part;
            return ok;
        });
    };
    // The crash guard around the GPU's first texture (backend.hpp MarkGpuRunning).
    if (use_gpu && impl_->gpu_unproven)
        MarkGpuRunning(impl_->cache, GpuLabel(Gpu()));
    const bool height_class = key && fx::ReliefHasHeight(relief);
    const bool gloss_wanted = candidate->gloss != fx::GlossClass::None && (height_class || gloss_key);
    bool failed = false;
    std::vector<float> normal_planes, height_planes, gloss_height, roughness_planes;
    if (key)
    {
        const std::vector<float> input = fx::ReliefModelInput(rgba, width, height, mw, mh);
        normal_planes = run(*nets[0], input, mw, mh);
        failed |= normal_planes.empty();
        if (height_class && !failed)
        {
            height_planes = run(*nets[1], input, mw, mh);
            failed |= height_planes.empty();
        }
    }
    if (gloss_wanted && !failed)
    {
        const std::vector<float> input = fx::ReliefModelInput(rgba, width, height, gw, gh);
        roughness_planes = run(*nets[2], input, gw, gh);
        failed |= roughness_planes.empty();
        // The gloss's grooves read the height at its own size: the map's halved, or the height network at the gloss's
        // size when the texture has no relief height.
        if (height_class && !failed)
            gloss_height = HalveTo(fx::ReliefModelHeight(height_planes, mw, mh), mw, mh, gw, gh);
        else if (!failed)
        {
            const std::vector<float> planes = run(*nets[1], input, gw, gh);
            failed |= planes.empty();
            if (!failed)
                gloss_height = fx::ReliefModelHeight(planes, gw, gh);
        }
    }
    impl_->stats.infer_seconds += infer_ms / 1000.0;
    if (use_gpu && impl_->gpu_unproven && !failed)
    {
        ClearGpuRunning(impl_->cache);
        impl_->gpu_unproven = false;
    }
    const double model_ms = lap(); // the inputs, padding and crops around the inferences, and the inferences
    if (failed && impl_->Yielded(use_gpu, candidate->name))
        return Outcome::Yielded;
    if (impl_->cancelled)
    {
        // A stopped inference's error varies (and its output, if any, is not the model's).
        failure = "cancelled";
        Log("relief %s: cancelled", candidate->name.c_str());
        return Outcome::Failed;
    }
    if (failed)
    {
        Log("ERROR: no relief map for %s: %s", candidate->name.c_str(), failure.c_str());
        return Outcome::Failed;
    }
    auto product = std::make_unique<ComputedTexture>();
    product->session = impl_->session;
    product->cancelled = impl_->finalize_cancelled;
    product->name = candidate->name; product->gloss_class = candidate->gloss;
    product->output = output; product->key = key; product->gloss_key = gloss_key; product->relief = relief;
    product->width = width; product->height = height; product->model_scale = model_scale;
    product->mw = mw; product->mh = mh; product->fw = fw; product->fh = fh; product->gw = gw; product->gh = gh;
    product->height_class = height_class; product->gloss_wanted = gloss_wanted;
    product->threads = static_cast<unsigned>(impl_->threads); product->provider = impl_->JobProvider();
    product->rgba = std::move(rgba); product->normal_planes = std::move(normal_planes);
    product->height_planes = std::move(height_planes); product->gloss_height = std::move(gloss_height);
    product->roughness_planes = std::move(roughness_planes); product->per_model = std::move(per_model);
    product->started = start; product->read_ms = read_ms; product->decode_ms = decode_ms;
    product->upscale_ms = upscale_ms; product->model_ms = model_ms; product->infer_ms = infer_ms;
    product->computed_at = Clock::now();
    return product;
}

ComputeResult Session::ComputeUpscale(const std::filesystem::path &textures, const std::filesystem::path &file, const UpscaleOutput &display,
                         int source_size, std::string &failure)
{
    // The ×2 alone (fx/upscale.hpp): a texture whose maps do not come from it (larger than the map) or that has none (the
    // characters). Its class decides the padding and whether it gets one at all.
    const Impl::CpuJob job{*impl_};
    const Clock::time_point start = Clock::now();
    std::error_code error;
    const std::string name = fx::NormalizeTexturePath(std::filesystem::relative(file, textures, error).generic_string());
    TxFile tx;
    // From the level the game loaded as its top (0: the file's), the probe level below it.
    if (error)
        return Outcome::Skipped;
    if (!tx.Load(file, [&source_size](const TxFile &t) {
            if (source_size <= 0)
                source_size = static_cast<int>(std::max(t.width, t.height));
            int level = 0;
            while (level + 1 < static_cast<int>(t.levels) &&
                   static_cast<int>(std::max(t.width >> level, t.height >> level)) > source_size)
                ++level;
            return level;
        }))
    {
        if (!tx.unreadable)
            return Outcome::Skipped;
        failure = tx.Error();
        return Outcome::Failed;
    }
    const fx::ReliefVerdict verdict = fx::ClassifyTextureName(name);
    const fx::UpscaleTarget target =
        fx::ClassifyUpscale(name, fx::NameSuggestsHeightMap(name) ? fx::ReliefClass::HeightMap : verdict.relief, false);
    const int level = tx.first;
    const uint32_t lw = std::max(1u, tx.width >> level), lh = std::max(1u, tx.height >> level);
    if (target == fx::UpscaleTarget::None || static_cast<int>(std::max(lw, lh)) != source_size ||
        !fx::UpscaleSize(static_cast<int>(lw), static_cast<int>(lh)))
        return Outcome::Skipped;
    const std::optional<uint64_t> key = Key(Candidate{file, name}, tx, fx::UpscaleMethod(display.bc), source_size);
    if (!key)
        return Outcome::Skipped;
    if (fx::HasUpscaled(display.directory, *key))
        return Outcome::Existing;
    if (impl_->compute_admission && !impl_->compute_admission(size_t(lw) * lh * 256 + (size_t{32} << 20))) {
        failure = "cancelled"; return Outcome::Failed;
    }
    std::vector<uint8_t> rgba(size_t(lw) * lh * 4), x2;
    const std::span<const uint8_t> bytes = tx.Level(level);
    if (bytes.size() < texture::d3d_level_bytes(tx.format, lw, lh) || !texture::decode_rgba8(tx.format, bytes.data(), lw, lh, rgba.data()))
    {
        failure = "unreadable level " + std::to_string(level);
        return Outcome::Failed;
    }
    const double decode_ms = Seconds(start) * 1000.0;
    // The device of this texture: the game's state (device_choice.hpp), as for the relief textures.
    bool use_gpu = false;
    if (!impl_->PickDevice(use_gpu, failure))
        return Outcome::Failed;
    Upscaler &upscaler = use_gpu ? *impl_->gpu_upscaler : *impl_->cpu_upscaler;
    if (use_gpu && impl_->gpu_unproven)
        MarkGpuRunning(impl_->cache, GpuLabel(Gpu()));
    if (!upscaler.Run(rgba, static_cast<int>(lw), static_cast<int>(lh), fx::UpscaleWraps(target, verdict.relief), x2, failure))
    {
        if (impl_->Yielded(use_gpu, name))
            return Outcome::Yielded;
        if (impl_->cancelled)
            failure = "cancelled";
        return Outcome::Failed;
    }
    if (use_gpu && impl_->gpu_unproven)
    {
        ClearGpuRunning(impl_->cache);
        impl_->gpu_unproven = false;
    }
    const double infer_ms = upscaler.infer_ms();
    if (impl_->cancelled) { failure = "cancelled"; return Outcome::Failed; }
    auto product = std::make_unique<ComputedTexture>();
    product->kind = ComputedTexture::Kind::Upscale;
    product->session = impl_->session; product->cancelled = impl_->finalize_cancelled;
    product->name = name; product->key = key; product->display = display; product->target = target;
    product->width = static_cast<int>(lw); product->height = static_cast<int>(lh);
    product->rgba = std::move(x2); product->started = start;
    product->decode_ms = decode_ms; product->infer_ms = infer_ms;
    product->threads = static_cast<unsigned>(impl_->threads); product->provider = impl_->JobProvider();
    product->computed_at = Clock::now();
    return product;
}


Finalizer::Finalizer() = default;
Finalizer::~Finalizer() = default;

FinalizeResult Finalizer::Run(std::unique_ptr<ComputedTexture> product)
{
    FinalizeResult result;
    if (!product || !product->cancelled) { result.failure = "no computed texture"; return result; }
    auto &p = *product;
    result.session = p.session; result.provider = p.provider;
    auto &cancelled = *p.cancelled;
    auto &failure = result.failure;
    const auto finalize_start = Clock::now();
    if (cancelled) { failure = "cancelled"; return result; }
    const auto start = p.started;
    auto mark = finalize_start;
    const auto lap = [&mark] {
        const auto now = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - mark).count();
        mark = now; return ms;
    };
    const auto acquire = [&](unsigned count) {
        return fx::SharedCpuAdmission().acquire(count, [&] { return cancelled.load(); });
    };
    auto material_permit = acquire(1);
    if (!material_permit) { failure = "cancelled"; return result; }
    if (p.kind == ComputedTexture::Kind::Upscale) {
        result.upscale = true;
        // WriteDisplay is single-threaded; this is its sole CPU permit.
        if (cancelled || !WriteDisplay(p.display, *p.key, p.rgba, p.width * 2, p.height * 2, p.name)) {
            failure = cancelled ? "cancelled" : "cannot write its file"; return result;
        }
        result.upscale_written = p.key;
        result.outcome = Outcome::Prepared;
        result.total_ms = Seconds(start) * 1000.0;
        result.finalize_ms = Seconds(finalize_start) * 1000.0;
        Log("×2 %s (%s): %dx%d, %.0f ms (read and decode %.0f, inference %.0f; finalize %.0f; %s)",
            p.name.c_str(), fx::UpscaleTargetName(p.target), p.width, p.height, result.total_ms,
            p.decode_ms, p.infer_ms, result.finalize_ms, p.provider.c_str());
        return result;
    }
    auto &rgba = p.rgba; auto &normal_planes = p.normal_planes; auto &height_planes = p.height_planes;
    auto &gloss_height = p.gloss_height; auto &roughness_planes = p.roughness_planes;
    auto &mw = p.mw; auto &mh = p.mh;
    const int fw = p.fw, fh = p.fh, gw = p.gw, gh = p.gh, width = p.width, height = p.height;
    const int model_scale = p.model_scale;
    const auto &key = p.key; const auto &gloss_key = p.gloss_key; const auto &output = p.output;
    const bool height_class = p.height_class, gloss_wanted = p.gloss_wanted;
    const auto relief = p.relief; const auto gloss_class = p.gloss_class;
    const auto &name = p.name; const auto &provider = p.provider; const auto &per_model = p.per_model;
    const double read_ms = p.read_ms, decode_ms = p.decode_ms, upscale_ms = p.upscale_ms;
    const double model_ms = p.model_ms, infer_ms = p.infer_ms;
    // The models' planes at the map's size (box-filtered when they ran on a ×2 at twice it).
    if (mw != fw || mh != fh)
    {
        for (std::vector<float> *planes : {&normal_planes, &height_planes})
            if (!planes->empty())
            {
                const size_t n = static_cast<size_t>(mw) * mh;
                std::vector<float> boxed;
                for (int c = 0; c < 3; ++c)
                {
                    const std::vector<float> one =
                        HalveTo(std::vector<float>(planes->begin() + c * n, planes->begin() + (c + 1) * n), mw, mh, fw, fh);
                    boxed.insert(boxed.end(), one.begin(), one.end());
                }
                *planes = std::move(boxed);
            }
        mw = fw;
        mh = fh;
    }
    fx::GlossField gloss;
    if (gloss_wanted)
    {
        int cw = width, ch = height;
        const std::vector<uint8_t> colour = HalveTo(rgba, cw, ch, gw, gh);
        gloss = fx::GlossOfRoughness(fx::ReliefModelHeight(roughness_planes, gw, gh), gloss_height, colour, gw, gh,
                                     gloss_class);
    }
    const double gloss_ms = lap();
    double encode_ms = 0.0, store_ms = 0.0;
    unsigned encode_threads = 1;
    const auto encode = [&](const fx::NormalMap &source, const std::vector<uint8_t> &texels,
                            std::vector<uint8_t> &encoded) {
        const auto begin = Clock::now();
        // Release the one-thread material permit before acquiring the encoder team: never nested.
        material_permit.reset();
        const unsigned participants = source.width > 512 || source.height > 512 ||
            size_t(texture::blocks_x(source.width)) * texture::blocks_x(source.height) < 256 ? 1u :
            std::min(4u, fx::SharedCpuAdmission().threads());
        if (!encoder_ || encoder_threads_ != participants) {
            encoder_ = std::make_unique<texture::EacEncoder>(participants);
            encoder_threads_ = participants;
        }
        auto permit = acquire(participants);
        const bool ok = static_cast<bool>(permit) && !texels.empty() && source.width > 0 && source.height > 0 && source.mip_count > 0 &&
                       encoder_->Encode(texture::EacFormat::Rg11, texels.data(), source.width, source.height,
                                               source.mip_count, encoded, cancelled);
        encode_ms += std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        encode_threads = std::max(encode_threads, encoder_->participants());
        if (!ok)
        {
            failure = cancelled ? "cancelled" : "invalid EAC input";
            Log("ERROR: no relief map for %s: %s", name.c_str(), failure.c_str());
        }
        return ok;
    };
    fx::NormalMap map;
    if (key)
    {
        fx::NormalMap rg8 = fx::NormalMapOfNormals(fx::ReliefModelNormals(normal_planes, mw, mh), mw, mh);
        if (height_class)
        {
            // The material's G: the gloss stretched over the map (empty: no gloss class, G 0).
            const std::vector<uint8_t> g = gloss.gloss.empty() ? std::vector<uint8_t>{} : Stretch(gloss.gloss, gw, gh, mw, mh);
            // The parallax trust reads the colour at the map's size.
            int aw = width, ah = height;
            const std::vector<uint8_t> albedo = HalveTo(rgba, aw, ah, mw, mh);
            if (aw != mw || ah != mh ||
                !fx::AddReliefMaterial(rg8, relief, fx::ReliefModelHeight(height_planes, mw, mh), g, albedo))
                Log("relief %s: flat, no height", name.c_str());
        }
        map = {rg8.width, rg8.height, rg8.mip_count, fx::NormalMapFormat::EacRg11, {}};
        if (!encode(rg8, rg8.rg, map.rg))
            return result;
        if (!rg8.material.empty())
        {
            if (!encode(rg8, rg8.material, map.material))
                return result;
            map.depth = rg8.depth;
            map.trust = std::move(rg8.trust);
            map.trust_coverage = rg8.trust_coverage;
        }
        if (cancelled)
        {
            failure = "cancelled";
            return result;
        }
        const auto store_start = Clock::now();
        const bool written = fx::WriteReliefMap(output, *key, map);
        store_ms += std::chrono::duration<double, std::milli>(Clock::now() - store_start).count();
        if (!written)
        {
            failure = "cannot write " + fx::ReliefFile(output, *key).string();
            Log("ERROR: no relief map for %s: %s", name.c_str(), failure.c_str());
            return result;
        }
        result.relief_written = key;
    }
    if (gloss_key)
    {
        if (!material_permit)
            material_permit = acquire(1);
        if (!material_permit) { failure = "cancelled"; return result; }
        const fx::NormalMap rg = fx::GlossMaterial(gloss);
        fx::NormalMap stored{rg.width, rg.height, rg.mip_count, fx::NormalMapFormat::EacRg11, {}};
        if (!encode(rg, rg.rg, stored.rg))
            return result;
        if (cancelled)
        {
            failure = "cancelled";
            return result;
        }
        const auto store_start = Clock::now();
        const bool written = fx::WriteGlossMap(output, *gloss_key, stored);
        store_ms += std::chrono::duration<double, std::milli>(Clock::now() - store_start).count();
        if (!written)
        {
            failure = "cannot write " + fx::GlossFile(output, *gloss_key).string();
            Log("ERROR: no gloss map for %s: %s", name.c_str(), failure.c_str());
            return result;
        }
        result.gloss_written = gloss_key;
    }
    const double write_ms = lap();
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    result.total_ms = ms;
    result.finalize_ms = std::chrono::duration<double, std::milli>(Clock::now() - finalize_start).count();
    double mean_gloss = 0.0;
    for (const float g : gloss.gloss)
        mean_gloss += g;
    mean_gloss = gloss.gloss.empty() ? 0.0 : mean_gloss / static_cast<double>(gloss.gloss.size());
    Log("relief %s: %dx%d%s%s, gloss %s %dx%d (mean %.2f), %.0f ms (read %.0f, decode %.0f, ×2 %.0f, models %.0f of which "
        "inference %.0f: %s; gloss %.0f, mips and material %.0f, EAC %.0f on %u threads, write %.0f; "
        "depth %.1f texels, parallax trust %.0f%%; %s)",
        name.c_str(), key ? mw : 0, key ? mh : 0, key ? "" : " (no relief)",
        model_scale > 1 ? " from its ×2" : "", fx::GlossClassName(gloss_class), gloss.width, gloss.height, mean_gloss,
        ms, read_ms, decode_ms, upscale_ms, model_ms, infer_ms, per_model.c_str(), gloss_ms,
        std::max(0.0, write_ms - encode_ms - store_ms), encode_ms, encode_threads, store_ms,
        static_cast<double>(map.depth), 100.0 * static_cast<double>(map.trust_coverage), provider.c_str());
    result.outcome = Outcome::Prepared;
    return result;
}

void Session::SetComputeAdmission(std::function<bool(size_t)> admission)
{
    impl_->compute_admission = std::move(admission);
}

void Session::AccountFinalized(const FinalizeResult &result)
{
    if (result.session != impl_->session || result.outcome != Outcome::Prepared || result.upscale) return;
    ++impl_->stats.prepared;
    impl_->stats.max_texture_ms = std::max(impl_->stats.max_texture_ms, result.total_ms);
}

Outcome Session::PrepareFile(const std::filesystem::path &textures, const std::filesystem::path &file,
    const std::filesystem::path &output, int map_size, std::string &failure, const UpscaleOutput *display)
{
    auto computed = ComputeFile(textures, file, output, map_size, failure, display);
    if (!computed.product) return computed.outcome;
    if (!impl_->synchronous_finalizer) impl_->synchronous_finalizer = std::make_unique<Finalizer>();
    auto result = impl_->synchronous_finalizer->Run(std::move(computed.product));
    AccountFinalized(result); failure = std::move(result.failure); return result.outcome;
}
Outcome Session::Upscale(const std::filesystem::path &textures, const std::filesystem::path &file,
    const UpscaleOutput &display, int source_size, std::string &failure)
{
    auto computed = ComputeUpscale(textures, file, display, source_size, failure);
    if (!computed.product) return computed.outcome;
    if (!impl_->synchronous_finalizer) impl_->synchronous_finalizer = std::make_unique<Finalizer>();
    auto result = impl_->synchronous_finalizer->Run(std::move(computed.product));
    AccountFinalized(result); failure = std::move(result.failure); return result.outcome;
}

std::vector<UpscaleCandidate> UpscaleCandidates(const std::filesystem::path &textures)
{
    std::vector<UpscaleCandidate> list;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(textures, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
    {
        std::error_code e;
        if (!it->is_regular_file(e) || Lower(it->path().extension().string()) != ".tx")
            continue;
        const std::string name = fx::NormalizeTexturePath(std::filesystem::relative(it->path(), textures, e).generic_string());
        const fx::ReliefClass relief =
            fx::NameSuggestsHeightMap(name) ? fx::ReliefClass::HeightMap : fx::ClassifyTextureName(name).relief;
        if (e || fx::ClassifyUpscale(name, relief, false) == fx::UpscaleTarget::None)
            continue;
        // The cut-out test of the game (effects texture_pixels: the level no larger than 128, alpha against 160/255).
        TxFile tx;
        if (!tx.Load(it->path(), [](const TxFile &t) {
                int level = 0;
                while (level + 1 < static_cast<int>(t.levels) && std::max(t.width >> level, t.height >> level) > 128u)
                    ++level;
                return level;
            }))
            continue;
        const int level = tx.first;
        const uint32_t lw = std::max(1u, tx.width >> level), lh = std::max(1u, tx.height >> level);
        const std::span<const uint8_t> bytes = tx.Level(level);
        std::vector<uint8_t> alpha(size_t(lw) * lh);
        fx::AlphaMask mask;
        const bool cutout = bytes.size() >= texture::d3d_level_bytes(tx.format, lw, lh) &&
                            texture::decode_alpha8(tx.format, bytes.data(), lw, lh, alpha.data()) &&
                            fx::MakeAlphaMask(std::move(alpha), static_cast<int>(lw), static_cast<int>(lh), 160.0f / 255.0f, mask);
        const fx::UpscaleTarget target = fx::ClassifyUpscale(name, relief, cutout);
        if (target != fx::UpscaleTarget::None && fx::UpscaleSize(static_cast<int>(tx.width), static_cast<int>(tx.height)))
            list.push_back({it->path(), name, target, static_cast<int>(tx.width), static_cast<int>(tx.height)});
    }
    std::sort(list.begin(), list.end(), [](const UpscaleCandidate &a, const UpscaleCandidate &b) { return a.name < b.name; });
    return list;
}

int Prune(const std::filesystem::path &textures, const std::vector<std::filesystem::path> &files,
          const std::filesystem::path &output, int map_size)
{
    // List is a generation queue, not the complete cache inventory: character maps are
    // prepared only on demand. Keep their current keys too, without scheduling inference.
    std::vector<std::filesystem::path> sources = files;
    std::error_code inventory_error;
    const auto inventory = Candidates(textures, &inventory_error);
    if (inventory_error)
        return -1; // A partial walk cannot establish which source textures were deleted.
    for (const Candidate &candidate : inventory)
        sources.push_back(candidate.file); // Also repair a possibly incomplete earlier background List.
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    std::vector<std::filesystem::path> current;
    for (const std::filesystem::path &file : sources)
    {
        std::error_code source_error;
        const std::optional<Candidate> found = Classify(textures, file, &source_error);
        if (source_error)
            return -1;
        if (!found)
            continue;
        const Candidate &candidate = *found;
        TxFile tx;
        if (!tx.LoadProbe(candidate.file))
        {
            if (tx.unsupported_cube)
                continue; // A valid cube cannot own a prepared relief/gloss key; it is not retryable I/O.
            // Missing after a successful root walk is a deleted source; unreadable/invalid is not.
            const bool exists = std::filesystem::exists(candidate.file, source_error);
            if (source_error || exists)
                return -1;
            continue;
        }
        const int probe = TxFile::ProbeLevel(tx);
        if (probe >= 0 && tx.Level(probe).size() !=
                              texture::d3d_level_bytes(tx.format, tx.width >> probe, tx.height >> probe))
            return -1; // A truncated read is uncertainty, not a deleted or unsupported texture.
        fx::ReliefClass relief;
        // A texture's own gloss map (fx/gloss_map.hpp): a gloss class without a relief height.
        if (const std::optional<uint64_t> gloss_key = GlossKey(candidate, tx))
            current.push_back(fx::GlossFile(output, *gloss_key));
        if (const std::optional<uint64_t> key = PreparedKey(candidate, tx, map_size, relief))
            current.push_back(fx::ReliefFile(output, *key));
    }
    std::sort(current.begin(), current.end());
    int removed = 0;
    std::error_code error;
    for (auto it = std::filesystem::directory_iterator(output, error);
         !error && it != std::filesystem::directory_iterator(); it.increment(error))
    {
        const auto &path = it->path();
        if ((path.extension() == ".relief" || path.extension() == ".gloss") &&
            !std::binary_search(current.begin(), current.end(), path))
        {
            const bool erased = std::filesystem::remove(path, error);
            if (error)
                return -1; // Earlier stale entries may be gone; current keys are never removed.
            removed += erased ? 1 : 0;
        }
    }
    return error ? -1 : removed;
}

std::string Result::Json() const
{
    char text[640];
    std::snprintf(text, sizeof(text),
                  "{\"status\":%d,\"total\":%d,\"prepared\":%d,\"existing\":%d,\"failed\":%d,\"skipped\":%d,\"removed\":%d,"
                  "\"seconds\":%.1f,\"infer_seconds\":%.1f,\"max_texture_ms\":%.0f,\"peak_rss_kb\":%lld,\"provider\":\"%s\"}",
                  status, total, prepared, existing, failed, skipped, removed, seconds, infer_seconds, max_texture_ms, peak_rss_kb,
                  provider.c_str());
    std::string json = text;
    if (!error.empty())
    {
        json.pop_back();
        json += ",\"error\":\"";
        for (char c : error)
            json += (c == '"' || c == '\\') ? ' ' : c;
        json += "\"}";
    }
    return json;
}

Result Prepare(const Job &job, const std::function<bool(const Progress &)> &progress)
{
    const Clock::time_point start = Clock::now();
    Result result;
    std::unique_ptr<Session> session = Session::Open(job.models, job.threads, job.output, result.error);
    if (!session)
    {
        result.status = 2;
        return result;
    }
    result.provider = session->stats().provider;
    const std::vector<std::filesystem::path> files = List(job.textures);
    Progress state;
    state.total = result.total = static_cast<int>(files.size());
    Log("relief preparation: %d candidate textures in %s, maps up to %d px into %s, %s", state.total,
        job.textures.string().c_str(), job.map_size, job.output.string().c_str(), result.provider.c_str());
    if (progress && progress(state))
        result.status = 1;
    for (const std::filesystem::path &file : files)
    {
        if (result.status != 0)
            break;
        state.current = file.filename().string();
        std::string failure;
        switch (session->PrepareFile(job.textures, file, job.output, job.map_size, failure, job.display ? &*job.display : nullptr))
        {
        case Outcome::Prepared: ++result.prepared; break;
        case Outcome::Existing: ++result.existing; break;
        case Outcome::Skipped: ++result.skipped; break;
        case Outcome::Failed: case Outcome::Yielded: ++result.failed; break; // the host tool never yields
        }
        ++state.done;
        if (progress && progress(state))
            result.status = 1;
    }
    // Keep processing after a failed texture, but never bless that partial pass as complete.
    // A subsequent CLI invocation retries missing maps and reuses those already written.
    if (result.status == 0 && result.failed > 0)
    {
        result.status = 2;
        result.error = std::to_string(result.failed) + " texture(s) failed; rerun preparation to retry";
    }
    if (result.status == 0)
    {
        result.removed = Prune(job.textures, files, job.output, job.map_size);
        if (result.removed < 0)
        {
            result.removed = 0;
            result.status = 2;
            result.error = "cache pruning deferred: texture inventory, source read or cache I/O failure";
        }
    }
    result.seconds = Seconds(start);
    result.infer_seconds = session->stats().infer_seconds;
    result.max_texture_ms = session->stats().max_texture_ms;
    result.peak_rss_kb = PeakRssKb();
    session.reset();
    ReleaseGpu(); // closed here, not by ncnn's exit handler after the driver's own (a crash at exit)
    Log("relief preparation %s: %d textures, %d prepared, %d already there, %d without relief, %d failed, %d stale maps "
        "removed; %.1f s (%.1f s inference, slowest texture %.0f ms), peak RSS %lld MB",
        result.status == 1 ? "cancelled" : result.status == 2 ? "failed" : "done", result.total, result.prepared, result.existing, result.skipped,
        result.failed, result.removed, result.seconds, result.infer_seconds, result.max_texture_ms, result.peak_rss_kb / 1024);
    return result;
}

} // namespace storm::relief
