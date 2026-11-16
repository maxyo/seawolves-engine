#!/usr/bin/env python3
"""Builds pbrify-upscale.ncnn.param / .bin (README.md, fx/upscale.hpp) from PBRify's 4x SPAN upscaler.

1. Kim2091/PBRify_Remix release v1.7.2 (CC0-1.0; the check target downloads and checks it, cmake/StormDerivedFiles.cmake, and
   passes its folder as --sources), its Extras/Old_Models/4x-PBRify_UpscalerSPANV4.pth (checked by sha256 here).
2. Exported to ONNX with spandrel (opset 17, input 1x3xHxW, output 1x3x4Hx4W, float 0..1, free size).
3. That float graph converted by pnnx into ncnn's .param / .bin (fp16 weights), as make_ncnn_models.py converts the
   relief's networks; the game runs it on the GPU and on the CPU.

Needs exactly upscale-requirements.txt (the build makes that venv). The game's build fetches the files this script made
on the reference machine (the bytes depend on the CPU's ISA; README.md); the host target storm_upscale_model_check
(storm_derived_files in ../CMakeLists.txt) runs it as: python -I make_upscale_model.py --sources <the release's
folder> --output <dir>; it writes pbrify-upscale.ncnn.param and pbrify-upscale.ncnn.bin into --output, and the target
checks them against the published pins.
"""
import argparse
import hashlib
import importlib.util
import sys
import types
from pathlib import Path

MEMBER = 'Extras/Old_Models/4x-PBRify_UpscalerSPANV4.pth'
MEMBER_SHA256 = 'd1f1fa1af7a5ab23fcf143e75aac4951e6c52d56bf77d9ccd4d004a4831d0461'


def stub_torchvision():
    tv = types.ModuleType('torchvision')
    transforms = types.ModuleType('torchvision.transforms')
    functional = types.ModuleType('torchvision.transforms.functional')
    def attribute(name):
        if name.startswith('__'):
            raise AttributeError(name)
        return type(name, (), {})

    for module in (tv, transforms, functional):
        module.__getattr__ = attribute
    tv.transforms, transforms.functional = transforms, functional
    sys.modules.update({'torchvision': tv, 'torchvision.transforms': transforms, 'torchvision.transforms.functional': functional})


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sources', type=Path, nargs='+', required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    # The release's folder, with or without its top folder (PBRify_Remix-1.7.2).
    found = [path for root in a.sources for path in (root / MEMBER, *root.glob('*/' + MEMBER)) if path.is_file()]
    if not found:
        raise SystemExit(f'{MEMBER} is not in {a.sources}')
    pth = found[0].read_bytes()
    if hashlib.sha256(pth).hexdigest() != MEMBER_SHA256:
        raise SystemExit(f'{MEMBER} does not match its pin')
    a.output.mkdir(parents=True, exist_ok=True)

    stub_torchvision()
    import shutil
    import subprocess
    import tempfile
    import spandrel
    import torch

    model = spandrel.ModelLoader().load_from_file(str(found[0])).model.eval()
    tool = Path(importlib.util.find_spec('pnnx').submodule_search_locations[0]) / 'pnnx'
    with tempfile.TemporaryDirectory() as work:
        graph = Path(work) / 'model.onnx'
        torch.onnx.export(model, torch.rand(1, 3, 64, 64, generator=torch.Generator().manual_seed(0)), str(graph),
                          input_names=['input'], output_names=['output'], opset_version=17,
                          dynamic_axes={'input': {2: 'h', 3: 'w'}, 'output': {2: 'H', 3: 'W'}}, dynamo=False)
        # The shape only traces the graph: ncnn's convolutions take any size.
        done = subprocess.run([str(tool), str(graph), 'inputshape=[1,3,176,176]'], cwd=work, capture_output=True, text=True)
        if done.returncode:
            raise SystemExit(f'pnnx failed:\n{done.stdout}{done.stderr}')
        for suffix in ('param', 'bin'):
            target = a.output / f'pbrify-upscale.ncnn.{suffix}'
            shutil.copyfile(Path(work) / f'model.ncnn.{suffix}', target)
            print(target, hashlib.sha256(target.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
