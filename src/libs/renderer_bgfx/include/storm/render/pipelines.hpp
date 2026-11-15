// Vulkan pipelines of a scene, created before it is shown (src/shim/pipeline_sets.hpp). Every pipeline the device
// creates is recorded under the scene that was loading or shown then, and kept with the device's pipeline cache;
// when that scene loads again, its recorded pipelines are created on the pipeline threads while it loads, and the
// loading fader stays closed (bounded) until the scene's draws have every pipeline they ask for. Nothing on the
// other backends.
#pragma once

#include <bgfx/bgfx.h>
#include <string>

namespace storm::shim { struct BaseTexture; }

namespace storm::render
{

// Game thread, when a scene starts loading ("location/<id>", "sea", "worldmap"): from now on created pipelines are
// recorded under `scene`, and the ones recorded under it in earlier runs are created ahead. The next fade-in may
// be held (HoldSceneReveal).
void BeginScenePipelines(const char *scene);

// Game thread, every frame of a fade-in (location/src/fader.cpp): true while the scene begun last still waits for
// pipelines and current-location resources. Locations never expire into an incomplete reveal;
// their deadline reports an error while rendering continues behind the closed fader.
// Other scene types retain the original bounded pipeline wait.
bool HoldSceneReveal();
// Location-scoped resources, independently of the Vulkan pipeline gate. Counts include jobs whose queues are empty
// because a worker has already taken them, disk-resident maps still needing upload, and initial shadow passes.
struct SceneLoadStatus {
    uint64_t generation = 0;
    bool active = false, error = false, retryable = false;
    std::string state;
    uint32_t textures = 0, relief = 0, shadows = 0, failed = 0;
    std::string detail;
};
SceneLoadStatus GetSceneLoadStatus();
void CompleteSceneReveal();
// Successful world TextureCreate, including cache hits, registers demand. Only mandatory model material
// failures use FailSceneTexture; optional probes and loading/interface pictures are excluded.
void RequireSceneTexture(shim::BaseTexture *texture);
void FailSceneTexture(const char *name);
// Explicit retry keeps the same required assets but starts a new bounded attempt. No automatic timer-based reveal.
bool RetrySceneResources(uint64_t generation);


// Game thread: a program made outside the shim (the interface's), by the names its shaders were created with
// (storm::render::CreateShader). Pipelines recorded for it wait for it instead of being forgotten and are created
// ahead once it is registered. Unregister it before bgfx::destroy.
void RegisterPipelineProgram(const char *vertex_shader, const char *fragment_shader, bgfx::ProgramHandle program);
void UnregisterPipelineProgram(bgfx::ProgramHandle program);

} // namespace storm::render
