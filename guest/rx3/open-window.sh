#!/usr/bin/env bash
# Build and open the native RX3 hardware window against an existing run-dev
# guest. This development bundle uses the repository's QEMU dylib/dependencies.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
RUN="${1:-$REPO/build/rx3/app}"
RUN="$(cd "$RUN" && pwd)"
cd "$REPO"
cargo build -p cdj3k-emu
python3 - "$REPO" "$RUN" <<'PY'
from pathlib import Path
import plistlib
import shutil
import sys
repo, run = map(Path, sys.argv[1:])
bundle = repo / 'build/rx3/XDJ-RX3.app/Contents'
(bundle / 'MacOS').mkdir(parents=True, exist_ok=True)
plistlib.dump({
    'CFBundleExecutable': 'cdj3k-emu',
    'CFBundleIdentifier': 'com.dj-emu.rx3.qa',
    'CFBundleName': 'XDJ-RX3',
    'CFBundleDisplayName': 'XDJ-RX3',
    'CFBundlePackageType': 'APPL',
    'CFBundleVersion': '2',
    'NSHighResolutionCapable': True,
    'LSEnvironment': {'DJ_EMU_RX3_RUN': str(run)},
}, (bundle / 'Info.plist').open('wb'))
for source in [repo / 'target/debug/cdj3k-emu',
               repo / 'qemu/install/lib/libcdj3k-emu-qemu.dylib']:
    destination = bundle / 'MacOS' / source.name
    # Replacing the inode keeps an already running window's executable intact.
    temporary = destination.with_suffix('.next')
    shutil.copy2(source, temporary)
    temporary.replace(destination)
PY
open "$REPO/build/rx3/XDJ-RX3.app"
