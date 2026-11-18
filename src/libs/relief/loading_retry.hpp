#pragma once
#include <atomic>
#include <cstdint>
namespace storm::relief {
// One generation-scoped GPU texture in a place's loading window (the game draws its scene under the loading cover,
// device_choice.hpp), not a claim that the GPU is free. At most one a minute, so the GPU work does not hold the load up.
// Shared by the renderer and worker; revoke is safe even during source decode.
struct LoadingRetry {
    explicit LoadingRetry(uint64_t until, uint64_t epoch = 0) : deadline_ms(until), generation(epoch) {}
    const uint64_t deadline_ms;
    const uint64_t generation; // stable across renewed deadlines; zero is not a recovery epoch
    std::atomic<bool> active{true}, used{false};
    bool take(uint64_t now, bool possible, uint64_t last_retry) {
        if (!active.load() || now >= deadline_ms || !possible ||
            (last_retry && (now < last_retry || now - last_retry < 60000))) return false;
        bool expected=false;
        return used.compare_exchange_strong(expected,true) && active.load();
    }
};
}
