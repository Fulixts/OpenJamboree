#!/usr/bin/env python3
# Builds the online server's libraries once, as static libraries the runtime links (like runtime/oh264/<os>):
#   runtime/mbedtls/<os>/libmbedtls.a   Mbed TLS 3.6.8 (Apache-2.0)   TLS for the host's built-in server
#   runtime/nghttp2/<os>/libnghttp2.a   nghttp2 1.71.0 (MIT)          HTTP/2 under gRPC
# Uses the same compiler as the runtime (gcc/ from setup.py on Windows, the system's on Linux).
#   python tools/build_net_libs.py [--force]
import glob, os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RT = os.path.join(HERE, 'runtime')
WIN = sys.platform.startswith('win')
OSDIR = 'win' if WIN else 'lin'


def tool(name):
    if WIN:
        p = os.path.join(HERE, 'gcc', 'Library', 'bin', 'x86_64-w64-mingw32-' + name + '.exe')
        if os.path.isfile(p): return p
    return shutil.which(name) or name


def build(lib, srcs, flags):
    out = os.path.join(RT, lib, OSDIR, 'lib%s.a' % lib)
    if os.path.isfile(out) and '--force' not in sys.argv:
        print('%s: already built' % out); return
    obj = os.path.join(HERE, '_work', 'obj_' + lib); os.makedirs(obj, exist_ok=True)
    cc = tool('gcc')
    env = dict(os.environ); env['PATH'] = os.path.dirname(cc) + os.pathsep + env.get('PATH', '')   # gcc finds its own DLLs
    def one(s):
        o = os.path.join(obj, os.path.basename(s)[:-2] + '.o')
        r = subprocess.run([cc, '-O2', '-w', '-c', s, '-o', o] + flags, capture_output=True, text=True, env=env)
        if r.returncode: raise SystemExit('%s failed (%d):\n%s%s' % (s, r.returncode, r.stdout[-1000:], r.stderr[-2000:]))
        return o
    with ThreadPoolExecutor(os.cpu_count() or 4) as ex: objs = list(ex.map(one, srcs))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.isfile(out): os.remove(out)
    subprocess.run([tool('ar'), 'rcs', out] + objs, check=True, env=env)
    print('%s: %d objects' % (out, len(objs)))


build('mbedtls', glob.glob(os.path.join(RT, 'mbedtls', 'library', '*.c')),
      ['-I', os.path.join(RT, 'mbedtls', 'include'), '-I', os.path.join(RT, 'mbedtls', 'library')])
build('nghttp2', glob.glob(os.path.join(RT, 'nghttp2', 'lib', '*.c')),
      ['-I', os.path.join(RT, 'nghttp2', 'lib', 'includes'), '-I', os.path.join(RT, 'nghttp2', 'lib'),
       '-DNGHTTP2_STATICLIB', '-DHAVE_STDINT_H'] + ([] if WIN else ['-DHAVE_ARPA_INET_H', '-DHAVE_NETINET_IN_H']))
