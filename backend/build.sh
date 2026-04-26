#!/usr/bin/env bash
# Build script for Linux / macOS
# Prerequisites: cmake, grpc, protobuf (via vcpkg or system package manager)
#
# vcpkg setup (one-time):
#   git clone https://github.com/microsoft/vcpkg
#   ./vcpkg/bootstrap-vcpkg.sh
#   ./vcpkg/vcpkg install grpc
#
# Then pass the toolchain file below.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# If using vcpkg, set VCPKG_ROOT env var
if [ -n "$VCPKG_ROOT" ]; then
  TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
else
  TOOLCHAIN=""
fi

echo "==> Configuring..."
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  $TOOLCHAIN

echo "==> Building..."
cmake --build "$BUILD_DIR" --config Release --parallel "$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

echo ""
echo "Build complete. Binaries:"
echo "  $BUILD_DIR/renderer_server"
echo "  $BUILD_DIR/renderer_client"
echo ""
echo "Run server:  $BUILD_DIR/renderer_server"
echo "Run client:  $BUILD_DIR/renderer_client [host:port]"
