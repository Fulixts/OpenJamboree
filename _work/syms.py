import os as _os
ROOT=_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))   # the Switch2Recomp folder
import sys, re
sys.path.insert(0, ROOT)
import nxloader, subprocess, pickle, os
cache=ROOT+r"\_work\syms.pkl"
if os.path.exists(cache): tab=pickle.load(open(cache,'rb'))
else:
    mods,_,_=nxloader.load_program(_os.environ.get("GAME_DUMP") or sys.argv[2])
    tab=[(m.base+v,n,m.name) for m in mods for v,n in m.syms.items()]
    names=[t[1] for t in tab]
    out=subprocess.run([ROOT+r"\_work\c++filt.exe"],input="\n".join(names),capture_output=True,text=True).stdout.split("\n")
    tab=[(a,out[i] if i<len(out) else n,m) for i,(a,n,m) in enumerate(tab)]
    pickle.dump(tab,open(cache,'wb'))
pat=re.compile(sys.argv[1])
for a,n,m in sorted(tab):
    if pat.search(n): print("%x %s %s"%(a,m,n))
