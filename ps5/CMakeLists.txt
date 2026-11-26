cmake_minimum_required(VERSION 3.24)
project(nfsu1_ps5 C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_C_COMPILER_TARGET STREQUAL "x86_64-sie-ps5")
 message(FATAL_ERROR "Use cmake/ps5.cmake")
endif()
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(PS5_DEPS "${ROOT}/build/deps" CACHE PATH "PS5 dependencies")
set(PS5_SDL_BUILD "${ROOT}/build/sdl3" CACHE PATH "SDL3 build")
set(PS5_SHADER_BUILD "${ROOT}/build/shader-compiler" CACHE PATH "glslang build")
set(RT "${ROOT}/upstream/xboxrecomp/src")
set(GEN "${ROOT}/build/gen")
file(GLOB generated CONFIGURE_DEPENDS "${GEN}/*.c")
if(NOT generated)
 message(FATAL_ERROR "Run tools/prepare.py first")
endif()
set(runtime platform/win32_compat.c kernel/kernel_path.c kernel/kernel_pool.c kernel/kernel_rtl.c
 kernel/kernel_memory.c kernel/kernel_file.c kernel/kernel_thread.c kernel/kernel_sync.c kernel/kernel_hal.c
 kernel/kernel_xbox.c kernel/kernel_ob.c kernel/kernel_io.c kernel/kernel_crypto.c kernel/kernel_thunks.c
 kernel/kernel_bridge.c kernel/guest_vmem.c kernel/xbox_memory_layout.c kernel/icall_feedback.c
 kernel/recomp_trace.c kernel/nv2a_pb_scan.c kernel/nv2a_pb_exec.c kernel/xbox_devbus.c
 audio/dsound_device.c audio/wma_decoder.c apu/apu_core.c apu/apu_vp.c apu/apu_dsp.c apu/apu_mmio_hook.c
 usb/ohci.c usb/usb_gamepad.c video/video_player.c video/video_pump.c video/fb_present.c d3d/d3d8_null.c
 nv2a/nv2a_core.c nv2a/nv2a_pgraph_d3d11.c nv2a/nv2a_pb_replay.c nv2a/nv2a_mmio_hook.c nv2a/nv2a_pb_test.c
 nv2a_vk/nv2a_vk.c nv2a_gl/gl_vsh.c nv2a_gl/gl_psh.c)
list(TRANSFORM runtime PREPEND "${RT}/")
add_library(nfsu1_runtime STATIC ${runtime})
add_library(nfsu1_game STATIC "${ROOT}/upstream/src/main.c" "${ROOT}/upstream/src/recomp_manual.c"
 "${ROOT}/upstream/src/nfsu1_extras.c" ${generated})
add_library(nfsu1_host STATIC src/memory_ps5.c src/input_ps5.c src/pad_map.c src/audio_ps5.c src/audio_native.c src/port_ui.c src/sha256.c src/xbe_check.c src/hd_textures.c src/fmv_replace.c src/fmv_video.c src/disc_image.c src/ui_text.c src/screen_ps5.c src/video_hz.c src/prof_ps5.c src/gear_pulse_ps5.c)
set(IMGUI "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
add_library(nfsu1_port STATIC src/port_menu.cpp "${IMGUI}/imgui.cpp" "${IMGUI}/imgui_draw.cpp"
 "${IMGUI}/imgui_tables.cpp" "${IMGUI}/imgui_widgets.cpp" "${IMGUI}/backends/imgui_impl_vulkan.cpp")
target_compile_definitions(nfsu1_port PRIVATE NFSU1_PS5=1 NFSU2_VULKAN=1 IMGUI_DISABLE_FILE_FUNCTIONS
 IMGUI_DISABLE_OBSOLETE_FUNCTIONS "NFSU1_PORT_FONT=\"${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/Roboto-Medium.ttf\""
 "NFSU1_WHATS_NEW=\"${CMAKE_CURRENT_SOURCE_DIR}/release/whats_new.txt\"")
target_compile_options(nfsu1_port PRIVATE -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
target_include_directories(nfsu1_port PRIVATE compat src "${IMGUI}" "${RT}/platform" "${RT}/nv2a_vk" "${ROOT}/upstream/src"
 "${ROOT}/upstream/xboxrecomp/include" "${RT}" "${PS5_DEPS}/PS5_Mesa/include"
 "${PS5_DEPS}/PS5_Vulkan/.deps/native/ps5-payload-sdk/target/include")
set_source_files_properties(src/port_menu.cpp PROPERTIES OBJECT_DEPENDS
 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/Roboto-Medium.ttf;${CMAKE_CURRENT_SOURCE_DIR}/release/whats_new.txt")
add_library(nfsu1_start OBJECT src/native_start.c src/main_ps5.c "${ROOT}/build/vulkan_shim.c")
foreach(t nfsu1_runtime nfsu1_game nfsu1_host nfsu1_start)
 target_compile_definitions(${t} PRIVATE NFSU1_PS5=1 NFSU1_PS5_DIRECT_MEMORY=1 NFSU2_VULKAN=1)
 # Native TLS: guest registers are __thread; -femulated-tls made each access a libkernel call.
 target_compile_options(${t} PRIVATE -fno-emulated-tls -ftls-model=local-exec -O2 -march=znver2 -fwrapv -fsigned-char -ffp-contract=off -fno-strict-aliasing -ffunction-sections -fdata-sections -Wno-format -Wno-pointer-sign -Wno-parentheses-equality -Wno-unused-value)
 target_include_directories(${t} PRIVATE compat src "${ROOT}/upstream/src" "${GEN}"
 "${ROOT}/upstream/xboxrecomp/include" "${RT}" "${RT}/platform" "${RT}/kernel" "${RT}/apu"
 "${RT}/nv2a" "${RT}/nv2a_vk" "${RT}/nv2a_gl" "${RT}/input" "${RT}/video" "${RT}/audio" "${RT}/d3d" "${RT}/usb"
 "${PS5_DEPS}/PS5_Mesa/include" "${PS5_DEPS}/sdl3/include" "${PS5_DEPS}/glslang"
 "${PS5_DEPS}/PS5_Vulkan/.deps/native/ps5-payload-sdk/target/include")
endforeach()
target_include_directories(nfsu1_host PRIVATE "${PS5_DEPS}/dav1d/include" "${ROOT}/build/dav1d/obj/include")
# The lifted game: -O2 and the PS5's Zen 2 (-march=znver2). Measured on the
# console (2026-11-26, Quick Race at 120 fps): see docs/PERFORMANCE.md.
set_source_files_properties(${generated} PROPERTIES COMPILE_OPTIONS "-O2;-march=znver2;-w")
# .text (recomp_0000..0047, guest 0x11000..0x1CD480: the game and the CRT,
# XAPI and maths libraries linked into it) with plain guest-memory accesses
# instead of volatile ones (RECOMP_MEM_VOLATILE, see recomp_types.h), so the
# compiler can keep values in registers. Every poll loop the spin-hint pass
# marks is in the D3D/DSOUND sections (0048-0051), which keep volatile; .text
# hands data to them through calls into other files, which order its stores,
# and its interlocked operations are atomics of their own.
foreach(f ${generated})
 get_filename_component(n "${f}" NAME_WE)
 string(REGEX MATCH "[0-9]+$" k "${n}")
 if(k AND k LESS_EQUAL 47)
  set_property(SOURCE "${f}" APPEND PROPERTY COMPILE_DEFINITIONS "RECOMP_MEM_VOLATILE=")
 endif()
endforeach()
