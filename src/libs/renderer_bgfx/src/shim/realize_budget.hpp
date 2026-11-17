// The game-thread time a frame of play spends making textures' GPU copies on their first draw.
//
// A texture's first draw makes its GPU copy in TexStore::sync on the game thread: the transcode cache key over all its
// levels, the cache file read, the DDS container and bgfx::createTexture; the swap of a finished transcode creates one
// more. On a phone that is up to tens of milliseconds per texture, and a new place in view brings many at once. While a
// scene is prepared behind its loading screen (the loading transaction, storm::loading::active, or the location/sea
// preparation up to its reveal, effects::ScenePreparing) nothing is budgeted. In play the 3D scene draws of
// Device::draw make textures only while the frame has spent less than kFrameMs on them, and always at least one, so
// that every texture is made within a few frames (2D draws, the HUD, menus and loading pictures, are never budgeted):
//
//   - a draw whose texture has no GPU copy yet and comes past the budget is left out of this frame, as a Vulkan draw
//     whose pipeline is still being made is (it appears a frame or two later; the frame counts as incomplete for
//     storm::render::LastIncompleteFrame);
//   - a finished transcode past the budget keeps drawing its interim copy and is swapped in (with its fade) later.
//
// Nothing is dropped silently: textures made, their time, the draws left out and the swaps postponed are counted (the
// renderer counters tex_realizes, tex_realize_ms, tex_realize_waits) and, for the frames of play, logged with the
// worst frame every 10 s while it happens ("texture realize (play): ...", shim/perf.cpp).
#pragma once

#include <algorithm>
#include <cstdint>

namespace storm::shim
{

class RealizeBudget
{
  public:
    // Of a 33 ms (30 fps) frame; a single texture over it still goes through, alone in its frame.
    static constexpr double kFrameMs = 4.0;

    // Whether a texture may be made now (and timed with made()). Unlimited while a scene is prepared.
    bool admit(bool preparing)
    {
        frame_.preparing |= preparing;
        return preparing || frame_.made == 0 || frame_.ms < kFrameMs;
    }
    void made(double ms)
    {
        frame_.ms += ms;
        ++frame_.made;
    }
    // admit() said no: a draw left out of this frame (its texture has no GPU copy yet), or the swap of a finished
    // transcode postponed (its interim copy draws meanwhile).
    void left_out() { ++frame_.left_out; }
    void postponed() { ++frame_.postponed; }

    struct Frame
    {
        double ms = 0;          // spent making textures
        uint32_t made = 0;      // textures made
        uint32_t left_out = 0;  // draws left out
        uint32_t postponed = 0; // transcode swaps postponed
        bool preparing = false; // a scene was prepared in this frame: not a frame of play
    };
    // At each presented frame: this frame's totals, and the budget starts over.
    Frame end_frame()
    {
        const Frame frame = frame_;
        frame_ = {};
        return frame;
    }

    // The frames of play since the last report (end_frame's results that are not `preparing`).
    struct Window
    {
        uint64_t made = 0, left_out = 0, postponed = 0;
        uint32_t frames_made = 0, frames_left_out = 0;
        double ms = 0, worst_ms = 0;
        void add(const Frame &frame)
        {
            if (frame.preparing)
                return;
            made += frame.made;
            left_out += frame.left_out;
            postponed += frame.postponed;
            frames_made += frame.made != 0;
            frames_left_out += frame.left_out != 0;
            ms += frame.ms;
            worst_ms = std::max(worst_ms, frame.ms);
        }
        bool empty() const { return made == 0 && left_out == 0 && postponed == 0; }
    };

  private:
    Frame frame_;
};

} // namespace storm::shim
