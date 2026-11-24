import os as _os
ROOT=_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))   # the Switch2Recomp folder
import sys, os, time
sys.path.insert(0, ROOT)
import switchrecomp as s
cc = ROOT+r"\gcc\Library\bin\x86_64-w64-mingw32-gcc.exe"
out = sys.argv[1] if len(sys.argv)>1 else ROOT+r"\_work\runtime_test.exe"
t=time.time(); s.build_runtime(cc, out, True, 'win32', print); print("built", out, round(time.time()-t), "s")
