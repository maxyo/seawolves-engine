# video

FFmpeg-based playback for the script-facing `CAviPlayer` entity (`xinterface/aviplayer`) and the
`VIDEO` interface node (`xi_video_rect`, which embeds a `CAviPlayer`). The FFmpeg build:
[tools/ffmpeg/README.md](../../../tools/ffmpeg/README.md).

## Decoder policy

Fixed per codec in `kPolicy` (`src/player.cpp`) and logged for every video:

| codec | Android | host |
| --- | --- | --- |
| WMV3, VC-1, WMV2 | software (FFmpeg) | software |
| VP9 | MediaCodec `vp9_mediacodec`, then software | software |
| VP8 | MediaCodec `vp8_mediacodec`, then software | software |
| WMA v2, WMA Pro, MP3, Vorbis, Opus | software | software |

A missing hardware VP8/VP9 decoder is the one deliberate fallback: logged as a warning, decoded in software
(late frames are dropped against the audio clock). A codec outside the table, an unreadable container or a
decode error ends the video with an error in the log, and the scripts receive `ievntEndVideo`.

## Repaired streams

One explicit, logged repair (warnings in the log name the file): a WMA Pro stream without its codec header.
TEHO's `court.wmv` is an AVI (WMV3 + WMA Pro 48 kHz stereo) whose WAVEFORMATEX has `cbSize` 0 and
`nBlockAlign` 1; FFmpeg's `wmapro` decoder refuses it. The player installs the 18-byte header the Windows Media
encoder writes (bits per sample from the file, mono/stereo channel mask, decode flags `0x00E0`, as in TEHO's
`Jessika.wmv`; of all decode flags only `0x00E0` decodes `court.wmv` without an error) and reads the packet size
from the first audio packet, where the packets' 4-bit sequence numbers count up (16384 bytes in `court.wmv`). A
stream that does not fit (more than two channels, no packet size or more than one that fits) fails the video
as before. The repaired soundtrack is 50 s, the picture 33 s: the last picture stays until the sound ends.

## Script interface

- `CreateEntity(&aviVideoObj, "CAviPlayer")`, `SendMessage(&aviVideoObj, "ls", MSG_SET_VIDEO_PLAY,
  "<name>.wmv")`: plays `resource\videos\<name>` full screen, letterboxed.
- `MSG_SET_VIDEO_FLAGS` bit 0: loop.
- `ievntEndVideo` is sent once when the video has played out (or could not be played); the scripts delete the
  entity in their handler. Deleting it earlier stops playback.
- A tap sends `Control Activation`/`Control Deactivation` of `ICancel`; the scripts' `IVideoBreak` decides
  whether the video may be skipped.
- The soundtrack plays at the music volume.

## Tests

`tests/video/run.py --dependency-sources <dir>` (host): generated WebM, ASF (WMV2 + WMA v2) and AVI (WMV2 + MP3
24 kHz) clips through the real pipeline with ASan/UBSan, and every video of the local game folders
(`--game-videos`, default TEHO's `RESOURCE/VIDEOS` under `$STORM_GAME_DIR` or `local-testing/extracted/app`):
each must show a picture and play sound. FFmpeg has no WMV3 or WMA Pro encoder, so those are covered by the
game files only. `--smoke FILE...` plays local media in real time.
