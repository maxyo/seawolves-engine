#!/usr/bin/env python3
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='storm-device-retry-') as d:
 binary=Path(d)/'check'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root/'src/libs/relief'),str(Path(__file__).with_name('check.cpp')),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
