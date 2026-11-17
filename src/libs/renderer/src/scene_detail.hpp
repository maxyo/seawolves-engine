#pragma once
#include <algorithm>
#include <cmath>

// The game's grass density and open-sea detail belong to the graphics preset (the settings
// screen no longer offers «Трава» and «Детализация моря»). The renderer writes the preset's values into the game's own
// option state, which the game's scripts already apply: the global iGrassQuality (0 full, 1 middle, 2 low, 3 off; the
// grass asks it through GOpt_GetGrassQuality, which also keeps the game's storm rule) and InterfaceStates.SeaDetails
// (0..1, SetSeaSettings turns it into the open sea's grid step when a sea is made). The sync looks once a second and
// writes only what differs, so the game's options loader, a loaded save and a preset change all end at the preset's
// values.
namespace storm::render
{
struct SceneDetail
{
    int grass;   // iGrassQuality
    float sea;   // InterfaceStates.SeaDetails
};

// «Низкая» halves the grass and coarsens the open sea for weak phones; «Средняя» and «Высокая» keep the game's
// defaults (full grass, finest sea).
inline SceneDetail PresetSceneDetail(int preset)
{
    constexpr SceneDetail kDetail[3] = {{1, 0.5f}, {0, 1.0f}, {0, 1.0f}};
    return kDetail[std::clamp(preset, 0, 2)];
}

inline bool SameSeaDetail(float a, float b)
{
    return std::fabs(a - b) < 1e-3f;
}
} // namespace storm::render
