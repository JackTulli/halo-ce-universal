#!/bin/bash
# Builds and runs port/win9x/tests/renderer/t_bench.c: the light renderer's processor time for a frame of
# the game's kinds of draws, behind t_fixed_device.c's recorder (no window, no OpenGL context).
#
#   tools/t_bench.sh [output folder] [frames]      (default build/win9x/tests/renderer/bench, 100 frames)
#
# From the repository's root, in the shell the Windows build is made in (clang, lld, the MSVC x86
# environment). Two executables: one with the Windows build's own flags (this processor's code), one
# with the Windows 98 build's processor (-march=pentium3, SSE only), both from the Windows build's
# command, so that the processor is the only difference.
set -e
out=${1:-build/win9x/tests/renderer/bench}
frames=${2:-100}
mkdir -p "$out"

line=$(ninja -t commands build/windows/halo.exe | grep -m1 -- '-c port/linux/src/d3d8_gl.c')
flags=$(echo "$line" | sed -e 's/^clang -MMD -MF [^ ]* //' -e 's/ -c [^ ]* -o [^ ]*$//' -e 's/ -flto=full//' \
	-e 's/ -fprofile-use=[^ ]*//' -e 's/ -Wno-profile-instr-unprofiled//' -e 's/ -Wno-profile-instr-out-of-date//' \
	-e 's/ -Wno-profile-instr-missing//' -e 's/ -Wno-backend-plugin//')
flags="$flags -DHALO_WINDOW_BASE=0x40000000UL"

units="port/linux/src/d3d8_gl.c port/linux/src/gl_functions.c port/linux/src/xgpu_fixed.c
	port/linux/src/xgpu_fixed_textures.c port/linux/src/nv2a_vsh_cpu.c port/linux/src/xbox_textures_convert.c
	port/linux/src/xbox_textures.c port/linux/src/nv2a_vsh.c port/linux/src/nv2a_psh.c port/linux/src/xgpu_text.c
	port/linux/src/nv2a_combiner_ff.c port/win9x/tests/renderer/t_bench.c"
build() {
	local name=$1 cpu_flags=$2 objects="" source object
	for source in $units; do
		object="$out/$name-$(basename "${source%.c}").o"
		clang $(echo "$flags" | sed -e "s/ -march=[^ ]*/ $cpu_flags/") -I port/linux/src -I port/win9x/tests/renderer \
			-Wno-unused-function -Wno-unused-variable -c "$source" -o "$object"
		objects="$objects $object"
	done
	clang --target=i686-pc-windows-msvc -fuse-ld=lld $objects -o "$out/t_bench-$name.exe"
}
build native "-march=native"
build p3 "-march=pentium3 -mfpmath=sse"

run() {
	echo "== t_bench $*"
	"$out/t_bench-$1.exe" "${@:2}"
}
for cpu in native p3; do
	run $cpu core all $frames -s
	run $cpu ext all $frames
	run $cpu plain all $frames
done
