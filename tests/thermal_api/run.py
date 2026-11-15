#!/usr/bin/env python3
"""Host checks for AThermal telemetry and the explicit-hold-only background gate.

--baseline-revision additionally requires the old production gate to fail the new
hot-forecast nonblocking contract. No device, Android SDK, GPU or wall-clock waits.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline-revision')
args = parser.parse_args()
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
    (temporary / "shim/shim.hpp").write_text("namespace storm::shim { void logf(const char *, ...); }\n")
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
    if args.baseline_revision:
        old = subprocess.check_output([
            'git', '-C', str(root), 'show',
            args.baseline_revision + ':src/libs/renderer_bgfx/src/effects/thermal.cpp',
        ], text=True)
        assert old.count("std::chrono::steady_clock::now()") == 1
        (temporary / "thermal_under_test.cpp").write_text(old.replace("std::chrono::steady_clock::now()", "TestClockNow()"))
        subprocess.run([
            os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D__ANDROID__",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
            "-I" + str(temporary), "-I" + str(root / "src/libs/renderer_bgfx/src"),
            str(Path(__file__).with_name("poll.cpp")), "-o", str(poll),
        ], check=True)
        negative = subprocess.run([str(poll)], capture_output=True, text=True)
        assert negative.returncode == 1 and 'FAILED: hot forecast must not pause background work' in negative.stderr, negative.stderr
        print('PASS negative control: old production gate fails hot-forecast nonblocking contract')
