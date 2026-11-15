#!/usr/bin/env python3
"""Production bridge body; --source-ref HEAD^ supplies a real old-code negative."""
from pathlib import Path
import argparse, os, subprocess, tempfile
here=Path(__file__).resolve().parent; root=here.parents[1]
p=argparse.ArgumentParser(); p.add_argument('--source-ref'); args=p.parse_args()
path='src/libs/battle_interface/src/land/battle_land.cpp'
source=(subprocess.check_output(['git','show',args.source_ref+':'+path],cwd=root,text=True)
        if args.source_ref else (root/path).read_text())
a=source.index('bool executeLandCommand('); b=source.index('\nvoid closeLandCommands()',a)
with tempfile.TemporaryDirectory(prefix='land-activation-') as temp:
    cpp=Path(temp)/'check.cpp'; binary=Path(temp)/'check'
    cpp.write_text((here/'check.cpp').read_text().replace('// PRODUCTION_BRIDGE',source[a:b]))
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-fsanitize=address,undefined',
        '-I'+str(root/'src/libs/util/include'),str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
