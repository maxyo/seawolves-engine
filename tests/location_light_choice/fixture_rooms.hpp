// Synthetic candle layouts with the shapes of the stock interiors (a two-ring chandelier, wall sconce pairs,
// independent fixtures nearby); metres. Generated here, not copied from any game's locators.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fixture_rooms
{
struct Candle { std::array<float, 3> position; uint64_t component; };
struct Room { const char *name; std::vector<Candle> candles; std::size_t old_count, count; };

// Two rings of 8 candles around (x, z): the inner ring (radius 0.4 m) at y, the outer (0.6 m) 0.4 m above it, aligned.
// Neighbours on the outer ring are 0.46 m apart, inner to outer 0.45 m: one fixture at 0.9 m, nine at 0.35 m.
inline void AddChandelier(std::vector<Candle> &candles, float x, float y, float z)
{
    const uint64_t first = candles.size();
    for (const auto [radius, height] : {std::array<float, 2>{0.4f, 0.0f}, std::array<float, 2>{0.6f, 0.4f}})
        for (int k = 0; k < 8; ++k)
        {
            const float angle = static_cast<float>(k) * 0.785398163f;
            candles.push_back({{x + radius * std::cos(angle), y + height, z + radius * std::sin(angle)}, first});
        }
}

inline std::vector<Room> Rooms()
{
    std::vector<Room> rooms;
    {
        Room room{"chandelier", {}, 9, 1};
        AddChandelier(room.candles, 0.0f, 3.0f, 0.0f);
        rooms.push_back(std::move(room));
    }
    {
        // Four sconces of two candles 0.12 m apart, 2 m from each other: four fixtures at either radius.
        Room room{"sconces", {}, 4, 4};
        for (int k = 0; k < 4; ++k)
            for (int c = 0; c < 2; ++c)
                room.candles.push_back({{2.0f * k, 1.8f + 0.12f * c, -4.0f}, static_cast<uint64_t>(2 * k)});
        rooms.push_back(std::move(room));
    }
    {
        // A chandelier, a candle 3.2 m below-forward of it and one 1.9 m beside its outer ring: three fixtures.
        Room room{"mixed", {}, 11, 3};
        AddChandelier(room.candles, 0.0f, 3.0f, 0.0f);
        room.candles.push_back({{0.0f, 0.5f, 3.2f}, 16});
        room.candles.push_back({{0.6f + 1.9f, 3.4f, 0.0f}, 17});
        rooms.push_back(std::move(room));
    }
    return rooms;
}
} // namespace fixture_rooms
