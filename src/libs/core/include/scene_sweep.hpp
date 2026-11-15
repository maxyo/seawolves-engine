#pragma once

// Engine side of tools/scene_sweep (see its README.md): lets an external runner load
// every game scene through the game's own scripts. Compiled only with
// -DSTORM_SCENE_SWEEP=ON (never in shipped builds) and inert unless the
// STORM_SCENE_SWEEP_DIR environment variable names the runner's control directory.
#ifdef STORM_SCENE_SWEEP
#include <cstdint>
#include <functional>
#include <string>

namespace storm::scene_sweep
{

// Called by the renderer after each presented frame.
void framePresented(uint32_t drawCalls);
// Set by the renderer: returns its counters since the previous call as key=value pairs.
// The sweep restarts the window at each event and appends it to each capture.
void setRendererWindow(std::function<std::string()> window);

// What the renderer reports about the applied sweep variant (run.py --variants), polled every frame while the script
// waits for it.
struct VariantState
{
    // The variant's own settings and programs draw: the renderer prepares new settings (links their programs, runs
    // its self-test) over many frames while the previous ones keep drawing.
    bool active = false;
    uint32_t programs = 0; // while not active: programs prepared for the pending settings so far
    // Products that go on generating in the background after the settings are active. A variant waits for one only
    // when it names it (wait=upscale, wait=relief, wait=bakes); otherwise the capture reports it as still busy.
    bool upscale = false; // neural x2 textures still being read, uploaded or made
    bool relief = false;  // a relief map (or the list of relief textures) being made
    bool bakes = false;   // a bake (town light, visibility, bounce, AO volume, far LOD) queued or running
};

// Set by the renderer: `apply` applies the graphics options of one entry of STORM_SCENE_SWEEP_VARIANTS ("name=value,..."
// without the sweep's own wait= items), so every scene is captured once per variant; `state` reports it.
using VariantHook = void (*)(const char *options);
using VariantStateHook = VariantState (*)();
void setVariantHook(VariantHook apply, VariantStateHook state);

// Set by the renderer: a value that changes whenever a frame is presented with scene draws left out (Vulkan: draws
// that wait for their pipelines, storm::render::LastIncompleteFrame). A capture window in which it changed did not
// show the whole scene, and the script renders another one (SceneSweepWindowComplete).
using IncompleteFrameHook = uint32_t (*)();
void setIncompleteFrameHook(IncompleteFrameHook hook);

} // namespace storm::scene_sweep
#endif
