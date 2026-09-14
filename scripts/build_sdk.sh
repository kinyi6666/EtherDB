#!/usr/bin/env bash
# ============================================================
#  EtherDB Client SDK build script (Linux / g++)
#
#  NEW build (additive): builds the etherdb_client library only.
#  The original src/client/CMakeLists.txt (shell) is NOT touched.
#
#  Output: src/bin/sdk/   (include/ + lib/ + README.md)
#
#  Usage:
#    ./scripts/build_sdk.sh                build static library (default)
#    ./scripts/build_sdk.sh --shared       build shared library (.so)
#    ./scripts/build_sdk.sh --clean        clean build dir then rebuild
#    ./scripts/build_sdk.sh --debug        build Debug config
# ============================================================
set -euo pipefail
cd "$(dirname "$0")/.."

CONFIG=Release
SHARED=OFF
CLEAN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --shared) SHARED=ON ;;
        --clean)  CLEAN=1 ;;
        --debug)  CONFIG=Debug ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
    shift
done

BUILD_DIR=build_sdk
if [ "$CLEAN" = "1" ] && [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
fi

echo "[EtherDB SDK] Configure: config=$CONFIG shared=$SHARED build_dir=$BUILD_DIR"
cmake -S src/client/sdk -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CONFIG" -DETHERDB_BUILD_SHARED="$SHARED"

echo "[EtherDB SDK] Build target: etherdb_client"
cmake --build "$BUILD_DIR" --target etherdb_client

echo "[EtherDB SDK] Done. SDK at src/bin/sdk"
ls -la src/bin/sdk 2>/dev/null || true
