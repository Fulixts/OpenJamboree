import os as _os
ROOT=_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))   # the Switch2Recomp folder
import sys, os
sys.path.insert(0, ROOT+r"\tool")
sys.argv=['x']
import get_deps as g
for p,sha in [('win-64/gxx_impl_win-64-16.2.0-hdbe55fc_7.conda','a9ee571df9ca3ec7ac8ec85916a12615e7c5ab0740d17f930abfb8730528935c')]:
    g.relocate(g.unpack(g.get(p,sha)), g.DEST); print("ok", p)
