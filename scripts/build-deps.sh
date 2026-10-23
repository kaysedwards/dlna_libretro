#!/bin/sh
#
# build-deps.sh - build the libraries the core needs from the git
# submodules in deps/, as static libraries, into deps/prefix.
#
#   ./scripts/build-deps.sh            build everything that is missing
#   ./scripts/build-deps.sh ffmpeg     (re)build just one: mbedtls, libssh,
#                                      libsmb2, pupnp, freetype or ffmpeg
#   JOBS=4 ./scripts/build-deps.sh     limit parallel compile jobs
#
# Afterwards a plain `make` picks the libraries up automatically (the
# Makefile prefers deps/prefix over system libraries when it exists) and
# links them into the core, so the resulting dlna_libretro.so does not
# depend on FFmpeg, pupnp or FreeType being installed.
#
# You do NOT need this script if your system already provides development
# packages for FFmpeg, libupnp and FreeType (and optionally libssh and
# libsmb2); `make` will use those.
#
# Build order matters: mbedTLS first (libssh and FFmpeg use it for
# encryption), then the rest.
#
# Requirements: a C compiler, make, cmake, pkg-config, and nasm (for
# FFmpeg's optimised assembly; or set FFMPEG_EXTRA="--disable-x86asm").
# FFmpeg's PNG decoder needs zlib, which every system has.
#
# Everything is built out of tree (in deps/build), so the submodule
# folders themselves stay untouched.

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS="$ROOT/deps"
PREFIX="$DEPS/prefix"
BUILD="$DEPS/build"
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
ONLY="$1"

mkdir -p "$PREFIX" "$BUILD"

say() {
   printf '\n=== %s ===\n' "$*"
}

need() {
   if [ ! -e "$DEPS/$1/$2" ]; then
      echo "deps/$1 is empty.  Did you run: git submodule update --init ?" >&2
      exit 1
   fi
}

want() {
   # Build $1 if asked for by name, or if nothing was named and its
   # pkg-config file is not installed yet.
   if [ -n "$ONLY" ]; then
      [ "$ONLY" = "$1" ]
   else
      [ ! -f "$PREFIX/lib/pkgconfig/$2.pc" ]
   fi
}

# ---------------------------------------------------------------------
# mbedTLS: TLS and crypto for FFmpeg's https:// and for libssh.  The 3.6
# long-term-support line is used; 4.x changed its API and not every
# library supports it yet.

if want mbedtls mbedtls; then
   need mbedtls CMakeLists.txt
   say "mbedTLS"
   cmake -S "$DEPS/mbedtls" -B "$BUILD/mbedtls" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
      -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
      -DMBEDTLS_USER_CONFIG_FILE="$ROOT/scripts/mbedtls-user-config.h" \
      -DENABLE_PROGRAMS=OFF \
      -DENABLE_TESTING=OFF
   cmake --build "$BUILD/mbedtls" --parallel "$JOBS"
   cmake --install "$BUILD/mbedtls"
   # Programs built against this mbedTLS (libssh, FFmpeg) must see the same
   # settings, or their idea of mbedTLS's structures would differ from the
   # library's.  The installed header (our build output, not the submodule)
   # gets the user configuration included at its end.
   CONF="$PREFIX/include/mbedtls/mbedtls_config.h"
   if ! grep -q "mbedtls-user-config.h" "$CONF"; then
      printf '\n/* Added by build-deps.sh */\n#include "%s"\n' \
         "$ROOT/scripts/mbedtls-user-config.h" >> "$CONF"
   fi
fi

# ---------------------------------------------------------------------
# libssh: SFTP (the core checks host keys itself; see src/sshio.h)

if want libssh libssh; then
   need libssh CMakeLists.txt
   say "libssh"
   cmake -S "$DEPS/libssh" -B "$BUILD/libssh" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_PREFIX_PATH="$PREFIX" \
      -DBUILD_SHARED_LIBS=OFF \
      -DWITH_MBEDTLS=ON \
      -DWITH_GSSAPI=OFF \
      -DWITH_ZLIB=OFF \
      -DWITH_SERVER=OFF \
      -DWITH_EXAMPLES=OFF \
      -DUNIT_TESTING=OFF \
      -DCLIENT_TESTING=OFF
   cmake --build "$BUILD/libssh" --parallel "$JOBS"
   cmake --install "$BUILD/libssh"
fi

# ---------------------------------------------------------------------
# libsmb2: SMB2/SMB3 shares.  Built without Kerberos: password (NTLM)
# logins still work, and the core does not drag in the Kerberos libraries.

if want libsmb2 libsmb2; then
   need libsmb2 CMakeLists.txt
   say "libsmb2"
   cmake -S "$DEPS/libsmb2" -B "$BUILD/libsmb2" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DBUILD_SHARED_LIBS=OFF \
      -DENABLE_EXAMPLES=OFF \
      -DCMAKE_DISABLE_FIND_PACKAGE_LibKrb5=ON \
      -DCMAKE_DISABLE_FIND_PACKAGE_GSSAPI=ON \
      -DHAVE_LIBKRB5=0 \
      -DHAVE_GSSAPI_GSSAPI_H=0
   cmake --build "$BUILD/libsmb2" --parallel "$JOBS"
   cmake --install "$BUILD/libsmb2"
fi

# ---------------------------------------------------------------------
# pupnp (libupnp + libixml)

if want pupnp libupnp; then
   need pupnp CMakeLists.txt
   say "pupnp"
   cmake -S "$DEPS/pupnp" -B "$BUILD/pupnp" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DUPNP_BUILD_SHARED=OFF \
      -DUPNP_BUILD_STATIC=ON \
      -DUPNP_BUILD_SAMPLES=OFF \
      -DUPNP_ENABLE_TESTING=OFF \
      -DIXML_ENABLE_TESTING=OFF \
      -DUPNP_ENABLE_TESTING_INTEGRATION=OFF \
      -DIXML_ENABLE_TESTING_INTEGRATION=OFF
   cmake --build "$BUILD/pupnp" --parallel "$JOBS"
   cmake --install "$BUILD/pupnp"
fi

# ---------------------------------------------------------------------
# FreeType

if want freetype freetype2; then
   need freetype CMakeLists.txt
   say "FreeType"
   cmake -S "$DEPS/freetype" -B "$BUILD/freetype" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DBUILD_SHARED_LIBS=OFF \
      -DFT_DISABLE_ZLIB=ON \
      -DFT_DISABLE_BZIP2=ON \
      -DFT_DISABLE_PNG=ON \
      -DFT_DISABLE_HARFBUZZ=ON \
      -DFT_DISABLE_BROTLI=ON
   cmake --build "$BUILD/freetype" --parallel "$JOBS"
   cmake --install "$BUILD/freetype"
fi

# ---------------------------------------------------------------------
# FFmpeg
#
# Only the pieces the core uses: demuxers, decoders, parsers, the network
# protocols, swscale and swresample.  mbedTLS provides https://; its
# Apache 2.0 licence requires --enable-version3, which makes this FFmpeg
# LGPL version 3 (see the licence notes in README.md).  --disable-autodetect keeps FFmpeg
# from quietly linking whatever optional libraries happen to be installed
# (VAAPI, X11, ...), which would make the static core depend on them.

if want ffmpeg libavformat; then
   need FFmpeg configure
   say "FFmpeg (this takes a while)"
   mkdir -p "$BUILD/ffmpeg"
   (
      cd "$BUILD/ffmpeg"
      # shellcheck disable=SC2086
      "$DEPS/FFmpeg/configure" \
         --prefix="$PREFIX" \
         --enable-static --disable-shared --enable-pic \
         --disable-programs --disable-doc \
         --disable-avdevice --disable-avfilter \
         --disable-encoders --disable-muxers --disable-devices \
         --disable-filters \
         --disable-autodetect --enable-zlib --enable-network \
         --enable-mbedtls --enable-version3 \
         --extra-cflags="-I$PREFIX/include" \
         --extra-ldflags="-L$PREFIX/lib" \
         $FFMPEG_EXTRA
      make -j"$JOBS"
      make install
   )
fi

say "Done.  Libraries are in deps/prefix; now run: make"
