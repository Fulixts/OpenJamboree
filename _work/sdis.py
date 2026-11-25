"""Rough Maxwell shader listing, from the runtime's own opcode table (runtime/shader.c): word index (the same
numbers as the 'case N' labels in the translated GLSL), guard predicate, mnemonic, raw word, and the fields of
xSETP. For F5 trace shader dumps (sh_<hash>_<stage>.bin). Mnemonics can be off where patterns overlap.
usage: python sdis.py <sh_..._vs.bin> [first] [last]"""
import os, re, struct, sys

here = os.path.dirname(os.path.abspath(__file__))
tab = re.findall(r'\{"([01-]{16})",O_[A-Z0-9_]*,"([A-Z0-9_.]*)"\}',
                 open(os.path.join(here, '..', 'runtime', 'shader.c')).read())
code = open(sys.argv[1], 'rb').read()
n = len(code) // 8
lo = int(sys.argv[2]) if len(sys.argv) > 2 else 11
hi = int(sys.argv[3]) if len(sys.argv) > 3 else n

def B(w, s, l): return (w >> s) & ((1 << l) - 1)

def name(w):
    top = w >> 48
    for p, nm in tab:
        if all(c == '-' or int(c) == (top >> (15 - i)) & 1 for i, c in enumerate(p)): return nm
    return '?'

for i in range(lo, min(hi, n)):
    if i >= 10 and (i - 10) % 4 == 0: continue          # scheduling word
    w = struct.unpack_from('<Q', code, i * 8)[0]
    if w == 0: continue
    g, gn = B(w, 16, 3), B(w, 19, 1)
    guard = '' if g == 7 and not gn else '@%sP%d ' % ('!' if gn else '', g)
    nm = name(w); x = ''
    if 'SETP' in nm:
        x = ' pd=P%d pb=P%d bop=%d cmp=%d' % (B(w, 3, 3), B(w, 0, 3), B(w, 45, 2), B(w, 49, 3) if 'ISETP' in nm else B(w, 48, 4))
    if nm in ('BRA', 'EXIT', 'BRK', 'CONT', 'SYNC', 'KIL', 'RET') and B(w, 0, 5) != 15:
        x = ' CC test %d' % B(w, 0, 5)
    if nm in ('ISET', 'FSET') and B(w, 47, 1): x = ' .CC'
    print('%4d %s%-10s %016x%s' % (i, guard, nm, w, x))
