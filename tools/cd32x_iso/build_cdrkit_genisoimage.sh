#!/usr/bin/env bash
set -euo pipefail

# Build only cdrkit's genisoimage utility for hosts without a packaged
# genisoimage/mkisofs.  The full cdrkit tree wants wodim/libcap; the CD32X
# build only needs genisoimage, so this script applies the small compatibility
# fixes needed by current GCC/CMake and builds that target.

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
DEFAULT_TARBALL_1="$ROOT_DIR/files/cd32x_iso/cdrkit_1.1.11.orig.tar.gz"
DEFAULT_TARBALL_2="/mnt/data/cdrkit_1.1.11.orig.tar.gz"
CDRKIT_TARBALL=${CDRKIT_TARBALL:-${1:-}}

if [ -z "$CDRKIT_TARBALL" ]; then
  if [ -f "$DEFAULT_TARBALL_1" ]; then
    CDRKIT_TARBALL=$DEFAULT_TARBALL_1
  elif [ -f "$DEFAULT_TARBALL_2" ]; then
    CDRKIT_TARBALL=$DEFAULT_TARBALL_2
  else
    echo "cdrkit tarball not found; pass it as argv[1] or set CDRKIT_TARBALL" >&2
    exit 1
  fi
fi

if ! command -v cmake >/dev/null; then
  echo "cmake is required to build cdrkit/genisoimage" >&2
  exit 1
fi
if ! command -v cc >/dev/null && ! command -v gcc >/dev/null; then
  echo "a host C compiler is required to build cdrkit/genisoimage" >&2
  exit 1
fi

BUILD_DIR=${BUILD_DIR:-$ROOT_DIR/build/cd32x/host-tools/cdrkit}
INSTALL_DIR=${INSTALL_DIR:-$ROOT_DIR/tools/cd32x_iso/bin}
SRC_DIR="$BUILD_DIR/src"
OBJ_DIR="$BUILD_DIR/build"

rm -rf "$BUILD_DIR"
mkdir -p "$SRC_DIR" "$OBJ_DIR" "$INSTALL_DIR"
tar -xzf "$CDRKIT_TARBALL" -C "$SRC_DIR" --strip-components=1

python3 - "$SRC_DIR" <<'PY'
from pathlib import Path
import sys
src = Path(sys.argv[1])

wodim = src / "wodim" / "CMakeLists.txt"
s = wodim.read_text()
s = s.replace('''ELSE(HAVE_SYS_CAPABILITY_H)\n   IF(CMAKE_SYSTEM_NAME MATCHES "Linux")\n      MESSAGE(FATAL_ERROR "Error: found a Linux system but no libcap header. Install libcap-dev.")\n   ENDIF(CMAKE_SYSTEM_NAME MATCHES "Linux")\nENDIF(HAVE_SYS_CAPABILITY_H)''', '''ELSE(HAVE_SYS_CAPABILITY_H)\n   MESSAGE(STATUS "libcap headers not found; building wodim support library without libcap")\nENDIF(HAVE_SYS_CAPABILITY_H)''')
wodim.write_text(s)

geniso = src / "genisoimage" / "genisoimage.c"
s = geniso.read_text()
if '#include "checksum.h"' not in s:
    s = s.replace('#include "exclude.h"\n', '#include "exclude.h"\n#include "checksum.h"\n')
geniso.write_text(s)

jte = src / "genisoimage" / "jte.c"
s = jte.read_text()
if '#include "md5.h"' not in s:
    if '#include "jte.h"\n' in s:
        s = s.replace('#include "jte.h"\n', '#include "jte.h"\n#include "md5.h"\n')
    elif '#include "checksum.h"\n' in s:
        s = s.replace('#include "checksum.h"\n', '#include "checksum.h"\n#include "md5.h"\n')
    else:
        s = '#include "md5.h"\n' + s
jte.write_text(s)
PY

cmake -S "$SRC_DIR" -B "$OBJ_DIR" -DCMAKE_C_FLAGS="-fcommon" -DCMAKE_INSTALL_PREFIX="$ROOT_DIR/tools/cd32x_iso" >/tmp/cd32x_cdrkit_cmake.log
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
cmake --build "$OBJ_DIR" --target genisoimage -j"$JOBS"
cp "$OBJ_DIR/genisoimage/genisoimage" "$INSTALL_DIR/genisoimage"
chmod +x "$INSTALL_DIR/genisoimage"
"$INSTALL_DIR/genisoimage" --version
