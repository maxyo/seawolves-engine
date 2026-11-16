#pragma once

namespace storm::loading
{
// Game-thread-only resource preparation inside the stock loader's native call.
// This is not a simulation pause: no game frame or script dispatcher runs here.
inline bool active = false;
inline bool ready = false;
inline bool first_frame = true;
// The stock loading screen (Renderer Start/EndProgressView) is up: the scripts opened it (ReloadProgressStart) and the
// transaction runs from their ReloadProgressEnd.
inline bool screen_up = false;
// Opens (true) / closes the stock loading screen, for a scene the scripts opened without it (the renderer sets it).
inline void (*screen)(bool open) = nullptr;
enum class Outcome { Cancel, Quit, Unavailable, Invalidated, Reentrant };
inline const char *OutcomeName(Outcome outcome)
{
    switch (outcome)
    {
    case Outcome::Cancel: return "cancelled";
    case Outcome::Quit: return "quit";
    case Outcome::Unavailable: return "unavailable";
    case Outcome::Invalidated: return "invalidated";
    case Outcome::Reentrant: return "reentrant";
    }
    return "unknown";
}
class Scope {
 public:
    // The caller checks `active` first: transactions never nest.
    Scope() { active = true; ready = false; first_frame = true; }
    ~Scope() { active = false; ready = false; }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};
}
