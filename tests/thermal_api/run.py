#!/usr/bin/env python3
"""Host checks for the AThermal symbol resolution and the pause decision (src/libs/renderer_bgfx/src/effects/thermal_api.hpp)."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="storm-thermal-") as directory:
    binary = Path(directory) / "check"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I" + str(root / "src/libs/renderer_bgfx/src/effects"),
        str(Path(__file__).with_name("check.cpp")), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
    # Exercise the real Android polling TU with a fake NDK and deterministic clock;
    # no Android SDK, hardware or waits. Only the steady-clock expression is replaced.
    temporary = Path(directory)
    (temporary / "android").mkdir()
    (temporary / "shim").mkdir()
    (temporary / "android/api-level.h").write_text("inline int android_get_device_api_level() { return 31; }\n")
    (temporary / "shim/shim.hpp").write_text("namespace storm::shim { inline void logf(const char *, ...) {} }\n")
    source = root / "src/libs/renderer_bgfx/src/effects/thermal.cpp"
    text = source.read_text()
    assert text.count("std::chrono::steady_clock::now()") == 1
    (temporary / "thermal_under_test.cpp").write_text(text.replace("std::chrono::steady_clock::now()", "TestClockNow()"))
    poll = temporary / "poll"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D__ANDROID__",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
        "-I" + str(temporary), "-I" + str(root / "src/libs/renderer_bgfx/src"),
        str(Path(__file__).with_name("poll.cpp")), "-o", str(poll),
    ], check=True)
    subprocess.run([str(poll)], check=True)
