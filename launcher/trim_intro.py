# Makes a short copy of the game's opening movie (the player's own bq_op.mp4) for "skip intro".
# Keeps the first N frames of the single H.264 track and rewrites the sample tables, so the game plays
# about a second of its intro and moves on to the title screen. Nothing is downloaded or shipped: the
# output is made on the player's PC from their own dump and lives in the game's overlay folder.
#
#   python trim_intro.py <bq_op.mp4> <out.mp4> [frames]
import struct, sys

CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl', b'edts', b'dinf'}


def boxes(buf, s, e):
    """yield (type, start, header length, size) for the boxes in buf[s:e]"""
    while s < e:
        sz, t = struct.unpack_from('>I4s', buf, s); hl = 8
        if sz == 1: sz = struct.unpack_from('>Q', buf, s + 8)[0]; hl = 16
        if sz == 0: sz = e - s
        yield t, s, hl, sz
        s += sz


def box(t, payload):
    return struct.pack('>I4s', 8 + len(payload), t) + payload


def full(t, vf, payload):
    return box(t, struct.pack('>I', vf) + payload)


def read_top(path):
    f = open(path, 'rb'); data = f.read()
    top = {t: (s, hl, sz) for t, s, hl, sz in boxes(data, 0, len(data))}
    return data, top


def trim(src, dst, keep):
    data, top = read_top(src)
    ms, mhl, msz = top[b'moov']
    moov = data[ms + mhl:ms + msz]
    traks = [(s, hl, sz) for t, s, hl, sz in boxes(moov, 0, len(moov)) if t == b'trak']
    if len(traks) != 1: raise SystemExit('expected one track, found %d' % len(traks))
    mvhd_ts = None
    for t, s, hl, sz in boxes(moov, 0, len(moov)):
        if t == b'mvhd':
            v = moov[s + hl]
            mvhd_ts = struct.unpack_from('>I', moov, s + hl + (12 if v == 0 else 20))[0]

    # sample tables of the one track
    st = {}
    def find(buf, s, e, path=()):
        for t, bs, hl, sz in boxes(buf, s, e):
            st.setdefault(t, (bs, hl, sz))
            if t in CONTAINERS: find(buf, bs + hl, bs + sz)
    ts, thl, tsz = traks[0]
    trak = moov[ts:ts + tsz]
    find(trak, 8, len(trak))

    def tab(t):
        s, hl, sz = st[t]; return trak[s + hl:s + sz]

    mdhd = tab(b'mdhd'); v = mdhd[0]
    media_ts = struct.unpack_from('>I', mdhd, 12 if v == 0 else 20)[0]
    stts = tab(b'stts'); n_stts = struct.unpack_from('>I', stts, 4)[0]
    if n_stts != 1: raise SystemExit('variable frame timing is not supported (stts %d)' % n_stts)
    count, delta = struct.unpack_from('>II', stts, 8)
    keep = min(keep, count)
    stsz = tab(b'stsz'); fixed, total = struct.unpack_from('>II', stsz, 4)
    sizes = [fixed] * keep if fixed else list(struct.unpack_from('>%dI' % keep, stsz, 12))
    stsc = tab(b'stsc'); stco = tab(b'stco') if b'stco' in st else None
    if struct.unpack_from('>I', stsc, 4)[0] != 1 or stco is None or struct.unpack_from('>I', stco, 4)[0] != 1:
        raise SystemExit('expected all frames in one chunk')
    chunk_off = struct.unpack_from('>I', stco, 8)[0]
    frames = data[chunk_off:chunk_off + sum(sizes)]

    new = {}
    new[b'stts'] = full(b'stts', 0, struct.pack('>III', 1, keep, delta))
    new[b'stsz'] = full(b'stsz', 0, struct.pack('>II', 0, keep) + struct.pack('>%dI' % keep, *sizes))
    new[b'stsc'] = full(b'stsc', 0, struct.pack('>IIII', 1, 1, keep, 1))
    if b'stss' in st:
        stss = tab(b'stss'); n = struct.unpack_from('>I', stss, 4)[0]
        sync = [x for x in struct.unpack_from('>%dI' % n, stss, 8) if x <= keep]
        new[b'stss'] = full(b'stss', 0, struct.pack('>I', len(sync)) + struct.pack('>%dI' % len(sync), *sync))
    if b'sdtp' in st:
        sd = tab(b'sdtp'); new[b'sdtp'] = box(b'sdtp', sd[:4] + sd[4:4 + keep])
    if b'ctts' in st:
        ct = tab(b'ctts'); vf = struct.unpack_from('>I', ct, 0)[0]; n = struct.unpack_from('>I', ct, 4)[0]
        runs, left = [], keep
        for i in range(n):
            c, o = struct.unpack_from('>II', ct, 8 + 8 * i)
            c = min(c, left); runs.append((c, o)); left -= c
            if not left: break
        new[b'ctts'] = full(b'ctts', vf, struct.pack('>I', len(runs)) + b''.join(struct.pack('>II', c, o) for c, o in runs))
    dur_media = keep * delta
    dur_movie = dur_media * mvhd_ts // media_ts

    def rebuild(buf, s, e, chunk_at):
        out = b''
        for t, bs, hl, sz in boxes(buf, s, e):
            body = buf[bs + hl:bs + sz]
            if t in CONTAINERS: out += box(t, rebuild(buf, bs + hl, bs + sz, chunk_at)); continue
            if t in new: out += new[t]; continue
            if t == b'stco': out += full(b'stco', 0, struct.pack('>II', 1, chunk_at)); continue
            b = bytearray(body)
            if t == b'mvhd': struct.pack_into('>I', b, 16 if b[0] == 0 else 24, dur_movie) if b[0] == 0 else struct.pack_into('>Q', b, 24, dur_movie)
            if t == b'tkhd':
                if b[0] == 0: struct.pack_into('>I', b, 20, dur_movie)
                else: struct.pack_into('>Q', b, 28, dur_movie)
            if t == b'mdhd':
                if b[0] == 0: struct.pack_into('>I', b, 16, dur_media)
                else: struct.pack_into('>Q', b, 24, dur_media)
            if t == b'elst':
                n = struct.unpack_from('>I', b, 4)[0]
                if n >= 1:
                    if b[0] == 0: struct.pack_into('>I', b, 8, dur_movie)
                    else: struct.pack_into('>Q', b, 8, dur_movie)
            out += struct.pack('>I4s', 8 + len(b), t) + bytes(b)
        return out

    fs, fhl, fsz = top[b'ftyp']
    ftyp = data[fs:fs + fsz]
    # two passes: the chunk offset depends on the size of moov, which does not depend on the offset's value
    moov_new = box(b'moov', rebuild(data, ms + mhl, ms + msz, 0))
    chunk_at = len(ftyp) + len(moov_new) + 8
    moov_new = box(b'moov', rebuild(data, ms + mhl, ms + msz, chunk_at))
    with open(dst, 'wb') as f:
        f.write(ftyp); f.write(moov_new); f.write(box(b'mdat', frames))
    return keep, keep * delta / media_ts


if __name__ == '__main__':
    n, secs = trim(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 60)
    print('intro: kept %d frames (%.2f s)' % (n, secs))
