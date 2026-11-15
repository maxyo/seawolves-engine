#pragma once
#include <array>
#include <set>
#include <string>
#include <string_view>
#if defined(__ANDROID__) && __has_include(<android/log.h>)
#include <android/log.h>
#define STORM_HUD_PRESENTER_LOG 1
#endif

namespace storm::android
{
// Touch HUD declarations carry protocol ownership only. Android never restores legacy
// screen-space HUD drawing, including before mount and after a runtime failure.
// World-space ship markers remain part of the scene.
struct HudPresenters
{
    bool land = false, sea = false, worldMap = false, spyglass = false;
    bool failed = false;
    bool declare(std::string_view hud)
    {
        bool *owner = hud == "land" ? &land : hud == "sea" ? &sea :
            hud == "worldmap" ? &worldMap : hud == "spyglass" ? &spyglass : nullptr;
        if (!owner) return false;
        *owner = true;
        failed = false;
        return true;
    }
    bool any() const { return land || sea || worldMap || spyglass; }
    std::string lost()
    {
        if (failed) return {};
        failed = true;
        return "HUD presenter lost: the UI runtime failed; touch HUD recovery required; legacy HUD remains disabled";
    }
};
inline HudPresenters hudPresenters;

enum class LegacyLayerKind
{
    Hud,   // screen-space HUD: always refused on Android
    World, // drawn in the scene: shown under the touch HUD too
};
struct LegacyLayer
{
    std::string_view name;
    LegacyLayerKind kind;
};
inline constexpr std::array<LegacyLayer, 12> kLegacyLayers{{
    {"land interface", LegacyLayerKind::Hud},     // location images and texts: the touch HUD's location line
    {"land command list", LegacyLayerKind::Hud},  // stock action list: the touch HUD's sheet
    {"log and actions", LegacyLayerKind::Hud},    // log lines and action hints
    {"active perk icons", LegacyLayerKind::Hud},  // perk icons over the time control
    {"timer bar", LegacyLayerKind::Hud},          // countdown bar
    {"interface cursor", LegacyLayerKind::Hud},   // mouse pointer of the original interface
    {"loading spinner", LegacyLayerKind::Hud},    // loading/saving spinner
    {"world map instruments", LegacyLayerKind::Hud}, // compass, date, coordinates, flag: the touch HUD's instruments
    {"world map fleet", LegacyLayerKind::Hud},    // ship signs of the squadron: the touch HUD's fleet
    {"ship bars", LegacyLayerKind::World},        // hull/sails/crew bars and relation icon over ships and forts
    {"ship pointer", LegacyLayerKind::World},     // pointer over an order's target ship
    // Not world-space, but the touch HUD has nothing in its place yet: shown until it is ported
    // into the HUD (two side-strength bars), then refused again.
    {"boarding status", LegacyLayerKind::World},  // crew strength bars of a deck fight
}};
inline const LegacyLayer *legacyLayer(std::string_view name)
{
    for (const auto &layer : kLegacyLayers)
        if (layer.name == name) return &layer;
    return nullptr;
}

// A legacy layer asks to draw. Android refuses every screen-space HUD layer (the touch HUD is
// the only HUD) and logs the first refusal of each. A world-space layer is never refused; an
// unclassified one is refused and reported as an error.
inline bool legacyDrawRefused(const char *layer)
{
    const LegacyLayer *known = legacyLayer(layer);
    static std::set<std::string> reported;
    const bool first = reported.insert(layer).second;
#ifdef STORM_HUD_PRESENTER_LOG
    if (first)
    {
        if (!known)
            __android_log_print(ANDROID_LOG_ERROR, "StormHud", "legacy layer '%s' is not classified (android_hud_presenter.hpp); refused", layer);
        else if (known->kind == LegacyLayerKind::World)
            __android_log_print(ANDROID_LOG_INFO, "StormHud", "world-space layer '%s' shown under the touch HUD", layer);
        else
            __android_log_print(ANDROID_LOG_WARN, "StormHud", "legacy HUD layer '%s' refused: Android uses the touch HUD", layer);
    }
#else
    (void)first;
#endif
    return !known || known->kind == LegacyLayerKind::Hud;
}
} // namespace storm::android
