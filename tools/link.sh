#!/usr/bin/env bash
# reUndr PS5, MIT. Runs in the reundr-ps5-builder image, see tools/build.sh.
# The pinned RADV link recipe and native runtime are GPL-3.0-or-later.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
vulkan="$root/build/deps/PS5_Vulkan"
# The pinned RADV recipe writes shared temporary maps inside its source tree.
# Serialize links across both targets, including separate Docker invocations.
mkdir -p "$root/build/ps5"
exec 9>"$root/build/ps5/link.lock"
flock 9
sdk="$vulkan/.deps/native/ps5-payload-sdk"
archive="$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
build="$root/build/ps5"
work="$root/build/linked"
mkdir -p "$work/obj" "$work/stubs"
export LLVM_CONFIG=/usr/bin/llvm-config-18 PS5_CLANG=/usr/bin/clang-18
export PATH=/usr/lib/llvm-18/bin:$PATH
resource=$(clang-18 --print-resource-dir)
mkdir -p "$resource/lib/linux"
cp "$root/build/deps/compiler-rt/libclang_rt.builtins-x86_64.a" "$resource/lib/linux/"
cc="$sdk/bin/prospero-clang"
cxx="$sdk/bin/prospero-clang++"
"$cxx" -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$vulkan/tooling/native/app_cpp_runtime.cpp" -o "$work/obj/cpp-runtime.o"
for pair in 'libSceAgc agc_canary_link_stub.c' 'libSceAgcDriver agc_driver_canary_link_stub.c'; do
    read -r library source <<<"$pair"
    "$cc" -std=c11 -O2 -fPIC -c "$vulkan/vendor/ps5/sdk/stubs/$source" -o "$work/obj/$library.o"
    "$sdk/bin/prospero-lld" --shared -soname "$library.prx" -o "$work/stubs/$library.so" "$work/obj/$library.o"
done
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$archive"
# Guest registers are native TLS (ps5/CMakeLists.txt): the pinned layout has no
# PT_TLS, so lld cannot resolve TPOFF. Add one; .tdata/.tbss move ahead of .data
# so the converter's data PT_LOAD (filesz ends at .data) still carries .tdata,
# and ALIGN(32) matches the converter's minimum PT_TLS alignment (0x20), which
# the loader uses for the main module's static TLS offset.
sed -e 's/^\( *\)dynamic PT_DYNAMIC FLAGS (0x6);/&\n\1tls PT_TLS;/' \
    -e '/^ *\.tdata : /d' -e '/^ *\.tbss : /d' \
    -e 's/^\( *\)\.data : { \*(\.data \.data\.\*) } : data/\1.tdata : ALIGN(32) { *(.tdata .tdata.*) } : data : tls\n\1.tbss : ALIGN(32) { *(.tbss .tbss.*) } : data : tls\n&/' \
    "$vulkan/tooling/native/ps5-pie.ld" > "$work/ps5-pie.ld"
grep -q 'tls PT_TLS' "$work/ps5-pie.ld" && [[ $(grep -c ': data : tls' "$work/ps5-pie.ld") == 2 ]]
cp "$vulkan/tooling/psbc/ps5-pie-unwind.ld" "$work/"
radv_linker_script=(-T "$work/ps5-pie-unwind.ld" -L "$work")
# The recipe groups its C++ archives; flatten that group into our app group.
inputs=()
for item in "${radv_link_inputs[@]}"; do
    case "$item" in --start-group|--end-group) continue;; esac
    inputs+=("$item")
done
# Use one visibility map: multiple anonymous maps can re-export operators.
flags=();skip=0
for item in "${radv_link_flags[@]}"; do
    if ((skip)); then skip=0;continue;fi
    if [[ $item == --version-script ]]; then skip=1;continue;fi
    flags+=("$item")
done
{
    printf '{local:\n_Zn*; _Zd*;\n'
    for item in "${flags[@]}"; do
        if [[ $item == --defsym=* ]]; then
            name=${item#--defsym=};printf '%s;\n' "${name%%=*}"
        fi
    done
    printf '*;};\n'
} > "$work/exports.map"
shaders=(
 "$root/build/shader-compiler/glslang/glslang/libglslang.a"
 "$root/build/shader-compiler/glslang/glslang/libglslang-default-resource-limits.a"
 "$root/build/shader-compiler/spirv-tools/source/opt/libSPIRV-Tools-opt.a"
 "$root/build/shader-compiler/spirv-tools/source/libSPIRV-Tools.a"
)
# Stubs describe dynamic imports. Their function bodies never enter eboot.bin.
stubs=()
for file in "$sdk"/target/lib/*.so; do
    case "$file" in *libScePosixForWebKit.so|*libSceGLSlimVSH.so|*libc_stub_weak.so) continue;; esac
    stubs+=("$file")
done
# --wrap: ps5/src/video_hz.c sees Mesa's VideoOut handle and its 119.88 Hz restore.
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" "${flags[@]}" \
    --version-script "$work/exports.map" --exclude-libs=ALL --no-dynamic-linker --gc-sections \
    --wrap=sceVideoOutOpen --wrap=sceVideoOutConfigureOutput \
    -e _start -o "$work/llvm-pie.elf" \
    "$build/CMakeFiles/nfsu1_start.dir/src/main_ps5.c.o" \
    "$build/CMakeFiles/nfsu1_start.dir/src/native_start.c.o" "$(find "$build/CMakeFiles/nfsu1_start.dir" -name vulkan_shim.c.o -print -quit)" "$work/obj/cpp-runtime.o" \
    "$build/CMakeFiles/nfsu1_port.dir/src/port_menu.cpp.o" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    --start-group "$build/libnfsu1_game.a" "$build/libnfsu1_runtime.a" "$build/libnfsu1_host.a" "$build/libnfsu1_port.a" \
    "$root/build/sdl3/libSDL3.a" "$root/build/dav1d/obj/src/libdav1d.a" "${shaders[@]}" "${inputs[@]}" \
    "$sdk/target/lib/libc.a" "$sdk/target/lib/libpthread.a" --end-group \
    --as-needed "${stubs[@]}"
llvm-readelf-18 --dyn-syms --wide "$work/llvm-pie.elf" > "$work/imports.txt"
llvm-readelf-18 --program-headers --wide "$work/llvm-pie.elf" > "$work/segments.txt"
# The native TLS offset is verified only for PT_TLS align 0x20 (see the sed
# above); a TLS variable with _Alignas(>32) would raise it. Fail, not ship.
tls_align=$(awk '$1 == "TLS" { print $NF }' "$work/segments.txt")
[[ $tls_align == 0x20 ]] || { echo "link.sh: PT_TLS align '$tls_align', expected 0x20" >&2; exit 1; }
