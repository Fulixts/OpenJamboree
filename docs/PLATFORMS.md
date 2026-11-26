# Platforms

| System | Recompile | Run | Notes |
| --- | --- | --- | --- |
| Windows 10/11 x64 | yes | yes | OpenGL 4.3+, in-game Esc menu, SDL3 pads (PlayStation, Xbox, Switch Pro) |
| Linux x86_64 | yes | built, not yet played through | X11 backend (works under Wayland through XWayland), OpenGL 4.3+, SDL2 pads |
| macOS | no | no | see below |

## Windows

```
python setup.py
python launcher/prepare.py --keys prod.keys --base base.nsp --update update-2.3.0.nsp
```

`setup.py` downloads GCC 16.2 (mingw-w64, conda-forge, sha256-checked) into `gcc/`. Any Python 3.9+ runs it; the
release package carries an embedded Python 3.14 for the Recomp Launcher (`setup.py --python`).

## Linux

```
sudo apt install build-essential cmake git nasm libx11-dev libgl-dev     # or your distro's equivalents
python3 setup.py
python3 launcher/prepare.py --keys prod.keys --base base.nsp --update update-2.3.0.nsp
```

`setup.py` uses the system's GCC, builds OpenH264 2.4.1 and Opus 1.5.2 from their pinned sources
(`tools/build_linux_libs.sh`) and gets SDL2 from PyPI's `pysdl2-dll` into `lib/`. To build the NSP extractor from
source it needs the .NET 10 SDK; the release package already has `tools/nsp_extract`.

Not on Linux yet: the in-game Esc menu (it is Win32 + OpenGL; the X11 backend needs its own input hooks) and SDL3 pads
(the X11 backend reads pads through SDL2, which also covers DualSense).

## macOS: why not yet

Three things stand in the way, in order of size:

1. **OpenGL 4.1.** macOS stops at OpenGL 4.1 and never got compute shaders, storage buffers or the other 4.3+ features
   the GL renderer (`runtime/gpu_gl.c`) is built on. The way through is the Vulkan renderer (`renderer=vulkan`, the NVN
   layer) on top of MoltenVK, which today is only built for Windows and Linux.
2. **16 KB pages on Apple Silicon.** The Switch uses 4 KB pages and the runtime maps the game's memory at 4 KB
   granularity (`runtime/plat.c`, `runtime/mem.c`). Apple Silicon only maps 16 KB pages, so every mapping, protection
   change and the fastmem layout would need to work at 16 KB with sub-page tracking.
3. **arm64 host.** The recompiled C is portable, but the runtime and the code generator assume an x86-64 host in places
   (SIMD helpers, atomics, the exception handling around memory faults). Intel Macs avoid this one.

Each is real work but none is impossible; the order to attack them is 1 (MoltenVK) on an Intel Mac, then 2 and 3.
