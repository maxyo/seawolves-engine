#pragma once

namespace storm::loading
{
// Game-thread-only resource preparation inside the stock loader's native call.
// This is not a simulation pause: no game frame or script dispatcher runs here.
inline bool active = false;
inline bool ready = false;
inline bool first_frame = true;
enum class Outcome { Cancel, Quit, Unavailable, Invalidated, Reentrant };
struct Aborted { Outcome outcome; };
class Scope {
 public:
    Scope() { if (active) throw Aborted{Outcome::Reentrant}; active = true; ready = false; first_frame = true; }
    ~Scope() { active = false; ready = false; }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};
}
