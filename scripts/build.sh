#!/bin/bash

# AA3 Linux Server - Build Script
# Simplifies building the server from source

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

echo "========================================="
echo "AA3 Linux Server - Build Script"
echo "========================================="
echo ""

# Check prerequisites
echo "Checking prerequisites..."

if ! command -v cmake &> /dev/null; then
    echo "✗ CMake not found. Install with: sudo apt-get install cmake"
    exit 1
fi

if ! command -v make &> /dev/null; then
    echo "✗ make not found. Install with: sudo apt-get install build-essential"
    exit 1
fi

if ! command -v g++ &> /dev/null; then
    echo "✗ g++ not found. Install with: sudo apt-get install build-essential"
    exit 1
fi

echo "✓ CMake: $(cmake --version | head -n1)"
echo "✓ GCC: $(g++ --version | head -n1)"
echo ""

# Verify Steamworks SDK
echo "Verifying Steamworks SDK..."
if [ ! -f "$PROJECT_ROOT/external/steamworks_sdk_linux/public/steam/steam_gameserver.h" ]; then
    echo "✗ Steamworks SDK not found"
    echo "  Run: bash scripts/setup_steamworks.sh"
    exit 1
fi
echo "✓ Steamworks SDK found"
echo ""

# Create and enter build directory
echo "Setting up build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Run CMake
echo "Running CMake..."
cmake ..

# Build
echo ""
echo "Building project..."
NPROCS=$(nproc)
echo "Using $NPROCS processors..."
make -j"$NPROCS"

# Verify output
echo ""
if [ -f "$BUILD_DIR/aa3_server" ]; then
    echo "✓ Build successful!"
    echo ""
    echo "Executable: $BUILD_DIR/aa3_server"
    echo ""
    echo "To run the server:"
    echo "  cd $BUILD_DIR"
    echo "  ./aa3_server -c ../config/aa3_server.ini"
    echo ""
    exit 0
else
    echo "✗ Build failed - executable not found"
    exit 1
fi
