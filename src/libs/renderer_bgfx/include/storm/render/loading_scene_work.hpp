#pragma once
#include <cstdint>

namespace storm::render {
// A frame decision, not a draw decision. Resource discovery still runs in HiddenPrepare.
// The caller supplies an actual prior opaque presentation and the current surface/target identity.
class LoadingSceneWork {
public:
    void begin(uint64_t frame, uint64_t generation, uint64_t surface, uint64_t target,
               bool active, bool opaque, bool pending, bool stable_target) {
        if (begun_ && frame_ == frame) return;
        const bool rediscover = dirty_ && generation_ == generation;
        frame_ = frame; generation_ = generation; surface_ = surface; target_ = target;
        ordinary_ = active && stable_target; eligible_ = ordinary_ && opaque; hidden_ = eligible_ && (pending || rediscover);
        dirty_ = false; presented_ = false; revoked_ = false; beauty_ = false; begun_ = true;
    }
    void beauty_submitted() { beauty_ = true; }
    void demand() { dirty_ = true; }
    bool hidden(uint64_t generation, uint64_t surface, uint64_t target) {
        if (begun_ && (generation_ != generation || surface_ != surface || target_ != target)) revoked_ = true;
        // Keep this frame's decision even on a changed main target. Never mix hidden/full passes;
        // a mismatch revokes its entire readiness qualification and the next frame relatches.
        return begun_ && hidden_;
    }
    // Only the renderer-owned post-chain swap transfers target ownership.
    // Preserve prior revocation and reject unrelated target/surface/generation changes.
    void post_target(uint64_t generation, uint64_t surface, uint64_t before, uint64_t after) {
        if (!begun_ || generation_ != generation || surface_ != surface || target_ != before || !after) {
            revoked_ = true;
            return;
        }
        target_ = after;
    }
    void presented(bool advanced, bool stable) { presented_ = advanced && stable; }
    bool complete(uint64_t generation, bool already_ready = false) const {
        return begun_ && generation_ == generation && (eligible_ || (already_ready && ordinary_)) && !hidden_ && !dirty_ && !revoked_ && beauty_ && presented_;
    }
    void end() { begun_ = false; } // even a failed present starts a new attempt, never reuses its latch
    void invalidate() { ordinary_ = eligible_ = false; revoked_ = true; dirty_ = true; presented_ = false; }
private:
    uint64_t frame_ = 0, generation_ = 0, surface_ = 0, target_ = 0;
    bool beauty_ = false, ordinary_ = false;
    bool begun_ = false, eligible_ = false, hidden_ = false, dirty_ = false, presented_ = false, revoked_ = false;
};
}
