#!/usr/bin/env python3
"""Purchase list export (mobile_goods_transfer.hpp): rows, ship line, order action, click geometry."""
import json, pathlib, subprocess, tempfile
here = pathlib.Path(__file__).resolve().parent
root = here.parents[1]
with tempfile.TemporaryDirectory(prefix='goods-transfer-') as tmp:
    exe = pathlib.Path(tmp) / 'check'
    subprocess.run(['c++', '-std=c++20', '-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(root / 'src/libs/xinterface/src'), '-I' + str(root / 'tests/mobile_common'), str(here / 'check.cpp'), '-o', str(exe)], check=True)
    model = json.loads(subprocess.check_output([str(exe)], text=True))
    assert model['screen'] == 'goodstransfer' and model['widgets'][-1]['node'] == 'ANDROID_GOODS_SHIP'
print('PASS goods transfer export: rows replace the collection texts, order action, missing facts, click geometry; ASAN/UBSAN')
