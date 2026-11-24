import os as _os
ROOT=_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))   # the Switch2Recomp folder
import sys,re,pickle,bisect
t=sorted(pickle.load(open(ROOT+r'\_work\syms.pkl','rb'))); k=[x[0] for x in t]
def s(a):
  i=bisect.bisect_right(k,a)-1; return '%s+%x'%(t[i][1][:100],a-t[i][0])
for l in open(sys.argv[1],errors='replace'):
  m=re.search(r'dyn: ([0-9a-f]+) from ([0-9a-f]+) (.*)',l)
  if not m: continue
  a=int(m.group(1),16); r=int(m.group(2),16); print(l[:10], m.group(1), s(a), ' <-', s(r)[:60], m.group(3))
