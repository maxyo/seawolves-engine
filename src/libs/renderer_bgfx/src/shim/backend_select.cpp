#include "backend_select.hpp"

#include "shim.hpp"

// The Vulkan headers bgfx itself compiles against (bgfx/3rdparty/khronos); no prototypes: every function is looked
// up in the loader this file opens.
#define VK_NO_PROTOTYPES
#define VK_NO_STDINT_H
#include <vulkan-local/vulkan_core.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <vector>

namespace storm::shim
{

namespace
{
// Closes the loader when the probe is done, whichever way it leaves.
struct Loader
{
    void *library = nullptr;
    ~Loader()
    {
        if (library)
            dlclose(library);
    }
    template <typename Function> Function symbol(const char *name) const
    {
        return reinterpret_cast<Function>(dlsym(library, name));
    }
};

std::string version_text(uint32_t version)
{
    char text[32];
    snprintf(text, sizeof(text), "%u.%u.%u", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
             VK_API_VERSION_PATCH(version));
    return text;
}

// bgfx's order (renderer_vk.cpp, the physical device loop): a discrete GPU, else an integrated one, else the first.
int rank(VkPhysicalDeviceType type)
{
    return type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
}
} // namespace

std::string VulkanDevice::describe() const
{
    if (!found())
        return problem;
    char text[512];
    snprintf(text, sizeof(text), "%s, Vulkan %s, driver 0x%x, vendor 0x%x device 0x%x%s", name.c_str(),
             version_text(api_version).c_str(), driver_version, vendor_id, device_id, software ? " (software)" : "");
    return text;
}

std::string VulkanDevice::cache_key() const
{
    char uuid[33];
    for (int i = 0; i < 16; ++i)
        snprintf(uuid + 2 * i, 3, "%02x", pipeline_cache_uuid[i]);
    return describe() + " | pipeline cache " + uuid;
}

VulkanDevice ProbeVulkan()
{
    VulkanDevice out;
    Loader loader;
#ifdef __ANDROID__
    loader.library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#else
    loader.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!loader.library)
    {
        out.problem = "no Vulkan loader on this system (libvulkan)";
        return out;
    }
    const auto create_instance = loader.symbol<PFN_vkCreateInstance>("vkCreateInstance");
    const auto destroy_instance = loader.symbol<PFN_vkDestroyInstance>("vkDestroyInstance");
    const auto enumerate_devices = loader.symbol<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    const auto device_properties = loader.symbol<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
    const auto device_features = loader.symbol<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures");
    const auto device_extensions =
        loader.symbol<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    if (!create_instance || !destroy_instance || !enumerate_devices || !device_properties || !device_features ||
        !device_extensions)
    {
        out.problem = "the Vulkan loader lacks core entry points";
        return out;
    }
    // Vulkan 1.0 loaders have no vkEnumerateInstanceVersion.
    out.instance_version = VK_API_VERSION_1_0;
    if (const auto instance_version = loader.symbol<PFN_vkEnumerateInstanceVersion>("vkEnumerateInstanceVersion"))
        instance_version(&out.instance_version);
    if (out.instance_version < VK_API_VERSION_1_1)
    {
        out.problem = "the Vulkan loader is " + version_text(out.instance_version) + ", older than 1.1";
        return out;
    }
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "storm-engine backend probe";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (const VkResult result = create_instance(&create, nullptr, &instance); result != VK_SUCCESS)
    {
        out.problem = "vkCreateInstance failed (" + std::to_string(result) + ")";
        return out;
    }
    uint32_t count = 0;
    enumerate_devices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    if (count == 0 || enumerate_devices(instance, &count, devices.data()) < VK_SUCCESS)
    {
        destroy_instance(instance, nullptr);
        out.problem = "Vulkan reports no physical device";
        return out;
    }
    devices.resize(count);
    VkPhysicalDevice chosen = devices[0];
    VkPhysicalDeviceProperties properties{};
    device_properties(chosen, &properties);
    for (const VkPhysicalDevice device : devices)
    {
        VkPhysicalDeviceProperties candidate{};
        device_properties(device, &candidate);
        if (rank(candidate.deviceType) > rank(properties.deviceType))
        {
            chosen = device;
            properties = candidate;
        }
    }
    VkPhysicalDeviceFeatures features{};
    device_features(chosen, &features);
    uint32_t extension_count = 0;
    device_extensions(chosen, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (extension_count)
        device_extensions(chosen, nullptr, &extension_count, extensions.data());
    extensions.resize(extension_count);
    const auto has = [&](const char *name) {
        for (const VkExtensionProperties &extension : extensions)
            if (std::strcmp(extension.extensionName, name) == 0)
                return true;
        return false;
    };
    out.name = properties.deviceName;
    out.api_version = properties.apiVersion;
    out.driver_version = properties.driverVersion;
    out.vendor_id = properties.vendorID;
    out.device_id = properties.deviceID;
    out.software = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    std::memcpy(out.pipeline_cache_uuid, properties.pipelineCacheUUID, sizeof(out.pipeline_cache_uuid));
    // bgfx enables both by name when it creates the device (renderer_vk.cpp).
    out.swapchain = has(VK_KHR_SWAPCHAIN_EXTENSION_NAME) && has(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
    out.border_color = has(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    out.anisotropy = features.samplerAnisotropy == VK_TRUE;
    out.max_anisotropy = out.anisotropy ? properties.limits.maxSamplerAnisotropy : 0.0f;
    out.vertex_texture_units = static_cast<int>(properties.limits.maxPerStageDescriptorSampledImages);
    out.large_points = features.largePoints == VK_TRUE;
    out.max_point_size = out.large_points ? properties.limits.pointSizeRange[1] : 1.0f;
    out.compressed_textures = features.textureCompressionETC2 == VK_TRUE || features.textureCompressionBC == VK_TRUE;
    destroy_instance(instance, nullptr);
    return out;
}

namespace
{
// A denylist entry: a GPU vendor, optionally a device, and the driver versions [from, to] (VkPhysicalDeviceProperties,
// the vendor's own encoding) that break the game on Vulkan, with the evidence. A crash on a device that is not listed
// is caught by the crash guard for that device and driver alone (and a renderer FATAL by Context::vulkan_failed).
//
// No capability of the device predicts these failures: the drivers listed report Vulkan 1.1 with everything
// DecideBackend checks. They are listed by driver build, the vendor's own version number, not by phone model.
struct DeniedDriver
{
    uint32_t vendor_id, device_id; // device_id 0: every device of the vendor
    uint32_t driver_from, driver_to;
    const char *evidence; // "Mali-G52 r20p0: the sea's pipeline crashes (Test Lab run <id>)"
};
constexpr uint32_t kVendorImagination = 0x1010;
constexpr std::array<DeniedDriver, 1> kDeniedDrivers{{
    // Every Imagination driver seen so far fails to create the game's pipelines; two driver lines, so the vendor's
    // whole range until a build is shown to work (then `driver_from`/`driver_to` narrow it):
    // - Galaxy A12, PowerVR Rogue GE8320, build 1.13@5776728 (Imagination's driverVersion is the build number,
    //   0x582558), Test Lab seawolves-loops13-s24-a12-20261118-130400: stretched triangles on the title's ships and
    //   the hero's legs not drawn (both right on Adreno), 3.8 s for the simplest pipeline, vkCreateGraphicsPipelines
    //   VK_ERROR_OUT_OF_HOST_MEMORY on the switch to «Средняя» (bgfx FATAL, the game ended).
    // - Pixel 10 Pro, PowerVR D-Series DXT-48-1536, Vulkan 1.4.303, build 25.1@6794074 (0x67ab5a), Test Lab
    //   seawolves-gpu-p10pro-20261118-135909: vkCreateGraphicsPipelines VK_ERROR_UNKNOWN for vs_ff_unlit + fs_ff
    //   (alpha blend, state 0x000000400656502f) on the title screen (bgfx FATAL, the game ended).
    // Since the FF fragment shaders discard once at their end (ff_finish.sh), both drivers create every pipeline of a
    // full run (Test Lab seawolves-discard-p10pro-20261118-160954 and seawolves-vk-a12-20261118-163335, render_backend
    // = Vulkan), but both stay here for what remains:
    // - Pixel 10 Pro: Vulkan is slower than OpenGL ES on the same scene: «Низкая» 39.7 fps, GPU 23.1 ms at 1406x630,
    //   against 60 fps at 720-900 lines on OpenGL ES (seawolves-gles-fix-p10pro-20261118-154348).
    // - Galaxy A12: the hero's legs are still not drawn, 5-15 s per pipeline, 0.6-2.4 fps.
    {kVendorImagination, 0, 0, UINT32_MAX,
     "Imagination PowerVR drivers run the game worse on Vulkan than on OpenGL ES (D-Series 25.1: «Низкая» GPU 23 ms "
     "at 630 lines against 60 fps at 720-900 lines on OpenGL ES; Rogue 1.13@5776728: geometry missing, 5-15 s "
     "pipelines; Test Lab 2026-11-18)"},
}};
} // namespace

std::string VulkanDenied(const VulkanDevice &device)
{
    for (const DeniedDriver &entry : kDeniedDrivers)
        if (entry.vendor_id == device.vendor_id && (!entry.device_id || entry.device_id == device.device_id) &&
            device.driver_version >= entry.driver_from && device.driver_version <= entry.driver_to)
            return entry.evidence;
    return {};
}

BackendDecision DecideBackend(render::Backend request, const VulkanDevice &device, const std::string &crash)
{
    if (request == render::Backend::OpenGLES)
        return {false, "render_backend asks for OpenGL ES"};
    const bool asked = request == render::Backend::Vulkan;
    const std::string prefix = asked ? "render_backend asks for Vulkan, but " : "";
    if (!device.found())
        return {false, prefix + device.problem};
    if (device.api_version < VK_API_VERSION_1_1)
        return {false, prefix + "the device has Vulkan " + version_text(device.api_version) + ", older than 1.1"};
    if (!device.swapchain)
        return {false, prefix + "the device lacks VK_KHR_swapchain or VK_KHR_maintenance1"};
    if (!crash.empty())
        return {false, prefix + "an earlier Vulkan start on this device and driver " + crash +
                           " (kept until the driver changes or the cache is cleared)"};
    if (asked)
        return {true, "render_backend asks for Vulkan"};
    if (device.software)
        return {false, "the Vulkan device is a CPU implementation, slower than OpenGL ES"};
    if (!device.compressed_textures)
        return {false, "the Vulkan device has neither ETC2 nor BC textures (the game's textures would take up to 8x "
                       "the memory)"};
    if (const std::string denied = VulkanDenied(device); !denied.empty())
        return {false, "this Vulkan driver is on the denylist: " + denied};
    return {true, "the device has Vulkan 1.1 with what the game needs and no recorded failure"};
}

VulkanStartGuard::VulkanStartGuard(std::filesystem::path directory, const VulkanDevice &device)
    : guard_(directory, "vulkan"), key_(device.cache_key())
{
    if (!guard_.enabled())
    {
        logf("render backend: no cache directory configured: a Vulkan start that kills the process is not remembered");
        return;
    }
    // The system's word on how the previous process ended (the app writes previous-exit.txt into the cache root, the
    // state directory's parent): a marker left by a process the system killed for memory or stopped is not a crash.
    if (const fx::GpuCrashGuard::Record left = guard_.take_leftover(fx::PreviousExit(directory.parent_path()));
        !left.empty() && !left.counted)
        logf("render backend: the previous Vulkan start on %s had not presented its first frames when the process "
             "ended, but %s: Vulkan stays possible",
             left.what.c_str(), left.why.c_str());
    else if (!left.empty())
        logf("ERROR: render backend: the previous Vulkan start never presented its first frames: the process died in "
             "it on %s",
             left.what.c_str());
    const fx::GpuCrashGuard::Record record = guard_.crashed();
    if (record.empty())
        return;
    if (record.what != key_)
    {
        // Another device or driver since: its failure says nothing about this one.
        logf("render backend: the Vulkan failure recorded on %s is for another driver; Vulkan is tried again on %s",
             record.what.c_str(), key_.c_str());
        guard_.forget();
        return;
    }
    crash_ = record.why == fx::GpuCrashGuard::kProcessDied ? "crashed the process" : "failed (" + record.why + ")";
}

void VulkanStartGuard::begin() const
{
    guard_.mark(key_);
}

void VulkanStartGuard::succeed() const
{
    guard_.clear();
}

void VulkanStartGuard::fail(const std::string &why) const
{
    guard_.record(key_, why);
    guard_.clear();
}

} // namespace storm::shim
