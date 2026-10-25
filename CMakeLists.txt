# =============================================================================
# unified CMake build for all targets.
#
#   Native Linux (default):
#     cmake -S . -B build/native && cmake --build build/native -j
#       -> build/native/cod2_linux   build/native/cod2_lnxded
#
#   Web (WebAssembly, requires the Emscripten SDK):
#     emcmake cmake -S . -B build/web && cmake --build build/web -j
#       -> build/web/cod2.html (+ cod2.js / cod2.wasm / cod2_fs.js)
#
#   Windows (MinGW cross-compile):
#     cmake -S . -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake
#     cmake --build build/win32 -j
#       -> build/win32/cod2_win32_ded.exe          (headless, default)
#     add -DCOD2_WIN32_CLIENT=ON for the SDL2/GL client (needs static SDL2 libs).
#
# compile_commands.json is emitted for IDE/clangd use.
# =============================================================================
cmake_minimum_required(VERSION 3.16)

# -m32 must be in effect when the native compiler is probed (multilib link
# tests). Seed it ONLY for the plain native build — the MinGW (i686) and
# Emscripten toolchains are 32-bit/wasm already and must not see -m32.
if(NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_C_FLAGS_INIT   "-m32")
  set(CMAKE_ASM_FLAGS_INIT "-m32")
endif()

project(cod2 LANGUAGES C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 99)

# git short hash for the crash-reporter build stamp.
execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD
                OUTPUT_VARIABLE COD2_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT COD2_GIT_HASH)
  set(COD2_GIT_HASH "unknown")
endif()

# Common warning-demotions used on every toolchain (reconstructed C trips these).
set(COD2_WNO
  -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration
  -Wno-error=implicit-int -Wno-error=int-conversion -Wno-error=return-mismatch)

set(COD2_SRC_DIR ${CMAKE_SOURCE_DIR}/src)

# =============================================================================
if(EMSCRIPTEN)
# ----------------------------------------------------------------------------
# WEB (WebAssembly via Emscripten)
# ----------------------------------------------------------------------------
option(COD2_WEB_WASMFS  "OPFS-backed WASMFS variant (-pthread; needs COOP/COEP)" OFF)
option(COD2_WEB_WORKERFS "True zero-copy WORKERFS variant (-pthread; needs COOP/COEP)" OFF)
option(COD2_WEB_SAFE_HEAP "Check WASM memory accesses while diagnosing runtime faults" OFF)
option(COD2_WEB_TESTING "Export command and dvar access for local integration tests" OFF)

set(WEB_GEN ${CMAKE_SOURCE_DIR}/build/web_gen)

set(WEB_GEN_C
  ${WEB_GEN}/wasm_asm_replacements.c ${WEB_GEN}/bss.c
  ${WEB_GEN}/data.c ${WEB_GEN}/link_stubs.c
  ${WEB_GEN}/missing_c_syms.c ${WEB_GEN}/import_pointers.c)

# --- web source set (mirrors WEB_SRCS) ---------------------------------------
file(GLOB_RECURSE WEB_PC_C    RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/PC/*.c")
file(GLOB_RECURSE WEB_MAC_C   RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/Mac/*.c")
file(GLOB_RECURSE WEB_STUBS_C RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/stubs/*.c")
file(GLOB         WEB_ROOT_C  RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/*.c")
set(WEB_C ${WEB_PC_C} ${WEB_MAC_C} ${WEB_STUBS_C} ${WEB_ROOT_C})
# native asm-data .c, and the blobs replaced by the generated web versions.
list(FILTER WEB_C EXCLUDE REGEX "^src/(data|import_pointers|literals|bss|rodata|strings)\\.c$")
list(FILTER WEB_C EXCLUDE REGEX "^src/stubs/(link_stubs|missing_c_syms)\\.c$")
# Miles/AIL driver is replaced by src/web/web_snd_driver.c (Web Audio) below.
list(FILTER WEB_C EXCLUDE REGEX "^src/PC/win32/snd_driver\\.c$")
# the Linux platform layer + bundled zlib + web glue are added explicitly.
list(APPEND WEB_C
  src/unix/linux_common.c src/unix/linux_input.c src/unix/linux_net.c src/unix/sysdiff_statehash.c
  src/PC/zlib/adler32.c src/PC/zlib/infblock.c src/PC/zlib/infcodes.c src/PC/zlib/inffast.c
  src/PC/zlib/inflate.c src/PC/zlib/inftrees.c src/PC/zlib/infutil.c src/PC/zlib/zutil.c
  src/web/webgl2_compat.c src/web/wasm_aliases.c src/web/cod2_wasmfs_mount.c src/web/builtin_engine_assets.c src/web/web_vid_scale.c src/web/web_master.c src/web/web_snd_driver.c src/web/web_net.c
  src/web/cgevent_keepalive.c)

add_executable(cod2 ${WEB_C} ${WEB_GEN_C})
target_compile_definitions(cod2 PRIVATE WEB_BUILD=1 SDL_DISABLE_IMMINTRIN_H)
target_include_directories(cod2 PRIVATE
  ${COD2_SRC_DIR}/PC/speex ${COD2_SRC_DIR}/PC/zlib ${COD2_SRC_DIR}
  ${COD2_SRC_DIR}/headers ${CMAKE_SOURCE_DIR})
# -O0 + PROXY_TO_PTHREAD + glTexImage2D hits emscripten mailbox unaligned-access
# (github.com/emscripten-core/emscripten/issues/23963). Use -O1 for web.
target_compile_options(cod2 PRIVATE -w -O2 -msimd128 -sUSE_SDL=2 ${COD2_WNO})
set_source_files_properties(src/PC/qcommon/crash_handler.c PROPERTIES
  COMPILE_DEFINITIONS "COD2_GIT_HASH=\"${COD2_GIT_HASH}\"")

set(WEB_LINK
  -O2 -g2
  -sUSE_SDL=2 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB -sSTACK_SIZE=5242880
  -sFORCE_FILESYSTEM=1 -sASSERTIONS=1 -sEXIT_RUNTIME=0 -sERROR_ON_UNDEFINED_SYMBOLS=0
  -sWARN_ON_UNDEFINED_SYMBOLS=0 -sMALLOC=emmalloc -sSUPPORT_LONGJMP=emscripten
  -sEXPORTED_RUNTIME_METHODS=callMain,FS,ccall,UTF8ToString
  --shell-file ${COD2_SRC_DIR}/web/shell_nzp.html
  -Wl,--allow-multiple-definition
  -Wl,--allow-undefined
  -Wl,--no-fatal-warnings)
if(COD2_WEB_SAFE_HEAP)
  # The original x86 layouts include unaligned accesses, valid in WASM.
  list(APPEND WEB_LINK -sSAFE_HEAP=2 -sSTACK_OVERFLOW_CHECK=2)
endif()
if(COD2_WEB_TESTING)
  list(APPEND WEB_LINK -sEXPORTED_FUNCTIONS=_main,_Cbuf_AddText,_Dvar_GetVariantString,_CL_GetLocalClientActiveCount)
  target_sources(cod2 PRIVATE src/web/web_test.c)
endif()
if(COD2_WEB_WASMFS)
  # OPFS ProxyWorker cannot start on the browser main thread. Run main() on a
  # worker via PROXY_TO_PTHREAD. Prefer OFFSCREEN_FRAMEBUFFER (proxy present to
  # the DOM canvas) over OFFSCREENCANVAS transfer — SDL2+OffscreenCanvas often
  # yields a black screen.
  target_compile_options(cod2 PRIVATE -DWEB_WASMFS -pthread)
  list(APPEND WEB_LINK -sWASMFS -pthread -sPTHREAD_POOL_SIZE=4 -sPROXY_TO_PTHREAD=1
       -sOFFSCREEN_FRAMEBUFFER=1
       -sEXPORTED_FUNCTIONS=_main,_cod2_mount_opfs,_CL_WebMaster_Begin,_CL_WebMaster_AddServer,_CL_WebMaster_UpdatePing,_CL_WebMaster_End)
elseif(COD2_WEB_WORKERFS)
  target_compile_options(cod2 PRIVATE -pthread)
  list(APPEND WEB_LINK -pthread -sPTHREAD_POOL_SIZE=4 -sPROXY_TO_PTHREAD=1
       -sOFFSCREENCANVAS_SUPPORT=1 "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas" -lworkerfs.js
       -sEXPORTED_FUNCTIONS=_main,_CL_WebMaster_Begin,_CL_WebMaster_AddServer,_CL_WebMaster_UpdatePing,_CL_WebMaster_End)
else()
  list(APPEND WEB_LINK -lidbfs.js)   # legacy MEMFS default (matches `make web`)
endif()
# SHELL: keeps duplicate --post-js flags (CMake otherwise de-dupes the flag).
target_link_options(cod2 PRIVATE ${WEB_LINK}
  "SHELL:--js-library ${COD2_SRC_DIR}/web/library_cod2_vid.js"
  "SHELL:--post-js ${COD2_SRC_DIR}/web/gl_proxied_frame_fix.js"
  "SHELL:--post-js ${COD2_SRC_DIR}/web/web_master.js"
  "SHELL:--post-js ${COD2_SRC_DIR}/web/web_net.js"
  "SHELL:--post-js ${COD2_SRC_DIR}/web/web_audio.js")
set_target_properties(cod2 PROPERTIES SUFFIX ".html"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# the browser-side loader sits next to cod2.html (the shell loads it as a sibling).
add_custom_command(TARGET cod2 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy ${COD2_SRC_DIR}/web/cod2_fs.js ${CMAKE_BINARY_DIR}/cod2_fs.js
  COMMAND ${CMAKE_COMMAND} -E copy ${COD2_SRC_DIR}/web/remote-config.js ${CMAKE_BINARY_DIR}/remote-config.js
  COMMAND ${CMAKE_COMMAND} -E copy ${COD2_SRC_DIR}/web/cod2-icon.png ${CMAKE_BINARY_DIR}/cod2-icon.png
  COMMAND ${CMAKE_COMMAND} -E copy ${COD2_SRC_DIR}/web/favicon.png ${CMAKE_BINARY_DIR}/favicon.png
  COMMAND ${CMAKE_COMMAND} -E copy ${COD2_SRC_DIR}/web/apple-touch-icon.png ${CMAKE_BINARY_DIR}/apple-touch-icon.png
  COMMAND ${CMAKE_COMMAND} -E env python3 ${CMAKE_SOURCE_DIR}/tools/patch_web_proxy_receive.py ${CMAKE_BINARY_DIR}/cod2.js)

# =============================================================================
elseif(WIN32)
# ----------------------------------------------------------------------------
# WINDOWS (MinGW cross-compile: cod2_win32_ded.exe / cod2_win32.exe)
# ----------------------------------------------------------------------------
option(COD2_WIN32_CLIENT "SDL2/GL client (needs static SDL2 libs) instead of the headless server" OFF)
option(COD2_WIN32_REAL_D3D9 "Real IDirect3DDevice9 renderer path (Wine d3d9.dll)" OFF)

# win32 C flags (mirror Makefile.win32 WCFLAGS); -fcommon is essential (tentative
# BSS defs in both bss.c and the reconstructed .c rely on the pre-GCC-10 merge).
add_compile_options(
  "$<$<COMPILE_LANGUAGE:C>:-w;-g;-msse;-fcommon;-DSDL_DISABLE_IMMINTRIN_H>"
  "$<$<COMPILE_LANGUAGE:C>:-include;${COD2_SRC_DIR}/headers/win32_compat.h>"
  "$<$<COMPILE_LANGUAGE:C>:-D_GNU_SOURCE=1;-DWIN32;-D_WIN32>"
  "$<$<COMPILE_LANGUAGE:C>:${COD2_WNO}>"
  "$<$<COMPILE_LANGUAGE:ASM>:-m32>")
include_directories(${COD2_SRC_DIR}/PC/speex ${COD2_SRC_DIR} ${COD2_SRC_DIR}/headers ${COD2_SRC_DIR}/win32/shims ${CMAKE_SOURCE_DIR})

# PE link: same seam aliases as Linux, but every C symbol carries PE's leading '_'.
set(COD2_WIN_LINK
  -static -static-libgcc -static-libstdc++
  -Wl,--allow-multiple-definition -Wl,--wrap,R_Error
  -Wl,--defsym,___mh_execute_header=0x1000
  -Wl,--defsym,_level_ptr=_level
  -Wl,--defsym,_g_entities_ptr=_g_entities
  -Wl,--defsym,_scr_const_ptr=_scr_const
  -Wl,--defsym,_playerCorpseInfo_ptr=_g_scr_data
  -Wl,--defsym,_g_renderer_ptr=_re
  -Wl,--defsym,_scrAnimPub_ptr=_scrAnimPub
  -Wl,--defsym,_scrCompPub_ptr=_scrCompilePub
  -Wl,--defsym,_scrParserPub_ptr=_scrParserPub
  -Wl,--defsym,_r_frontEndData_ptr=_rg
  -Wl,--defsym,_r_sys_ptr=_ri
  -Wl,--defsym,_r_limits_ptr=_vidConfig
  -Wl,--defsym,_sv_ptr=_sv
  -Wl,--defsym,_svs_ptr=_svs
  -Wl,--defsym,_cg_globUI=_legacyHacks
  -Wl,--defsym,_g_time=_imp_level_bgs
  -Wl,--defsym,_g_time_ptr=_imp_bgs)
set(COD2_WIN_LIBS ws2_32 winmm z stdc++ winpthread dbghelp)

if(COD2_WIN32_CLIENT)
  add_compile_definitions(W32_CLIENT)
  include_directories(${COD2_SRC_DIR}/win32/sdl2/include)
  list(APPEND COD2_WIN_LINK -L${COD2_SRC_DIR}/win32/sdl2/lib)
  list(APPEND COD2_WIN_LIBS SDL2 opengl32 imm32 ole32 oleaut32 version uuid
       setupapi shell32 dinput8 gdi32 advapi32 user32 kernel32 winmm ws2_32)
  set(W32_EXE cod2_win32)
else()
  add_compile_definitions(DEDICATED)
  set(W32_EXE cod2_win32_ded)
endif()
if(COD2_WIN32_REAL_D3D9)
  add_compile_definitions(GFX_REAL_D3D9)
endif()

# source set (mirror WIN_C_SRCS): src/PC, src/Mac, src/stubs, src/win32, and
# src-root files, minus the native-asm
# data .c, cpp_trampoline, agl_stubs, and the bundled-zlib inflate split (use -lz).
file(GLOB_RECURSE W_PC_C    CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/PC/*.c")
file(GLOB_RECURSE W_MAC_C   CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/Mac/*.c")
file(GLOB_RECURSE W_STUBS_C CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/stubs/*.c")
file(GLOB_RECURSE W_WIN_C   CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/win32/*.c")
file(GLOB         W_ROOT_C  CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/*.c")
set(WIN_C ${W_PC_C} ${W_MAC_C} ${W_STUBS_C} ${W_WIN_C} ${W_ROOT_C})
list(FILTER WIN_C EXCLUDE REGEX "^src/(data|import_pointers|literals)\\.c$")
list(FILTER WIN_C EXCLUDE REGEX "^src/stubs/(cpp_trampoline|agl_stubs)\\.c$")
list(FILTER WIN_C EXCLUDE REGEX "^src/PC/zlib/(inflate|infblock|infcodes|inffast|inftrees|infutil|adler32|zutil)\\.c$")
list(APPEND WIN_C src/blobs/bss.c src/unix/sysdiff_statehash.c)
if(COD2_WIN32_CLIENT)
  list(APPEND WIN_C src/unix/linux_input.c)   # pure-SDL2 event pump, works on win32
else()
  list(REMOVE_ITEM WIN_C src/win32/gl_loader.c)  # client-only (real GL loader)
endif()

# ELF .S data blobs -> PE/COFF .S, then assemble (data/literals/import_pointers/vtables).
set(W32GEN ${CMAKE_SOURCE_DIR}/build/win_gen)
set(WIN_ASM_GEN ${W32GEN}/data.win.S ${W32GEN}/literals.win.S ${W32GEN}/import_pointers.win.S)
set_source_files_properties(${WIN_ASM_GEN} PROPERTIES LANGUAGE ASM)

# PE link aliases for C++ symbols stored in the (C-compiled) data blobs: the C
# compiler prefixes them to '___ZN...' (common syms, no address until link), so
# we point the '__ZN...' references at them with --defsym (an asm .set can't
# bind a common symbol). Generated at configure time from the blob sources.
file(STRINGS ${W32GEN}/cxx_defsyms_c.txt COD2_CXX_DEFSYMS_C)
file(STRINGS ${W32GEN}/cxx_defsyms_asm.txt COD2_CXX_DEFSYMS_ASM)
foreach(pair ${COD2_CXX_DEFSYMS_C} ${COD2_CXX_DEFSYMS_ASM})
  list(APPEND COD2_WIN_LINK -Wl,--defsym,${pair})
endforeach()

set_source_files_properties(src/PC/qcommon/crash_handler.c PROPERTIES
  COMPILE_DEFINITIONS "COD2_GIT_HASH=\"${COD2_GIT_HASH}\"")

add_executable(${W32_EXE} ${WIN_C} ${WIN_ASM_GEN})
# The reconstructed symbol aliases exceed Windows' command-line limit. Put
# driver options in a response file as well as the object list emitted by Ninja.
string(REPLACE ";" "\n" COD2_WIN_LINK_RESPONSE "${COD2_WIN_LINK}")
file(WRITE "${CMAKE_BINARY_DIR}/cod2-win-link.rsp" "${COD2_WIN_LINK_RESPONSE}\n")
target_link_options(${W32_EXE} PRIVATE "@${CMAKE_BINARY_DIR}/cod2-win-link.rsp")
target_link_libraries(${W32_EXE} PRIVATE ${COD2_WIN_LIBS})
set_target_properties(${W32_EXE} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} SUFFIX ".exe")

# Optional, experimental: build the renderer as a swap-in DLL plus a DLL-loading
# engine exe. Only meaningful for the win32 client; built explicitly
# (cmake --build build-win32 --target gfxdll), never part of `all`.
#   -> gfx_d3d_mp_x86_s.dll        (renderer DLL, exports GetRefAPI)
#   -> cod2_win32_gfxdll.exe       (engine that LoadLibrary's it at runtime)
# The renderer<->engine bridge is PRE-GENERATED and committed under build/gfxdll/:
# lean_*.win.S are the engine data blobs sliced down to only the closure the
# renderer references (the rest -- and the ~2200 function refs that were no-op
# stubs -- dropped), plus the thunks/stubs/data/glue/defsyms for that lean
# boundary. Pure compile+link -- no scripts or build-time tools.
if(COD2_WIN32_CLIENT)
  file(GLOB GFXDLL_RENDERER CONFIGURE_DEPENDS
    ${COD2_SRC_DIR}/PC/gfx_d3d/*.c ${COD2_SRC_DIR}/Mac/DirectX_9/*.c)
  set(GFXDLL_UTIL
    ${COD2_SRC_DIR}/PC/qcommon/q_math.c       ${COD2_SRC_DIR}/PC/universal/assertive.c
    ${COD2_SRC_DIR}/PC/universal/com_convexhull.c ${COD2_SRC_DIR}/PC/universal/com_math.c
    ${COD2_SRC_DIR}/PC/universal/com_memory.c ${COD2_SRC_DIR}/PC/universal/com_shared.c
    ${COD2_SRC_DIR}/PC/universal/fft.c        ${COD2_SRC_DIR}/PC/universal/q_parse.c
    ${COD2_SRC_DIR}/PC/universal/q_shared.c   ${COD2_SRC_DIR}/PC/universal/surfaceflags.c
    ${COD2_SRC_DIR}/stubs/cpp_compat.c        ${COD2_SRC_DIR}/stubs/link_stubs.c
    ${COD2_SRC_DIR}/stubs/macos_compat.c      ${COD2_SRC_DIR}/stubs/material_state_maps.c
    ${COD2_SRC_DIR}/stubs/material_tech_names.c ${COD2_SRC_DIR}/stubs/missing_c_syms.c
    ${COD2_SRC_DIR}/win32/gl_loader.c         ${COD2_SRC_DIR}/blobs/bss.c)
  add_library(gfx_d3d_mp_x86_s SHARED
    build/gfxdll/force_thunks.s build/gfxdll/printfwd.c
    build/gfxdll/cpp_abi.s      build/gfxdll/glue.c
    ${GFXDLL_RENDERER} ${GFXDLL_UTIL}
    build/gfxdll/lean_data.win.S build/gfxdll/lean_literals.win.S
    build/gfxdll/lean_import_pointers.win.S
    build/gfxdll/thunks.s build/gfxdll/stubs.c build/gfxdll/data.c)
  set_source_files_properties(
    build/gfxdll/force_thunks.s build/gfxdll/cpp_abi.s build/gfxdll/thunks.s
    build/gfxdll/lean_data.win.S build/gfxdll/lean_literals.win.S
    build/gfxdll/lean_import_pointers.win.S
    PROPERTIES LANGUAGE ASM)
  file(STRINGS build/gfxdll/cxx_defsyms.txt GFXDLL_CXXDEF)
  set(GFXDLL_LINK -static -static-libgcc -Wl,--allow-multiple-definition
                  -Wl,--defsym,___mh_execute_header=0x1000)
  foreach(p ${GFXDLL_CXXDEF})
    list(APPEND GFXDLL_LINK -Wl,--defsym,${p})
  endforeach()
  execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=libstdc++.a
                  OUTPUT_VARIABLE GFX_LSCPP OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=libsupc++.a
                  OUTPUT_VARIABLE GFX_LSUP OUTPUT_STRIP_TRAILING_WHITESPACE)
  target_link_options(gfx_d3d_mp_x86_s PRIVATE ${GFXDLL_LINK}
    -L${COD2_SRC_DIR}/win32/sdl2/lib ${CMAKE_SOURCE_DIR}/build/gfxdll/gfxdll.def)
  target_link_libraries(gfx_d3d_mp_x86_s PRIVATE
    -Wl,-Bstatic SDL2 -Wl,-Bdynamic
    opengl32 gdi32 user32 kernel32 winmm imm32 ole32 oleaut32 version uuid
    setupapi shell32 dinput8 advapi32 hid ws2_32 ${GFX_LSCPP} ${GFX_LSUP})
  set_target_properties(gfx_d3d_mp_x86_s PROPERTIES
    PREFIX "" SUFFIX ".dll" RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

  # the DLL-loading engine exe: the client, with the 3 loader TUs compiled
  # -DCOD2_GFX_DLL (CL_InitRef LoadLibrary's the DLL at runtime).
  add_library(gfxdll_loader OBJECT
    ${COD2_SRC_DIR}/PC/client_mp/cl_main_mp.c
    ${COD2_SRC_DIR}/PC/win32/gfx_v60_threads.c
    ${COD2_SRC_DIR}/PC/win32/gfx_dll_v60.c)
  target_compile_definitions(gfxdll_loader PRIVATE COD2_GFX_DLL)
  set(WIN_C_GFXDLL ${WIN_C})
  list(REMOVE_ITEM WIN_C_GFXDLL
    src/PC/client_mp/cl_main_mp.c src/PC/win32/gfx_v60_threads.c src/PC/win32/gfx_dll_v60.c)
  add_executable(cod2_win32_gfxdll
    ${WIN_C_GFXDLL} ${WIN_ASM_GEN} $<TARGET_OBJECTS:gfxdll_loader>)
  target_link_options(cod2_win32_gfxdll PRIVATE ${COD2_WIN_LINK})
  target_link_libraries(cod2_win32_gfxdll PRIVATE ${COD2_WIN_LIBS})
  set_target_properties(cod2_win32_gfxdll PROPERTIES
    SUFFIX ".exe" RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

  add_custom_target(gfxdll DEPENDS gfx_d3d_mp_x86_s cod2_win32_gfxdll)
endif()

# =============================================================================
else()
# ----------------------------------------------------------------------------
# NATIVE Linux (cod2_linux + cod2_lnxded)
# ----------------------------------------------------------------------------
option(COD2_DEBUG "-O0, no hardening (gdb / canary-UB work; mirrors DEBUG=1)" OFF)
set(COD2_FEATURE_CFLAGS "" CACHE STRING "Pass-through feature gates, e.g. -DCOD2_PATCH_13=1")
set(COD2_WWWDL_LIBS "-l:libcurl.so.4" CACHE STRING "1.3 redirected-download backend lib")
set(COD2_LIBSTDCPP "/usr/lib32/libstdc++.so.6" CACHE STRING "32-bit libstdc++")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SDL2 sdl2)
endif()
if(NOT SDL2_FOUND)
  set(SDL2_INCLUDE_DIRS /usr/include/SDL2)
  set(SDL2_LIBRARIES SDL2)
endif()

if(COD2_DEBUG)
  set(COD2_OPT_FLAGS -O0)
  set(COD2_HARDEN_CFLAGS "")
  set(COD2_HARDEN_LDFLAGS "")
else()
  set(COD2_OPT_FLAGS
      -O2 -fno-strict-aliasing -fwrapv -fno-strict-overflow
      -fno-delete-null-pointer-checks -mincoming-stack-boundary=2 -D_FORTIFY_SOURCE=2)
  set(COD2_HARDEN_CFLAGS -fstack-protector-strong -fstack-clash-protection)
  set(COD2_HARDEN_LDFLAGS -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
endif()

separate_arguments(COD2_FEATURE_LIST UNIX_COMMAND "${COD2_FEATURE_CFLAGS}")

add_compile_options(
  "$<$<COMPILE_LANGUAGE:C>:-m32;-fno-pie;-no-pie;-w;-g;-msse;-fno-omit-frame-pointer>"
  "$<$<COMPILE_LANGUAGE:C>:-DSDL_DISABLE_IMMINTRIN_H>"
  "$<$<COMPILE_LANGUAGE:C>:${SDL2_CFLAGS_OTHER}>"
  "$<$<COMPILE_LANGUAGE:C>:${COD2_OPT_FLAGS}>"
  "$<$<COMPILE_LANGUAGE:C>:${COD2_WNO}>"
  "$<$<COMPILE_LANGUAGE:C>:${COD2_HARDEN_CFLAGS}>"
  "$<$<COMPILE_LANGUAGE:C>:${COD2_FEATURE_LIST}>"
)
include_directories(${COD2_SRC_DIR}/PC/speex ${COD2_SRC_DIR} ${COD2_SRC_DIR}/headers ${CMAKE_SOURCE_DIR} ${SDL2_INCLUDE_DIRS})

add_link_options(-m32 -no-pie -L/usr/lib32 -L/usr/lib/i386-linux-gnu -rdynamic ${COD2_HARDEN_LDFLAGS})

set(COD2_LINK_EXTRA
  -Wl,--wrap,R_Error
  -Wl,--defsym,__mh_execute_header=0x1000
  -Wl,--defsym,level_ptr=level
  -Wl,--defsym,g_entities_ptr=g_entities
  -Wl,--defsym,scr_const_ptr=scr_const
  -Wl,--defsym,playerCorpseInfo_ptr=g_scr_data
  -Wl,--defsym,g_renderer_ptr=re
  -Wl,--defsym,scrAnimPub_ptr=scrAnimPub
  -Wl,--defsym,scrCompPub_ptr=scrCompilePub
  -Wl,--defsym,scrParserPub_ptr=scrParserPub
  -Wl,--defsym,r_frontEndData_ptr=rg
  -Wl,--defsym,r_sys_ptr=ri
  -Wl,--defsym,r_limits_ptr=vidConfig
  -Wl,--defsym,sv_ptr=sv
  -Wl,--defsym,svs_ptr=svs
  -Wl,--defsym,cg_globUI=legacyHacks
  -Wl,--defsym,g_time=imp_level_bgs
  -Wl,--defsym,g_time_ptr=imp_bgs
  -Wl,--defsym,methods=methods_003138c0)

set(COD2_LIBS GL X11 m pthread dl z ${COD2_LIBSTDCPP} ${SDL2_LIBRARIES})
if(COD2_WWWDL_LIBS)
  separate_arguments(COD2_WWWDL_LIST UNIX_COMMAND "${COD2_WWWDL_LIBS}")
  list(APPEND COD2_LIBS ${COD2_WWWDL_LIST})
endif()

file(GLOB_RECURSE COD2_PC_C    CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/PC/*.c")
file(GLOB_RECURSE COD2_MAC_C   CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/Mac/*.c")
file(GLOB_RECURSE COD2_STUBS_C CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/stubs/*.c")
file(GLOB         COD2_ROOT_C  CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/*.c")
set(ALL_C ${COD2_PC_C} ${COD2_MAC_C} ${COD2_STUBS_C} ${COD2_ROOT_C})
file(GLOB_RECURSE COD2_PC_ASM    CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/PC/*.S")
file(GLOB_RECURSE COD2_STUBS_ASM CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/stubs/*.S")
file(GLOB         COD2_ROOT_ASM  CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR} "${COD2_SRC_DIR}/*.S")
set(ALL_ASM ${COD2_PC_ASM} ${COD2_STUBS_ASM} ${COD2_ROOT_ASM})

list(FILTER ALL_C EXCLUDE REGEX "(^|/)(inflate|infblock|infcodes|inffast|inftrees|infutil|adler32|zutil)\\.c$")
list(FILTER ALL_C EXCLUDE REGEX "^src/(data|import_pointers|literals)\\.c$")
list(FILTER ALL_ASM EXCLUDE REGEX "^src/(data|import_pointers|literals|vtables)\\.S$|^src/stubs/cpp_trampoline\\.S$")

list(APPEND ALL_C
  src/blobs/bss.c src/unix/linux_common.c src/unix/linux_input.c src/unix/linux_net.c src/unix/sysdiff_statehash.c)

set(GEN_DIR ${CMAKE_SOURCE_DIR}/build/native_gen)
if(NOT EXISTS ${GEN_DIR}/import_pointers_native.c)
  message(FATAL_ERROR "missing ${GEN_DIR}/import_pointers_native.c (a prebuilt DATA_PORTABLE artifact)")
endif()
set(GEN_C ${GEN_DIR}/data32.c ${GEN_DIR}/literals32.c ${GEN_DIR}/import_pointers_native.c)

set_source_files_properties(src/PC/qcommon/crash_handler.c PROPERTIES
  COMPILE_DEFINITIONS "COD2_GIT_HASH=\"${COD2_GIT_HASH}\"")

set(COD2_DED_GUARDED src/Mac/Main/mac_main.c src/PC/qcommon/common.c)
list(REMOVE_ITEM ALL_C ${COD2_DED_GUARDED})

add_library(cod2_objs OBJECT ${ALL_C} ${ALL_ASM} ${GEN_C})

add_executable(cod2_linux $<TARGET_OBJECTS:cod2_objs> ${COD2_DED_GUARDED})
target_link_libraries(cod2_linux PRIVATE ${COD2_LIBS})
target_link_options(cod2_linux PRIVATE ${COD2_LINK_EXTRA})

add_executable(cod2_lnxded $<TARGET_OBJECTS:cod2_objs> ${COD2_DED_GUARDED})
target_compile_definitions(cod2_lnxded PRIVATE DEDICATED)
target_link_libraries(cod2_lnxded PRIVATE ${COD2_LIBS})
target_link_options(cod2_lnxded PRIVATE ${COD2_LINK_EXTRA})

set_target_properties(cod2_linux cod2_lnxded PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

endif()
