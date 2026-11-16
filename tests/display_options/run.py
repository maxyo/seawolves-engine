#!/usr/bin/env python3
"""Host checks for the display options read from the appSettings channel (src/libs/util/include/platform/display_options.hpp)."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="storm-display-options-") as directory:
    binary = Path(directory) / "check"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I" + str(root / "src/libs/util/include"),
        str(Path(__file__).with_name("check.cpp")), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
