// Vulkan pipelines of a scene, created before it is shown (src/shim/pipeline_sets.hpp). Every pipeline the device
// creates is recorded under the scene that was loading or shown then, and kept with the device's pipeline cache;
// when that scene loads again, its recorded pipelines are created on the pipeline threads while it loads, and the
// loading fader stays closed (bounded) until the scene's draws have every pipeline they ask for. Nothing on the
// other backends.
#pragma once

#include "scene_load_status.hpp"

namespace storm::shim { struct BaseTexture; }

namespace storm::render
{

// Game thread, when a scene starts loading ("location/<id>", "sea", "worldmap"): from now on created pipelines are
// recorded under `scene`, and the ones recorded under it in earlier runs are created ahead. The next fade-in may
// be held (HoldSceneReveal). `owner`: the entity whose scene it is (0 for none); its deletion before the reveal
// abandons the load (AbandonSceneLoad).
void BeginScenePipelines(const char *scene, uint64_t owner);

// Game thread, every frame of a fade-in (location/src/fader.cpp): true while the scene begun last still waits for
// pipelines and current-location resources. Locations never expire into an incomplete reveal;
// explicit resource failures report an error while rendering continues behind the closed fader.
// Other scene types retain the original bounded pipeline wait.
bool HoldSceneReveal();
// Location-scoped resources, independently of the Vulkan pipeline gate. Counts include jobs whose queues are empty
// because a worker has already taken them, disk-resident maps still needing upload, and initial shadow passes.

void CompleteSceneReveal();
// Fader arms only its full-screen solid opaque rectangle. The shim acknowledges a successful submit.
void ArmSceneOpaqueDraw(bool full_screen, bool solid, uint8_t alpha);
void SceneOpaqueDrawSubmitted(bool full_coverage, bool additive_blend);
void SceneOpaquePresented(bool frame_advanced, bool stable_backbuffer);
// One frame of the stock loading transaction (renderer RunEnd), before its Present: the budget after which a missing
// opaque cover no longer holds the readiness (LoadingCoverBudget).
void SceneLoadingTransactionFrame();
void BeginSceneWork(uint64_t frame, uint64_t surface, uint64_t target, bool stable_target);
bool HideSceneBeauty(uint64_t surface, uint64_t target);
void ScenePostTarget(uint64_t surface, uint64_t before, uint64_t after);
// A draw of the game picture was submitted (the loading frame qualifies only with one).
void SceneBeautySubmitted();
struct SceneLoadingBudget {
    uint64_t generation = 0, measured_at_ms = 0;
    double gpu_ms = 0;
    bool eligible = false;
};
SceneLoadingBudget GetSceneLoadingBudget();
void SetSceneLoadingWindow(uint64_t generation, uint64_t deadline_ms);
// Successful world TextureCreate, including cache hits, registers demand. Only mandatory model material
// failures use FailSceneTexture (`requester`: the model that needs it); optional probes and loading/interface
// pictures are excluded. A failed texture is logged as an ERROR and never holds the reveal; one the game's data names
// but does not ship (KnownMissingGameTexture) is logged once per session as a WARNING instead.
void RequireSceneTexture(shim::BaseTexture *texture);
void FailSceneTexture(const char *name, const char *requester);
// A texture the game's own models name but the game does not ship, in any folder or archive: a defect of its data, the
// same in every session. Returns where the game names it, nullptr for any other texture (a missing one of those is a
// real failure). Matched with or without "resource\textures\" and ".tx", by file name in any folder, or by folder and file
// name for a name the game ships in other folders.
const char *KnownMissingGameTexture(const char *name);
// Explicit retry keeps the same required assets but starts a new generation. Only in-flight texture uploads that made
// no progress within effects::SceneTextureStall's budget are released by time (logged as an ERROR each).
bool RetrySceneResources(uint64_t generation);

} // namespace storm::render
