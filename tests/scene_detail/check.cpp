// The preset's grass and sea detail (src/libs/renderer/src/scene_detail.hpp).
#include "scene_detail.hpp"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
static void expect(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int main()
{
    using namespace storm::render;
    for (int preset = -1; preset <= 3; ++preset)
    {
        const SceneDetail detail = PresetSceneDetail(preset);
        expect(detail.grass >= 0 && detail.grass <= 3, "grass is one of the game's four qualities");
        expect(detail.sea >= 0.0f && detail.sea <= 1.0f, "sea detail is in the game's 0..1 range");
    }
    expect(PresetSceneDetail(2).grass == 0 && SameSeaDetail(PresetSceneDetail(2).sea, 1.0f), "Высокая keeps the game's defaults");
    expect(PresetSceneDetail(1).grass == 0 && SameSeaDetail(PresetSceneDetail(1).sea, 1.0f), "Средняя keeps the game's defaults");
    expect(PresetSceneDetail(0).grass > PresetSceneDetail(1).grass && PresetSceneDetail(0).sea < PresetSceneDetail(1).sea,
           "Низкая is lighter than Средняя");
    expect(PresetSceneDetail(7).grass == PresetSceneDetail(2).grass, "out-of-range presets clamp");
    expect(SameSeaDetail(0.5f, 0.5004f) && !SameSeaDetail(0.5f, 0.51f), "sea detail compares with a small tolerance");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
