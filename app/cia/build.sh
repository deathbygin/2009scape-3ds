#!/bin/bash
# Builds the installable title from the client already compiled by make. Run from anywhere, inside msys.
set -e
export DEVKITPRO=/opt/devkitpro PATH=/opt/devkitpro/tools/bin:$PATH
cd "$(dirname "$0")/.."
mkdir -p cia/out
python3 cia/make_art.py cia/out
bannertool makebanner -i cia/out/banner.png -a cia/out/silence.wav -o cia/out/banner.bnr
bannertool makesmdh -s "2009scape 3DS" -l "2009scape 3DS client" -p "matth" -i cia/out/icon.png -o cia/out/icon.smdh
makerom -f cia -o rs3ds_client.cia -target t -exefslogo -elf rs3ds_client.elf -rsf cia/rs3ds.rsf -icon cia/out/icon.smdh -banner cia/out/banner.bnr
makerom -f cci -o cia/out/rs3ds_client.cci -target t -exefslogo -elf rs3ds_client.elf -rsf cia/rs3ds.rsf -icon cia/out/icon.smdh -banner cia/out/banner.bnr
ls -la rs3ds_client.cia cia/out/rs3ds_client.cci
