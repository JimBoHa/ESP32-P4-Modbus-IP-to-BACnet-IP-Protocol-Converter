#!/usr/bin/env python3
"""Repeatable local verification; never contacts a field device."""
import subprocess
import sys
from pathlib import Path

root=Path(__file__).resolve().parents[1]
commands=[
    [sys.executable,'tools/generate_ats_map.py','--check'],
    ['cmake','-S','tests','-B','build-host','-DCMAKE_BUILD_TYPE=Debug','-DENABLE_SANITIZERS=ON'],
    ['cmake','--build','build-host','-j','6'],
    ['ctest','--test-dir','build-host','--output-on-failure'],
    [sys.executable,'-m','unittest','discover','-s','tests','-p','test_modbus_loopback.py','-v'],
    [sys.executable,'-m','compileall','-q','tools'],
    ['git','diff','--check'],
    ['git','diff','--cached','--check'],
]
for command in commands:
    print('+ '+' '.join(command),flush=True)
    subprocess.run(command,cwd=root,check=True)
