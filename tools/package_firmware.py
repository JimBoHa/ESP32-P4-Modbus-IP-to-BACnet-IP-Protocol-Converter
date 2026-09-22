#!/usr/bin/env python3
"""Package a built gateway image. Run with the ESP-IDF Python environment."""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    root=Path(__file__).resolve().parents[1]
    build=args.build_dir.resolve()
    dest=args.output.resolve()
    project=json.loads((build/'project_description.json').read_text())
    if project['project_name']!='ats_modbus_bacnet':
        raise SystemExit('Refusing to package a different firmware project')
    if dest.exists():
        raise SystemExit(f'Destination already exists: {dest}; choose a new package path')
    dest.mkdir(parents=True)
    flash=json.loads((build/'flasher_args.json').read_text())
    command=[sys.executable,'-m','esptool','--chip','esp32p4','merge_bin',
             '-o',str(dest/'initial-flash.bin'),'--flash_mode','dio',
             '--flash_size','32MB','--flash_freq','80m']
    for address,name in flash['flash_files'].items():
        command.extend([address,str(build/name)])
    subprocess.run(command,check=True)
    shutil.copy2(build/'ats_modbus_bacnet.bin',dest/'application.bin')
    shutil.copy2(root/'README.md',dest/'README-source.md')
    shutil.copytree(root/'docs',dest/'docs')
    shutil.copy2(root/'THIRD_PARTY_NOTICES.md',dest/'THIRD_PARTY_NOTICES.md')
    shutil.copytree(root/'third_party'/'bacnet-stack'/'license',dest/'third-party-licenses'/'bacnet-stack')
    idf=Path(project['idf_path'])
    for rel in ['LICENSE','components/freertos/FreeRTOS-Kernel/LICENSE.md',
                'components/lwip/lwip/COPYING','components/json/cJSON/LICENSE',
                'components/newlib/COPYING.NEWLIB']:
        output=dest/'third-party-licenses'/'esp-idf'/rel
        output.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(idf/rel,output)
    compiler=shutil.which('riscv32-esp-elf-gcc')
    if not compiler:
        raise SystemExit('Run packaging inside the ESP-IDF environment (RISC-V compiler missing)')
    gcc=Path(compiler).resolve().parent.parent/'share'/'licenses'/'gcc'
    for rel in ['COPYING.RUNTIME','COPYING3','COPYING']:
        src=gcc/rel
        if not src.exists(): src=gcc/'gcc'/rel
        output=dest/'third-party-licenses'/'gcc'/rel
        output.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(src,output)
    source=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
    dirty=bool(subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=root,text=True).strip())
    if dirty:
        raise SystemExit('Commit the reviewed source before making a delivery package')
    with zipfile.ZipFile(dest/'source.zip','w',zipfile.ZIP_DEFLATED) as archive:
        tracked=subprocess.check_output(['git','ls-files','-z'],cwd=root).decode().split('\0')
        for rel in tracked:
            if rel and (root/rel).is_file(): archive.write(root/rel,'source/'+rel)
        sub=root/'third_party'/'bacnet-stack'
        tracked=subprocess.check_output(['git','ls-files','-z'],cwd=sub).decode().split('\0')
        for rel in tracked:
            if rel and (sub/rel).is_file(): archive.write(sub/rel,'source/third_party/bacnet-stack/'+rel)
        archive.writestr('source/SOURCE_COMMIT.txt',source+'\n')
    manifest={
        'project':project['project_name'],'version':project['project_version'],
        'idf_version':project.get('idf_ver'),'target':'esp32p4',
        'board':'Waveshare ESP32-P4-POE-ETH, 32 MB flash',
        'source_commit':source,'tracked_source_dirty':dirty,
        'flash_offset':'0x0','image':'initial-flash.bin',
        'hardware_tested':False,
        'note':'Private site-configured package. Verify silicon revision and back up the attached board before flashing; no force override.',
    }
    (dest/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (dest/'FLASHING.md').write_text('''# Site test package

This firmware has not yet run on the physical ESP32-P4. Connect its Ethernet
port to the controls LAN and its USB-C programming port to the Raspberry Pi
using a data cable. No GPIO or ATS terminal wiring is needed.

Identify the USB port and silicon revision, then save the board's existing
32 MB flash before installation. This build uses the pre-v3 ESP32-P4 family
with minimum revision 1. Do not force a revision mismatch; rebuild for the
reported silicon instead. The commissioning guide gives the full sequence.

With the ESP-IDF Python environment active, and PORT replaced by the
confirmed board serial device:

```sh
python -m esptool --chip esp32p4 --port PORT chip_id
python -m esptool --chip esp32p4 --port PORT flash_id
python -m esptool --chip esp32p4 --port PORT read_flash 0x0 0x2000000 original-flash.bin
python -m esptool --chip esp32p4 --port PORT write_flash 0x0 initial-flash.bin
```

The installation replaces existing firmware. Preserve the recovery file
privately. application.bin is an application-only image, not a merged USB
image, and this version has no OTA upload endpoint.

Ethernet uses DHCP. Read the assigned address from the USB log or DHCP
lease named kohler-ats-gateway. BACnet device instance is 75181, UDP47808.
Refresh Metasys device and field-point discovery for that instance.

Public read-only diagnostics: /api/status and /api/points on HTTP port80.
The source README and docs describe qualified/unavailable measurements.
''')
    checks=[]
    for item in sorted(dest.rglob('*')):
        if item.is_file():
            checks.append(hashlib.sha256(item.read_bytes()).hexdigest()+'  '+str(item.relative_to(dest)))
    (dest/'SHA256SUMS').write_text('\n'.join(checks)+'\n')
    print(json.dumps(manifest,indent=2))


if __name__=='__main__':
    main()
