// The whole numbers the screens read from the game's captions and table cells ("23 824", "Наши деньги 1 500 песо",
// "317 / 700", "55%"): one reading rule for every screen. The game sends many of its figures only as text; typed
// facts in place of these captions are a later step (the adapters' ANDROID_* facts already carry some).
#pragma once

#include <optional>
#include <string_view>

namespace storm::ui::screens
{

class Bindings;

// The first whole number of a game text. Its digits may be grouped by thousands with a space or a no-break space
// ("23 824", "11 575" → 23824; a group is exactly three digits, so "5 10" → 5); a minus sign right before
// the first digit makes it negative ("-3"); it ends at any other character ("4.6 / 140" → 4, "40 : 9 / 40" → 40,
// "100%" → 100). nullopt when the text holds no digit ("", "-", "—") or the number does not fit an int.
std::optional<int> ParseAmount(std::string_view text);

// The number of a caption or cell the game always writes with one: ParseAmount, else `none` and an error line,
// once per screen and `what` (the caption's widget or column), since the screen then shows a figure it made up.
// A text that may hold no number by design reads ParseAmount itself and says what its absence means.
int Amount(Bindings &bindings, std::string_view text, const char *what, int none = 0);

} // namespace storm::ui::screens
