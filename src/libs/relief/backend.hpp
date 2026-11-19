// Where the relief networks run: ncnn, the one inference runtime, on the GPU through Vulkan compute or on
// the CPU, both running the same graphs (fp16 weights, made by pnnx of the float models). One interface for both, for
// the relief maps (prepare.cpp) and the texture upscale. Which device a game uses is measured once, logged with both numbers and
// kept in the cache root (ChooseDevice).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace storm::relief
{
struct LoadingRetry;

// One network on one device. Not thread-safe (Cancel aside).
class Network
{
  public:
    virtual ~Network() = default;
    // One image: three colour planes (planar, `width` x `height`, 0..1) in, three planes of `scale` times the size out
    // (`scale`: the network's own, 1 for the relief networks). A GPU network leaves the outer `halo` texels of the
    // output (the input's padding: what the network needs around a texel) unset. False on an error or after Cancel.
    virtual bool Infer(const float *image, int width, int height, float *planes, std::string &error) = 0;
    // Any thread: the running inference stops as soon as it can, and every later one.
    virtual void Cancel() = 0;
    // Worker only: exact current-location opaque loading permission; CPU networks ignore it.
    // Revocation/deadline remain shared with the renderer. This grants no inference by itself.
    virtual void SetCpuAdmission(std::function<std::shared_ptr<void>()>) {}
    virtual void SetLoadingRetry(std::shared_ptr<LoadingRetry>) {}
    // Worker only, GPU networks: while `yield()` is true (the player sees the game's scene: device_choice.hpp) the
    // running inference stops at its next submission with the error "yielded"; unlike Cancel the network stays usable.
    // CPU networks ignore it.
    virtual void SetYield(std::function<bool()>) {}
    // "CPU, 6 threads of 8 cores" / "GPU Adreno (TM) 740, Vulkan, 176 px tiles".
    virtual std::string Describe() const = 0;
    // The largest inner tile it runs at once (the CPU's: within kCpuNetworkBytes); a caller that tiles its image itself
    // (the upscaler) stays within it, so the network does not cut its tiles again.
    virtual int TileCeiling() const { return 1 << 30; }
};

// The memory of a session's CPU networks (ncnn's blobs and workspace; they run one after the other): never more than
// this held at once, all of it returned to the system after each texture (CpuRuntime::EndJob). It fits the 4 GB minimum
// phone, where the game itself takes 0.6..0.9 GB: the tiles are sized from it (cpu_tiles.hpp), and an allocation past
// it fails the run (the tile is lowered, logged); a texture that does not fit at the smallest tile is not made (logged).
inline constexpr size_t kCpuNetworkBytes = size_t{160} << 20;

// The CPU side of ncnn for the networks of one session (they run one after the other): its thread count and tile size.
class CpuRuntime
{
  public:
    // `threads` cores (ncnn's simpleomp pool, waiting threads sleeping). `tile`: the networks' largest inner tile, -1
    // cpu_tiles.hpp kMaxCpuTile (the checks set one; 0 none): each run's tile is the largest within it and the memory
    // bound. Null and `error` on failure. Logged.
    static std::unique_ptr<CpuRuntime> Open(int threads, std::string &error, int tile = -1);
    virtual ~CpuRuntime() = default;
    // An ncnn graph (.ncnn.param text, .ncnn.bin weights; both must outlive the network) on the CPU, its padding
    // `halo` texels, its output `scale` times the input; the image runs in tiles with the halo around each.
    virtual std::unique_ptr<Network> Load(std::span<const uint8_t> param, std::span<const uint8_t> bin, int halo, int scale,
                                          std::string &error) = 0;
    // The thread count of its networks' next runs (the game's CPU budget changes with its phase); nothing reloaded.
    virtual void SetThreads(int threads) = 0;
    // The most bytes its networks held at once since the last EndJob (at most kCpuNetworkBytes).
    virtual size_t JobPeak() const = 0;
    // After each texture: every byte its networks kept for reuse goes back to the system.
    virtual void EndJob() = 0;
};

// The Vulkan compute device ncnn would use (its default one): created on first use, kept for the process.
struct GpuDevice
{
    bool available = false;
    std::string name, driver; // the device's name and driver version
    std::string why;          // when not available
};
const GpuDevice &Gpu();
// Closes the Vulkan instance when no GPU network is alive (the generator's idle release and shutdown, the system's
// low memory, the end of Prepare); the next Gpu() opens it again. True when it was closed now. It must be closed
// before the process exits: ncnn's own exit handler closes it after the driver's exit handlers have run, and crashes.
bool ReleaseGpu();

// An ncnn graph (.ncnn.param text, .ncnn.bin weights; both must outlive the network) on the GPU. The image is run in
// tiles of the inner area with `halo` texels around each (the network's reach: the tiles join without a seam), each
// tile one short GPU submission, so the game's frames are not held up. `scale`: the network's output scale.
std::unique_ptr<Network> LoadGpuNetwork(std::span<const uint8_t> param, std::span<const uint8_t> bin, int halo, int scale,
                                        std::string &error);

enum class Device
{
    Cpu,
    Gpu,
};
const char *DeviceName(Device device);

// The device for this game's relief networks: measured once (a probe tile of `probe` on both, after a warm-up),
// the faster taken; kept in `cache` (the cache root: «Очистить кэш» measures again) for this GPU, driver, model and
// thread count. Logged with both numbers either way; no Vulkan compute device: the CPU, logged with the reason.
// `param` / `bin`: the probe network's ncnn graph, run on both devices. A choice kept from before opens neither.
// The host tool runs on it; in the game the game's state decides each texture's device (device_choice.hpp).
Device ChooseDevice(const std::filesystem::path &cache, std::span<const uint8_t> param, std::span<const uint8_t> bin, int halo,
                    int threads);

// The crash guard of the GPU work (ChooseDevice), on the shared storm/fx/gpu_crash_guard.hpp: `cache`'s marker holds
// GpuLabel of the GPU (kGpuOpening while Vulkan opens), written before GPU work starts and cleared after it succeeded.
// One left behind at the next start means the process died in the driver, unless the system says it ended the
// process itself (fx::PreviousExit: killed for memory, stopped by the user or the system, updated; logged, not kept):
// logged at error, that GPU and driver take the CPU («Очистить кэш» clears it).
// The version of our GPU inference code in every label: a crash recorded by another version of it (or another driver)
// is not this one's, so an update tries the GPU again (measured, and guarded the same way).
inline constexpr const char *kGpuPath = "relief GPU path 2 (staged submissions)";
inline constexpr const char *kGpuOpening = "opening Vulkan, relief GPU path 2 (staged submissions)";
std::string GpuLabel(const GpuDevice &gpu); // "<name>, driver <version>, <kGpuPath>"
void MarkGpuRunning(const std::filesystem::path &cache, const std::string &what);
void ClearGpuRunning(const std::filesystem::path &cache);
// The GPU (GpuLabel) or kGpuOpening that crashed a previous run, or empty.
std::string GpuCrash(const std::filesystem::path &cache);

} // namespace storm::relief
