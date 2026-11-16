#ifdef STORM_VM_VERIFY
#include "vm_verify.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include <spdlog/spdlog.h>

namespace storm::vm_verify
{
namespace
{
constexpr std::array<const char *, static_cast<size_t>(Check::Count)> kNames = {
    "name_code", "attribute_by_code", "attribute_by_index", "attribute_by_name", "float_text", "event_statuses"};
std::array<uint64_t, static_cast<size_t>(Check::Count)> counts{}; // the game thread only

const struct AtExit
{
    ~AtExit()
    {
        Report();
    }
} atExit;
} // namespace

void Count(Check check) noexcept
{
    counts[static_cast<size_t>(check)]++;
}

void Mismatch(const char *format, ...) noexcept
{
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    spdlog::critical("vm verify: MISMATCH: {}", text);
    spdlog::shutdown();
    std::fprintf(stderr, "vm verify: MISMATCH: %s\n", text);
    std::abort();
}

void Report() noexcept
{
    std::string line = "vm verify: no mismatch; checks:";
    for (size_t n = 0; n < counts.size(); n++)
        line += std::string(" ") + kNames[n] + "=" + std::to_string(counts[n]);
    std::fprintf(stderr, "%s\n", line.c_str());
    if (spdlog::default_logger())
        spdlog::info("{}", line);
}
} // namespace storm::vm_verify
#endif
