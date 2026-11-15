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
};
SceneLoadStatus GetSceneLoadStatus();
} // namespace storm::render
