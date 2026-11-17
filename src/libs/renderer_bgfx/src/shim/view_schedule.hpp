// The execution order of the game's views (render::kGameViewBegin..kGameViewEnd) within one bgfx frame.
//
// The shim opens a view whenever the game changes its render target, in submission order. A pass into another target
// in the middle of the scene (the sea's environment and sun-road cube faces, the ripple simulation, the sea's depth
// capture) cut the scene's pass in two: on a tile GPU every cut stores, resolves and reloads the scene's multisampled
// colour and depth. ViewSchedule records what every view writes and reads (bgfx texture ids) and its lane, and before
// the frame is handed to bgfx orders the views by lane where no dependency forbids it (bgfx::setViewOrder: submission,
// uniforms and per-view state stay as they were, only the execution order changes):
//   Target   other render targets (a texture the scene samples later): ahead of everything;
//   Scene    the 3D scene: the game picture before the scene boundary;
//   Post     the post chain: after the scene's last view by construction (it records no read of the scene);
//   Picture  the processed picture the interfaces (and the 3D after the scene boundary) draw on.
// A view waits for the earlier views it conflicts with (read after write, write after write, write after read); of the
// views whose wait is over the one of the earliest lane runs next, ties in submission order. A view of another lane
// that still runs between two scene views (it reads what the scene drew) cuts the scene: logged once per pair.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace storm::shim
{

class ViewSchedule
{
  public:
    enum class Lane : uint8_t
    {
        Target,
        Scene,
        Post,
        Picture,
    };
    struct Clear
    {
        uint16_t flags = 0;
        uint32_t rgba = 0;
        float z = 1.0f;
        uint8_t stencil = 0;
        bool known = false; // set_clear recorded it (views the effects set up themselves keep theirs unknown)
    };

    explicit ViewSchedule(uint16_t first) : first_(first) {}
    // A new bgfx frame: the game views start again at `first`.
    void begin();
    // `view` writes `writes` (bgfx texture ids) as `lane`; a view opened again forgets what it held.
    void open(uint16_t view, Lane lane, const char *name, std::initializer_list<uint16_t> writes);
    void read(uint16_t view, uint16_t texture);
    void write(uint16_t view, uint16_t texture);
    // The clear the view was given (bgfx::setViewClear), kept to add discard flags once the order is known.
    void set_clear(uint16_t view, const Clear &clear);
    // The execution order of the `count` views used from `first` (`order` receives their ids).
    void plan(uint16_t count, std::vector<uint16_t> &order);
    // After plan(): the last view in execution order that writes `texture` (UINT16_MAX: none), and a view's clear.
    uint16_t last_writer(uint16_t texture) const;
    const Clear &clear_of(uint16_t view) const { return views_[view - first_].clear; }
    // After plan(): the scene's views and its passes (runs of them no view of another lane cuts).
    uint32_t scene_views() const { return scene_views_; }
    uint32_t scene_passes() const { return scene_passes_; }

  private:
    struct View
    {
        bool opened = false;
        Lane lane = Lane::Picture;
        std::string name;
        std::vector<uint16_t> writes, reads;
        Clear clear;
    };
    static bool conflict(const View &earlier, const View &later);
    View &at(uint16_t view);

    uint16_t first_;
    std::vector<View> views_;
    std::vector<uint16_t> order_; // the last plan, view ids
    uint32_t scene_views_ = 0, scene_passes_ = 0;
    std::vector<std::string> logged_; // "<view> after <view it stopped at>" already logged
};

} // namespace storm::shim
