// Which bgfx backend the game window runs: Vulkan or OpenGL ES (storm/render/frame_owner.hpp, SetRenderBackend).
//
// The choice is made before bgfx::init from four things: what was asked for (engine.ini render_backend, the Android
// graphics setting of the same name), what the system's Vulkan says about itself (ProbeVulkan: the loader and the
// device bgfx would take, read without bgfx), the denylist of driver versions known to be bad (VulkanDenied) and the
// crash guard's record (VulkanStartGuard: an earlier Vulkan start on this device and driver that killed the process
// or failed). `auto`, the default, takes Vulkan wherever the device has what the game needs (DecideBackend). Vulkan
// then still has to initialise in bgfx and pass the backend self-test (Context::start); if it does not, the device is
// started again on OpenGL ES and that driver is recorded. Every outcome is one "render backend:" log line with the
// reason (the application's diagnostics export and the graphics settings show it).
#pragma once

#include "storm/fx/gpu_crash_guard.hpp"
#include "storm/render/frame_owner.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace storm::shim
{

// What the Vulkan loader and its preferred physical device report. `problem` is empty when a device was read.
struct VulkanDevice
{
    std::string problem;       // why there is no usable answer (no loader, no instance, no device ...)
    uint32_t instance_version = 0;
    std::string name;          // VkPhysicalDeviceProperties::deviceName
    uint32_t api_version = 0;  // of the device
    uint32_t driver_version = 0;
    uint32_t vendor_id = 0, device_id = 0;
    bool software = false;     // VK_PHYSICAL_DEVICE_TYPE_CPU (lavapipe, SwiftShader)
    uint8_t pipeline_cache_uuid[16] = {};
    bool swapchain = false;    // VK_KHR_swapchain and VK_KHR_maintenance1: bgfx's device needs both
    bool border_color = false; // VK_EXT_custom_border_color: D3DSAMP_BORDERCOLOR other than opaque black
    bool anisotropy = false;   // samplerAnisotropy
    float max_anisotropy = 0.0f;
    int vertex_texture_units = 0; // limits.maxPerStageDescriptorSampledImages
    bool large_points = false; // largePoints: point lists wider than one pixel (the stars)
    float max_point_size = 1.0f;
    // textureCompressionETC2 or textureCompressionBC: the game's DXT textures stay compressed (shim objects.cpp
    // resolve_tier); without either they are decoded to RGBA8, up to 8x the memory OpenGL ES 3 (ETC2 always) needs.
    bool compressed_textures = false;

    bool found() const { return problem.empty(); }
    // "Adreno (TM) 740, Vulkan 1.3.128, driver 0x80273000, vendor 0x5143 device 0x43050a01" or the problem.
    std::string describe() const;
    // What a cache of this device's pipelines is valid for: device, driver and the driver's own cache id.
    std::string cache_key() const;
};

// Loads the system's Vulkan loader, creates an instance and reads the physical device bgfx's Vulkan backend would
// take (a discrete GPU, else an integrated one, else the first). Nothing stays loaded or alive afterwards.
VulkanDevice ProbeVulkan();

struct BackendDecision
{
    bool vulkan = false;
    std::string reason;
};

// Driver versions known to break the game on Vulkan (crashes, corrupted pictures): `render_backend = auto` runs
// OpenGL ES there. Only entries backed by evidence (a device report, a Test Lab run) go into the table in
// backend_select.cpp, each with where it came from. Empty when the device is not listed, else why it is.
std::string VulkanDenied(const VulkanDevice &device);

// Pure: the backend to try first. `crash`: the reason of the crash guard's record for this device and driver (an
// earlier Vulkan start that killed the process or failed), empty when there is none.
//
// Vulkan, asked for or not, needs the loader and a device with Vulkan 1.1, VK_KHR_swapchain and VK_KHR_maintenance1
// (bgfx's device), and no crash record. `auto` takes it when the device also is a hardware GPU (no CPU
// implementation: lavapipe, SwiftShader), keeps the textures compressed (ETC2 or BC) and is not denylisted.
BackendDecision DecideBackend(render::Backend request, const VulkanDevice &device, const std::string &crash);

// The crash guard of a Vulkan start (storm/fx/gpu_crash_guard.hpp, <cache>/RenderBackend/vulkan-*.txt): the marker
// holds the device's cache_key from before bgfx::init until frames were presented (Context::present) or the device
// was shut down in an orderly way. One left at the next start is logged at error; that device and driver run OpenGL
// ES until the driver changes or «Очистить кэш». A failed bgfx::init or self-test is recorded the same way.
class VulkanStartGuard
{
  public:
    // `directory`: empty: no guard, logged.
    VulkanStartGuard(std::filesystem::path directory, const VulkanDevice &device);
    // Why this device and driver do not start Vulkan (a crash or a failure recorded for it), or empty.
    const std::string &crash() const { return crash_; }
    void begin() const;   // before bgfx::init on Vulkan
    void succeed() const; // frames were presented, or the device was shut down in an orderly way
    void fail(const std::string &why) const; // Vulkan was given up in an orderly way (bgfx::init, the self-test)

  private:
    fx::GpuCrashGuard guard_;
    std::string key_, crash_;
};

} // namespace storm::shim
