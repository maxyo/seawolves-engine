// PirateEncounterPercent(): the script side of «Частота пиратов на карте» (platform/pirate_frequency.hpp). The value
// is read on every call, so a change on the settings screen counts from the next encounter on, without a restart.
#include "core.h"
#include "fs.h"
#include "s_import_func.h"
#include "script_libriary.h"
#include "v_s_stack.h"
#include "vma.hpp"

#include <platform/android_ui.hpp>
#include <platform/pirate_frequency.hpp>

#include <spdlog/spdlog.h>

#include <optional>

namespace
{
class PirateFrequencyScriptLib : public SCRIPT_LIBRIARY
{
  public:
    bool Init() override;
};

CREATE_SCRIPTLIBRIARY(PirateFrequencyScriptLib)

constexpr int kNotSet = -1;

// engine.ini's value, read once (kNotSet when the key is absent).
int iniPirateFrequency()
{
    static const int value = [] {
        auto ini = fio->OpenIniFile(core.EngineIniFileName());
        return ini ? static_cast<int>(ini->GetInt(nullptr, "pirate_frequency", kNotSet)) : kNotSet;
    }();
    return value;
}

// The configured percentage, or the default; one log line per distinct value read (valid, missing or invalid).
int pirateFrequency()
{
    int value = iniPirateFrequency();
    const char *source = "engine.ini pirate_frequency";
#ifdef __ANDROID__
    value = storm::android::graphicsSetting("pirate_frequency", value);
    source = "the app setting pirate_frequency";
#endif
    static std::optional<int> reported;
    if (storm::game::validPirateFrequency(value))
    {
        // Once per value: the log shows what the encounter script got, and every change of the setting.
        if (reported != value)
        {
            reported = value;
            spdlog::info("pirate frequency: {}% of the random pirate encounters kept ({})", value, source);
        }
        return value;
    }
    if (reported != value)
    {
        reported = value;
        if (value == kNotSet)
            spdlog::info("pirate frequency: {} is not set; the default {}% of the random pirate encounters is kept", source,
                         storm::game::kPirateFrequencyDefault);
        else
            spdlog::error("pirate frequency: {} is {}, not one of 20, 50, 100; the default {}% is used", source, value,
                          storm::game::kPirateFrequencyDefault);
    }
    return storm::game::kPirateFrequencyDefault;
}

uint32_t PirateEncounterPercent(VS_STACK *stack)
{
    auto *result = stack->Push();
    if (!result)
        return IFUNCRESULT_FAILED;
    result->Set(static_cast<int32_t>(pirateFrequency()));
    return IFUNCRESULT_OK;
}
} // namespace

bool PirateFrequencyScriptLib::Init()
{
    IFUNCINFO info;
    info.pFuncName = "PirateEncounterPercent";
    info.pReturnValueName = "int";
    info.nArguments = 0;
    info.pFuncAddress = PirateEncounterPercent;
    core.SetScriptFunction(&info);
    return true;
}
