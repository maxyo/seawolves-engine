#!/usr/bin/env python3
"""Compile actual texture-selection/binding statements; assert mask LOD follows bound pixels, no GPU."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2];check=Path(__file__).resolve().parent;fx=root/'src/libs/renderer_bgfx/fx'
text=Path(os.environ.get('STORM_CHARACTER_DRAW_SOURCE',str(root/'src/libs/renderer_bgfx/src/shim/draw.cpp'))).read_text()
start=text.index('bgfx::TextureHandle handle = store->handle;',text.index('const uint32_t flags = sampler_flags(stage, *store);'))
end=text.index('++perf().texture_binds;',start)
with tempfile.TemporaryDirectory(prefix='storm-character-binding-') as d:
    source=Path(d)/'check.cpp';binary=Path(d)/'check'
    source.write_text((check/'binding_check.cpp').read_text().replace('PRODUCTION_BINDING',text[start:end]))
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all','-I'+str(fx/'include'),str(source),str(fx/'src/skin_tone.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
