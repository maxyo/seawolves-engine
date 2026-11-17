// A model texture that cannot be created: one the game's data names but does not ship (KnownMissingGameTexture) is a
// known data defect, logged once per session as a WARNING with the model that asked for it; any other is an ERROR of
// the scene being loaded. A scene whose owner was deleted before its reveal is abandoned with an ERROR. No GPU: the
// renderer's log lines are collected through its sink.
#include <bits/stdc++.h>
#include "effects/scene_readiness.hpp"
#include "storm/render/log.hpp"
#include "storm/render/pipelines.hpp"
#include "scene_load_status.hpp"
using namespace storm;
using render::LogLevel;
namespace {
std::vector<std::pair<LogLevel, std::string>> lines;
void Collect(LogLevel level, const char *line) { lines.emplace_back(level, line); }
size_t Count(LogLevel level, std::string_view text) {
    return std::count_if(lines.begin(), lines.end(), [&](const auto &l) {
        return l.first == level && l.second.find(text) != std::string::npos;
    });
}
} // namespace
int main() {
    // The game's spellings of the one known texture, in any folder; nothing else.
    for (const char *name : {"PHONG256_BW.GIF", "resource\\textures\\PHONG256_BW.GIF.tx", "ships\\Sloop1\\phong256_bw.gif",
                             "resource/textures/ships/PHONG256_BW.GIF.tx"})
        assert(render::KnownMissingGameTexture(name) && std::strstr(render::KnownMissingGameTexture(name), "sloop1_mast2.gm"));
    // A name the game ships in other folders: only the folder it lacks it in.
    for (const char *name : {"resource\\textures\\AMMO\\CHECKER.TGA.tx", "AMMO\\checker.tga", "ammo/checker.tga"})
        assert(render::KnownMissingGameTexture(name) && std::strstr(render::KnownMissingGameTexture(name), "blade_29.gm"));
    for (const char *name : {"A_big.tga", "resource\\textures\\A_BIG.TGA.tx"})
        assert(render::KnownMissingGameTexture(name) && std::strstr(render::KnownMissingGameTexture(name), "storeSmall_locators.gm"));
    for (const char *name : {"resource\\textures\\LIGHTING\\DAY1\\LOCATIONS\\DECKS\\DECK01\\SHADOW.TGA.tx",
                             "lighting\\storm\\locations\\decks\\deck01\\shadow.tga"})
        assert(render::KnownMissingGameTexture(name) && std::strstr(render::KnownMissingGameTexture(name), "deck01.gm"));
    assert(render::KnownMissingGameTexture("PHONG256.GIF") && render::KnownMissingGameTexture("TORNADO\\ITEMS.TGA"));
    for (const char *name : {"A_big22.tga", "phong256.gif.bak", "phong256_bw.gif.bak", "items.tga",
                             "lighting/locations/decks/deck01/shadow", "lighting/day1/x/locations/decks/deck01/shadow",
                             "lighting/day1/locations/decks/deck01/shadow2", "ships\\Sloop1\\sloop1.tga", "", "resource\\textures\\",
                             "checker.tga", "resource\\textures\\characters\\checker.tga.tx", "x\\ammo\\checker.tga"})
        assert(!render::KnownMissingGameTexture(name));
    assert(!render::KnownMissingGameTexture(nullptr));

    render::SetLogSink(Collect);
    effects::BeginSceneResources("sea", 0);
    render::FailSceneTexture("resource\\textures\\PHONG256_BW.GIF.tx", "resource\\models\\ships\\Sloop1\\Sloop1_mast2.gm");
    render::FailSceneTexture("PHONG256_BW.GIF", "resource\\models\\ships\\Frigate_h1\\Frigate_h1.gm");
    effects::BeginSceneResources("location/FortFrance_town", 0); // another scene, the same session
    render::FailSceneTexture("ships\\Sloop1\\PHONG256_BW.GIF", "resource\\models\\ships\\Sloop1\\sloop1_mast201.gm");
    // One WARNING for the session, naming the first model; no ERROR for it in any scene.
    assert(Count(LogLevel::Warning, "WARNING: known game data defect: texture 'phong256_bw.gif' requested by "
                                    "resource\\models\\ships\\Sloop1\\Sloop1_mast2.gm") == 1);
    assert(Count(LogLevel::Warning, "") == 1);
    assert(Count(LogLevel::Error, "") == 0);

    // Any other missing model texture is still an ERROR of the scene being loaded, once per texture there.
    render::FailSceneTexture("resource\\textures\\ships\\nosuch.tga.tx", "resource\\models\\ships\\x.gm");
    render::FailSceneTexture("resource\\textures\\ships\\nosuch.tga.tx", "resource\\models\\ships\\y.gm");
    assert(Count(LogLevel::Error, "ERROR: location readiness: location/FortFrance_town: texture 'ships/nosuch' "
                                  "requested by resource\\models\\ships\\x.gm could not be created") == 1);
    assert(Count(LogLevel::Error, "") == 1);
    // A name shipped in other folders: its WARNING names the folder; the same name elsewhere is a real failure.
    render::FailSceneTexture("resource\\textures\\AMMO\\CHECKER.TGA.tx", "resource\\models\\Ammo\\blade_29.gm");
    render::FailSceneTexture("AMMO\\checker.tga", "resource\\models\\Ammo\\blade_29.gm");
    assert(Count(LogLevel::Warning, "WARNING: known game data defect: texture 'ammo/checker' requested by "
                                    "resource\\models\\Ammo\\blade_29.gm") == 1);
    assert(Count(LogLevel::Warning, "") == 2);
    render::FailSceneTexture("resource\\textures\\ships\\checker.tga.tx", "resource\\models\\ships\\z.gm");
    assert(Count(LogLevel::Error, "texture 'ships/checker'") == 1);

    effects::BeginSceneResources("location/OldWorld_Port", 42);
    const auto status = render::GetSceneLoadStatus();
    assert(status.active && status.owner == 42);
    render::AbandonSceneLoad(status.generation + 1); // not this load
    assert(render::GetSceneLoadStatus().active);
    render::AbandonSceneLoad(status.generation);
    assert(!render::GetSceneLoadStatus().active);
    assert(Count(LogLevel::Error, "ERROR: location readiness: location/OldWorld_Port was unloaded before its reveal") == 1);
    render::SetLogSink(nullptr);
    std::puts("PASS a texture the game's data names but does not ship: one WARNING per session; any other missing: ERROR; an abandoned scene: ERROR");
}
