#pragma once

namespace storm::render
{
// Where the renderer's log lines go (effects, textures, relief, the shim's own): once a sink is set, to it alone
// (the engine's logs, which also write logcat) with the line's level: `Error` for the lines that report a failure
// ("ERROR: ..."), `Warning` for a known defect that is not a failure ("WARNING: ...", e.g. of the game's own data);
// before that, or with none (tools, tests), to stderr and, on Android, logcat. Any thread may log: the sink must be
// thread safe.
enum class LogLevel { Info, Warning, Error };
using LogSink = void (*)(LogLevel level, const char *line);
void SetLogSink(LogSink sink);
} // namespace storm::render
