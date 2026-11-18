# pd_vita_sim: the Vita port's own files (../main.c, ../video.c, ../audio.c, ../host.c) on a desktop, over fake_vita.c's
# Sony and vita2d calls: a script plays the buttons and the touch screen, `shot` writes the 960x544 screen as a PNG. For
# looking at the Vita's layouts and flows without a Vita (or a Vita emulator).
#
#   cmake -S vita/desktop -B build-vita-sim -DVITA_HEADERS=<vita-headers checkout>/include && cmake --build build-vita-sim
#
# VITA_HEADERS is vita-headers' include folder alone (psp2/, psp2common/ ...), not the SDK's arm-vita-eabi/include, whose
# newlib headers would stand in for the desktop's C library.
cmake_minimum_required(VERSION 3.13)
project(pd_vita_sim C)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VITA_HEADERS "" CACHE PATH "vita-headers' include folder")
if(NOT EXISTS "${VITA_HEADERS}/psp2/ctrl.h")
    message(FATAL_ERROR "VITA_HEADERS=<vita-headers>/include is needed (https://github.com/vitasdk/vita-headers)")
endif()

foreach(off BUILD_SHARED BUILD_GL BUILD_GLES2 BUILD_GLES3 BUILD_PERF BUILD_TEST BUILD_SUITE BUILD_LIBRETRO BUILD_PYTHON
            USE_DEBUGGERS USE_GDB_STUB USE_EDITLINE USE_FFMPEG USE_DISCORD_RPC USE_LZMA USE_LUA USE_ELF USE_SQLITE3
            USE_LIBZIP USE_MINIZIP USE_PNG USE_ZLIB BUILD_LTO)
    set(${off} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(on BUILD_STATIC DISABLE_FRONTENDS DISABLE_DEPS M_CORE_GBA M_CORE_GB SKIP_INSTALL_ALL)
    set(${on} ON CACHE BOOL "" FORCE)
endforeach()
set(SKIP_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO}/third_party/mgba" mgba EXCLUDE_FROM_ALL)
target_compile_definitions(mgba PRIVATE FIXED_ROM_BUFFER) # as the Vita build (../CMakeLists.txt)
find_package(Threads REQUIRED)

file(GLOB CORE_SOURCES "${REPO}/3ds/core/*.c")
file(GLOB HOST_SOURCES "${REPO}/3ds/host/*.c")
set(CPP "${REPO}/app/src/main/cpp")
# The themes: the app's (3ds/core/pd_games_gen.c) and the Vita's own, listed together by themes_looks_gen.c
# (tools/gen_themes.py), so the generated list is built under another name.
set_source_files_properties("${REPO}/3ds/core/pd_games_gen.c" PROPERTIES COMPILE_DEFINITIONS "pd_themes=pd_themes_3ds;pd_theme_count=pd_theme_count_3ds")
add_executable(pd_vita_sim fake_vita.c ../main.c ../video.c ../backdrop.c ../themes_gen.c ../sfx.c ../themes_looks_gen.c ../audio.c ../host.c ../emu_none.c ${CORE_SOURCES} ${HOST_SOURCES}
    "${REPO}/3ds/desktop/pd_png.c"
    "${CPP}/pk_host.c" "${CPP}/pk_call.c" "${CPP}/pk_wide.c" "${CPP}/pk_gb_audio.c" "${CPP}/pk_cheats.c" "${CPP}/pk_gb_field.c"
    "${CPP}/pk_wide_gb.c" "${CPP}/pk_flight.c"
    "${REPO}/linux/fly.c" "${REPO}/linux/flash.c" "${REPO}/linux/flight.c" "${REPO}/linux/tm_teach.c" "${REPO}/linux/field_items.c"
    "${REPO}/linux/gb_field.c" "${REPO}/linux/wild.c" "${REPO}/linux/hud.c" "${REPO}/linux/quick_menu.c")
target_include_directories(pd_vita_sim PRIVATE "${VITA_HEADERS}" "${REPO}/third_party/libvita2d/libvita2d/include"
    "${REPO}/third_party/mgba/include" "${REPO}/third_party/mgba/src" "${CMAKE_CURRENT_BINARY_DIR}/mgba/include"
    "${REPO}/3ds/core" "${REPO}/linux" "${CPP}")
target_compile_definitions(pd_vita_sim PRIVATE M_CORE_GBA M_CORE_GB BUILD_STATIC FIXED_ROM_BUFFER PK_WIDE_NO_GUARD PK_WIDE_CACHE
    PK_WIDE_MAX_VIEW_W=400 PK_WIDE_MAX_VIEW_H=240 "PD_VERSION=\"sim\"")
target_compile_options(pd_vita_sim PRIVATE -std=gnu11 -O2 -g -Wall -Wno-unused-parameter -Wno-deprecated-declarations)
target_link_libraries(pd_vita_sim mgba Threads::Threads m)
target_link_options(pd_vita_sim PRIVATE -Wl,--wrap=sceKernelCreateThread -Wl,--wrap=pd_mixer_attach -Wl,--wrap=pd_mixer_frame -Wl,--wrap=pd_mixer_release)
