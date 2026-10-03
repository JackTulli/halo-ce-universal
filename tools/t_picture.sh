#!/bin/bash
# Builds and runs port/win9x/tests/renderer/t_picture.c: the light renderer's picture (display.renderer
# "gl1") compared pixel by pixel with the Xbox's, computed by the test itself, with the device's draws
# rasterized by a software OpenGL 1.1 behind t_fixed_device.c's recorder. No window, no OpenGL context.
#
#   tools/t_picture.sh [output folder] [guard]      (default build/win9x/tests/renderer/picture)
#
# From the repository's root, in the shell the Windows build is made in (clang, lld, the MSVC x86
# environment), as tools/t_fixed_device.sh. With "guard" the device's units are compiled so that every
# allocation of theirs ends at a page without access (port/win9x/tests/renderer/t_picture_guard_alloc.h):
# a read or write past the end of an array stops the test.
set -e
out=${1:-build/win9x/tests/renderer/picture}
guard=${2:-}
mkdir -p "$out"

line=$(ninja -t commands build/windows/halo.exe | grep -m1 -- '-c port/linux/src/d3d8_gl.c')
flags=$(echo "$line" | sed -e 's/^clang -MMD -MF [^ ]* //' -e 's/ -c [^ ]* -o [^ ]*$//' -e 's/ -flto=full//' \
	-e 's/ -fprofile-use=[^ ]*//' -e 's/ -Wno-profile-instr-unprofiled//' -e 's/ -Wno-profile-instr-out-of-date//' \
	-e 's/ -Wno-profile-instr-missing//' -e 's/ -Wno-backend-plugin//')
flags="$flags -DHALO_WINDOW_BASE=0x40000000UL"

units="port/linux/src/d3d8_gl.c port/linux/src/gl_functions.c port/linux/src/xgpu_fixed.c
	port/linux/src/xgpu_fixed_textures.c port/linux/src/nv2a_vsh_cpu.c port/linux/src/xbox_textures_convert.c
	port/linux/src/xbox_textures.c port/linux/src/nv2a_vsh.c port/linux/src/nv2a_psh.c port/linux/src/xgpu_text.c
	port/linux/src/nv2a_combiner_ff.c port/win9x/tests/renderer/t_picture.c"
guarded=" d3d8_gl.c xgpu_fixed.c xgpu_fixed_textures.c nv2a_vsh_cpu.c xbox_textures_convert.c xbox_textures.c "
objects=""
for source in $units; do
	object="$out/$(basename "${source%.c}").o"
	extra="-I port/linux/src -I port/win9x/tests/renderer -Wno-unused-function -Wno-unused-variable"
	case "$guarded" in
	*" $(basename "$source") "*) [ "$guard" = guard ] && extra="$extra -include port/win9x/tests/renderer/t_picture_guard_alloc.h" ;;
	esac
	clang $flags $extra -c "$source" -o "$object"
	objects="$objects $object"
done
clang --target=i686-pc-windows-msvc -fuse-ld=lld $objects -o "$out/t_picture.exe"

status=0
run() {
	echo "== t_picture $*"
	"$out/t_picture.exe" "$@" -d "$out" || status=1
}
run core all 32
run core all 16
run ext all 32
run ext all 16
run ext ARB_multitexture 32
run ext none 16
run plain all 32
run plain all 16
run ext simple,all 32
exit $status
