#include "wdm_label_layout.hpp"

#include <cassert>
#include <array>
#include <cstdio>
#include <vector>

using storm::worldmap::LabelBox;
using storm::worldmap::LabelLayout;

namespace
{
LabelBox Box(float x, float y, uint32_t weight, bool shown = false)
{
    return {x, y, x + 100.0f, y + 20.0f, weight, shown};
}
std::vector<bool> Shown(const std::vector<LabelBox> &boxes)
{
    std::vector<bool> out;
    for (const LabelBox &box : boxes)
        out.push_back(box.shown);
    return out;
}
} // namespace

#ifdef STORM_LABEL_PROJECTION_CHECK
#include "storm_glm.h"
#include "platform/android_worldmap.hpp"
#include <limits>
#include <string>

namespace
{
// Compile the exact LRender projection/layout block, with only its renderer/core
// plumbing replaced. The actual GLM transform, perspective divide, HUD mailbox,
// label rectangles and collision layout are production code.
struct ProjectionHarness
{
    struct Label
    {
        glm::vec3 pos{};
        float width = 150, height = 30, heightView = 100000;
        float l = 0, t = 0, r = 0, b = 0;
        bool onScreen = false, placed = true;
        std::string text = "Dominica";
        int icon = -1;
        uint32_t weight = 100;
    };
    struct Camera { float realHeight = 1; } camera;
    struct Objects { Camera *camera; } objects{&camera};
    struct Core { float GetDeltaTime() const { return 16; } } core;
    Objects *wdmObjects = &objects;
    std::vector<Label> labels{Label{}};
    std::vector<int32_t> labelSort;
    std::vector<LabelBox> labelBoxes;
    LabelLayout labelLayout;
    void Render(const glm::mat4 &mtx, float w, float h, float scale = 1)
    {
        const float pxPerDp = 1, outline = 1.5f;
        constexpr float kLabelGapDp = 4, kLabelHoldDp = 3;
#include "label_projection.inc"
    }
};

int ProjectionChecks()
{
    int anchorFailures = 0, visibilityFailures = 0, samples = 0;
    auto &hud = storm::android::worldMapHud;
    storm::android::WorldMapFrame frame;
    frame.available = true;
    frame.owner = 19;
    frame.gameplayRevision = 2;
    hud.update(frame, 1);
    hud.resetActivity();
    const auto area = [&](const std::string &geometry) {
        assert(hud.offer(std::to_string(hud.revision()) + "\t" + std::to_string(hud.owner().activity) +
                         "\tlabelarea\t" + geometry, 2));
    };
    ProjectionHarness harness;
    auto &label = harness.labels[0];
    const auto anchored = [&](float x, float y, float scale = 1) {
        ++samples;
        const bool correct = std::abs((label.l + label.r) * 0.5f - x) < .003f &&
                             std::abs(label.t - y) < .003f &&
                             std::abs(label.r - label.l - label.width * scale) < .003f;
        anchorFailures += !correct;
    };
    const auto visible = [&](bool expected) {
        visibilityFailures += label.onScreen != expected || label.placed != expected;
    };
    // Same island projection in both captured viewports. Compass/date/days may
    // overlap the caption; showing/hiding/moving/insetting HUD never moves it.
    for (const auto viewport : {std::array<float, 2>{960, 540}, {1280, 720}})
    {
        const auto [w, h] = viewport;
        const auto projection = storm::math::projectionMatrix(1.2f, w, h, 1, 10000);
        const float x = w - 195, y = 150, z = 500;
        label.pos = {(x * 2 / w - 1) * z / projection[0][0],
                     (1 - y * 2 / h) * z / projection[1][1], z};
        for (const auto &geometry : std::vector<std::string>{"none", std::to_string(int(w)) + ":" + std::to_string(int(h)) + ":" +
                                    std::to_string(int(w - 258)) + ":15:240:219",
                                    std::to_string(int(w)) + ":" + std::to_string(int(h)) + ":10:30:240:219",
                                    std::to_string(int(w)) + ":" + std::to_string(int(h)) + ":" +
                                    std::to_string(int(w - 290)) + ":40:240:219"})
        {
            area(geometry);
            harness.Render(projection, w, h);
            anchored(x, y);
            visible(true);
        }
        // A caption partly outside the viewport exits naturally; no screen clamp.
        area("none");
        for (const float edgeX : {-40.0f, w + 40.0f, w + 160.0f})
        {
            label.pos.x = (edgeX * 2 / w - 1) * z / projection[0][0];
            harness.Render(projection, w, h);
            anchored(edgeX, y);
            visible(edgeX < w + label.width / 2);
        }
    }
    // Fixed world anchor through camera pan, pitch/yaw, distance and zoom changes.
    // Independent screen oracle: transform to camera coordinates, then use the
    // field-of-view tangent; do not copy label-box placement or projectPoints.
    area("none");
    label.pos = {100, 20, 1000};
    for (const float yaw : {-0.2f, 0.0f, 0.2f})
        for (const float pitch : {-0.15f, 0.0f, 0.15f})
            for (const float pan : {-70.0f, 70.0f})
                for (const float distance : {-200.0f, 200.0f})
                    for (const float fov : {0.8f, 1.4f})
                    {
                        const float w = 1280, h = 720, scale = 1.5f;
                        const auto view = glm::inverse(storm::math::eulerMatrix(pitch, yaw, 0, pan, 0, distance));
                        const auto cameraPoint = view * glm::vec4(label.pos, 1);
                        const float x = w / 2 + cameraPoint.x / cameraPoint.z / std::tan(fov / 2) * w / 2;
                        const float y = h / 2 - cameraPoint.y / cameraPoint.z / std::tan(h / w * fov / 2) * h / 2;
                        harness.Render(storm::math::projectionMatrix(fov, w, h, 1, 10000) * view, w, h, scale);
                        anchored(x, y, scale);
                        visible(true);
                    }
    // The D3D LH projection has clip.w=view.z. Behind-camera and camera-plane
    // points must not produce mirrored captions, including visible -> behind ->
    // visible: both state bits must clear even though the old rectangle survives.
    const auto projection = storm::math::projectionMatrix(1.2f, 960, 540, 1, 10000);
    for (const float z : {500.0f, -500.0f, 500.0f, 0.0f, 500.0f})
    {
        label.pos = {0, 0, z};
        harness.Render(projection, 960, 540);
        visible(z > 0);
        if (z > 0) anchored(480, 270);
    }
    for (const float bad : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    {
        label.pos = {bad, 0, 500};
        harness.Render(projection, 960, 540);
        visible(false);
    }
    std::printf("projection checks: %d anchored samples; anchor failures=%d; visibility failures=%d\n",
                samples, anchorFailures, visibilityFailures);
    return anchorFailures || visibilityFailures;
}
} // namespace
#endif

int main()
{
    LabelLayout layout;
    constexpr float gap = 8.0f, hold = 6.0f;
    // Apart: all show.
    {
        std::vector<LabelBox> boxes{Box(0, 0, 1), Box(200, 0, 1), Box(0, 100, 50)};
        layout.Place(boxes, gap, hold);
        assert((Shown(boxes) == std::vector<bool>{true, true, true}));
    }
    // Colliding: the heavier shows whatever the order; equal weights: the first in the game's list.
    {
        std::vector<LabelBox> boxes{Box(0, 0, 1), Box(50, 5, 100), Box(60, 10, 50), Box(300, 0, 50), Box(330, 5, 50)};
        layout.Place(boxes, gap, hold);
        assert((Shown(boxes) == std::vector<bool>{false, true, false, true, false}));
    }
    // A chain: the heaviest hides its neighbour, which then hides nothing (a hidden label takes no room).
    {
        std::vector<LabelBox> boxes{Box(0, 0, 100), Box(90, 0, 50), Box(180, 0, 1)};
        layout.Place(boxes, gap, hold);
        assert((Shown(boxes) == std::vector<bool>{true, false, true}));
    }
    // The gap: closer than it collides.
    {
        std::vector<LabelBox> boxes{Box(0, 0, 100), Box(105, 0, 1)};
        layout.Place(boxes, gap, hold);
        assert(!boxes[1].shown);
        boxes = {Box(0, 0, 100), Box(110, 0, 1)};
        layout.Place(boxes, gap, hold);
        assert(boxes[1].shown);
    }
    // No flicker: a camera that jitters by a pixel across the gap's edge (a pan, a zoom) does not
    // swap the labels every frame. A label shown keeps its place down to gap - hold; a hidden one
    // needs the whole gap.
    {
        std::vector<LabelBox> boxes;
        int changes = 0;
        bool last = true;
        for (int frame = 0; frame < 200; ++frame)
        {
            // From 30 px apart to 0 and back, with a +-1 px jitter every frame.
            const float distance = (frame < 100 ? 30.0f - 0.3f * frame : 0.3f * (frame - 100)) + (frame % 2 ? 1.0f : -1.0f);
            const bool shown = boxes.empty() ? true : boxes[1].shown;
            boxes = {Box(0, 0, 100, true), Box(100.0f + distance, 0, 1, shown)};
            layout.Place(boxes, gap, hold);
            changes += boxes[1].shown != last;
            last = boxes[1].shown;
        }
        std::printf("the lighter label changed %d times over the approach and the retreat\n", changes);
        assert(changes == 2); // hidden once on the way in, shown once on the way out
    }
    // Deterministic: the same boxes and last choice give the same result.
    {
        std::vector<LabelBox> a{Box(0, 0, 50), Box(40, 0, 50), Box(80, 0, 50), Box(120, 0, 50)}, b = a;
        layout.Place(a, gap, hold);
        LabelLayout other;
        other.Place(b, gap, hold);
        assert(Shown(a) == Shown(b));
        assert((Shown(a) == std::vector<bool>{true, false, false, true}));
    }
#ifdef STORM_LABEL_PROJECTION_CHECK
    return ProjectionChecks();
#else
    return 0;
#endif
}
