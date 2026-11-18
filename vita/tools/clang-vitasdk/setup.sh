#!/bin/bash
# A VitaSDK built from source around the machine's own clang / lld, for building the Vita port where VitaSDK's
# prebuilt toolchain can't be downloaded (only git reaches GitHub) and there's no ARM GCC. Same ABI as VitaSDK's GCC
# (armv7-a, Cortex-A9, NEON, hard float, Thumb-2, signed char, short enums): vita-headers' struct-size checks pass.
# Players and anyone with VitaSDK installed don't need this: see ../../README.md.
#
#   vita/tools/clang-vitasdk/setup.sh <sdk dir> [<work dir>]        then: export VITASDK=<sdk dir> PATH=<sdk dir>/bin:$PATH
#
# Needs clang, lld and llvm-ar / -objcopy / -nm / -readelf (18 tried), cmake, make, python3, and zlib / libyaml headers.
# Takes ~15 minutes on two cores (newlib and the ~20000 import stubs are most of it).
set -euo pipefail
SDK="$(mkdir -p "$1" && cd "$1" && pwd)"
WORK="$(mkdir -p "${2:-$SDK/../vitasdk-src}" && cd "${2:-$SDK/../vitasdk-src}" && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"
JOBS="$(nproc)"
TARGET="$SDK/arm-vita-eabi"
mkdir -p "$SDK/bin" "$TARGET/include" "$TARGET/lib" "$SDK/share"
export PATH="$SDK/bin:$PATH"

clone() { [ -d "$WORK/$2" ] || git clone --depth 1 "$1" "$WORK/$2"; }
clone https://github.com/vitasdk/vita-headers vita-headers
clone https://github.com/vitasdk/vita-toolchain vita-toolchain
clone https://github.com/vitasdk/newlib newlib
clone https://github.com/vitasdk/buildscripts buildscripts
clone https://github.com/WolfgangSt/libelf libelf # libelf 0.8.x, Michael Riepe's (vita-elf-create needs it, not elfutils')
if [ ! -d "$WORK/llvm-project" ]; then
    git clone --depth 1 --filter=blob:none --sparse -b llvmorg-18.1.3 https://github.com/llvm/llvm-project "$WORK/llvm-project"
    git -C "$WORK/llvm-project" sparse-checkout set compiler-rt/lib/builtins compiler-rt/include
fi

# The wrappers: clang as arm-vita-eabi-gcc / g++, GNU as's command line on clang's assembler, ld.lld laid out as VitaSDK's
# patched GNU ld, llvm's binutils, and vita-pack-vpk on Python's zipfile (the real one needs libzip).
install -m755 "$HERE/bin/arm-vita-eabi-gcc" "$HERE/bin/arm-vita-eabi-link" "$HERE/bin/arm-vita-eabi-as" "$HERE/bin/vita-pack-vpk" "$SDK/bin/"
cp "$SDK/bin/arm-vita-eabi-gcc" "$SDK/bin/arm-vita-eabi-g++"
cp "$SDK/bin/arm-vita-eabi-gcc" "$SDK/bin/arm-vita-eabi-cc"
for t in ar ranlib nm objcopy objdump strip readelf size strings; do
    printf '#!/bin/sh\nexec llvm-%s "$@"\n' "$t" > "$SDK/bin/arm-vita-eabi-$t"
    chmod +x "$SDK/bin/arm-vita-eabi-$t"
done
printf '#!/bin/sh\nexec ld.lld "$@"\n' > "$SDK/bin/arm-vita-eabi-ld" && chmod +x "$SDK/bin/arm-vita-eabi-ld"

# Host tools: libelf, then vita-toolchain (vita-elf-create, vita-make-fself, vita-mksfoex, vita-libs-gen-2 ...).
if [ ! -f "$SDK/host/lib/libelf.a" ]; then
    cd "$WORK/libelf"
    patch -p1 -N < "$WORK/buildscripts/patches/libelf.patch" || true
    CFLAGS="-O2 -fPIC -std=gnu89" ./configure --prefix="$SDK/host" --disable-shared --disable-nls > /dev/null
    make -j"$JOBS" > /dev/null && make install > /dev/null
fi
if [ ! -x "$SDK/bin/vita-elf-create" ]; then
    cd "$WORK/vita-toolchain"
    git submodule update --init --depth 1
    sed -i 's/libelf=0.8.13/libelf>=0.8.12/' cmake/Modules/Findlibelf.cmake
    python3 - <<'EOF'
import re
p = 'src/CMakeLists.txt'; s = open(p).read()
for old in ('find_package(libzip REQUIRED)', 'include_directories(${libzip_INCLUDE_DIRS})',
            'target_link_libraries(vita-pack-vpk ${libzip_LIBRARIES} ${zlib_LIBRARIES})', 'install(TARGETS vita-pack-vpk DESTINATION bin)'):
    s = s.replace(old, '')
s = re.sub(r'add_executable\(vita-pack-vpk[^)]*\)', '', s)
open(p, 'w').write(s)
EOF
    PKG_CONFIG_PATH="$SDK/host/lib/pkgconfig" cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$SDK" -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build build -j"$JOBS" > /dev/null && cmake --install build > /dev/null
    cp cmake_toolchain/vita.cmake cmake_toolchain/vita.toolchain.cmake "$SDK/share/"
fi

# Sony's headers, then newlib (VitaSDK's fork, its configure as buildscripts has it).
cp -r "$WORK/vita-headers/include/"* "$TARGET/include/"
cp "$WORK/vita-headers/vita.header_warn.cmake" "$TARGET/"
if [ ! -f "$TARGET/lib/libc.a" ]; then
    mkdir -p "$WORK/build-newlib" && cd "$WORK/build-newlib"
    "$WORK/newlib/configure" "CFLAGS_FOR_TARGET=-g -O2 -ffunction-sections -fdata-sections -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-implicit-int" \
        CC_FOR_TARGET=arm-vita-eabi-gcc AR_FOR_TARGET=arm-vita-eabi-ar AS_FOR_TARGET=arm-vita-eabi-as RANLIB_FOR_TARGET=arm-vita-eabi-ranlib \
        LD_FOR_TARGET=arm-vita-eabi-ld NM_FOR_TARGET=arm-vita-eabi-nm OBJDUMP_FOR_TARGET=arm-vita-eabi-objdump \
        READELF_FOR_TARGET=arm-vita-eabi-readelf STRIP_FOR_TARGET=arm-vita-eabi-strip \
        --target=arm-vita-eabi --prefix=/ --with-build-sysroot="$TARGET" --enable-newlib-io-long-long --enable-newlib-register-fini \
        --disable-newlib-supplied-syscalls --enable-newlib-long-time_t --disable-nls --enable-newlib-iconv --enable-newlib-mb > /dev/null
    make -j"$JOBS" > /dev/null && make install DESTDIR="$SDK" > /dev/null
fi

# libgcc's part: compiler-rt's builtins; GCC's crti.o: an empty _init / _fini (constructors run from .init_array).
python3 "$HERE/build_builtins.py" "$WORK/llvm-project/compiler-rt/lib/builtins" "$TARGET/lib/libclang_rt.builtins-arm.a" "$SDK/bin/arm-vita-eabi-gcc"
arm-vita-eabi-gcc -O2 -c "$HERE/crti.c" -o "$TARGET/lib/crti.o"

# The import stubs for every library in vita-headers' database.
if [ ! -f "$TARGET/lib/libSceLibKernel_stub.a" ]; then
    mkdir -p "$WORK/build-stubs" && cd "$WORK/build-stubs"
    vita-libs-gen-2 -yml="$WORK/vita-headers/db" -output=.
    make ARCH="$SDK/bin/arm-vita-eabi" -j"$JOBS" > /dev/null
    cp *.a "$TARGET/lib/"
fi
echo "VitaSDK (clang) in $SDK: export VITASDK=$SDK PATH=$SDK/bin:\$PATH"
