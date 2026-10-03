#!/bin/bash
# Builds and runs port/win9x/tests/renderer/t_fixed_device.c: the light renderer (display.renderer
# "gl1") behind the device's Direct3D entry points, against a recording stand-in for OpenGL.
#
#   tools/t_fixed_device.sh [output folder]      (default build/win9x/tests/renderer/fixed)
#
# From the repository's root, in the shell the Windows build is made in (clang, lld, the MSVC x86
# environment). The units are compiled with the Windows build's own command (ninja -t commands
# build/windows/halo.exe, without LTO and the profile) and with the memory window at 0x40000000,
# where a test process can map it. Nothing of the game's build is touched, and no window or OpenGL
# context is opened.
set -e
out=${1:-build/win9x/tests/renderer/fixed}
mkdir -p "$out"

line=$(ninja -t commands build/windows/halo.exe | grep -m1 -- '-c port/linux/src/d3d8_gl.c')
flags=$(echo "$line" | sed -e 's/^clang -MMD -MF [^ ]* //' -e 's/ -c [^ ]* -o [^ ]*$//' -e 's/ -flto=full//' \
	-e 's/ -fprofile-use=[^ ]*//' -e 's/ -Wno-profile-instr-unprofiled//' -e 's/ -Wno-profile-instr-out-of-date//' \
	-e 's/ -Wno-profile-instr-missing//' -e 's/ -Wno-backend-plugin//')
flags="$flags -DHALO_WINDOW_BASE=0x40000000UL"

# FF_DIR: where nv2a_combiner_ff.c and its header are, when not yet in port/linux/src
ff=${FF_DIR:-port/linux/src}
units="port/linux/src/d3d8_gl.c port/linux/src/gl_functions.c port/linux/src/xgpu_fixed.c
	port/linux/src/xgpu_fixed_textures.c port/linux/src/nv2a_vsh_cpu.c port/linux/src/xbox_textures_convert.c
	port/linux/src/xbox_textures.c port/linux/src/nv2a_vsh.c port/linux/src/nv2a_psh.c port/linux/src/xgpu_text.c
	$ff/nv2a_combiner_ff.c port/win9x/tests/renderer/t_fixed_device.c"
objects=""
for source in $units; do
	object="$out/$(basename "${source%.c}").o"
	extra=""
	# (with FF_DIR, the header next to that nv2a_combiner_ff.c is the one its users see)
	case "$source" in
	*xgpu_fixed*.c|*t_fixed_device.c)
		[ "$ff" = port/linux/src ] || extra="-include port/linux/src/xgpu.h -include $ff/nv2a_combiner_ff.h" ;;
	esac
	clang $flags $extra -c "$source" -o "$object"
	objects="$objects $object"
done
clang --target=i686-pc-windows-msvc -fuse-ld=lld $objects -o "$out/t_fixed_device.exe"

status=0
run() {
	echo "== t_fixed_device $*"
	"$out/t_fixed_device.exe" "$@" || status=1
}
# plain OpenGL 1.1; a driver with the extensions, used not at all, multitexture alone, all of
# them; OpenGL 1.5; 32-bit textures
run plain all
run ext none
run ext ARB_multitexture
run ext all
run core all
run core all 32
run ext simple,all
# debug.gpu_stats
run core all -s
exit $status
