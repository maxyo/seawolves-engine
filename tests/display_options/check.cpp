// The display options read from the "appSettings" channel (platform/display_options.hpp).
#include <platform/display_options.hpp>

#include <cstdio>
#include <cstdlib>

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
    expect(FlatJsonInt(R"({"a":1,"ui_scale": 115})", "ui_scale") == 115, "integer member");
    expect(!FlatJsonInt(R"({"ui_scale":1.5})", "ui_scale"), "a fraction is not an integer");
    expect(!FlatJsonInt(R"({"ui_scale":"115"})", "ui_scale"), "a string is not an integer");
    expect(!FlatJsonInt(R"({"x":1})", "ui_scale"), "absent");
    const DisplayOptions good = ReadDisplayOptions(R"({"fx_preset":1,"ui_scale":130,"hud_transparency":60,"left_handed":1})");
    expect(good.ui_scale == 130 && good.hud_transparency == 60 && good.left_handed == 1 && good.problems.empty(), "valid steps are taken");
    const DisplayOptions bad = ReadDisplayOptions(R"({"ui_scale":90,"hud_transparency":25,"left_handed":2})");
    expect(bad.ui_scale == kUiScaleDefault && bad.hud_transparency == kHudTransparencyDefault &&
               bad.problems.find("ui_scale 90") != std::string::npos && bad.problems.find("hud_transparency 25") != std::string::npos && bad.left_handed == 0 &&
               bad.problems.find("left_handed 2") != std::string::npos,
           "invalid values fall back to the defaults and are named");
    const DisplayOptions missing = ReadDisplayOptions("{}");
    expect(missing.ui_scale == 100 && missing.hud_transparency == 0 && missing.problems.find("is missing") != std::string::npos,
           "missing values are named");
    expect(OneOf(kUiScales, kUiScaleDefault) && OneOf(kHudTransparencies, kHudTransparencyDefault), "defaults are steps");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
