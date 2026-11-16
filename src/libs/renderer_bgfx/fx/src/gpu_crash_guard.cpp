#include "storm/fx/gpu_crash_guard.hpp"

#include <fstream>
#include <system_error>

namespace storm::fx
{

namespace
{
void write(const std::filesystem::path &file, const std::string &text)
{
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    std::ofstream out(file, std::ios::trunc);
    out << text << std::flush; // through to the file before the guarded work can kill the process
}
} // namespace

GpuCrashGuard::GpuCrashGuard(std::filesystem::path directory, std::string name)
{
    if (directory.empty())
        return;
    marker_ = directory / (name + "-running.txt");
    record_ = std::move(directory) / (name + "-crashed.txt");
}

void GpuCrashGuard::mark(const std::string &what) const
{
    if (enabled())
        write(marker_, what + "\n");
}

void GpuCrashGuard::clear() const
{
    std::error_code ignored;
    if (enabled())
        std::filesystem::remove(marker_, ignored);
}

GpuCrashGuard::Record GpuCrashGuard::take_leftover(std::string_view previous_exit) const
{
    if (!enabled())
        return {};
    Record leftover;
    {
        std::ifstream in(marker_);
        if (!std::getline(in, leftover.what) || leftover.what.empty())
            return {};
    }
    clear();
    if (!ExitMayBeCrash(previous_exit))
    {
        leftover.counted = false;
        leftover.why = "the system ended the process (" + std::string(previous_exit) + ") while it ran: not a crash";
        return leftover;
    }
    leftover.why = kProcessDied;
    record(leftover.what, leftover.why);
    return leftover;
}

std::string PreviousExit(const std::filesystem::path &cache_root)
{
    std::ifstream in(cache_root / "previous-exit.txt");
    std::string reason;
    in >> reason;
    return reason;
}

bool ExitMayBeCrash(std::string_view reason)
{
    return reason.empty() || reason == "unknown" || reason == "crash" || reason == "crash_native" || reason == "anr" ||
           reason == "signaled" || reason == "initialization_failure";
}

GpuCrashGuard::Record GpuCrashGuard::crashed() const
{
    Record out;
    if (!enabled())
        return out;
    std::ifstream in(record_);
    if (std::getline(in, out.what))
        std::getline(in, out.why);
    return out;
}

void GpuCrashGuard::record(const std::string &what, const std::string &why) const
{
    if (enabled())
        write(record_, what + "\n" + why + "\n");
}

void GpuCrashGuard::forget() const
{
    std::error_code ignored;
    if (enabled())
        std::filesystem::remove(record_, ignored);
}

} // namespace storm::fx
