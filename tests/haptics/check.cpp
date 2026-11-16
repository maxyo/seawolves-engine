// The haptic moments and their gaps (platform/android_haptics.hpp).
#include <platform/android_haptics.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

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
    using namespace storm::android;
    HapticThrottle throttle;
    expect(throttle.Allow(Haptic::Hit, 1000), "the first hit plays");
    expect(!throttle.Allow(Haptic::Hit, 1100), "a hit 100 ms later is the same hit");
    expect(throttle.Allow(Haptic::Block, 1100), "another kind is not held back");
    expect(throttle.Allow(Haptic::Hit, 1250), "a hit after its gap plays");
    expect(throttle.Allow(Haptic::Boarding, 0), "the first boarding plays even at time 0");
    expect(!throttle.Allow(Haptic::Boarding, 2999), "a boarding within 3 s is the same boarding");
    expect(throttle.Allow(Haptic::Broadside, 5000) && !throttle.Allow(Haptic::Broadside, 5500) && throttle.Allow(Haptic::Broadside, 5600),
           "a broadside every 600 ms at most");
    expect(!throttle.Allow(Haptic::Count, 9000), "no effect for an unknown kind");
    expect(std::string(HapticName(Haptic::Hit)) == "hit" && std::string(HapticName(Haptic::Block)) == "block" &&
               std::string(HapticName(Haptic::Broadside)) == "broadside" && std::string(HapticName(Haptic::Boarding)) == "boarding",
           "the names the application plays");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
