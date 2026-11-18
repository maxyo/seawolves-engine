#pragma once

namespace storm::relief
{
// Which device a relief texture runs on (relief.hpp Session::SetGame). The game's state decides it, not a measurement:
//   * while the player sees the game's scene the GPU is the game's, and the texture runs on the CPU (a GPU texture beside
//     the game's frames took 13-39 s on a Pixel 10 Pro, PowerVR D-Series, and halved the frame rate meanwhile);
//   * while the game leaves the GPU free (a loading or an interface screen without a scene, the game in the
//     background), or grants a texture its place's loading window (loading_retry.hpp), on the GPU;
//   * otherwise (the game draws a scene under its loading cover outside such a window) on the CPU;
//   * without the game (the host tool), on the device measured for this machine (backend.hpp ChooseDevice).
// A GPU texture stops at its next submission when the player sees the scene (Outcome::Yielded: back to the queue,
// for the CPU).
enum class DeviceReason
{
    None,          // no texture has chosen yet
    NoGpu,         // no usable GPU (none, no graphs, a crash recorded, its networks failed to load)
    Measured,      // the host tool: the device measured for this machine
    SceneShown,    // the player sees the game's scene
    GameBusy,      // the game draws its scene under the loading cover
    GpuFree,       // the game leaves the GPU free
    LoadingWindow, // the place's loading window
};
struct DeviceChoice
{
    bool gpu = false;
    DeviceReason why = DeviceReason::None;
};
// `loading_window()` is asked only when the window decides (it consumes the window's one texture).
template<class LoadingWindow>
DeviceChoice ChooseTextureDevice(bool gpu_possible, bool game, bool measured_gpu, bool scene_shown, bool gpu_free,
                                 LoadingWindow &&loading_window)
{
    if (!gpu_possible)
        return {false, DeviceReason::NoGpu};
    if (!game)
        return {measured_gpu, DeviceReason::Measured};
    if (scene_shown)
        return {false, DeviceReason::SceneShown};
    if (gpu_free)
        return {true, DeviceReason::GpuFree};
    if (loading_window())
        return {true, DeviceReason::LoadingWindow};
    return {false, DeviceReason::GameBusy};
}
inline const char *DeviceReasonText(DeviceReason why)
{
    switch (why)
    {
    case DeviceReason::None: return "no texture yet";
    case DeviceReason::NoGpu: return "no usable GPU";
    case DeviceReason::Measured: return "the device measured for this machine";
    case DeviceReason::SceneShown: return "the player sees the game's scene: the GPU is the game's";
    case DeviceReason::GameBusy: return "the game draws its scene under the loading cover";
    case DeviceReason::GpuFree: return "the game leaves the GPU free: a loading or an interface screen, or the game in the background";
    case DeviceReason::LoadingWindow: return "the place's loading window";
    }
    return "?";
}
}
