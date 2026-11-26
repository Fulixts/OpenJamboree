# OpenJamboree - Recomp Launcher setup step (the "prepare" in recomplauncher.json).
# Builds Super Mario Party Jamboree on the player's PC from THEIR OWN dump:
#   base game .nsp + update 2.3.0 .nsp + their own prod.keys.
# Nothing from Nintendo is in this release or downloaded by it. The keys and the game never leave the PC.
#
#   python launcher/prepare.py --keys prod.keys --base base.nsp --update update.nsp [--data <folder for the unpacked dump>]
#
# Windows and Linux. The Recomp Launcher runs it with --data={data}; by hand, --data defaults to ./data.
# Prints "progress <0..1> <text>" lines for the launcher. Exit 0 = the game is ready. On a failure the reason is the
# last line printed (the launcher shows the last 10 lines).
import argparse, hashlib, json, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # the release folder ({install})
GAME = 'SuperMarioPartyJamboree'
TITLE, PATCH = '0100965017338000', '0100965017338800'
UPDATE_VERSION, UPDATE_NAME = 458752, '2.3.0'
# exefs/main of each game version this port is known to build: a backstop after unpacking
KNOWN_MAIN = {
    'c84556140322105b8c705feef3dbf5bde2ccb8556fb96687e2c6fe4837db82fb': '2.3.0',
}
# bump when a release changes what the build makes (runtime, recompiler, extras): the next run rebuilds
BUILD_VERSION = 2
PY = sys.executable


def progress(f, text):
    print('progress %.2f %s' % (f, text), flush=True)


def fail(msg):
    print(msg, flush=True)
    raise SystemExit(1)


def run(args, what):
    p = subprocess.run(args, cwd=HERE)
    if p.returncode: fail('%s failed (exit %d). The lines above say why.' % (what, p.returncode))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def stamp(*paths):
    """cheap identity of the picked files: name, size, modified time"""
    return [[os.path.basename(p), os.path.getsize(p), int(os.path.getmtime(p))] for p in paths]


def nsp_extract():
    """the release ships tools/nsp_extract(.exe); a source checkout builds it once with the .NET SDK"""
    exe = 'nsp_extract.exe' if os.name == 'nt' else 'nsp_extract'
    for p in (os.path.join(HERE, 'tools', exe), os.path.join(HERE, 'tools', 'nsp_extract', 'out', exe)):
        if os.path.isfile(p): return p
    dotnet = shutil.which('dotnet')
    if not dotnet: fail('tools/%s is missing: use a release package, or install the .NET 10 SDK to build it' % exe)
    rid = 'win-x64' if os.name == 'nt' else 'linux-x64'
    run([dotnet, 'publish', os.path.join(HERE, 'tools', 'nsp_extract'), '-c', 'Release', '-r', rid,
         '-o', os.path.join(HERE, 'tools', 'nsp_extract', 'out')], 'Building nsp_extract')
    return os.path.join(HERE, 'tools', 'nsp_extract', 'out', exe)


def nsp_meta(tool, keys, nsp):
    """(title id, version, type) from the NSP's content meta, in seconds"""
    p = subprocess.run([tool, '--info', keys, nsp], capture_output=True, text=True)
    if p.returncode == 3: fail("Your prod.keys can't open %s. Dump prod.keys again from your Switch, after updating "
                               "its firmware to the one the game needs." % os.path.basename(nsp))
    for line in p.stdout.splitlines():
        if line.startswith('meta '):
            _, tid, ver, kind = line.split()
            return tid.lower(), int(ver), kind
    fail('%s is not a Switch game package that can be read (%s).' % (os.path.basename(nsp), (p.stderr or p.stdout).strip()[-200:]))


def check_inputs(tool, keys, base, update):
    b, u = nsp_meta(tool, keys, base), nsp_meta(tool, keys, update)
    if b[0] == PATCH and u[0] == TITLE:
        fail('The base game and the update are swapped: pick the base game .nsp first and the update second.')
    if b[0] == PATCH: fail('The file picked as the base game is the update (%s). Pick the base game .nsp.' % PATCH)
    if b[0] != TITLE: fail('The base game .nsp is another game (title %s). Pick Super Mario Party Jamboree (%s).' % (b[0], TITLE))
    if u[0] == TITLE: fail('The file picked as the update is the base game. Pick the update %s .nsp.' % UPDATE_NAME)
    if u[0] != PATCH: fail('The update .nsp is not a Super Mario Party Jamboree update (title %s).' % u[0])
    if u[1] != UPDATE_VERSION:
        fail('Your update is version v%d. OpenJamboree needs %s (v%d): update the game on your Switch to %s and dump '
             'the update again.' % (u[1], UPDATE_NAME, UPDATE_VERSION, UPDATE_NAME))
    print('copy checked: base %s v%d, update %s v%d (%s)' % (b[0], b[1], u[0], u[1], UPDATE_NAME), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--keys', required=True)
    ap.add_argument('--base', required=True)
    ap.add_argument('--update', required=True)
    ap.add_argument('--data', default=os.path.join(HERE, 'data'))
    a = ap.parse_args()
    for what, p in (('prod.keys', a.keys), ('base game', a.base), ('update', a.update)):
        if not os.path.isfile(p): fail('The %s file is missing: %s' % (what, p))

    dump = os.path.join(a.data, 'dump')
    games = os.path.join(HERE, 'games')
    marker = os.path.join(games, 'openjamboree_build.json')
    inputs = stamp(a.base, a.update)
    built = [os.path.join(games, f) for f in ((GAME + '.exe', GAME + '_code.dll') if os.name == 'nt' else (GAME, GAME + '_code.so'))]
    main_bin = os.path.join(dump, 'exefs', 'main')

    # 0) the right files, before anything slow
    progress(0.01, 'Checking your copy of the game')
    tool = nsp_extract()
    check_inputs(tool, a.keys, a.base, a.update)

    old = {}
    if os.path.isfile(marker):
        try: old = json.load(open(marker))
        except ValueError: old = {}
    if (old.get('inputs') == inputs and old.get('build') == BUILD_VERSION and os.path.isfile(main_bin)
            and all(os.path.isfile(f) for f in built)):
        progress(1.0, 'Already built'); return
    if os.path.isfile(marker): os.remove(marker)

    # 1) compiler and libraries for this OS (setup.py: GCC from conda-forge on Windows, the system's on Linux)
    progress(0.03, 'Getting the compiler (once)')
    run([PY, '-u', os.path.join(HERE, 'setup.py'), '--deps-only'], 'Getting the compiler')

    # 2) unpack the user's dump (skipped when the same files were already unpacked)
    if old.get('inputs') != inputs or not os.path.isfile(main_bin):
        progress(0.08, 'Unpacking your game (about 5 GB, a few minutes)')
        run([tool, a.keys, a.base, a.update, dump], 'Unpacking the game')

    # 3) backstop: the unpacked code is the one this port knows
    h = sha256(main_bin)
    ver = KNOWN_MAIN.get(h)
    if not ver: fail('The unpacked game code is not the %s one this port supports (exefs/main %s).' % (UPDATE_NAME, h[:16]))

    # 4) runtime + game code (GCC, several minutes the first time; reused when nothing changed)
    progress(0.35, 'Building the runtime')
    cc = ['--cc', 'gcc'] if os.name != 'nt' else []
    run([PY, '-u', os.path.join(HERE, 'switchrecomp.py'), '--build-runtime'] + cc, 'Building the runtime')
    progress(0.45, 'Recompiling the game code (10 to 30 minutes the first time)')
    os.makedirs(games, exist_ok=True)
    run([PY, '-u', os.path.join(HERE, 'switchrecomp.py'), dump, '--link', '-o', GAME, '--outdir', games] + cc,
        'Recompiling the game')
    for f in built:
        if not os.path.isfile(f): fail('The build left no %s.' % os.path.basename(f))

    # 5) extras: PlayStation/Xbox pads (SDL3.dll next to the game) and the 1 second opening for skip_intro,
    #    cut from the player's own movie (the menu's "Pular o vídeo de abertura" switches between the two)
    progress(0.95, 'Pads and the short opening movie')
    sdl = os.path.join(HERE, 'tools', 'SDL3.dll')
    if os.name == 'nt' and os.path.isfile(sdl): shutil.copy2(sdl, os.path.join(games, 'SDL3.dll'))
    movie = os.path.join(dump, 'romfs', 'movie', 'flow', 'op', 'movie', 'bq_op.mp4')
    short = os.path.join(games, GAME + '_overlay', 'skip_intro', 'movie', 'flow', 'op', 'movie', 'bq_op.mp4')
    if os.path.isfile(movie):
        os.makedirs(os.path.dirname(short), exist_ok=True)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import trim_intro
        trim_intro.trim(movie, short, 60)

    tmp = marker + '.tmp'
    with open(tmp, 'w') as f: json.dump({'inputs': inputs, 'main_sha256': h, 'version': ver, 'build': BUILD_VERSION}, f, indent=1)
    os.replace(tmp, marker)
    progress(1.0, 'Ready')


if __name__ == '__main__':
    main()
