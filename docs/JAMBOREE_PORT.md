# Super Mario Party Jamboree (Switch 1, v1.1.0, title 0100965017338000): PC port notes

Working log and spec for the static recompilation of Jamboree with Switch2Recomp 0.4.1 (MZ Labs).
The runtime source in `runtime/` is patched here; every change is in git (`git log` in this folder).

## 1. Status

| Area | State |
| --- | --- |
| Boot, loading screen | works |
| White screen freeze after the loading screen | **fixed** (bug 1) |
| Intro movie (bq_op.mp4, H.264 via NVDEC → OpenH264) | works, 60 FPS, ~5 min, unskippable on first boot (same as console) |
| Title screen ("Press L+R") | renders, music plays (SM_BGM_TITLE) |
| Title screen load | **fixed** (bug 3): no stall, balloon characters no longer black |
| Pressing L+R at the title | **fixed** (bug 4): save is created, player-count screen, Party Plaza hub |
| Saves | `bqSaveData`, `bqSaveData2` written to `<name>_data/save/user/`; intro skippable (+) once a save exists |
| Menus | player count, character select (3D preview), Party Plaza hub (walk, talk, Toad's tutorial), balloon tour and mode list (`menu01.nro`), party rules, opponent/board setup |
| Boards | Mega Wiggler's Tree Party (`bd01.nro`) loads and plays: intro, dice/items/board menu, HUD, CPU turns, 55-60 FPS |
| Minigames | not reached yet |
| Load hitches | ~6 s frozen image when `menu00.nro` and `bd01.nro` load (CPU side: GL thread working through a ~26 MB batch, probably ASTC decoding on the CPU) |
| To check | mode list screen has a flat gray backdrop (may be a missing blurred background) |
| Reading the user's dump in place | works (`game_dump=`), nothing copied |
| Input | keyboard via window messages, XInput controllers supported by the runtime |
| Audio | audren mixer runs (DSP-ADPCM voices, BFSTM streams); not checked by ear |

Fastest path to a board with a save (keys): + skips the intro, Q+E at the title, A through the plaza, then in the
balloon pick Mario Party; ~2.5 min from launch. With `_work/run.ps1`: `-Keys` = `12:0x0D@500,22:0x51+0x45@500,
26:0x51+0x45@500`, then `0x20` (A) every 3 s from 34 to 100 s, `104:0x28,108:0x5A,114:0x5A`, then A every 4 s.

Keyboard (runtime defaults): arrows = D-pad, WASD = left stick, X/Space = A, Z = B, C = X, V = Y, Q = L, E = R,
1 = ZL, 3 = ZR, Enter = +, Backspace = -. The title needs L+R = **Q+E together**.

## 2. Bugs

### Bug 1: white screen after the loading screen (fixed)
- Symptom: loading screen, then white window forever. Log: `6 s without a new image`, GL thread stuck in
  `glCompressedTexSubImage*`, compile thread stuck "loading a cached shader".
- Evidence: an external stack dump (`_work/stackdump.c`) showed one NVIDIA driver thread deep in recursion (its GLSL
  compiler) while every other GL thread waited on the driver's global SRW lock. Compiling each dumped shader alone
  (`_work/glcompile.c`) found two fragment shaders (`5e9b4b7872bf24a6`, `dccc83b42d9056a8`) that never finish (> 20 s).
- Cause: they use an indirect branch (`BRX`, a switch jump table). The translator finds the table through the `LDC`
  that loads it, but looked only 8 instructions back. Jamboree loads it ~30 instructions earlier, so the translator
  made every instruction a `case` of a `while(true){switch(pc)}` loop (1211 cases), which NVIDIA can't structure.
- Fix (`runtime/shader.c`, `brx_targets2`): search up to 64 instructions back, stop at another write of the
  register or at control flow. 1211 → 23 cases, compiles in ms, the intro plays.

### Bug 2: dropped bindless texture fetch in the board vertex shader `e79ac5262cbfd67d` (fixed)
- `TEX.B (untracked handle)`: the handle comes from `IADD r19 = RZ + c[11][0x140]`; the tracker only followed RZ.
- Fix (`runtime/shader.c`, `track_src`): `IADD`/`LOP` (cbuf form) with RZ and no negate/invert is the cbuf value.
  Untranslated instruction count in the log went from 2 to 0.

### Bug 3: ~70 s freeze when the title screen loads, shader `1f72046c692943ee` (fixed)
- Same shape as bug 1 (1163 switch cases; NVIDIA works ~70 s then fails with "too many instructions", so that
  effect is also missing). Its three `BRX` all have their `LDC` close by.
- First cause found: the "another write of the register" check read bits 0-7 as a destination on every opcode. On
  `ISETP/FSETP` those bits are two predicates (P1 + PT reads as r15, the table register). Fixed.
- Second cause (from the new `shader: BRX at N: ...` log line): a predicated `BRA` between the `LDC` and the `BRX`
  stopped the search. The switch's `LDC` dominates its `BRX`, so only another write of the register stops it now.
- Result: no stall, no compile failure, and the title's balloon characters (drawn with that shader, black before)
  render correctly.

### Bug 4: pressing L+R at the title aborts the game (fixed)
- Log: `svcBreak` from `nn::diag::detail::AbortImpl(Result)` called by
  `bq::flag::GameFlagModule::On(GameFlag, bool, PlayerID)` at `0x81e0ccc`, because
  `bq::save::SaveDataMgr::FindPreselectedUserSaveData()` returned null (slot flag at `+0xE6C0` unset): the game never
  mounted a save (no `EnsureSaveData`, no fsp-srv 51).
- Chain, found with the new `SWR_TRACE_DYN` (below): `boot.nro` calls `bq::AccountMgrModule::EntryAppBootAccount`,
  which takes the uid stored at `nn::bezel::AccountModule` impl `+0` and asks the save manager to load it
  (`0x8245e90`). That needs `AccountModule::IsAccountValid(uid)`; the stored uid was **zero**, so it failed, the slot
  flag was cleared and nothing was mounted.
- Why zero: `AccountModule` init (`0x878e3e0`) first calls `nn::account::PopOpenUsers` (acc:u0 131
  `ListOpenContextStoredUsers`). Only if that returns nobody does it call `OpenPreselectedUser` and store that uid.
  The runtime answered 131 with the user, which on a console only happens when another application stored an open
  user before jumping to this one. The game took the "relaunched" path and never stored a boot account.
- Fix (`runtime/svc_core.c`): cmd 131 returns an all-zero list, as on a normal launch. `SWR_OPEN_CONTEXT_USER=1`
  brings the old answer back in case another game needs it.
- Result: `EnsureSaveData` + `OpenSaveDataFileSystem` at boot, `bqSaveData`/`bqSaveData2` written to
  `<name>_data/save/user/`. L+R loads `menu00.nro`: "How many people will play on this system?", then the Party
  Plaza hub with Mario walking. With a save, the intro shows "−/+ Skip" and + skips it.

### Bug 5: ~40 s freeze when boarding the Party Plaza balloon (fixed)
- `menu01.nro` loads, then no new image for ~40 s; GL thread inside the driver, compile thread on one shader.
- Log: `shader: BRX at 389: no LDC of r12 found before it` and `BRX at 328: no LDC of r9 ...`. With
  `SWR_BRX_DUMP=1` (lists the 64 instructions before such a BRX) neither register is written in those 64: the table
  loads are even further back than on the boards.
- Fix (`runtime/shader.c`): the search runs back to the start of the program (it still stops at another write of the
  register or at BRX/CAL/RET/JMP). No BRX warnings left, no freeze; the balloon tour plays straight through.

### Build fixes (the source as shipped didn't build with the bundled GCC 16)
- `plat.c`: missing `#include <math.h>`; `svc_core.c`: `ipc_warn_once` used before its declaration.
- `plat.c`: prebuilt `vp9/win/libvpx.a` calls `_setjmp`, which the UCRT import library no longer exports; a jump shim
  to `__intrinsic_setjmp`.
- The GCC from `Install.bat` has no C++ compiler (the runtime needs it for `bcn/*.cpp`): `_work/get_gxx.py` adds
  conda-forge `gxx_impl_win-64 16.2.0` (sha256-checked) to `gcc/`. This belongs in `tool/get_deps.py`.
- The rebuilt runtime is ~5 MB; the shipped one was ~20 MB because it also had the optional `renderer=vulkan/eden`
  libraries, whose prebuilt archives aren't in this package. `renderer=opengl` (default) is what's used.

## 3. Features added

- `game_dump=` (new `[Game]` section in the game's `.ini`): exefs and romfs are read from the user's own extracted
  dump where it is. Saves, `sdmc` and fonts stay in `<name>_data/`. If neither `game_dump` nor `<name>_data/` has
  `exefs/main`, a folder picker asks for the dump (3 tries, then a clear error). The dump's `main.npdm` program id
  must match the recompiled code's title id.
- `switchrecomp.py --link`: recompile without copying exefs/romfs (5.4 GB), writes `game_dump=` into the `.ini`.

## 4. Diagnostic switches added

| Env var | Effect |
| --- | --- |
| `SWR_DRAW_SYNC=1` (with `SWR_NO_GLQ=1`) | `glFinish` after every draw, logs each draw and the slow ones |
| `SWR_CMP_LOG=1` | logs each shader program compile start/finish with its time |
| `SWR_IPC_LOG=1` | logs every IPC call and result except hid/audio/nvdrv |
| `SWR_TRACE_DYN=lo-hi,clo-chi` (hex) | logs each function in `[lo,hi)` the first time it's called through the dispatch table from a return address in `[clo,chi)` (calls between modules, e.g. `boot.nro` → main, main → sdk) |
| `SWR_OPEN_CONTEXT_USER=1` | acc:u0 131 answers with the user again (pre-bug-4 behaviour) |
| `SWR_BRX_DUMP=1` | with `no LDC found`, lists the 64 instructions before the BRX |
| (always) `shader: BRX at ...` | why a jump table couldn't be resolved |

Tools in `_work/`: `run.ps1` (launch, screenshots through the runtime's F8 capture, keys posted to the focused game
window with `Q+E` chords and `vk@ms` holds, kill; the runtime ignores keys while its window isn't focused, and a
desktop capture of the GL window comes out black), `fn.sh` (print the generated C of one function), `dynsym.py`
(symbolize `SWR_TRACE_DYN` output), `build.py`
(rebuild the runtime, ~30 s), `stackdump.c` + `resolve.py` (every host thread's stack, symbolized), `glcompile.c`
(compile one GLSL file on the real driver and time it), `syms.py` (search the dump's symbol tables), `thr.py`
(symbolize `threads.txt`), `get_gxx.py`.

## 5. What the finished product is

A Windows build that plays Jamboree from the user's own dump, without shipping anything of Nintendo's.

What the user does:
1. Dumps their own copy of the game and the 1.1.0 update on their own Switch, extracts `exefs/` and `romfs/` with
   their own keys (hactool), as in `README.txt`.
2. Runs the package's tool once: it recompiles `exefs/` into `<Game>_code.dll` on their PC (GCC, a few minutes) and
   links their dump folder (`--link`, nothing copied).
3. Starts the game. Settings in `<Game>.ini`; saves in `<Game>_data/save/`.

What the distributed package contains (no game data, no game code):

| Ship | Why |
| --- | --- |
| `SwitchRecomp.exe` (tool), `switchrecomp.py`, `a64.py`, `codegen.py`, `nxloader.py`, `brt.py`, `tools_sym.py` | the recompiler that turns the user's own exefs into a DLL locally |
| `bin/runtime_win64_win32.exe` | the runtime (OS services, GPU → OpenGL, audio, video) with the fixes above |
| `runtime/` source, `LICENSE.txt`, `NOTICE.txt`, `README*`, `CHANGELOG.txt` | AGPL v3 requires the source; NOTICE requires the MZ Labs credit and startup screen |
| `Install.bat`, `tool/get_deps.py` | downloads Python and GCC (+ g++) on the user's PC, hash-checked |
| `tcc/` | small compiler fallback (LGPL) |
| `docs/JAMBOREE_PORT.md` | this file |

Never shipped (each one is Nintendo's work or derived from it):

| Do not ship | What it is |
| --- | --- |
| `Recomp/exefs/`, `Recomp/romfs/`, `Recomp/control*` | the dump itself (code, 5.4 GB of assets, icon) |
| `games/Recomp_data/` | a copy of the dump, plus the user's saves |
| `games/Recomp_code.dll`, `games/Recomp_build/` | the game's code translated to C and compiled: a derivative of the game's code |
| `games/shader_cache/`, `games/captures/`, `games/*.log`, `threads.txt`, dumped `shader_*.glsl` | the game's shaders, frame dumps, logs |
| `games/Recomp.hints` | addresses inside the game's code (generated per user) |
| Nintendo fonts, keys, firmware | never needed: the runtime has its own font fallback |

Rule: everything the game needs is read from the user's own dump at build or run time. A release must pass a check
that no file in it came out of the dump (`.gitignore` here already excludes `games/`, `gcc/`, `python/`).

## 6. Files needed at run time (per user install)

```
<Game>.exe                 runtime (copy of bin/runtime_win64_win32.exe)
<Game>_code.dll            built locally from the user's exefs
<Game>.ini                 options; [Game] game_dump= points at the user's dump
<dump>/exefs/main, main.npdm, rtld, sdk, subsdk0           user's dump (read in place)
<dump>/romfs/...           user's dump: Archive/, movie/, nro/ (boards + minigames), stream/, boot.nbinit, ac.nx.archiveconfig
<Game>_data/control.nacp, icon_AmericanEnglish.dat         title and icon for the tool (copied, user-local)
<Game>_data/save/          created by the game
```

Note: the minigames and boards are NROs in `romfs/nro/NX_Release/`. The recompiler compiles them too
(`nxloader.romfs_nros`), so the romfs NROs are part of the code build input as well.

## 7. Next steps
1. Play a full turn into a minigame (first minigame NRO), then the other boards.
2. The ~6 s load hitches (`menu00`, `bd01`): profile the GL thread batch (ASTC on the CPU vs. upload).
3. The gray backdrop behind the mode list.
4. Recompile with `Recomp.hints` (72 addresses the static pass missed; they run in the interpreter now).
5. Audio by ear, controller input with a real pad, performance on long sessions.
6. Packaging: fold `get_gxx.py` into `tool/get_deps.py`, make `--link` the tool's default, add CHANGELOG lines.
