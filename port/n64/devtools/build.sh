#!/bin/bash
# Regenerate the N64 ninja file and build the ROM; prints errors or the ROM.
cd /home/jack/halo-ce-universal || exit 1
python3 tools/n64_build.py > /dev/null || exit 1
if ! ninja -f build/n64/build.ninja "$@" > build/n64/last-build.log 2>&1; then
	grep -E "error|FAILED|undefined" build/n64/last-build.log | head -30
	exit 1
fi
ls -la --time-style=+%H:%M:%S build/n64/halo.z64 | awk '{print $5, $6, $7}'
