# CD-i / CD-RTOS notes for static recompilation

These are facts worked out while porting *Link: The Faces of Evil*. The Green
Book (CD-i Full Functional Specification, May 1994) describes the APIs but
mostly omits numeric codes. The codes below were inferred from the game's
library wrappers and the parameter lists in the spec. An entry marked *(seen)*
was confirmed by actual use in that game.

## Disc and boot

* Sectors are raw 2352 bytes, Mode 2: sync (12), header (4), subheader
  (file, channel, submode, coding, ×2), then data. Form 1 carries 2048 bytes
  and Form 2 carries 2324 (audio uses 2304).
* The disc label is at LSN 16 (`"CD-I "` at data offset 1). The application
  identifier at label offset 574 names the boot module, for example `cdi_link`.
* Real-time files (`.rtr`, `.rtf`) have file number ≥ 1. Their directory size
  counts only that file's sectors, and padding sectors use file number 0.
  Empty sectors (submode with no V/A/D bit) can still carry EOR, trigger or EOF
  and **must be processed** for play-control purposes.

## OS-9 process start (what the runtime emulates)

* The kernel copies idata into static storage and applies irefs. The first
  table is code-relative (+module base) and the second data-relative (+data
  base).
* On entry, **a6 = static storage + 0x8000**, a5 = a7 = parameter area,
  a3 = module, d5 = parameter size and d6 = total memory.
* MPW-built programs (Link): idata begins with a jump table header
  `[size, a5 offset]` followed by `dc.w seg; jmp abs.l` stubs copied to
  `a5+0x20`. `jsr N(a5)` jumps to a stub in RAM, which costs one extra
  instruction. Count it, or cycle-exact comparisons with the interpreter
  drift. Function names are present as MacsBug symbols after each function.
* Microware C programs (the Philips "bumper"): a6-relative data and calls of
  the form `movea.l #off,aN / jsr d8(pc,aN.l)`.

## Kernel behaviour the games rely on

* **Signals** are delivered asynchronously. Games busy-wait on flags that
  their intercept handlers set, so translated code must yield at backward
  branches.
* **F$Sleep and F$Wait clear the signal mask.** The standard pattern is
  "mask, start an operation, sleep".
* While an intercept routine runs, signals are masked. F$RTE restores the
  context and unmasks.
* F$Alarm `A$Cycle` (d1=2) with time bit 31 set means the time is in 1/256 s.
  Link's 16 Hz game tick is `0x80000010`.
* F$Fork + F$Wait: a forked child (for example the bumper) runs to completion
  while the parent waits. Asynchronous signal sources must target the process
  that requested them.
* Common system call numbers: F$Link 00, F$Load 01, F$UnLink 02, F$Fork 03,
  F$Wait 04, F$Exit 06, F$Send 08, F$Icpt 09, F$Sleep 0A, F$ID 0C, F$Time 15,
  F$UnLoad 1D, F$RTE 1E, F$TLink 21, F$SetSys 27, F$SRqMem 28, F$SRtMem 29,
  F$Alarm 56, F$SigMask 57, F$SRqCMem 5C, I$Create 83, I$Open 84, I$Delete 87,
  I$Seek 88, I$Read 89, I$Write 8A, I$ReadLn 8B, I$WritLn 8C, I$GetStt 8D,
  I$SetStt 8E, I$Close 8F.
* Error codes: E$BMode CB, E$MemFul CF, E$UnkSvc D0, E$EOF D3, E$PNNF D8,
  E$MNF DD, E$DevBsy FA.

## Devices and the CSD

* Games read `/nvr/csd`. It holds text Device Status Descriptors of the form
  `type:name:params` ending in CR. Types:
  * 1 is the CD control unit (`/cd`);
  * 2 is the audio processor (`/ap`);
  * 3 is video (`/vid`, with `LI="525"` or `"625"`);
  * 4 is NVRAM (`/nvr`, `SZ#8`);
  * 5 is the pointer (`/ptr`, `CL="d"` for a manoeuvring device).
* GetStat 0x43 on `/nvr` returns free NVRAM bytes in d2 *(seen)*.

## CDFM (`/cd` files)

| SetStat | Code | Notes |
|---------|------|-------|
| SS_Play | 0x2D | a0 = PCB *(seen)* |
| SS_Abort | 0x31 | *(seen)* |
| SS_Seek | 0x30 | |
| SS_Enable / SS_Disable / SS_Cont / SS_Stop / SS_Pause | 0x39 / 0x38 / 0x37 / 0x36 / 0x33 | probable numbering |

* **PCB:** Stat.w, Sig.w, Rec.l, Chan.l, AChan.w, then CIL pointers for video,
  audio and data.
  * PCB_Sig is sent on EOR, trigger and finish. A **trigger** sector
    (submode bit 4) signals whatever its channel, even one the play does
    not select. Each hole's opening animation advances one picture per
    trigger: the triggers are on the video channel (14) while the play
    selects only channels 0 and 15. Signalling selected channels only
    left gaps of up to 90 fields between pictures until the stream
    ended and the game switched to its own timer.
  * Rec is decremented on each EOR, and the play ends when it reaches 0.
* **PCL:** Ctrl.b, res, SMode.b, Type.b, Sig.w, Nxt.l, Buf.l, BufSz.l (in
  sectors), Err.l, res.w, Cnt.l.
  * When the buffer fills, Ctrl bit 0 is set, the CIL entry advances to Nxt,
    and PCL_Sig is sent.
  * A full PCL that comes round again is an overrun. That ends the play with
    an error.
* Plays run at 75 sectors per second (1×). Audio sectors whose channel is in
  AChan go to the audio processor. Otherwise they go to the audio CIL.

## Audio (`/ap`)

* SS_SM = 0x3B. These are SetStat unless marked GetStat:
  * Creat 0 (coding, groups) returns the id and a0 = data;
  * Out 1 (id, a0 = asynchronous status block);
  * Off 2;
  * Close 3;
  * Info 0 is GetStat and returns a0 = descriptor.
* **SM_Out on a busy processor queues the new soundmap** to start when the
  current sector finishes. Games double-buffer music this way and pace
  themselves on the completion signal (ASY_Sig), so completions must be
  sample-exact.
* SS_SC = 0x3D, SC_Atten d3: bytes 2 and 0 are the direct paths (L→L, R→R),
  bytes 3 and 1 the cross paths. 0xFF is off.
* ADPCM coding byte:
  * bits 0-1 select stereo (01 = stereo);
  * bits 2-3 select the rate (00 = 37.8 kHz, 01 = 18.9 kHz);
  * bits 4-5 select the sample size (00 = 4-bit, 01 = 8-bit).

  A sound group is 128 bytes: 16 parameter bytes (copies at 4..11) followed by
  112 data bytes. The filter coefficients are the XA ones.

## Video (`/vid`)

* SS_DC = 0x56 (d2 = function):

  | Code | Function | Code | Function |
  |------|----------|------|----------|
  | 0 | CrFCT | 0xA | WrLI |
  | 1 | RdFCT | 0xB | DlLCT |
  | 2 | WrFCT | 0xC | FLnk |
  | 3 | RdFI | 0xD | LLnk |
  | 4 | WrFI | 0xE | Exec |
  | 5 | DlFCT | 0x11 | SSig |
  | 6 | CrLCT | 0x12 | Relea |
  | 7 | RdLCT | 0x1B | PRdLCT |
  | 8 | WrLCT | 0x1C | PWrLCT |
  | 9 | RdLI | | |

  Seen: 0, 2, 4, 6, 7, 0xA, 0xC, 0xD, 0xE, 0x11 and 0x1C. 0xF and 0x13 are
  probably Intl and SetCmp; they're ignored.
* LCT line numbers are in UCM coordinates: normal-resolution tables hold half
  the lines. "P" variants use physical lines.
* SS_GC = 0x52: Pos 0, Show 1, Hide 2, Ptn 3, Col 4, Org 5, Blnk 6.
  Coordinates are high resolution (768×480).
* SS_PT = 0x59: Coord 0 (GetStat) returns d0 = buttons (bit 15 = active) and
  d1 = H:V. SSig 1, Relea 2, **Pos 3, Org 4** (note this order).
* Video interrupts come only from DCP instruction `$60`. DC_SSig counts them.
* DCP opcodes:

  | Opcode | Meaning |
  |--------|---------|
  | 10 | NOP |
  | 20 | link (driver-internal) |
  | 40 | display line start |
  | 60 | interrupt |
  | 78 | display parameters (RMS bits 0-1, 10 = run-length) |
  | 80-BF | CLUT entry within the current bank |
  | C0 | coding: CM0 bits 0-3, CM1 bits 8-11, NM bit 19, CS bit 22 |
  | C1 | transparency: TA bits 0-3, TB bits 8-11, bit 23 = no mix |
  | C2 | plane order |
  | C3 | bank |
  | C4 / C6 | transparent colour |
  | C7 / C9 | mask colour |
  | CA / CB | DYUV start values |
  | D0-D7 | mattes |
  | D8 | backdrop |
  | D9 / DA | mosaic |
  | DB / DC | ICF (0-63) |

* RL7: a byte with bit 7 set starts a run of colour b&0x7F whose length is the
  next byte (0 = to the end of the line). Otherwise the byte is a single pixel.
* DYUV deltas: 0,1,4,9,16,27,44,79,128,177,212,229,240,247,252,255.
* Colours are in studio range (black 16, white 235).
* Link draws sprites with **compiled sprite routines loaded from disc into
  RAM**. They run in the interpreter.

## Timing model used by the runtime

* Emulated time counts executed 68k instructions (`CPU_IPS` = 4M per second).
* Video fields (60 Hz), CD sectors (75 Hz), alarms and soundmap completions are
  scheduled on that clock. Pacing to the wall clock happens once per field.
* Translated code yields only at backward branches (`BUDGET`), and the
  interpreter yields by the same rule. That keeps `--interp` runs comparable
  frame for frame.

## Additions from *The Wacky World of Miniature Golf*

This title (Sidewalk Studio, 1993) was built with Microware C and the Balboa
CD-I runtime library. It needed the following on top of the first port.

### Microware C programs

* Calls between library and program go through an **a6 jump table**: the
  initialized data holds `jmp abs.l` stubs, relocated by the code-relative
  irefs, and code calls them with `jsr d16(a6)` (a6 = static storage + 0x8000).
  The analyzer reads the stubs from idata and resolves these calls
  (`a6calls`). Generated code calls the target directly while a6 has the main
  program's value and counts the stub as one executed instruction, like the
  interpreter.
* Other code-relative irefs are function-pointer tables (event handlers,
  state machines). They are accepted as entry points when they start with a
  prologue (`link An,#-n` with n ≥ 0 and even, or `movem.l ...,-(sp)`); the
  rest point at string constants in the code area.
* No MacsBug symbols: functions are named `sub_XXXXXX` in backtraces.
* `trap #n` (n ≠ 0) is followed by a function word, as for OS-9 trap
  libraries. This game has one, unreached in normal play (`trap #5`, func 8).

### Kernel

* **F$Event ($53)**, function in d1: Link 0, UnLink 1, Creat 2 (d0 = initial
  value, d2.w = wait increment, d3.w = signal increment, a0 = name), Delete 3,
  Wait 4 / WaitR 5 (d2..d3 = range), Read 6, Info 7, Signal 8, Pulse 9,
  Set 10, SetR 11 *(seen: 0, 2, 4, 6, 8)*. **Ev$Wait returns the value that
  satisfied the wait, before the wait increment is added**: the Balboa
  dispatcher loops on "returned 0".
  A signal arriving during Ev$Wait runs its handler, then the wait is
  executed again (the analyzer makes every `os9 $53` a resumable entry).
* **F$DatMod ($25)** creates a data module (the game makes `cdi_DbgDCP`).
* **F$Julian ($20)**: d0 = 00hhmmss, d1 = yyyymmdd → seconds since midnight,
  Julian day number.
* A file can hold several modules; they are **word aligned** (not long).
  `cdi_putt.config` holds 7 data modules and the `cdi_bpsys` subroutine
  module.
* **System-state F$Alarm**: made from driver code (the module swaps
  `D_Proc` (76) to `D_SysPrc` (80) in the system globals first), it runs a
  routine instead of sending a signal. a0 points to a register image:
  R$pc (0x42) is the routine, R$sr (0x40) its status register, other
  registers as given; the routine returns with `rts`. Treating it as a
  user alarm sends signal 0 (S$Kill) to the game, which kills the
  Philips bumper animation.

### SS_SLink: driver extension modules

* I$SetStt **0x5A** on `/vid` with a0 = module name links a subroutine
  module into the video driver. The module's exec offset points to four word
  offsets (relative to that table): init, setstat, getstat, terminate.
* Driver calling convention as used by the module: a1 = path descriptor,
  a2 = driver static storage, a5 = caller's register image
  (R$d0..R$a6 at 0..0x3b), a6 = system globals. The original driver
  routines are at 70(a2) (SetStat) and 74(a2) (GetStat); they take
  d1 = code, d2.. = parameters, return carry/d1 on error, and return
  results **both in the registers and in the register image**.
* `cdi_bpsys` (Balboa) handles SetStat calls whose d2 is 0x4200..0x4212
  (even): video environments (sets of FCT/LCT, cursor, pointer clip and
  callbacks into the game). 0x4212 returns the version (0x1323). Its
  system alarm polls SS_PT Coord, clips, moves the cursor (SS_GC Pos,
  SS_PT Pos) and calls the game back on pointer events.
* The runtime runs the module in the interpreter (`runtime/drvext.c`).

### CDFM

* **SS_Abort** ends the play with PCB_Stat bit 15 set and **$060A** written
  to PCB_Sig, then sends the original PCB_Sig. The game checks for exactly
  that code to tell a deliberate abort (intro skipped) from an error. The
  signal is delivered as the SetStat returns, so the game's abort handler
  runs before the code after the call. Give Up depends on it: it aborts the
  hole's stream, registers an "after the clip" callback and starts its voice
  clip. If the handler has not yet cleared the game's "stream playing" flag
  (a6-31274), the clip routine defers itself through the stop routine
  (0x103160), which clears that callback, and the game waits forever after
  the clip.
* SS_Play while a play is active replaces it (refusing it with E$DevBsy
  breaks the game).
* **SS_Play waits for the seek**: no seek when the play continues where
  the head is, otherwise about 11 sector periods plus a share of the
  distance (up to 38, about 0.5 s). The game frees a screen's data buffers
  when it aborts its stream, while that screen's hotspot objects (built in
  place in those buffers) are still linked. Without seek time the next load
  overwrites them at once and the pointer routine reads garbage.

### Timing the game depends on

* **CPU speed.** The game only works at about the speed of the real 68070
  (15.5 MHz with wait states). The runtime uses 1.6 million instructions
  per second (`g_cpu_ips` in `cpu.c`, `--cpu-ips`). Below about 1.5 MIPS the
  game cannot decode a picture within a field and 10 fps animation judders
  (5/7 fields); at 2 MIPS and above skipping the intro can leave a black
  screen. At the template's 4 MIPS, two display
  swaps could happen in one field, and screen changes left stale parts
  behind (the hole's bottom bar over the clubhouse) or went black.
* **DC_SSig requests queue up.** A second request before the first has
  fired must not replace it. Dropping one leaves the game waiting for a
  signal that never comes.
* A call to an impossible address (top byte set, odd, or below $400) is
  skipped and logged instead of being fatal (`m68k_call`, interpreter
  `jsr`). This works around a race in the game's pointer handling that
  real hardware timing usually wins.

## Snapshots, run-ahead and look-ahead (runtime design)

* A snapshot can only be taken where no 68000 code is in progress on the
  native C stack: the outermost run loop, between dispatches, outside driver
  routines (drvext) and child processes. Translated code already unwinds to
  that loop at every yield.
* Blocking calls must not wait inside the call. F$Sleep and Ev$Wait advance
  time by one scheduler event and return to their own `os9` instruction
  (which the analyzer makes an entry point), remembering their state in the
  process (`block_pc`, deadline or range). So every field boundary is
  reached at the safe point.
* Everything the machine depends on is in the snapshot: CPU, RAM up to the
  heap's high-water mark, kernel (allocator, processes, alarms, scheduler,
  paths, modules, events), NVRAM files (kept in memory and written to disk
  only from the real timeline), CDFM, audio (including decoded sound maps
  and the disc-audio ring) and video (FCT/LCT contents, registers, cursor,
  pointer).
* Runs stay deterministic: run-ahead and motion smoothing leave the recorded
  audio bit-identical, run-ahead frame k equals plain frame k + N, and a
  quickload replays the same frames as the run that made the save.
