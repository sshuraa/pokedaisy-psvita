# PokeDaisy for the PS Vita: libmgba (the submodule, built by its own CMake for the Vita) + the 3DS port's portable
# companion core and session + main.c, packed into a .vpk. See README.md.
#
#   cmake -S vita -B build-vita && cmake --build build-vita     (VITASDK set, as VitaSDK's own install leaves it)
cmake_minimum_required(VERSION 3.16)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Set VITASDK to your VitaSDK install (https://vitasdk.org)")
    endif()
endif()

project(pokedaisy C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(MGBA_DIR "${REPO}/third_party/mgba")
set(VITA2D_DIR "${REPO}/third_party/libvita2d/libvita2d")
foreach(dir MGBA_DIR VITA2D_DIR)
    if(NOT EXISTS "${${dir}}")
        message(FATAL_ERROR "${${dir}} is missing - run: git submodule update --init third_party/mgba third_party/libvita2d")
    endif()
endforeach()

set(VITA_APP_NAME "PokeDaisy")
set(VITA_TITLEID "PKDY00001")
set(VITA_VERSION "00.91")
# The app's version, for the library's SETTINGS > APP.
file(STRINGS "${REPO}/app/build.gradle.kts" PD_VERSION_LINE REGEX "versionName = \"")
string(REGEX REPLACE ".*versionName = \"([^\"]+)\".*" "\\1" PD_VERSION "${PD_VERSION_LINE}")
set(PD_VITA_BUILD "v0.10" CACHE STRING "the Vita port's own build, after the app's version")

# --- libmgba: the same switches as the 3DS and Android builds, mGBA's own PS Vita platform code (PSP2) underneath ---
set(PSP2 ON)
add_definitions(-DPSP2)
foreach(off BUILD_SHARED BUILD_GL BUILD_GLES2 BUILD_GLES3 BUILD_PERF BUILD_TEST BUILD_SUITE BUILD_LIBRETRO BUILD_PYTHON
            BUILD_QT BUILD_SDL USE_DEBUGGERS USE_GDB_STUB USE_EDITLINE USE_FFMPEG USE_DISCORD_RPC USE_LZMA USE_LUA USE_ELF
            USE_SQLITE3 USE_LIBZIP USE_MINIZIP USE_PNG USE_ZLIB USE_EPOXY BUILD_LTO)
    set(${off} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(on BUILD_STATIC DISABLE_FRONTENDS DISABLE_DEPS M_CORE_GBA M_CORE_GB SKIP_INSTALL_ALL)
    set(${on} ON CACHE BOOL "" FORCE)
endforeach()
set(SKIP_LIBRARY OFF CACHE BOOL "" FORCE)
# One ROM buffer, the app's (main.c's romBuffer), patched in place - as the 3DS build has it. Without it mGBA copies the ROM
# the first time anything writes to it (the native mixer's hook, REUSABLE TMS ...) and frees the copy the companion reads:
# v0.1 crashed on the BAG tab's item icons that way. A compile definition on mGBA and the app (mGBA's own 3DS platform
# sets it the same way; its flags.h only reaches the Python bindings).
add_subdirectory("${MGBA_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/mgba" EXCLUDE_FROM_ALL)
# mGBA's Vita platform files go into the library by a glob (psp2-*.c): psp2-context.c is its own front end (vita2d, its
# GUI runner), not a platform service, and this app has its own.
get_target_property(MGBA_SOURCES mgba SOURCES)
list(FILTER MGBA_SOURCES EXCLUDE REGEX "psp2-context\\.c$")
set_target_properties(mgba PROPERTIES SOURCES "${MGBA_SOURCES}")
# ... and that front end's libraries (vita2d, the camera ...) go with it: the app links what it uses itself.
set_target_properties(mgba PROPERTIES LINK_LIBRARIES "" INTERFACE_LINK_LIBRARIES "")
# The app's EXPAND reads the renderer's 32-bit pixels (each carries its layer in the top byte), as on the 3DS.
get_target_property(MGBA_DEFS mgba COMPILE_DEFINITIONS)
if(MGBA_DEFS)
    list(REMOVE_ITEM MGBA_DEFS COLOR_16_BIT COLOR_5_6_5)
    set_target_properties(mgba PROPERTIES COMPILE_DEFINITIONS "${MGBA_DEFS}")
endif()
# blip_add_delta costs about the same for a step of nothing; main.c's pd_blip_delta skips those (as the 3DS build).
set_source_files_properties("${MGBA_DIR}/src/gba/audio.c" DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mgba"
    PROPERTIES COMPILE_DEFINITIONS "blip_add_delta=pd_blip_delta")
target_compile_options(mgba PRIVATE -O3 -Wno-format)
target_compile_definitions(mgba PRIVATE FIXED_ROM_BUFFER)
# Thread-locals: VitaSDK's GCC is built without TLS (it emulates them). The two in this build are only ever touched by the
# game's thread here - mGBA's mCoreThread context (this app runs the core itself) and pk_call.c's step cap (the 3DS's
# render core calls from another) - so they're plain statics.
set_source_files_properties("${MGBA_DIR}/src/core/thread.c" DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mgba"
    PROPERTIES COMPILE_DEFINITIONS "_Thread_local=")

# --- vita2d (GXM), the parts this app draws with: no PNG / JPEG / FreeType loaders ---
add_library(pd_vita2d STATIC
    "${VITA2D_DIR}/source/vita2d.c" "${VITA2D_DIR}/source/vita2d_texture.c" "${VITA2D_DIR}/source/vita2d_draw.c"
    "${VITA2D_DIR}/source/utils.c" "${VITA2D_DIR}/source/vita2d_pgf.c" "${VITA2D_DIR}/source/vita2d_pvf.c"
    "${VITA2D_DIR}/source/bin_packing_2d.c" "${VITA2D_DIR}/source/texture_atlas.c" "${VITA2D_DIR}/source/int_htab.c"
    "${VITA2D_DIR}/shader/compiled/clear_v_gxp.o" "${VITA2D_DIR}/shader/compiled/clear_f_gxp.o"
    "${VITA2D_DIR}/shader/compiled/color_v_gxp.o" "${VITA2D_DIR}/shader/compiled/color_f_gxp.o"
    "${VITA2D_DIR}/shader/compiled/texture_v_gxp.o" "${VITA2D_DIR}/shader/compiled/texture_f_gxp.o"
    "${VITA2D_DIR}/shader/compiled/texture_tint_f_gxp.o")
target_include_directories(pd_vita2d PUBLIC "${VITA2D_DIR}/include")
target_compile_options(pd_vita2d PRIVATE -O2 -w)

# --- the app ---
file(GLOB CORE_SOURCES "${REPO}/3ds/core/*.c")
file(GLOB HOST_SOURCES "${REPO}/3ds/host/*.c")
set(CPP "${REPO}/app/src/main/cpp")
# The themes: the app's (3ds/core/pd_games_gen.c) and the Vita's own, listed together by themes_looks_gen.c
# (tools/gen_themes.py), so the generated list is built under another name.
set_source_files_properties("${REPO}/3ds/core/pd_games_gen.c" PROPERTIES COMPILE_DEFINITIONS "pd_themes=pd_themes_3ds;pd_theme_count=pd_theme_count_3ds")
add_executable(pokedaisy.elf main.c video.c backdrop.c themes_gen.c sfx.c themes_looks_gen.c audio.c host.c emu_none.c ${CORE_SOURCES} ${HOST_SOURCES}
    "${CPP}/pk_host.c" "${CPP}/pk_call.c" "${CPP}/pk_wide.c" "${CPP}/pk_gb_audio.c" "${CPP}/pk_cheats.c" "${CPP}/pk_gb_field.c"
    "${CPP}/pk_wide_gb.c" "${CPP}/pk_flight.c"
    "${REPO}/linux/fly.c" "${REPO}/linux/flash.c" "${REPO}/linux/flight.c" "${REPO}/linux/tm_teach.c" "${REPO}/linux/field_items.c"
    "${REPO}/linux/gb_field.c" "${REPO}/linux/wild.c" "${REPO}/linux/hud.c" "${REPO}/linux/quick_menu.c")
target_include_directories(pokedaisy.elf PRIVATE
    "${MGBA_DIR}/include" "${MGBA_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}/mgba/include"
    "${REPO}/3ds/core" "${REPO}/linux" "${CPP}")
# mGBA's headers size its structs by these, so ours must match the library's (see the colour note above).
target_compile_definitions(pokedaisy.elf PRIVATE M_CORE_GBA M_CORE_GB BUILD_STATIC FIXED_ROM_BUFFER PK_WIDE_NO_GUARD PK_WIDE_CACHE
    "PD_VERSION=\"${PD_VERSION} ${PD_VITA_BUILD}\"")
# EXPAND's buffers at the top screen's size, as on the 3DS (pd_session.c checks its window fits).
target_compile_definitions(pokedaisy.elf PRIVATE PK_WIDE_MAX_VIEW_W=400 PK_WIDE_MAX_VIEW_H=240)
target_compile_options(pokedaisy.elf PRIVATE -std=gnu11 -O2 -Wall -Wextra -Wno-format -Wno-unused-parameter)
target_link_libraries(pokedaisy.elf mgba pd_vita2d
    SceAppUtil_stub SceAudio_stub SceCommonDialog_stub SceCtrl_stub SceDisplay_stub SceGxm_stub SceIme_stub
    SceAppMgr_stub ScePgf_stub ScePvf_stub ScePower_stub SceRtc_stub SceSysmodule_stub SceTouch_stub
    SceKernelDmacMgr_stub m)
set_source_files_properties("${CPP}/pk_call.c" PROPERTIES COMPILE_DEFINITIONS "__thread=")
target_link_options(pokedaisy.elf PRIVATE -Wl,-q -Wl,--wrap=sceKernelCreateThread -Wl,--wrap=pd_mixer_attach -Wl,--wrap=pd_mixer_frame -Wl,--wrap=pd_mixer_release)

vita_create_self(eboot.bin pokedaisy.elf UNSAFE)
vita_create_vpk(pokedaisy.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
