#pragma once

namespace storm::relief
{
// A CPU detour must not become permanent just because the pending lane is background work or standalone x2.
// Called by admission as well as both inference paths, so admission cannot hide an already due GPU retry.
inline constexpr int kGpuRetryTextures = 8;
inline constexpr double kGpuRetrySeconds = 60.0;
inline bool RetryGpuDue(bool possible, bool on_gpu, bool gpu_free, double cpu_seconds, int cpu_textures)
{
    return possible && !on_gpu && (gpu_free || cpu_seconds >= kGpuRetrySeconds || cpu_textures >= kGpuRetryTextures);
}
inline bool UseGpuForTexture(bool possible, bool on_gpu, bool gpu_free, double cpu_seconds, int cpu_textures)
{
    return on_gpu || RetryGpuDue(possible, on_gpu, gpu_free, cpu_seconds, cpu_textures);
}
inline bool WaitForGpuRetry(bool possible, bool on_gpu, bool gpu_free, double cpu_seconds, int cpu_textures)
{
    return possible && !on_gpu && !RetryGpuDue(possible, on_gpu, gpu_free, cpu_seconds, cpu_textures);
}
}
