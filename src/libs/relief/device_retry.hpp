#pragma once

namespace storm::relief
{
// A CPU detour must not become permanent just because the pending lane is background work or standalone x2.
// Called by admission as well as both inference paths, so admission cannot hide an already due GPU retry.
inline constexpr int kGpuRetryTextures = 8;
inline constexpr double kGpuRetrySeconds = 60.0;
// A retry while the game draws its scene that was still slower than the CPU doubles the wait for the next one (up to
// 16x: 16 min or 128 textures). Each such retry runs a whole texture on the GPU beside the game's frames: on a
// PowerVR D-Series (Pixel 10 Pro, OpenGL ES) one took 13-39 s and halved the frame rate meanwhile, every minute.
// A retry while the game leaves the GPU free (gpu_free) is not counted and stays immediate; a retry that holds resets it.
inline constexpr int kMaxRetryDoublings = 4;
inline int RetryScale(int failed_retries)
{
    return 1 << (failed_retries < 0 ? 0 : failed_retries > kMaxRetryDoublings ? kMaxRetryDoublings : failed_retries);
}
inline bool RetryGpuDue(bool possible, bool on_gpu, bool gpu_free, double cpu_seconds, int cpu_textures, int failed_retries)
{
    const int scale = RetryScale(failed_retries);
    return possible && !on_gpu &&
           (gpu_free || cpu_seconds >= kGpuRetrySeconds * scale || cpu_textures >= kGpuRetryTextures * scale);
}
inline bool UseGpuForTexture(bool possible, bool on_gpu, bool gpu_free, double cpu_seconds, int cpu_textures,
                             int failed_retries)
{
    return on_gpu || RetryGpuDue(possible, on_gpu, gpu_free, cpu_seconds, cpu_textures, failed_retries);
}
inline bool WaitForGpuRetry(bool possible, bool on_gpu, bool gpu_free, double cpu_seconds, int cpu_textures,
                            int failed_retries)
{
    return possible && !on_gpu && !RetryGpuDue(possible, on_gpu, gpu_free, cpu_seconds, cpu_textures, failed_retries);
}
}
