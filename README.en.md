# mpv-joc — E-AC-3 JOC (Dolby Atmos) playback for mpv

An mpv C plugin (cplugins feature). It takes the object audio out of an E-AC-3
JOC stream, rebuilds it with the [JustOneCacophony](https://github.com/TheM14/JustOneCacophony)
renderer, and hands the result to mpv as an ordinary audio track. The renderer,
the engine and the settings are the same ones the upstream frontends
the upstream project's command-line frontend uses, so the samples delivered to the player
are **bit-identical**.

* Output: binaural (HRTF) or a 2.0 – 7.1 speaker layout;
* Output device: mpv's own audio output, i.e. WASAPI on Windows (optionally
  forced, optionally exclusive);
* Configuration: mpv's own configuration file, with one `joc-*` option namespace;
* Input: bare `.eac3` / `.ec3`, or the E-AC-3 JOC track of an MP4 / Matroska file
  (a video track keeps playing normally);
* Seeking: mpv's seek works; the renderer re-enters the bitstream at the target.

## License

This repository (plugin, patch, scripts, documentation) is **MIT**, see [LICENSE](LICENSE).

The code under `fork/` is compiled into mpv, so distributing the resulting player is also
subject to mpv's own license: LGPL-2.1-or-later (GPL-2.0-or-later when built with
`-Dgpl=true`). mpv's license text ships with this repository as [LICENSE-mpv](LICENSE-mpv).
The `joc_core` renderer is not distributed here; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Relationship to the upstream project

This repository contains the **mpv-side integration only**: the cplugin (`src/`) and the
ad_joc patch (`fork/`). The object-audio renderer, E-AC-3/EMDF parsing, SOFA and Rosella
support come from [TheM14/JustOneCacophony](https://github.com/TheM14/JustOneCacophony); the behaviour reference is
the upstream project's `joc_cli` (the comparison basis). `joc_core.dll` is not distributed here — it is built from
the upstream project, and none of its code is copied or modified in this repository.

## Repository layout

One renderer (`joc_core.dll`), two integration shapes:

| Shape | Where | mpv requirement |
|---|---|---|
| **cplugin**: drop-in for stock mpv | `src/` `config/` `tools/` | built with cplugins |
| **ad_joc patch**: compile the decoder into mpv | `fork/` | build mpv from source (see [fork/README.md](fork/README.md)) |

## Requirements

* mpv with **cplugins** support (official Windows builds have it; check
  `List of enabled features` in `mpv --msg-level=all=v`).
* An `ffmpeg` executable (found on `PATH` by default, `joc-ffmpeg` overrides).
* HRTF data for binaural output: a SOFA measurement set or a Rosella
  `.personalized_headphone` model. **Not bundled**; speaker layouts do not need it.

## Install

```text
<mpv>\\scripts\\joc_mpv.dll                     the plugin
<mpv>\\joc_core.dll                      the renderer (same binary as JustOneCacophonyCXX)
<mpv>\\HRTF\\binaural.sofa               default binaural data
<mpv>\\HRTF\\binaural.personalized_headphone   (Rosella model, either one)
```

`joc_core.dll` must not go into `scripts\\`: mpv offers every file in that
directory to the script loader. The plugin looks for the renderer at `joc-core`,
then `<plugin dir>\\joc_core.dll`, then `<plugin dir>\\..\\joc_core.dll`, then `PATH`.

Add to `mpv.conf`:

```ini
script-opts=joc-output=binaural,joc-hrtf-source=sofa
```

Or load it explicitly:

```powershell
mpv --script="C:\\path\\to\\joc_mpv.dll" --script-opts=joc-output=speaker,joc-layout=7.1 movie.mkv
```

## Options

All keys live in mpv's `--script-opts`, prefixed `joc-`; unknown keys are
ignored and reported in `joc_decoder.log`.

| Key | Values | Default | Meaning |
|---|---|---|---|
| `joc-enabled` | yes / no | yes | disable the plugin entirely |
| `joc-output` | binaural / speaker | binaural | output form |
| `joc-layout` | 2.0 3.1 5.1 7.1 5.1.2 5.1.4 7.1.2 7.1.4 9.1.4 9.1.6 | 7.1 | speaker layout |
| `joc-hrtf-source` | sofa / rosella | sofa | binaural HRTF input |
| `joc-hrtf` | path | empty | empty: default name under `<plugin>\\HRTF\\` or `<plugin>\\..\\HRTF\\` |
| `joc-mode` | near / mid / far | mid | binaural mode |
| `joc-gain-db` | float (dB) | 0 | render gain (binaural can exceed full scale) |
| `joc-tail` | seconds | 5.0 | binaural room tail cap (never delivered as playback time) |
| `joc-int24` | yes / no | no | deliver 24-bit fixed point instead of float32 |
| `joc-object-delay` | samples | 1473 | OAMD object delay, matching the existing implementation |
| `joc-threads` | integer | 0 | kernel threads, 0 = automatic |
| `joc-ffmpeg` | path | ffmpeg | ffmpeg used for the core PCM |
| `joc-core` | path | empty | explicit `joc_core.dll` |
| `joc-wasapi` | auto / yes / no | auto | `yes` sets `ao=wasapi` |
| `joc-exclusive` | yes / no | no | sets `audio-exclusive=yes` |
| `joc-chunk` | seconds | 2.0 | audio bytes written per HTTP chunk |
| `joc-mute-until-ready` | yes / no | yes | silence the file's own track until the rendered one is selected |
| `joc-hrtf-cache` | auto / memory / disk / none | auto | compiled-HRTF cache; auto = `<mpv config dir>\\joc-hrtf-cache` |
| `joc-hrtf-cache-dir` | path | empty | explicit cache directory |
| `joc-attach` | yes / no | yes | no: serve the stream without adding the track (capture/verification) |
| `joc-verbose` | yes / no | no | log every HTTP request and reposition |
| `joc-log` | path | empty | log file; empty: `<mpv config dir>\\joc_decoder.log` |

Layouts wider than 7.1 cannot reach a WASAPI endpoint (8 channels maximum), so
`9.1.4` / `9.1.6` are delivered without a channel mask and will normally not map
correctly; use `7.1.4` or below.

## How it works

```text
mpv — video and subtitles as usual
 │
 └─ file-loaded ─► probe (bare stream walked directly; container: ask ffmpeg which
                    track is E-AC-3 and whether it carries JOC)
                     │
                     ▼
             joc_core.dll (joc_stream.h)
                     ▲ E-AC-3 syncframes + core PCM from ffmpeg
                     │
         ┌───────────┴────────────┐
         │  rendered as WAV (float32 / int24, with a channel mask)
         │  served by an in-process loopback HTTP endpoint with Range support
         └───────────┬────────────┘
                     ▼
        mpv: audio-add http://127.0.0.1:<port>/joc.wav select
```

mpv reads the track as a seekable WAV file, so its demuxer, cache, A/V sync,
WASAPI output and seeking all work unmodified.

Startup: at `start-file` the plugin probes and creates the renderer while mpv is
still opening the file, and silences the file's own track; at `file-loaded` it
`audio-add ... select`s the rendered track and restores the previous mute state.
Inputs the probe rejects are unmuted immediately (a container whose E-AC-3 track
turns out not to be JOC spends the ~100 ms ffmpeg probe muted; set
`joc-mute-until-ready=no` to disable the behaviour). Measured on a 30 s 7.1.4 clip:
about 0.6 s cold, about 0.37 s once the compiled HRTF is cached. That window is
silence rather than the plain bed, and mpv pauses the audio clock across the
switch, so no content is skipped. The plugin only repositions the
renderer when the requested byte offset differs from the current position, using
the component's seek policy (target frame minus 64 pre-roll frames, then the
output before the target is dropped).

A seek differs from a from-start render only by a very low noise floor (below
−65 dBFS in measurement); the position itself does not drift.

## Relationship to the fork approach

The established solution in this space takes the other path: `mpv-omniphony` is a
**fork of mpv** that adds a `--ad=orender` module, handing demuxed access units to
`liborender`; the codec decoding itself lives in dynamically loaded bridge DLLs
(`harletty_dolby_bridge.dll` and friends, E-AC-3 JOC included). That gives a native
track: no extra track, no startup switch, correct codec information.

This plugin deliberately stays on stock mpv (cplugins) and pays for it with a
0.3–0.6 s preparation window at startup and an external PCM track (shown as
pcm_f32le). When the native behaviour is required, the right move is an `ad_joc`
module in mpv, built the way mpv-omniphony does it: mpv's AD layer receives
AVPackets, so FFmpeg does not have to change and the renderer stays `joc_core.dll`.
A working prototype of that route — patch against mpv master (2026-10-17), new
sources and build instructions — lives in [`fork/`](fork/README.md); its
`--ad=joc` output was measured bit-identical to `joc_cli` for both binaural and
7.1. The cost is maintaining your own mpv build (MSYS2/MinGW on Windows).

## Verification

`tools/verify_parity.ps1` renders a reference WAV with `joc_cli`, fetches what the
plugin actually delivers, and compares the bytes. On this machine (mpv v0.41.0,
FFmpeg 8.0, a 30 s 7.1.4 JOC clip):

| Case | Result |
|---|---|
| binaural float32 (SOFA) | 1,440,768 samples bit-identical |
| binaural int24 (Rosella) | 1,440,768 samples bit-identical |
| speaker 7.1 float32 | 1,440,768 samples bit-identical |

The reference is longer only by the binaural room tail: the component and this
plugin cut the delivered stream at the file's duration, `joc_cli`'s
`--binaural-output` does not.

## Build

Needs a JustOneCacophonyCXX checkout (for its `joc_core` target):

```powershell
pwsh -File tools/build.ps1 -JocCoreSource C:\\path\\to\\JustOneCacophonyCXX
```

The plugin lands in `build\\Release\\joc_mpv.dll` with `joc_core.dll` beside it.
Plugin only, against an existing `joc_core.dll`:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`/fp:precise` and a static CRT are fixed: the floating-point behaviour is part of
the byte-exact acceptance.

## Troubleshooting

`joc_decoder.log` is written to mpv's configuration directory (next to
`mpv.conf`), together with `joc_ffmpeg_*.log`; it cannot live in `scripts\\`
because mpv would try to load it as a script. `--msg-level=all=info` shows the
`JOC: ...` status line.

* **No JOC message, just plain E-AC-3** — the file has no JOC metadata, or the
  container's audio track is not E-AC-3; mpv keeps playing the original track.
* **"cannot find the HRTF file"** — put `binaural.sofa` in `<mpv>\\HRTF\\` or
  point `joc-hrtf` at it.
* **Which track is in use?** — `joc-verbose=yes` and read the `probe:` / `start:`
  lines.

## License

MIT, see `LICENSE`. The renderer comes from JustOneCacophonyCXX and the decode
engine from the upstream project's frontend code; see `THIRD_PARTY_NOTICES.md`.
