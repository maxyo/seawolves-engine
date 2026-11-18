#!/usr/bin/env python3
"""Host check of the stall watchdog (src/libs/diagnostics stall_watchdog.hpp, util platform/stall_watch.hpp): a game
thread blocked in a phase and a render thread blocked in its present are reported with what they do, their backtraces
name the blocking functions, reports repeat while the stall lasts, recovery and the background end it, a stall
inside a load is a warning."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="storm-stall-watchdog-") as directory:
    binary = Path(directory) / "check"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
        "-fno-omit-frame-pointer", "-rdynamic", "-pthread",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I" + str(root / "src/libs/diagnostics/include"), "-I" + str(root / "src/libs/util/include"),
        str(Path(__file__).with_name("check.cpp")), str(root / "src/libs/diagnostics/src/stall_watchdog.cpp"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
