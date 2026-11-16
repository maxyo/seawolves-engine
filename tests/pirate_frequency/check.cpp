// The steps and the default of «Частота пиратов на карте» (platform/pirate_frequency.hpp).
#include <platform/pirate_frequency.hpp>

#include <cstdio>
#include <cstdlib>
#include <iterator>

static int failures = 0;
static void expect(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int main()
{
    using namespace storm::game;
    expect(validPirateFrequency(kPirateFrequencyDefault), "the default is one of the steps");
    expect(kPirateFrequencyDefault == 20, "the default keeps one random pirate encounter in five (owner request)");
    expect(validPirateFrequency(100), "100: the game's own frequency is offered");
    for (int bad : {-1, 0, 19, 21, 101, 500})
        expect(!validPirateFrequency(bad), "values between and outside the steps are invalid");
    for (size_t i = 1; i < std::size(kPirateFrequencies); ++i)
        expect(kPirateFrequencies[i - 1] < kPirateFrequencies[i], "steps rise from «Редко» to «Как в игре»");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
