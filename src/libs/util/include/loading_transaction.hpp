#pragma once

namespace storm::loading
{
// Game-thread-only resource preparation inside the stock loader's native call.
// This is not a simulation pause: no game frame or script dispatcher runs here.
inline bool active = false;
inline bool ready = false;
inline bool first_frame = true;
// Set by the renderer when it cannot draw the loading frame (no stock artwork): the transaction aborts.
inline bool unavailable = false;
enum class Outcome { Cancel, Quit, Unavailable, Invalidated, Reentrant, Stalled };
inline const char *OutcomeName(Outcome outcome)
{
    switch (outcome)
    {
    case Outcome::Cancel: return "cancelled";
    case Outcome::Quit: return "quit";
    case Outcome::Unavailable: return "unavailable";
    case Outcome::Invalidated: return "invalidated";
    case Outcome::Reentrant: return "reentrant";
    case Outcome::Stalled: return "stalled";
    }
    return "unknown";
}
class Scope {
 public:
    // The caller checks `active` first: transactions never nest.
    Scope() { active = true; ready = false; first_frame = true; unavailable = false; }
    ~Scope() { active = false; ready = false; unavailable = false; }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};
}
