#!/usr/bin/env python3
"""Decode synthetic clips through src/libs/video with the engine's own FFmpeg build.

Clips are generated here with the system ffmpeg (red then blue picture, 440 Hz tone), never
committed: WebM VP9+Vorbis (Caribbean Legend's format), WebM VP8+Opus, ASF WMV2+WMAv2 and AVI
WMV2+MP3 24 kHz (TEHO's containers and codecs; FFmpeg has no WMV3 or WMA Pro encoder) and a
silent WebM VP9 that plays on the wall clock. Then every video in the --game-videos folders
(default: TEHO's RESOURCE/VIDEOS of the local game, $STORM_GAME_DIR or
<workspace>/local-testing/extracted/app; skipped with a SKIP line when absent) must show a
picture and decode its sound without an error: this covers WMV3, WMA Pro and the repaired
court.wmv. The decoders of the player's policy must all be in tools/ffmpeg/build.py.
--smoke plays local media files in real time and reports what was shown.

  python3 tests/video/run.py --dependency-sources build/host-ci/sources [--game-videos DIR ...] [--smoke FILE ...]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--dependency-sources', type=Path, required=True)
p.add_argument('--build-dir', type=Path, default=Path(__file__).resolve().parents[2] / 'build/video-check')
p.add_argument('--game-videos', type=Path, nargs='*', help='folders whose videos are probed (default: the local TEHO game)')
p.add_argument('--probe-seconds', type=float, default=3)
p.add_argument('--smoke', type=Path, nargs='*', default=[])
p.add_argument('--smoke-seconds', type=float, default=10)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
build = a.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
if shutil.which('ffmpeg') is None:
    sys.exit('tests/video needs the system ffmpeg (libvpx, libvorbis, libopus) to generate its clips')

# Every software decoder of the player's policy is built (a missing one fails only when a video needs it).
policy = re.findall(r'\{AV_CODEC_ID_\w+, "(\w+)", (?:nullptr|"\w+")\}', (root / 'src/libs/video/src/player.cpp').read_text())
built = re.search(r"'--enable-decoder=([\w,]+)'", (root / 'tools/ffmpeg/build.py').read_text()).group(1).split(',')
missing = sorted(set(policy) - set(built))
if not policy or missing:
    sys.exit(f'FAIL: decoders of the player policy not in tools/ffmpeg/build.py: {missing or "policy not found"}')

sources = a.dependency_sources.resolve()
prefix = Path(subprocess.check_output([sys.executable, str(root / 'tools/ffmpeg/build.py'), '--source', str(sources / 'ffmpeg'),
                                       '--cache', str(sources.parent / 'ffmpeg-cache'), '--target', 'host'], text=True).strip())
binary = build / 'video-check'
cxx = os.environ.get('CXX', 'c++')
subprocess.run([cxx, '-std=c++20', '-O1', '-g', '-pthread', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-fno-omit-frame-pointer', '-I' + str(root / 'src/libs/video/include'), '-isystem', str(prefix / 'include'),
                str(root / 'src/libs/video/src/player.cpp'), str(Path(__file__).with_name('check.cpp')),
                '-L' + str(prefix / 'lib'), '-Wl,-rpath,' + str(prefix / 'lib'),
                '-lavformat', '-lavcodec', '-lswscale', '-lswresample', '-lavutil', '-o', str(binary)], check=True)

picture = 'color=c=red:s=160x120:r=10:d={half}[a];color=c=blue:s=160x120:r=10:d={half}[b];[a][b]concat=n=2:v=1:a=0'
clips = [  # name, video codec, audio codec, seconds
    ('vp9-vorbis.webm', ['-c:v', 'libvpx-vp9', '-deadline', 'realtime', '-cpu-used', '8'], ['-c:a', 'libvorbis'], 2.0),
    ('vp8-opus.webm', ['-c:v', 'libvpx', '-deadline', 'realtime'], ['-c:a', 'libopus'], 2.0),
    ('wmv2-wmav2.asf', ['-c:v', 'wmv2', '-q:v', '2'], ['-c:a', 'wmav2', '-b:a', '64k'], 2.0),
    # TEHO's Pirate.wmv: an AVI with MP3 at 24 kHz.
    # The AVI muxer would shift the picture by the MP3 encoder delay and leave out frame 1 (the clip check
    # expects one frame every 0.1 s).
    ('wmv2-mp3.avi', ['-c:v', 'wmv2', '-q:v', '2', '-avoid_negative_ts', 'disabled'],
     ['-c:a', 'libmp3lame', '-b:a', '64k', '-ar', '24000'], 2.0),
    ('vp9-silent.webm', ['-c:v', 'libvpx-vp9', '-deadline', 'realtime', '-cpu-used', '8'], None, 1.0),
]
for name, video, audio, seconds in clips:
    clip = build / name
    if not clip.exists():
        command = ['ffmpeg', '-nostdin', '-v', 'error', '-y', '-filter_complex', picture.format(half=seconds / 2)]
        if audio:
            command += ['-f', 'lavfi', '-i', f'sine=frequency=440:sample_rate=44100:duration={seconds}', *audio, '-ac', '2']
        command += [*video, '-pix_fmt', 'yuv420p', str(clip)]
        subprocess.run(command, check=True)
    subprocess.run([str(binary), 'clip', str(clip), '160', '120', str(seconds), '1' if audio else '0'], check=True,
                   timeout=300)  # a deadlock guard, not a time budget
# The workspace holds local-testing/: the checkout's parent, or further up for a worktree inside local-testing/.
workspace = next((q for q in root.parents if (q / 'local-testing').is_dir()), root.parent)
game_dir = Path(os.environ.get('STORM_GAME_DIR', workspace / 'local-testing/extracted/app'))
folders = a.game_videos if a.game_videos is not None else [game_dir / 'RESOURCE/VIDEOS']
probed = 0
for folder in folders:
    videos = sorted(f for f in folder.rglob('*') if f.suffix.lower() in ('.wmv', '.webm')) if folder.is_dir() else []
    if not videos:
        print(f'SKIP: no game videos in {folder}', flush=True)
    for media in videos:
        subprocess.run([str(binary), 'probe', str(media), str(a.probe_seconds)], check=True, timeout=180)
        probed += 1
for media in a.smoke:
    subprocess.run([str(binary), 'smoke', str(media), str(a.smoke_seconds)], check=True, timeout=a.smoke_seconds + 60)
print(f'PASS: video decode (VP9/Vorbis, VP8/Opus, ASF WMV2/WMAv2, AVI WMV2/MP3, silent), audio clock, wall clock, pause; '
      f'{probed} game videos', flush=True)
