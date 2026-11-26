#!/bin/sh
# Linux entry of the Recomp Launcher's prepare step (recomplauncher.json): the Windows package runs
# python/python.exe -u launcher/prepare.py ...; on Linux the system's python3 runs the same script.
cd "$(dirname "$0")/.." || exit 1
if [ "$1" = "-u" ]; then shift 2; fi   # drop "-u launcher/prepare.py", shared with the Windows command line
exec python3 -u launcher/prepare.py "$@"
