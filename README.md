# The Wacky World of Miniature Golf with Eugene Levy: static recompilation

A native Windows build of the Philips CD-i game *The Wacky World of Miniature
Golf with Eugene Levy* (Sidewalk Studio / Philips Interactive Media, 1993).
The game's 68000 program (`cdi_putt`) is translated ahead of time into C. A
runtime provides the parts of the CD-i the code expects: the CD-RTOS (OS-9)
kernel, the CD file manager, the video decoder (with a Direct3D 11 presenter
at any resolution) and the ADPCM audio processor.

It is based on the CD-i static recompilation template from the *Link: The
Faces of Evil* project. A full-interpreter run (`--interp`) of the same
session gives bit-identical frames and audio.

## Installing (players)

Releases contain no game data and no prebuilt game: the game is compiled on
your PC from your own disc image. You need the disc image of the game
(`.cue` + `.bin`, raw 2352-byte sectors). Building needs Python 3, CMake and
the Visual Studio 2022 Build Tools (C++); Setup offers to install any that
are missing with `winget` (the Build Tools are a few GB and ask for
administrator rights).

From the [releases page](https://github.com/tools81/wackygolf-recomp/releases),
download one of:

* **`wackygolf-recomp-<version>-setup.exe`**: the installer. It installs for
  the current user only (no administrator rights), asks for the disc image,
  builds the game and adds Start-menu (and optionally desktop) shortcuts.
* **`wackygolf-recomp-<version>-portable.zip`**: unzip anywhere (a USB stick
  works), run `Setup.cmd` once to choose the disc image and build, then start
  the game with `Play.cmd`.

Everything stays in the game's folder: saves, quicksaves, settings
(`saves\settings.ini`) and the log in `saves\`, and the disc image path in
`rom.cfg` (`rom=<path to the .cue>`). Nothing is written to AppData or the
registry, apart from the installer's own uninstall entry and shortcuts. The
shaders are compiled into the program. No command-line switch is needed for
this.

**Updates.** The shortcut and `Play.cmd` start the game through
`scripts\launch.ps1`. It asks GitHub for the latest release; if it is newer
than the installed `VERSION`, it downloads the release, copies it over the
folder (keeping `saves\`, `rom.cfg`, `rom\` and `extracted\`), rebuilds the game
from the disc and starts it. If you are offline or the update fails, the
installed version starts. Set `WACKYGOLF_NO_UPDATE=1` (or run
`launch.ps1 -NoUpdate`) to skip the check.

`wackygolf.exe --version` prints the version.

**Making a release** (maintainers): bump `VERSION`, commit, then run
`powershell -File installer\make-release.ps1` to build both packages in
`dist\` from the committed files, tag `v<version>` and upload them to a GitHub
release. The updater looks for the `*-portable.zip` asset.

## Building

Requirements: Windows 10+, Visual Studio 2022 (C compiler), CMake 3.19+ and
Python 3.8+. No third-party libraries are needed.

1. Put your disc image (`.bin` + `.cue`, raw 2352-byte sectors) in `rom/`,
   or set `rom=<path to the .cue>` in `rom.cfg`, or set `RECOMP_ROM`.
   The game data is not included.
2. Run `build.bat`. It extracts the program from the disc, recompiles it into
   `gen/`, builds with MSVC and copies `wackygolf.exe` to the project root.
3. Run `wackygolf.exe`. It finds the disc image through `rom.cfg` or in
   `rom/` next to the program, or you can pass the path to the `.cue` or
   `.bin`.

Saved data (the emulated NVRAM files) and the log go to
`saves/` next to the program.

## Controls

The game is played with the CD-i pointer.

| Input | CD-i |
|-------|------|
| Mouse | pointer position |
| Left mouse button, Z, Space, A | button 1 |
| Right mouse button, X, Ctrl, S | button 2 |
| Arrow keys (Shift = faster), gamepad stick or d-pad | move the pointer |
| Gamepad A / B | button 1 / button 2 |
| Enter, C, gamepad X or Start | both buttons |
| Tab (hold) | fast forward |
| F2 | quicksave to the current slot |
| F3 | next slot (1-10) |
| F4 | load the current slot |
| F6 | run-ahead: 0, 1, 2, 3 fields |
| F7 | motion smoothing on/off (default off) |
| F8 | frame pacing on/off (default on) |
| F9 | aspect: 4:3 with blurred sides, 4:3, stretch, zoom |
| F10 | filter: sharp, smooth, CRT, nearest, Cartoon HD, Cartoon HD+ |
| F11 or Alt+Enter | full screen |
| Esc | quit |

Clicking during the intro skips it.

## Enhancements

* **Cartoon HD and Cartoon HD+ filters (F10).** An edge-directed upscaler
  for the game's flat-shaded cartoon art. A prepass measures edge direction
  at source resolution. The output pass smooths along edges and keeps them
  sharp across, so stair-stepped outlines become clean, anti-aliased curves
  at any resolution. HD+ is crisper, smooths flat areas more, darkens ink
  lines slightly and adds a little colour. Both need Direct3D (not `--gdi`).
* **Run-ahead (F6, default 1).** After each real field the machine state is
  saved, the next fields are emulated with the current input, the last one
  is shown, and the state is restored. The picture is 1-3 fields (17-50 ms)
  ahead of the machine, so reactions to input appear that much sooner.
  Audio, saves and NVRAM come from the real timeline only.
* **Quicksave (F2 save, F3 slot, F4 load).** 10 slots, kept in memory and in
  `saves/quicksave_<n>.state`, so they survive restarts. A slot only loads
  in the build that made it. Loading also restores the game's NVRAM (its saved
  data) to what it was at that point.
* **Smooth presentation.** The game is synced to the display when its
  refresh rate is a multiple of 60 Hz (60, 120, 240), so every frame is
  shown for the same number of refreshes. On other displays a timer paces
  it (`--no-vsync` forces the timer). The audio queue absorbs the small
  difference between the display's refresh and 60 Hz.
* **Frame pacing (F8, default on).** The disc delivers a picture every 7.5
  sectors and the game's decoding time varies, so a 10 fps animation lands
  5 and 7 fields apart instead of 6 and 6, a visible judder. Pacing holds a
  picture that arrives earlier than the recent rhythm for a field or two,
  so the intervals even out. It changes only when pictures are shown, not
  the game.
* **60 fps pointer and motion smoothing (F7).** The game moves its cursor
  sprite only about 33 times a second. It is now drawn every field at the
  newest pointer position, without delay. Most pictures are cartoon
  animation streamed from the disc at 10-30 frames a second. Motion
  smoothing finds the next picture by running ahead, so it adds no delay, and
  draws motion-compensated in-betweens at 60 fps where the change is
  movement. Drawn animation (new poses), zooms and cuts are left as they are,
  because interpolating them only produces smears. Expect smoother pans,
  slides and small movements, not hand-drawn animation turned into 60 fps.
  It roughly doubles the CPU time of the emulator, and the look-ahead makes
  about one frame in a hundred take longer than a refresh on this machine,
  so it is off by default.

## What this game needed

On top of the template (see `docs/CDI_NOTES.md` for details):

* **Microware C code discovery.** Calls go through `jmp abs.l` stubs in the
  initialized data (`jsr d16(a6)`), and function pointers sit in relocated
  data tables. `tools/analyze.py` reads both from the module's idata and irefs.
  This took coverage from 22 KB to about 118 KB of the 143 KB code area.
* **OS-9 events (F$Event)**, used by the Balboa runtime's message
  dispatcher, plus F$DatMod and F$Julian.
* **The `cdi_bpsys` driver extension.** The game links this Balboa module
  into the video driver with SS_SLink. The runtime runs the module's own
  68000 code in a driver context (`runtime/drvext.c`), with its calls to the
  driver routed to the HLE video and pointer code. This includes the
  system-state alarm that tracks the pointer and moves the cursor.
* **CDFM details:** SS_Abort reports the abort with the status and code the
  game checks for, which makes skipping the intro work.
* **Mouse input** in the host, mapped to the CD-i pointer through the current
  aspect mode.

## Status

* The whole attract sequence plays: the Philips bumper, the Sidewalk Studio
  logo, the titles, the sketchbook intro, Rollo, the clubhouse menu and the
  demo. Clicking skips the intro.
* The clubhouse menu, Sign In and the holes respond to the pointer. Audio
  (CD streams and sound maps) plays.
* Recompiled and fully interpreted runs give identical frames and audio.
* Returning from a hole could leave the hole's bottom bar over the clubhouse
  (or over Sign In), and clicking (for example to swing) could crash with
  "CPU exception 4 at 4f100000". Both came from timing that was faster than
  a real CD-i (see the timing notes below), and both are fixed. A last-resort
  guard also skips calls to impossible addresses, which the game can make
  in a rare race of its own; this is logged as "ignored call to invalid
  address". Random-input runs of 15 minutes and longer no longer crash.

## Configuration (game.json)

```json
{
  "title": "The Wacky World of Miniature Golf with Eugene Levy",
  "exe_name": "wackygolf",
  "modules": [
    {"file": "auto", "base": "0x100000", "extra_entries": [], "a5_table": "auto"}
  ]
}
```

* The first module is the program the player boots (`"auto"` = the disc
  label's application, `cdi_putt`).
* Other program modules the game loads would be listed here with a fixed
  `base` below `0x140000`. `cdi_bpsys` is not listed: it is a small driver
  extension that runs in the interpreter.
* `extra_entries` are code addresses the analyzer cannot find (absolute
  hex). None are needed for this game.
* `a5_table`: `"auto"` detects MPW-style programs. This game is Microware C
  (a6 tables are detected automatically).

To see what the analyzer found, run `python tools/analyze.py`. It writes a
disassembly to `build/listing_cdi_putt.asm`, with each a6 jump-table call
annotated with its target.

## Layout

| Path | Contents |
|------|----------|
| `tools/cdi_extract.py` | Green Book filesystem reader. Writes program files and `_app.txt` (`--all` also dumps the real-time streams). |
| `tools/m68k.py` | MC68000 decoder |
| `tools/os9mod.py` | OS-9 module header, idata and irefs |
| `tools/analyze.py` | Code discovery (recursive descent plus the patterns above) |
| `tools/gen.py` | 68000→C generator, flag liveness, dispatch tables, `gen/game_config.h` |
| `tools/config.py` | `game.json` loader |
| `tools/gen_tests.py` | Per-instruction differential tests |
| `tools/bmp2png.py` | Converts frame dumps to PNG |
| `tools/contact.py` | Tiles frame dumps into one contact-sheet PNG |
| `tools/wavlevels.py` | Prints per-second audio levels of a recorded WAV |
| `runtime/cpu.c` | Dispatcher and run loop |
| `runtime/interp.c` | 68000 interpreter, for RAM code and the `--interp` reference mode |
| `runtime/alu.c` | Shifts, division and BCD with exact flags |
| `runtime/os9.c` | OS-9 kernel: memory, processes (fork/wait/exit), signals, alarms (user and system state), events, data modules, paths, NVRAM, CSD |
| `runtime/state.c` | Snapshots: one function per module both saves and loads its state |
| `runtime/frame.c` | Frame loop: presentation, run-ahead, look-ahead, quicksave slots, on-screen messages |
| `runtime/motion.c` | Motion smoothing (motion estimation and interpolation) |
| `runtime/drvext.c` | Driver extension modules linked with SS_SLink (`cdi_bpsys`), run in the interpreter |
| `runtime/cdfm.c` | CD file manager: reads, and real-time SS_Play with PCB/PCL/CIL |
| `runtime/video.c` | UCM display control (FCT/LCT), MCD212-style renderer, cursor, pointer |
| `runtime/audio.c` | ADPCM levels A/B/C, sound maps with gapless SM_Out, attenuation |
| `runtime/render_d3d11.c` | GPU presenter: aspect modes and sharp/smooth/CRT filters |
| `runtime/host_win32.c` | Window, input (keyboard and XInput), waveOut, pacing, scripted and fuzz input |
| `test/` | Instruction differential tester and ALU reference vectors |
| `VERSION` | Release version (compiled into the program, compared by the updater) |
| `Setup.cmd`, `Play.cmd` | Portable package: choose the disc and build; start the game |
| `scripts/` | `setup.ps1` (portable setup), `build-game.ps1` (checks/installs build tools, builds), `launch.ps1` (update check, then start), `common.ps1` |
| `installer/` | `wackygolf.iss` (Inno Setup) and `make-release.ps1` (builds the zip and the installer from committed files) |
| `docs/CDI_NOTES.md` | CD-i facts and API codes worked out during the first port |

## Debugging workflow

* **Find what fails.** Unimplemented OS calls and SetStat/GetStat codes are
  logged to `saves/recomp.log`. Fatal errors also show a 68k register dump and a
  stack backtrace with function names.
* **Debug options:**
  * `--trace` / `--trace3` log syscalls with callers, signals and disc activity.
  * `--headless --frames N --dump K` runs fast without a window and writes every
    Kth frame as a BMP.
  * `--script "field:key:dur,..."` scripts input (keys `1 2 L R U D`).
  * `--fuzz SEED` drives the game with random input (set
    `RECOMP_FUZZ_CALM=1` for a slower, more human pace).
  * `--wav out.wav` records the audio.
  * `--runahead N`, `--motion` / `--no-motion`, `--pacing` / `--no-pacing`,
    `--no-vsync`, `--cpu-ips N` and `--no-cursor-smooth`
    override the saved settings. Headless runs default to no run-ahead and no
    motion smoothing, so their frames are the real ones.
  * Script keys `S` and `O` (for example `--script "3000:S:1,3600:O:1"`)
    quicksave and quickload.
  * `--dump-from N` starts frame dumps at field N.
  * Environment variables `RECOMP_CADENCE=1` (fields where the shown picture
    changes), `RECOMP_FRAMETIME=1` (work per displayed frame) and
    `RECOMP_PRESENTLOG=1` (intervals between presented frames) log timing.
  * `RECOMP_FUZZ_NOEXIT=1` ignores clicks on the clubhouse Exit door, so
    `--fuzz` sessions keep exploring instead of quitting.
  * `--capture FRAME WxH FILTERS PREFIX` (windowed) renders field FRAME
    offscreen at WxH with each listed filter (for example `045`) to
    `PREFIX_f<n>.bmp`. `tools/crop.py` puts the same crop of several images
    side by side.
  * Environment variable `RECOMP_RAMDUMP=file` writes the 16 MB memory
    image when a `--frames` run ends.
* **Check the translation.** Run the same headless session twice, once with
  `--interp`, which executes everything in the interpreter, and compare the
  frame dumps and WAVs. Headless runs are deterministic: emulated time is
  counted in instructions, and the clock uses a fixed epoch. Any difference
  points to a translation bug.
* **Unit-level checks.** `python tools/gen_tests.py` and
  `python test/gen_alu_vectors.py` generate test sources. Then build the
  `insn_test` and `alu_test` targets:

  ```
  cmake --build build\cmake --config Release --target insn_test alu_test
  build\cmake\Release\insn_test.exe rom\<image>.bin 200
  build\cmake\Release\alu_test.exe
  ```

* Code that jumps into RAM is not an error. It runs in the interpreter. Jumps to
  *undiscovered* addresses inside a recompiled module are fatal. Add those
  addresses to `extra_entries`, or teach `analyze.py` the pattern.

## Things a new game may need

* **More CD-RTOS calls.** The runtime implements what the first game used. See
  `docs/CDI_NOTES.md` for what's known and what isn't. The CD-i Full Functional
  Specification (Green Book, May 1994) documents the calls but gives no numeric
  function codes. Infer those from the game's library wrappers: look at the
  `d2` values passed with `os9 I$SetStt`.
* **UCM drawing functions** (DM_/DP_/DR_/RG_ and text). These aren't implemented
  yet because the first game drew everything itself.
* **PAL games.** The CSD in `os9.c` (`write_csd`) advertises a 525-line NTSC
  player and the video runs 240 lines at 60 Hz. A 625-line title needs
  `LI="625"`, 280 lines (`SCREEN_LINES` in `video.c`) and `FIELD_RATE` 50.
* **Full Motion Video** (MPEG cartridge) isn't supported.
