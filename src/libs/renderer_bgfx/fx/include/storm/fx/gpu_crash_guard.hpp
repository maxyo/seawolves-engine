// The crash guard of GPU work that can kill the process inside a driver (the relief's GPU inference, a Vulkan start of
// the renderer): a marker written through before the work and removed after it succeeded. A marker found at the next
// start means the process died in that work. It becomes the crash record, and the GPU and driver it names keep their
// alternative (the CPU, OpenGL ES) until the driver changes or «Очистить кэш» removes the files. The callers log a
// found marker at error and say in their choice line why the alternative runs.
//
// Two files in `directory`: <name>-running.txt (the marker: what was running) and <name>-crashed.txt (the record:
// what crashed, then why). Nothing here decides or logs; the callers do.
#pragma once

#include <filesystem>
#include <string>

namespace storm::fx
{

class GpuCrashGuard
{
  public:
    // What a record says: `what` is the caller's label (GPU and driver), `why` a sentence for the logs.
    struct Record
    {
        std::string what, why;
        bool empty() const { return what.empty(); }
    };

    // The `why` of a record made from a leftover marker.
    static constexpr const char *kProcessDied = "the process died in it";

    // An empty `directory`: no files, nothing is remembered (the caller says so).
    GpuCrashGuard(std::filesystem::path directory, std::string name);

    bool enabled() const { return !marker_.empty(); }
    // Before the guarded work: `what` (one line) is flushed to the marker.
    void mark(const std::string &what) const;
    // After the guarded work ended without killing the process.
    void clear() const;
    // At start, before reading the record: a marker left by the previous run becomes the record ("the process died
    // in it") and is returned; empty when there was none.
    Record take_leftover() const;
    // What the record holds (empty: no crash recorded).
    Record crashed() const;
    // An orderly failure kept like a crash (a self-test that failed): same record, the caller's reason.
    void record(const std::string &what, const std::string &why) const;
    // The record is for another GPU or driver than the one running now: measured again.
    void forget() const;

  private:
    std::filesystem::path marker_, record_;
};

} // namespace storm::fx
