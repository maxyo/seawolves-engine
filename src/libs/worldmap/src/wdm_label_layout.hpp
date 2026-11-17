// Where each world-map label goes so that names do not cover one another. The game gives each label
// a weight (worldmap_init.c: "Weight is use when shake intersection labels": islands 100, towns 50,
// shores 1) but the engine never used it: zoomed out, the names were drawn over one another.
//
// Every label shows (names must not vanish when the map is zoomed out). The labels
// are taken by weight (heavier first; equal weights in their order in the game's list); each takes
// the first side of its world point (below it, the game's place, then above, right, left) where its
// box keeps `gap` clear of every box already placed. A label keeps last frame's side while it is no
// closer than `gap - hold` there, so a pan or a zoom does not swap sides every frame. A label with no
// free side stays below its point, drawn under the heavier ones (`clear` false). The result depends
// only on the boxes and the last frame's sides, never on the frame's timing.
// Header-only, standard library only (host check tests/worldmap_labels).
#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace storm::worldmap
{

// The size of a label of a kind the game hid above `heightView` (worldmap_init.c: islands 1000, towns 250, shores
// 200) at the camera's `height`: 1 up to that height, then shrinking with the map (heightView / height), never below
// `floor`. Every label shows at every height (names must not vanish when the map is zoomed out); the
// shrink keeps the crowded small names of a zoomed-out map apart and readable.
inline float HeightScale(float heightView, float height, float floor)
{
    if (!(heightView > 0.0f) || !(height > heightView))
        return 1.0f;
    return std::max(floor, heightView / height);
}

enum class LabelSide : int8_t
{
    None = -1, // not placed in the last frame
    Below,     // centred under the point (the game's place)
    Above,
    Right,
    Left,
};

struct LabelBox
{
    float x = 0.0f, y = 0.0f;          // in: the label's world point on the screen, px
    float width = 0.0f, height = 0.0f; // in: its size on the screen, px
    uint32_t weight = 0;
    LabelSide side = LabelSide::None; // in: last frame's side; out: this frame's
    bool clear = false;               // out: it keeps the gap to every heavier label
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f; // out: the box on the screen, px
};

class LabelLayout
{
  public:
    void Place(std::span<LabelBox> boxes, float gap, float hold)
    {
        order_.resize(boxes.size());
        std::iota(order_.begin(), order_.end(), size_t{0});
        std::stable_sort(order_.begin(), order_.end(),
                         [&](size_t a, size_t b) { return boxes[a].weight > boxes[b].weight; });
        placed_.clear();
        for (const size_t index : order_)
        {
            LabelBox &box = boxes[index];
            const auto fits = [&](LabelSide side, float clear) {
                Put(box, side, gap * 0.5f);
                return std::none_of(placed_.begin(), placed_.end(), [&](const LabelBox *other) {
                    return box.left < other->right + clear && other->left < box.right + clear &&
                           box.top < other->bottom + clear && other->top < box.bottom + clear;
                });
            };
            const LabelSide last = box.side;
            box.clear = true;
            if (last == LabelSide::None || !fits(last, gap - hold))
            {
                constexpr LabelSide kSides[] = {LabelSide::Below, LabelSide::Above, LabelSide::Right, LabelSide::Left};
                const auto *free = std::find_if(std::begin(kSides), std::end(kSides),
                                                [&](LabelSide side) { return side != last && fits(side, gap); });
                box.clear = free != std::end(kSides);
                box.side = box.clear ? *free : LabelSide::Below;
                Put(box, box.side, gap * 0.5f);
            }
            placed_.push_back(&box);
        }
    }

  private:
    // The box on `side` of the point; a side box keeps `margin` from the point
    static void Put(LabelBox &box, LabelSide side, float margin)
    {
        switch (side)
        {
        case LabelSide::Above:
            box.left = box.x - 0.5f * box.width, box.top = box.y - box.height;
            break;
        case LabelSide::Right:
            box.left = box.x + margin, box.top = box.y - 0.5f * box.height;
            break;
        case LabelSide::Left:
            box.left = box.x - margin - box.width, box.top = box.y - 0.5f * box.height;
            break;
        default:
            box.left = box.x - 0.5f * box.width, box.top = box.y;
            break;
        }
        box.right = box.left + box.width;
        box.bottom = box.top + box.height;
    }

    std::vector<size_t> order_;
    std::vector<const LabelBox *> placed_;
};

} // namespace storm::worldmap
