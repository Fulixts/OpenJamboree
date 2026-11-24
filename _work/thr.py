import os as _os
ROOT=_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))   # the Switch2Recomp folder
import sys, re, pickle, bisect
tab=sorted(pickle.load(open(ROOT+r"\_work\syms.pkl",'rb')))
keys=[t[0] for t in tab]
def s(a):
    i=bisect.bisect_right(keys,a)-1
    if i<0: return hex(a)
    n=tab[i][1]; n=re.sub(r'^_ZN','',n)[:70]
    return "%x:%s+%x"%(a,n,a-tab[i][0])
cur=None
for l in open(sys.argv[1],errors='replace'):
    if l.startswith('thread '): cur=l.strip()[:110]
    elif l.strip().startswith('stack:') and cur:
        fr=[int(x,16) for x in l.split()[1:] if x!='0']
        print(cur); print('    '+'\n    '.join(s(a) for a in fr[:9])); cur=None
