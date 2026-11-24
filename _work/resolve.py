import os as _os
ROOT=_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))   # the Switch2Recomp folder
import sys, subprocess, re, bisect
exe=sys.argv[1]; dump=open(sys.argv[2]).read()
nm=subprocess.run([ROOT+r"\gcc\Library\bin\x86_64-w64-mingw32-nm.exe","-n",exe],capture_output=True,text=True).stdout
syms=[]
for l in nm.splitlines():
    p=l.split()
    if len(p)==3 and p[1] in 'tT' and 'branch_instr' not in p[2]: syms.append((int(p[0],16),p[2]))
addrs=[a for a,_ in syms]
def res(m):
    off=int(m.group(1),16); a=0x140000000+off; i=bisect.bisect_right(addrs,a)-1
    return "Recomp+%x [%s+%x]"%(off,syms[i][1],a-syms[i][0]) if i>=0 else m.group(0)
print(re.sub(r"Recomp\+([0-9a-f]+)",res,dump))
