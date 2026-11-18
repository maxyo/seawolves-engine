# FFmpeg for in-game video

`build.py` builds the minimal FFmpeg used by `src/libs/video` from the pinned source
(`tools/android/engine_sources.py`: FFmpeg n8.1.3, commit `1041abdc962f`). CMake runs it at
configure time (`cmake/ffmpeg.cmake`); `tests/video/run.py` runs it for the host check.

## Cache

A build is stored under `<cache>/<target>-<key>/` (`include/`, `lib/`, `configure.txt`,
`COPYING.LGPLv2.1`). The key hashes the source revision, the configure flags, the compiler's
`--version` and `BUILD_FORMAT` in `build.py`; any change builds a new prefix, an unchanged one is reused
by every build directory and worktree pointing at the same cache. The default cache is
`<prepared sources>/../ffmpeg-cache` (e.g. `build/android-ci/ffmpeg-cache`); override with
`-DSTORM_FFMPEG_CACHE=`. A lock file serialises concurrent builds of one target. Compiles go
through `tools/ccache/launcher` (not part of the key; a missing ccache fails the build, see
`tools/ccache/README.md`).

## Licence

LGPL-2.1-or-later only: no `--enable-gpl`, `--enable-version3` or `--enable-nonfree`.
`build.py` fails unless the configured `FFMPEG_LICENSE` is "LGPL version 2.1 or later". The
libraries ship as unmodified shared objects so that they can be replaced. Notice for the APK:
`third_party/licenses/FFmpeg-LGPL-2.1.txt`; the build's `configure.txt` is packaged next to it
as `FFmpeg-configure.txt`.

## Configure flags

Common to every target:

```
--disable-everything --disable-autodetect --disable-programs --disable-doc --disable-network
--disable-avdevice --disable-avfilter --disable-debug
--enable-shared --disable-static --enable-pic
--enable-avcodec --enable-avformat --enable-swscale --enable-swresample --enable-pthreads
--enable-demuxer=asf,avi,matroska
--enable-decoder=wmv3,vc1,wmv2,wmav2,wmapro,mp3float,vp8,vp9,vorbis,opus
--enable-parser=mpegaudio
```

Each component is there because a game video needs it (ffprobe over the game files;
`tests/video/run.py --game-videos` decodes every one of them):

| component | game files |
| --- | --- |
| demuxer `asf`, decoder `wmav2` | TEHO `RESOURCE/VIDEOS/*.wmv` except `Pirate.wmv` and `court.wmv` |
| decoder `wmv3` (pulls in `vc1`) | TEHO AkellaLogo, Blackmark, Title, Credits_ru, intro_Sharle, Door, England, France, Holland, Spain, Jessika, Reef, Outro_home, Outro_stay, Pirate, court (640x480 / 640x384, VC-1 Main) |
| decoder `wmv2` | TEHO Artefact, BlackPearl, Idol, Temple (640x384), Ending, Siege, VDDrepair (720x480) |
| decoder `wmapro` | TEHO Jessika (ASF), court (AVI, WMA Pro 48 kHz without its codec header: repaired by the player, `src/libs/video/README.md`) |
| demuxer `avi` | TEHO Pirate, court (`.wmv` name, RIFF AVI inside) |
| decoder `mp3float`, parser `mpegaudio` | TEHO Pirate (MP3 24 kHz stereo in AVI; the AVI demuxer splits the chunks into MP3 frames with the parser) |
| demuxer `matroska` (WebM), decoders `vp9`, `vorbis` | Caribbean Legend 2.0 `Resource/Videos/*.webm` (VP9 1920x1080, Vorbis 48 kHz) |
| decoder `vp8` | Caribbean Legend 2.0: 6 videos (VP8 + Vorbis) |
| decoder `opus` | Caribbean Legend 2.0 `Resource/Videos/tutorial/*.webm` (VP9 + Opus) |

`vc1` costs nothing extra: FFmpeg's `wmv3` decoder is the VC-1 decoder. Other bitstream filters and
parsers the decoders need (e.g. `vp9_superframe_split`) come with the decoders' own dependencies. No
protocols: the player reads through its own I/O callbacks. `tests/video/run.py` checks that every
software decoder of the player's policy (`kPolicy`, `src/libs/video/src/player.cpp`) is in `build.py`'s decoder list.

`android-arm64` adds:

```
--enable-cross-compile --target-os=android --arch=aarch64 --cpu=armv8-a
--enable-jni --enable-mediacodec --enable-decoder=vp8_mediacodec,vp9_mediacodec
--extra-ldflags=-Wl,-z,max-page-size=16384
```

MediaCodec gives hardware VP8/VP9 decoding (policy: `src/libs/video/README.md`); the
libraries link `libmediandk.so` and `libandroid.so` from the platform. The NEON assembly is on.
The 16 KiB page alignment is required of every library in the APK.

`android-x86_64` (emulator debug builds only; release APKs are arm64) has the same Android flags
with `--arch=x86_64 --disable-x86asm` instead of the arm64 CPU flags (no nasm on the development
hosts, so the x86 SIMD paths are off).

`host` adds `--disable-x86asm` (no nasm on the development hosts; the host build only serves
tests and the desktop development build).
