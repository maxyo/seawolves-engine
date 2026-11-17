// The HUD's time acceleration (android_time_scale.hpp) against the game's own rules, ported from
// the stock scripts: the TimeScaleFaster / TimeScaleSlower handler (seadogs.c ProcessControls),
// procInterfacePrepare and Start_InterfaceDoExit (interface.c) and GetSeaTimeScale (x2), the latter
// also as the app's script patch interface-exit-time-reset changes it (x1, counter 0, perk off).
#include "android_time_scale.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace storm::android;

namespace
{
struct Game
{
    int32_t timeScaleCounter = 0;
    float timeScale = 1.f;
    bool timeSpeedPerk = false;
    bool worldMap = false;
    bool disableMapEnter = false; // a fight near: x6 at sea
    bool noFast = false;          // pchar.questTemp.NoFast
    bool interfaceOpen = false;
    bool exitResetPatch = false;  // the app's script fix interface-exit-time-reset is applied
    int activations = 0;

    bool counter(int32_t &value) const
    {
        value = timeScaleCounter;
        return true;
    }
    void setCounter(int32_t value)
    {
        timeScaleCounter = value;
    }
    float scale() const
    {
        return timeScale;
    }
    // seadogs.c ProcessControls, "TimeScaleFaster" / "TimeScaleSlower" (bSeaActive, no boarding).
    void activate(const char *control)
    {
        ++activations;
        if (noFast) return;
        int32_t &c = timeScaleCounter;
        if (std::string(control) == "TimeScaleFaster")
            c += c >= 12 ? 4 : c >= 4 ? 2 : 1;
        else
            c -= c >= 16 ? 4 : c >= 6 ? 2 : 1;
        if (c < -2) c = -2;
        if (disableMapEnter) { if (c > 20) c = 20; }
        else if (!worldMap) { if (c > 28) c = 28; }
        else if (c > 8) c = 8;
        if (worldMap && c > 6) c = 6;
        const float next = 1.f + static_cast<float>(c) * 0.25f;
        if (c == 0)
        {
            if (timeSpeedPerk)
            {
                timeScale = 1.f;
                timeSpeedPerk = false;
            }
        }
        else
        {
            timeScale = next;
            timeSpeedPerk = true;
        }
    }
    // interface.c procInterfacePrepare: the scene's layers are frozen (EngineLayersOffOn(false)),
    // the scale goes to x1 under the interface.
    void openInterface()
    {
        interfaceOpen = true;
        if (timeSpeedPerk) timeScale = 1.f;
    }
    // interface.c Start_InterfaceDoExit. Stock: x2 (GetSeaTimeScale) whatever the counter says.
    // Patched (interface-exit-time-reset, owner decision): x1 with the counter and the perk, as the
    // stock x1 resets (EndAboveForm).
    void closeInterface()
    {
        interfaceOpen = false;
        if (!timeSpeedPerk) return;
        if (!exitResetPatch)
        {
            timeScale = 2.f;
            return;
        }
        timeScale = 1.f;
        timeScaleCounter = 0;
        timeSpeedPerk = false;
    }
};

// What the HUD's button asks for (ui hud.cpp BuildTimeScale): the step after the scale it shows.
int nextStep(float scale)
{
    const int current = scale < 1.5f ? 0 : scale < 3.f ? 1 : scale < 6.f ? 2 : 3;
    return (current + 1) % 4;
}

void tap(Game &game)
{
    requestTimeScaleStep(game, nextStep(game.timeScale));
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 0.01f;
}
} // namespace

int main()
{
    assert(timeScaleStep("TimeScaleX1") == 0 && timeScaleStep("TimeScaleX8") == 3 && timeScaleStep("TimeScale") == -1);

    { // At sea the four steps go round.
        Game game;
        tap(game), assert(near(game.timeScale, 2.f) && game.timeScaleCounter == 4);
        tap(game), assert(near(game.timeScale, 4.f) && game.timeScaleCounter == 12);
        tap(game), assert(near(game.timeScale, 8.f) && game.timeScaleCounter == 28);
        tap(game), assert(near(game.timeScale, 1.f) && game.timeScaleCounter == 0 && !game.timeSpeedPerk);
    }

    // The shipped game: x2 / x4 / x8 → ship screen → closed resets to x1 (the app's script patch);
    // the HUD shows x1 and its next tap gives x2, at sea and on the world map (x2 and its x2.5 limit).
    for (const bool worldMap : {false, true})
        for (int taps = 1; taps <= (worldMap ? 2 : 3); ++taps)
        {
            Game game;
            game.exitResetPatch = true;
            game.worldMap = worldMap;
            for (int i = 0; i < taps; ++i) tap(game);
            assert(game.timeSpeedPerk);
            game.openInterface();
            game.closeInterface();
            assert(near(game.timeScale, 1.f) && game.timeScaleCounter == 0 && !game.timeSpeedPerk);
            assert(nextStep(game.timeScale) == 1);
            tap(game), assert(near(game.timeScale, 2.f) && game.timeScaleCounter == 4 && game.timeSpeedPerk);
        }

    // Without the patch (a game version it is not made for): x4 / x8 → ship screen → closed leaves
    // x2 with the counter at x4 / x8. Every tap still changes the scale: the regression was a button
    // that did nothing at x2 after x4.
    for (const int taps : {2, 3})
    {
        Game game;
        for (int i = 0; i < taps; ++i) tap(game);
        game.openInterface();
        assert(near(game.timeScale, 1.f));
        game.closeInterface();
        assert(near(game.timeScale, 2.f));
        tap(game), assert(near(game.timeScale, 4.f) && game.timeScaleCounter == 12);
        tap(game), assert(near(game.timeScale, 8.f));
        tap(game), assert(near(game.timeScale, 1.f) && game.timeScaleCounter == 0);
        tap(game), assert(near(game.timeScale, 2.f));
    }

    { // The world map holds x2.5: x2 → x2.5 → x1, also after the ship screen without the patch.
        Game game;
        game.worldMap = true;
        tap(game), assert(near(game.timeScale, 2.f));
        tap(game), assert(near(game.timeScale, 2.5f) && game.timeScaleCounter == 6);
        game.openInterface();
        game.closeInterface();
        assert(near(game.timeScale, 2.f) && game.timeScaleCounter == 6);
        tap(game), assert(near(game.timeScale, 2.5f) && game.timeScaleCounter == 6);
        tap(game), assert(near(game.timeScale, 1.f) && game.timeScaleCounter == 0);
    }

    { // A fight holds x6: x4 → x8 asked → x6, then round to x1.
        Game game;
        game.disableMapEnter = true;
        tap(game), tap(game), tap(game);
        assert(near(game.timeScale, 6.f));
        tap(game), assert(near(game.timeScale, 1.f) && game.timeScaleCounter == 0);
    }

    { // A quest lock refuses: nothing changes, the counter included.
        Game game;
        game.noFast = true;
        tap(game);
        assert(near(game.timeScale, 1.f) && game.timeScaleCounter == 0 && game.activations == 1);
    }

    { // A step the engine already runs at with the counter in step is not activated again.
        Game game;
        tap(game);
        const int activations = game.activations;
        requestTimeScaleStep(game, 1);
        assert(game.activations == activations && near(game.timeScale, 2.f));
    }

    std::puts("PASS: time acceleration steps, interface exit with and without the x1 reset patch, world-map and fight limits, quest lock");
}
