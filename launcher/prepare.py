# OpenJamboree - Recomp Launcher setup step (the "prepare" in recomplauncher.json).
# Builds Super Mario Party Jamboree on the player's PC from THEIR OWN dump:
#   base game .nsp + update 2.3.0 .nsp + their own prod.keys.
# Nothing from Nintendo is in this release or downloaded by it. The keys and the game never leave the PC.
#
#   python prepare.py --keys prod.keys --base base.nsp --update update.nsp --data <launcher data folder>
#
# Prints "progress <0..1> <text>" lines for the launcher. Exit 0 = the game is ready.
import argparse, hashlib, json, os, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # the release folder ({install})
GAME = 'SuperMarioPartyJamboree'
# exefs/main of each game version this port is known to build. Other versions are refused before minutes of work.
KNOWN_MAIN = {
    'c84556140322105b8c705feef3dbf5bde2ccb8556fb96687e2c6fe4837db82fb': '2.3.0',
}
PY = sys.executable


def progress(f, text):
    print('progress %.2f %s' % (f, text), flush=True)


def run(args, what):
    p = subprocess.run(args, cwd=HERE)
    if p.returncode:
        raise SystemExit('%s failed (exit %d)' % (what, p.returncode))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def stamp(*paths):
    """cheap identity of the picked files: name, size, modified time"""
    return [[os.path.basename(p), os.path.getsize(p), int(os.path.getmtime(p))] for p in paths]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--keys', required=True)
    ap.add_argument('--base', required=True)
    ap.add_argument('--update', required=True)
    ap.add_argument('--data', required=True)
    a = ap.parse_args()
    for p in (a.keys, a.base, a.update):
        if not os.path.isfile(p): raise SystemExit('missing file: %s' % p)

    dump = os.path.join(a.data, 'dump')
    games = os.path.join(HERE, 'games')
    marker = os.path.join(games, 'openjamboree_build.json')
    inputs = stamp(a.base, a.update)

    # 1) compiler (GCC from conda-forge, hash-checked, into this folder)
    progress(0.01, 'Getting the compiler (once, about 300 MB)')
    gcc = os.path.join(HERE, 'gcc', 'Library', 'bin', 'x86_64-w64-mingw32-gcc.exe')
    gxx = os.path.join(HERE, 'gcc', 'Library', 'bin', 'x86_64-w64-mingw32-g++.exe')
    if not os.path.isfile(gcc):
        run([PY, '-u', '-I', os.path.join(HERE, 'tool', 'get_deps.py'), 'gcc'], 'Downloading GCC')
    if not os.path.isfile(gxx):
        run([PY, '-u', '-I', os.path.join(HERE, '_work', 'get_gxx.py')], 'Downloading G++')

    # 2) unpack the user's dump (skipped when the same files were already unpacked)
    old = {}
    if os.path.isfile(marker):
        try: old = json.load(open(marker))
        except ValueError: old = {}
    main_bin = os.path.join(dump, 'exefs', 'main')
    if old.get('inputs') != inputs or not os.path.isfile(main_bin):
        progress(0.05, 'Unpacking your game (about 5 GB, a few minutes)')
        run([os.path.join(HERE, 'tools', 'nsp_extract.exe'), a.keys, a.base, a.update, dump], 'Unpacking the game')

    # 3) right version?
    h = sha256(main_bin)
    ver = KNOWN_MAIN.get(h)
    if not ver:
        raise SystemExit('This copy of the game is a version this port does not support yet (exefs/main %s). '
                         'Pick the base game and the 2.3.0 update.' % h[:16])
    print('game version %s' % ver, flush=True)

    # 4) runtime + game code (GCC, several minutes the first time; reused when nothing changed)
    progress(0.35, 'Building the runtime')
    run([PY, '-u', os.path.join(HERE, 'switchrecomp.py'), '--build-runtime'], 'Building the runtime')
    progress(0.45, 'Recompiling the game code (10 to 30 minutes the first time)')
    os.makedirs(games, exist_ok=True)
    run([PY, '-u', os.path.join(HERE, 'switchrecomp.py'), dump, '--link', '-o', GAME, '--outdir', games],
        'Recompiling the game')
    for f in (GAME + '.exe', GAME + '_code.dll'):
        if not os.path.isfile(os.path.join(games, f)): raise SystemExit('the build left no %s' % f)

    # 5) extras: PlayStation/Xbox pads (SDL3.dll next to the game) and the 1 second opening for skip_intro,
    #    cut from the player's own movie (the menu's "Pular o vídeo de abertura" switches between the two)
    progress(0.95, 'Pads and the short opening movie')
    import shutil
    sdl = os.path.join(HERE, 'tools', 'SDL3.dll')
    if os.path.isfile(sdl): shutil.copy2(sdl, os.path.join(games, 'SDL3.dll'))
    movie = os.path.join(dump, 'romfs', 'movie', 'flow', 'op', 'movie', 'bq_op.mp4')
    short = os.path.join(games, GAME + '_overlay', 'skip_intro', 'movie', 'flow', 'op', 'movie', 'bq_op.mp4')
    if os.path.isfile(movie):
        os.makedirs(os.path.dirname(short), exist_ok=True)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import trim_intro
        trim_intro.trim(movie, short, 60)

    json.dump({'inputs': inputs, 'main_sha256': h, 'version': ver}, open(marker, 'w'), indent=1)
    progress(1.0, 'Ready')


if __name__ == '__main__':
    main()
