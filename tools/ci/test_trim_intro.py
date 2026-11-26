# CI check for launcher/trim_intro.py on a synthetic MP4 (no game data): one video track, all frames in one chunk,
# like the game's opening movie. Trims 120 frames to 60 and checks the tables, durations and frame bytes.
import os, struct, sys, tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'launcher'))
import trim_intro as T

box, full = T.box, T.full
N, DELTA, TS = 120, 1000, 60000
sizes = [100 + (i * 37) % 900 for i in range(N)]
frames = b''.join(bytes([i & 0xFF]) * s for i, s in enumerate(sizes))


def movie(chunk_off):
    mvhd = full(b'mvhd', 0, struct.pack('>IIII', 0, 0, 1000, N * DELTA * 1000 // TS) + b'\0' * 80)
    tkhd = full(b'tkhd', 0, struct.pack('>IIIII', 0, 0, 1, 0, N * DELTA * 1000 // TS) + b'\0' * 60)
    elst = full(b'elst', 0, struct.pack('>IIiI', 1, N * DELTA * 1000 // TS, 0, 0x10000))
    mdhd = full(b'mdhd', 0, struct.pack('>IIIIHH', 0, 0, TS, N * DELTA, 0x55C4, 0))
    hdlr = full(b'hdlr', 0, b'\0' * 4 + b'vide' + b'\0' * 13)
    stsd = full(b'stsd', 0, struct.pack('>I', 1) + box(b'avc1', b'\0' * 78))
    stbl = box(b'stbl', stsd
               + full(b'stts', 0, struct.pack('>III', 1, N, DELTA))
               + full(b'stsc', 0, struct.pack('>IIII', 1, 1, N, 1))
               + full(b'stsz', 0, struct.pack('>II', 0, N) + struct.pack('>%dI' % N, *sizes))
               + full(b'stco', 0, struct.pack('>II', 1, chunk_off))
               + full(b'stss', 0, struct.pack('>IIII', 3, 1, 61, 101))
               + box(b'sdtp', b'\0' * 4 + bytes(range(N)))
               + full(b'ctts', 0, struct.pack('>I', 2) + struct.pack('>IIII', 50, 2000, 70, 1000)))
    minf = box(b'minf', full(b'vmhd', 1, b'\0' * 8) + stbl)
    trak = box(b'trak', tkhd + box(b'edts', elst) + box(b'mdia', mdhd + hdlr + minf))
    return box(b'moov', mvhd + trak)


ftyp = box(b'ftyp', b'isom\0\0\0\0isomavc1')
moov = movie(0)
data = ftyp + movie(len(ftyp) + len(moov) + 8) + box(b'mdat', frames)

d = tempfile.mkdtemp()
src, dst = os.path.join(d, 'in.mp4'), os.path.join(d, 'out.mp4')
open(src, 'wb').write(data)
kept, secs = T.trim(src, dst, 60)
assert kept == 60 and abs(secs - 1.0) < 1e-9, (kept, secs)

out, top = T.read_top(dst)
s, hl, sz = top[b'mdat']
assert out[s + hl:s + sz] == frames[:sum(sizes[:60])], 'frame bytes differ'
found = {}
def walk(buf, a, b):
    for t, bs, h, n in T.boxes(buf, a, b):
        found[t] = buf[bs + h:bs + n]
        if t in T.CONTAINERS: walk(buf, bs + h, bs + n)
walk(out, 0, len(out))
assert struct.unpack_from('>II', found[b'stsz'], 4) == (0, 60)
assert struct.unpack_from('>III', found[b'stts'], 4) == (1, 60, DELTA)
assert struct.unpack_from('>I', found[b'stco'], 8)[0] == s + hl, 'chunk offset does not point at mdat'
assert struct.unpack_from('>I', found[b'stss'], 4)[0] == 1
assert struct.unpack_from('>I', found[b'mdhd'], 16)[0] == 60 * DELTA
ct = found[b'ctts']; n = struct.unpack_from('>I', ct, 4)[0]
assert sum(struct.unpack_from('>I', ct, 8 + 8 * i)[0] for i in range(n)) == 60
print('trim_intro ok: 60 frames, %d bytes' % sz)
