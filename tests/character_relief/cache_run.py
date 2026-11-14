#!/usr/bin/env python3
"""Compile exact production skin probe/source lifecycle with draw/texture doubles, ASan/UBSan."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2];check=Path(__file__).resolve().parent
fx=root/'src/libs/renderer_bgfx/fx';effects=root/'src/libs/renderer_bgfx/src/effects'
def function(path,declaration):
    text=Path(path).read_text();start=text.index(declaration);brace=text.index('{',start);depth=1;i=brace+1
    while depth:depth+=(text[i]=='{')-(text[i]=='}');i+=1
    return text[start:i]
production=function(os.environ.get('STORM_CHARACTER_LIGHTING_SOURCE',str(effects/'lighting.cpp')),'std::array<float, 8> Lighting::skin_tone(')+'\n'+function(effects/'effects.cpp','void SetEffectsTextureSource(')
with tempfile.TemporaryDirectory(prefix='storm-character-cache-') as d:
    source=Path(d)/'check.cpp';binary=Path(d)/'check'
    source.write_text((check/'cache_check.cpp').read_text().replace('PRODUCTION_FUNCTIONS',production))
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(fx/'include'),'-I'+str(root/'src/libs/renderer_bgfx/include'),
        str(source),str(fx/'src/skin_tone.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
