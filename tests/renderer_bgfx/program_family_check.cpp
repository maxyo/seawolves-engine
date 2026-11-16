// The lit and baked program families (src/effects/lighting.hpp FfFamily, renderer_bgfx/CMakeLists.txt ff_families):
//   * every program Lighting can pick is embedded, with its opaque twin: fs_ff_<family>[_<s><n|np><h>] for every
//     family, specular, relief and sun shadow variant; the old suffix variants (linear light, the soft filter) are gone;
//   * ChooseFamily gives every combination of what a draw reads (lamps, AO volume, town wall, rain) a family that holds
//     all of it, and no family that holds it with fewer blocks exists: a draw without lamps or the AO volume never
//     carries the lamps' code, a lit draw takes only lit / lit_lamps.
// No GPU: the embedded shader table and the family rule only.
#include "device.hpp"
#include "effects/lighting.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace storm;
using effects::FfBlocks;
using effects::FfFamily;

namespace
{

int failures = 0;

void expect(bool ok, const std::string &what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

bool holds(const FfBlocks &has, const FfBlocks &needs)
{
    return (has.lamps || !needs.lamps) && (has.ao || !needs.ao) && (has.town_wall || !needs.town_wall) &&
           (has.wet || !needs.wet);
}

// `a` holds fewer blocks than `b` and none `b` lacks.
bool narrower(const FfBlocks &a, const FfBlocks &b)
{
    return holds(b, a) && !holds(a, b);
}

bool lit_family(FfFamily family)
{
    return family == FfFamily::Lit || family == FfFamily::LitLamps;
}

} // namespace

int main()
{
    int programs = 0;
    for (size_t f = 0; f < effects::kFfFamilies; ++f)
    {
        const FfFamily family = static_cast<FfFamily>(f);
        for (const char *specular : {"", "s"})
            for (const char *relief : {"", "n", "np"})
                for (const char *shadow : {"", "h"})
                {
                    const std::string suffix = std::string(specular) + relief + shadow;
                    const std::string name = std::string("fs_ff_") + effects::FamilyName(family) + (suffix.empty() ? "" : "_" + suffix);
                    const uint32_t id = shim::find_shader_by_name(name.c_str());
                    expect(id != UINT32_MAX, name + " is not embedded");
                    expect(id == UINT32_MAX || shim::opaque_twin(id) != UINT32_MAX, name + " has no opaque twin");
                    ++programs;
                }
    }
    for (const char *old : {"fs_ff_lit_l", "fs_ff_baked_snplo", "fs_ff_lit_o", "fs_ff_lit_snpl"})
        expect(shim::find_shader_by_name(old) == UINT32_MAX, std::string(old) + " is still embedded");

    for (int lit = 0; lit < 2; ++lit)
        for (int bits = 0; bits < 16; ++bits)
        {
            const FfBlocks needs{.lamps = (bits & 1) != 0, .ao = (bits & 2) != 0, .town_wall = (bits & 4) != 0,
                                 .wet = (bits & 8) != 0};
            if (lit && needs.town_wall)
                continue; // a lit draw is never a town wall (Lighting::prepare)
            const FfFamily chosen = effects::ChooseFamily(lit != 0, needs);
            const std::string what = std::string(lit ? "lit" : "baked") + " draw with blocks " + std::to_string(bits) +
                                     " -> " + effects::FamilyName(chosen);
            expect(lit_family(chosen) == (lit != 0), what + ": a family of the other kind");
            expect(holds(effects::FamilyBlocks(chosen), needs), what + ": the family lacks a block the draw reads");
            for (size_t f = 0; f < effects::kFfFamilies; ++f)
            {
                const FfFamily other = static_cast<FfFamily>(f);
                if (lit_family(other) == (lit != 0) && holds(effects::FamilyBlocks(other), needs))
                    expect(!narrower(effects::FamilyBlocks(other), effects::FamilyBlocks(chosen)),
                           what + ": " + effects::FamilyName(other) + " is narrower");
            }
        }
    expect(effects::ChooseFamily(false, {.town_wall = true, .wet = true}) == FfFamily::Baked, "a wet town wall");
    expect(effects::ChooseFamily(false, {.ao = true}) == FfFamily::BakedLamps, "an interior's baked draw");
    expect(effects::ChooseFamily(true, {.wet = true}) == FfFamily::Lit, "a lit draw in the rain");
    expect(!effects::FamilyBlocks(FfFamily::Lit).lamps && !effects::FamilyBlocks(FfFamily::Baked).lamps,
           "a lampless family has the lamps");
    if (failures)
        return 1;
    std::printf("PASS: %d lit / baked programs in %zu families, each with its opaque twin; the family rule\n", programs,
                effects::kFfFamilies);
    return 0;
}
