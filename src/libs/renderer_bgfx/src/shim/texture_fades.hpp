// The process's texture fades (texture_fade.hpp): the frame's time for every draw's TextureFade, the retired handles
// and the fade line.
#pragma once

#include "texture_fade.hpp"

#include <bgfx/bgfx.h>

namespace storm::shim
{

class TextureFades
{
  public:
    TextureFades();
    // The current frame's time (ms): every draw of a frame fades by the same share.
    uint64_t now() const { return now_; }
    uint32_t epoch() const { return epoch_; }
    // A handle a store's draws may have bound is replaced: destroyed once no fade can read it (kTextureRetireMs).
    void retire(bgfx::TextureHandle handle);
    // Once per frame, before bgfx::frame (Context::run_frame): the frame's time, the retired handles due, and at most
    // once a second the fade line for the swaps since the last one.
    void frame();
    // The renderer stops: every retired handle is destroyed now and the stores' remembered handles are forgotten.
    void shutdown();
    TextureFadeCounts &counts() { return counts_; }
    size_t retired() const { return retirement_.size(); }
    // Host checks: the clock in ms (null: the steady clock).
    void set_clock(uint64_t (*clock)());

  private:
    uint64_t (*clock_)();
    uint64_t now_ = 0;
    uint64_t logged_at_ = 0;
    uint32_t epoch_ = 1;
    TextureRetirement retirement_;
    TextureFadeCounts counts_;
};

TextureFades &texture_fades();

} // namespace storm::shim
