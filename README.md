# OpenJamboree

> **This fork** ([Fulixts/OpenJamboree](https://github.com/Fulixts/OpenJamboree), based on
> [drewkellar/OpenJamboree](https://github.com/drewkellar/OpenJamboree)) targets the **2.3.0 update** and adds:
>
> - **Recomp Launcher support** ([`recomplauncher.json`](recomplauncher.json)): pick your base game `.nsp`, the 2.3.0
>   update `.nsp` and your `prod.keys`, and the launcher's Easy Setup unpacks, checks and builds the game
>   ([`launcher/prepare.py`](launcher/prepare.py), [`tools/nsp_extract`](tools/nsp_extract)).
> - **In-game menu on Esc** (Share + Options / View + Menu on a pad): picture quality (TV 1080p / handheld 720p,
>   internal resolution up to 400%), dynamic resolution, shadows, 60 FPS lock, V-Sync, language (Português do Brasil
>   included), fullscreen, quit ([`runtime/menu.cpp`](runtime/menu.cpp), Dear ImGui).
> - **Skip the 3 minute opening**: a 1 second cut of your own movie, through a LayeredFS-style romfs layer
>   ([`launcher/trim_intro.py`](launcher/trim_intro.py), `runtime/svc_fs.c`).
> - **PlayStation, Xbox and Switch Pro pads natively** through SDL3 (DualSense tested).
> - **Texture cache on disk**: ASTC textures converted for the PC are kept, so loads are much shorter from the second time.
>
> Next: **LAN play** (local wireless / LDN over the network, e.g. ZeroTier) and **mods** (romfs layers).
> Development happens on `main`; build steps below are the same, pointed at a 2.3.0 dump.

An unofficial native PC port of **Super Mario Party Jamboree** (Nintendo Switch, v1.1.0) made by static
recompilation. The game's ARM64 code is translated to C and compiled into a Windows module on your own PC; a runtime
provides the Switch OS services, the GPU (as OpenGL), audio and video.

**You need your own dump of the game.** This repository contains no game code, assets, keys or firmware, and it never
downloads any. Everything the game needs is read from your dump at build time and run time, and the dump is never
copied into or merged with the project.

Built on **SwitchRecomp by MZ Labs** (AGPL v3), with fixes for this game. See [`NOTICE.txt`](NOTICE.txt).

## Status

| Area | State |
| --- | --- |
| Boot, intro movie, title screen | works |
| Saves | created and loaded; intro skippable once a save exists |
| Menus, Party Plaza hub, tutorial, balloon tour, mode list | works |
| Boards | Mega Wiggler's Tree Party plays (dice, HUD, CPU turns), 55-60 FPS |
| Minigames | work (played through to the results and back to the board) |
| Dialogs, text, dice counters, character previews | render (they were clipped away before) |
| Board spaces | render along the paths |
| Minigame select | roulette over the board (bonus, 1 vs 3, 2 vs 2 and 4-player rounds seen) |
| Button prompts | follow the pad: Xbox labels by default, Switch positions with `pad_buttons=1` |
| Known issues | character select can stutter when switching quickly; occasional small lag spikes; more minigames and boards still to test (see the playtest log in the port notes) |

Full notes, every bug with its cause and fix, and the plan: [`docs/JAMBOREE_PORT.md`](docs/JAMBOREE_PORT.md).

## What you need

- Windows 10/11 x64, a GPU with OpenGL 4.6 (tested on an NVIDIA RTX 4060 Ti).
- Your own Super Mario Party Jamboree with the 1.1.0 update, dumped from your own Switch, with `exefs/` and `romfs/`
  extracted using your own keys.

## Setup

1. Run `Install.bat` once. It downloads Python and GCC (hash-checked) into this folder. Then run
   `python\python.exe _work\get_gxx.py` to add the C++ compiler the runtime needs, and build the runtime with
   `python\python.exe switchrecomp.py --build-runtime`.
2. Recompile your dump without copying it: `python switchrecomp.py <path to your dump> --link`. This builds
   `games/<Game>.exe` and `games/<Game>_code.dll` on your PC and writes `game_dump=` into `games/<Game>.ini`.
3. Start `games/<Game>.exe`. If it can't find the dump it asks for the folder and checks that it's the right game.

Saves go to `games/<Game>_data/save/`. Controls and options: [`README.txt`](README.txt) (F1 in game shows the keys).
Keyboard: arrows = D-pad, WASD = left stick, X/Space = A, Z = B, C = X, V = Y, Q = L, E = R, Enter = +, Backspace = -.
Esc opens the in-game menu; quitting is a button there.

## Never commit or share

The `.gitignore` blocks these, but check `git status` before every push:

- the dump (`exefs/`, `romfs/`, `control.nacp`, any `.nsp/.xci/.nca/.nso/.nro`), keys, firmware
- anything under `games/`: the built game, `*_code.dll`, `*_build/` (the game's code as C), `*_data/`, saves
- shader caches, captures, screenshots, logs, `threads.txt`, `.hints`, the `_work/` symbol caches
- tokens or credentials of any kind

## Credits and licence

- SwitchRecomp by MZ Labs: recompiler, runtime and tools, AGPL v3 with the additional terms in `NOTICE.txt`. The
  SwitchRECOMP startup screen and notice stay as they are.
- Third-party components keep their own licences (listed in `README.txt` and next to each component).
- Upstream SwitchRecomp readme: [`docs/SWITCH2RECOMP_README.md`](docs/SWITCH2RECOMP_README.md).

Not affiliated with or endorsed by Nintendo. Super Mario Party Jamboree and all related names are trademarks of
Nintendo. This project contains none of Nintendo's code or assets.
