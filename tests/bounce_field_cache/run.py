#!/usr/bin/env python3
"""CPU-only production lamp/gather + cache controls, private compiler/cache, no renderer or GPU."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[2]
fx = root / "src/libs/renderer_bgfx/fx"
sources = [Path(__file__).with_name("check.cpp")] + [fx / "src" / (s + ".cpp") for s in
             ("bounce", "sun_fit", "cutout", "bakes", "light_falloff", "lamp_shadows", "log")]
header = fx / "include/storm/fx/bounce_field_cache.hpp"
inputs = sources + [header, root / "src/libs/renderer_bgfx/src/effects/bounce.hpp",
                   root / "src/libs/renderer_bgfx/src/effects/bounce.cpp"]
before = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
with tempfile.TemporaryDirectory(prefix="storm-bounce-field-") as temp:
    binary = Path(temp) / "check"
    command = ["nice", "-n", "10", os.environ.get("CXX", "c++"), "-std=c++20", "-O0", "-g1", "-pthread",
               "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
               "-fno-omit-frame-pointer", "-I" + str(fx / "include"), *map(str, sources), "-o", str(binary)]
    start = time.monotonic()
    subprocess.run(command, check=True, timeout=600)  # hang guards only: the host suite compiles many checks at once
    built = time.monotonic()
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    subprocess.run(["nice", "-n", "10", str(binary), str(Path(temp) / "cache")], check=True, timeout=120)
    ended = time.monotonic()
    assert before == {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    print(json.dumps({"compile_s": built - start, "runtime_s": ended - built,
                      "binary_sha256": digest, "source_sha256": before, "command": command}, indent=2))
# TemporaryDirectory removes compiler output and test-only cache on success or failure.
