#!/usr/bin/env python3
# OpenJamboree setup: gets what this OS needs to recompile the game and builds the runtime. Replaces Install.bat.
#
#   python setup.py              compiler + libraries for this OS, then the runtime (bin/runtime_*)
#   python setup.py --deps-only  only the compiler and libraries
#   python setup.py --python     (Windows, for release packages) also the embedded Python 3.14 in python/
#
# Windows: GCC 16.2 (mingw-w64) from conda-forge into gcc/, every package checked against its sha256.
# Linux:   the system's GCC/G++ and X11/OpenGL headers (it says what to install if they are missing), OpenH264 and
#          Opus built from their pinned sources (tools/build_linux_libs.sh), SDL2 from PyPI's pysdl2-dll into lib/.
# Nothing is installed system-wide and no game files are touched. The game itself: launcher/prepare.py.
import argparse, hashlib, os, shutil, subprocess, sys, urllib.request, zipfile, io

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = sys.platform.startswith('win')
LINUX = sys.platform.startswith('linux')
PY_NUPKG = ('https://api.nuget.org/v3-flatcontainer/python/3.14.9/python.3.14.9.nupkg',
            '7cd9d07aec975212344dc6f7e45019378cf6775dd564181e269010a1eddaaec7')
GXX_WIN = ('win-64/gxx_impl_win-64-16.2.0-hdbe55fc_7.conda', 'a9ee571df9ca3ec7ac8ec85916a12615e7c5ab0740d17f930abfb8730528935c')


def say(m): print(m, flush=True)


def run(args, what, **kw):
    say('> ' + what)
    r = subprocess.run(args, cwd=HERE, **kw)
    if r.returncode: raise SystemExit('%s failed (exit %d)' % (what, r.returncode))


def embedded_python():
    if os.path.isfile(os.path.join(HERE, 'python', 'python.exe')): return
    url, sha = PY_NUPKG
    say('Python 3.14.9 (embedded, about 16 MB)')
    data = urllib.request.urlopen(url, timeout=120).read()
    if hashlib.sha256(data).hexdigest() != sha: raise SystemExit('the Python download is damaged (checksum)')
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for n in z.namelist():
            if not n.startswith('tools/') or n.endswith('/'): continue
            dst = os.path.join(HERE, 'python', *n.split('/')[1:])
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as f: f.write(z.read(n))


def deps_windows():
    sys.path.insert(0, os.path.join(HERE, 'tool'))
    import get_deps as g
    gcc = os.path.join(HERE, 'gcc', 'Library', 'bin')
    if not os.path.isfile(os.path.join(gcc, 'x86_64-w64-mingw32-gcc.exe')):
        run([sys.executable, '-u', os.path.join(HERE, 'tool', 'get_deps.py'), 'gcc'], 'GCC 16.2 from conda-forge')
    if not os.path.isfile(os.path.join(gcc, 'x86_64-w64-mingw32-g++.exe')):
        say('> G++ 16.2 from conda-forge'); g.relocate(g.unpack(g.get(*GXX_WIN)), g.DEST)
    return os.path.join(gcc, 'x86_64-w64-mingw32-gcc.exe')


def deps_linux():
    missing = [t for t in ('gcc', 'g++', 'make', 'cmake', 'git') if not shutil.which(t)]
    for h in ('/usr/include/X11/Xlib.h', '/usr/include/GL/glcorearb.h'):
        if not os.path.isfile(h): missing.append(h)
    if missing:
        raise SystemExit('Missing: %s\nInstall them, for example:\n'
                         '  Debian/Ubuntu: sudo apt install build-essential cmake git nasm libx11-dev libgl-dev\n'
                         '  Fedora:        sudo dnf install gcc gcc-c++ make cmake git nasm libX11-devel mesa-libGL-devel\n'
                         '  Arch:          sudo pacman -S base-devel cmake git nasm libx11 mesa' % ', '.join(missing))
    run(['bash', os.path.join(HERE, 'tools', 'build_linux_libs.sh')], 'OpenH264 and Opus from source')
    if not os.path.isdir(os.path.join(HERE, 'lib')):
        run([sys.executable, '-u', os.path.join(HERE, 'tool', 'get_deps.py'), 'sdl'], 'SDL2 (controllers) from PyPI')
    return 'gcc'


def main():
    ap = argparse.ArgumentParser(description='Gets the compiler and libraries, then builds the runtime.')
    ap.add_argument('--deps-only', action='store_true')
    ap.add_argument('--python', action='store_true', help='Windows: also the embedded Python in python/ (release packages)')
    a = ap.parse_args()
    if sys.version_info < (3, 9): raise SystemExit('Python 3.9 or newer is needed')
    if WIN:
        if a.python: embedded_python()
        cc = deps_windows()
    elif LINUX:
        cc = deps_linux()
    elif sys.platform == 'darwin':
        raise SystemExit('macOS is not supported yet: see docs/PLATFORMS.md (OpenGL 4.1 limit and 16 KB pages on Apple Silicon).')
    else:
        raise SystemExit('unsupported system: ' + sys.platform)
    run([sys.executable, '-u', os.path.join(HERE, 'tools', 'build_net_libs.py')], 'Mbed TLS and nghttp2 (online play host)')
    if a.deps_only: return
    run([sys.executable, '-u', os.path.join(HERE, 'switchrecomp.py'), '--build-runtime', '--cc', cc], 'the runtime')
    say('\nReady. Build the game from your dump:\n'
        '  python launcher/prepare.py --keys prod.keys --base base.nsp --update update-2.3.0.nsp')


if __name__ == '__main__':
    main()
