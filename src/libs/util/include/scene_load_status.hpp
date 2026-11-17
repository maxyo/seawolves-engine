// Plain scene preparation status shared by renderer and platform UI. No renderer backend dependencies.
#pragma once

#include <cstdint>
#include <string>

namespace storm::render
{
struct SceneLoadStatus {
    uint64_t generation = 0;
    bool active = false, error = false, retryable = false;
    std::string state;
    uint32_t textures = 0, relief = 0, shadows = 0, failed = 0;
    std::string detail;
    std::string phase; // preparing only: "generating" or "cooling"; UI localizes it, never displays detail
    uint64_t owner = 0; // the entity whose scene this is (BeginScenePipelines)
};
SceneLoadStatus GetSceneLoadStatus();
void CompleteSceneReveal();
// The scene's owner was deleted before its reveal (a location whose load failed): reported as an ERROR, the load ends
// unrevealed and the game goes on without it.
void AbandonSceneLoad(uint64_t generation);
// Core lifecycle/transaction boundary; no renderer backend dependency.
// Revocation is generation scoped; resume requires a new valid opaque Present.
void SuspendSceneCpuBudget(uint64_t generation);
void EndSceneCpuBudget(uint64_t generation);
} // namespace storm::render
