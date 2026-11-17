#pragma once
#include <algorithm>
#include <iterator>

// «Частота пиратов на карте»: the percentage of the game's random pirate encounters on the world map that are kept.
// The app's reviewed script patch of PROGRAM/Encounters/encounters_map.c calls PirateEncounterPercent() (worldmap
// pirate_frequency_script.cpp) and drops the rest; quest encounters are never dropped. 100 is the game as it is.
// Where it comes from: engine.ini `pirate_frequency`, and on Android the app's settings (GraphicsSettings
// "pirate_frequency", the same steps and default), which win. A missing or invalid value is logged and the default used.
namespace storm::game
{
// The steps the settings screen offers: «Редко», «Реже», «Как в игре».
inline constexpr int kPirateFrequencies[] = {20, 50, 100};
// Default: five times fewer random pirates than the game has.
inline constexpr int kPirateFrequencyDefault = 20;

inline bool validPirateFrequency(int percent)
{
    return std::find(std::begin(kPirateFrequencies), std::end(kPirateFrequencies), percent) != std::end(kPirateFrequencies);
}
} // namespace storm::game
