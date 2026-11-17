// The execution order of the game's views (src/shim/view_schedule.hpp) without a GPU: views into other targets run
// ahead of the scene unless the scene drew what they read or read their target before they draw it again; the scene
// keeps its own order; nothing passes an unrecorded view; the scene's passes are counted in execution order.
#include "view_schedule.hpp"

#include <cstdio>
#include <vector>

using storm::shim::ViewSchedule;
using Lane = ViewSchedule::Lane;

namespace
{
bool expect(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

constexpr uint16_t kFirst = 8;
// Texture ids.
constexpr uint16_t kSceneColor = 1, kSceneDepth = 2, kWaves = 3, kCube = 4, kPicture = 5, kCopy = 6, kBackdrop = 7,
                   kSeaDepth = 8;

std::vector<uint16_t> plan(ViewSchedule &s, uint16_t count)
{
    std::vector<uint16_t> order;
    s.plan(count, order);
    for (uint16_t &id : order)
        id = static_cast<uint16_t>(id - kFirst);
    return order;
}
} // namespace

int main()
{
    bool ok = true;
    {
        // The sea at «Высокая»: scene, cube faces, the backdrop beside the scene, its capture, wave steps, the sea
        // in the scene, post chain, interface.
        ViewSchedule s(kFirst);
        s.begin();
        s.open(kFirst + 0, Lane::Scene, "scene", {kSceneColor, kSceneDepth});
        s.open(kFirst + 1, Lane::Target, "backdrop", {kBackdrop});
        s.open(kFirst + 2, Lane::Target, "cube", {kCube});
        s.open(kFirst + 3, Lane::Target, "cube", {kCube});
        s.open(kFirst + 4, Lane::Target, "sea depth", {kSeaDepth});
        s.read(kFirst + 4, kBackdrop);
        s.open(kFirst + 5, Lane::Target, "waves", {kWaves});
        s.open(kFirst + 6, Lane::Target, "waves", {kWaves});
        s.open(kFirst + 7, Lane::Scene, "scene", {kSceneColor, kSceneDepth});
        for (const uint16_t texture : {kCube, kSeaDepth, kWaves})
            s.read(kFirst + 7, texture);
        s.open(kFirst + 8, Lane::Post, "post", {kPicture});
        s.read(kFirst + 8, kSceneColor);
        s.open(kFirst + 9, Lane::Picture, "interface", {kPicture});
        ok &= expect(plan(s, 10) == std::vector<uint16_t>{1, 2, 3, 4, 5, 6, 0, 7, 8, 9} && s.scene_passes() == 1 &&
                         s.scene_views() == 2,
                     "the target views run ahead of the scene, in their order: one scene pass");
        ok &= expect(s.last_writer(kSceneColor) == kFirst + 7, "the scene's last view in execution order");
    }
    {
        // A copy of the scene into a texture (StretchRect) reads what the scene drew: it stays, the scene is cut.
        ViewSchedule s(kFirst);
        s.begin();
        s.open(kFirst + 0, Lane::Scene, "scene", {kSceneColor, kSceneDepth});
        s.open(kFirst + 1, Lane::Target, "copy", {kCopy});
        s.read(kFirst + 1, kSceneColor);
        s.open(kFirst + 2, Lane::Target, "cube", {kCube});
        s.open(kFirst + 3, Lane::Scene, "scene", {kSceneColor, kSceneDepth});
        ok &= expect(plan(s, 4) == std::vector<uint16_t>{2, 0, 1, 3} && s.scene_passes() == 2,
                     "a target view that reads the scene stays; an independent one still moves");
    }
    {
        // The scene read the cube before it is drawn again (last frame's faces): the new faces must not run first.
        ViewSchedule s(kFirst);
        s.begin();
        s.open(kFirst + 0, Lane::Scene, "scene", {kSceneColor, kSceneDepth});
        s.read(kFirst + 0, kCube);
        s.open(kFirst + 1, Lane::Target, "cube", {kCube});
        s.open(kFirst + 2, Lane::Target, "copy", {kCopy});
        s.read(kFirst + 2, kCube);
        s.open(kFirst + 3, Lane::Scene, "scene", {kSceneColor, kSceneDepth});
        ok &= expect(plan(s, 4) == std::vector<uint16_t>{0, 1, 2, 3},
                     "write after read keeps the order, and what reads the staying view's target stays too");
    }
    {
        // A view nothing was recorded for is a barrier; begin() forgets the previous frame.
        ViewSchedule s(kFirst);
        s.begin();
        s.open(kFirst + 0, Lane::Scene, "scene", {kSceneColor, kSceneDepth});
        s.open(kFirst + 2, Lane::Target, "cube", {kCube});
        ok &= expect(plan(s, 3) == std::vector<uint16_t>{0, 1, 2}, "nothing passes an unrecorded view");
        s.begin();
        s.open(kFirst + 0, Lane::Scene, "scene", {kSceneColor, kSceneDepth});
        s.open(kFirst + 1, Lane::Target, "cube", {kCube});
        ok &= expect(plan(s, 2) == std::vector<uint16_t>{1, 0}, "begin() forgets the previous frame");
    }
    {
        // The clear of a view as the shim set it; a view the effects set up keeps an unknown one.
        ViewSchedule s(kFirst);
        s.begin();
        s.open(kFirst + 0, Lane::Picture, "interface", {kPicture});
        s.set_clear(kFirst + 0, {0x2, 0x112233ff, 1.0f, 0, false});
        s.open(kFirst + 1, Lane::Post, "post", {kPicture});
        plan(s, 2);
        ok &= expect(s.clear_of(kFirst + 0).known && s.clear_of(kFirst + 0).rgba == 0x112233ff &&
                         !s.clear_of(kFirst + 1).known,
                     "recorded clears are known, the others not");
    }
    return ok ? 0 : 1;
}
