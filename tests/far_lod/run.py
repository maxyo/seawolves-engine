#!/usr/bin/env python3
"""Build/run the actual far LOD simplifier and GEOM runtime integration on synthetic models.

Runtime uses real geometry translation units and manually advances their actual posted
bake lambda. External geometry, frame and cache services are recording host fixtures.
Pass offline dependency sources to avoid downloads; build directory is reused.
"""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir', type=Path, required=True)
p.add_argument('--dependency-sources', type=Path, help='Parent of extracted fmt/ and spdlog/ sources')
p.add_argument('--glm-source', type=Path, help='Extracted pinned GLM source')
p.add_argument('--meshoptimizer-source', type=Path, help='Extracted pinned meshoptimizer source')
p.add_argument('--sanitize', action='store_true', help='Run both actual simplifier/runtime under ASan and UBSan')
a = p.parse_args()
here = Path(__file__).resolve().parent
build = a.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
command = ['cmake', '-S', str(here), '-B', str(build), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
           '-DSTORM_TEST_SANITIZE=' + ('ON' if a.sanitize else 'OFF')]
if a.dependency_sources:
    command.append('-DSTORM_TEST_DEPENDENCY_SOURCES=' + str(a.dependency_sources.resolve()))
if a.glm_source:
    command.append('-DFETCHCONTENT_SOURCE_DIR_STORM_GLM=' + str(a.glm_source.resolve()))
if a.meshoptimizer_source:
    command.append('-DFETCHCONTENT_SOURCE_DIR_STORM_MESHOPTIMIZER=' + str(a.meshoptimizer_source.resolve()))
subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
subprocess.run(['cmake', '--build', str(build), '--parallel', '2', '--target',
                'far_lod_check', 'far_lod_runtime_check'], check=True)
subprocess.run([str(build / 'far_lod_check')], check=True)
raise SystemExit(subprocess.run([str(build / 'far_lod_runtime_check'), str(build)]).returncode)
