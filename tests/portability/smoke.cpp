#include "float_to_int.h"
#include <cfenv>
#include <cmath>
#include <iostream>
#include "math_inlines.h"

int main()
{
    if (std::fesetround(FE_TONEAREST) != 0) return 1;
    if (ftoi(2.5f) != 2 || fftol(-1.75f) != -1 ||
        storm::math::round_to_int(2.5f) != 2 ||
        storm::math::round_to_int(-1.5f) != -2 ||
        storm::math::truncate_to_int(-1.75f) != -1) return 2;
    std::cout << "Portable math check passed (not an engine startup test)\n";
}
