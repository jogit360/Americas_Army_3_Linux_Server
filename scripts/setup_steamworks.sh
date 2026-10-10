#!/bin/bash

# AA3 Linux Server - Steamworks SDK Setup Script
# This script helps download and verify the Steamworks SDK installation

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
SDK_DIR="$PROJECT_ROOT/external/steamworks_sdk_linux"

echo "========================================="
echo "AA3 Linux Server - Steamworks SDK Setup"
echo "========================================="
echo ""

# Check if SDK already exists
if [ -d "$SDK_DIR" ]; then
    echo "✓ Steamworks SDK directory found at: $SDK_DIR"
    echo ""
    echo "Verifying SDK structure..."

    # Verify critical files
    critical_files=(
        "public/steam/steam_gameserver.h"
        "public/steam/isteamgameserver.h"
        "public/steam/steamtypes.h"
        "redistributable_bin/linux64/libsteamgameserver.so"
    )

    all_exist=true
    for file in "${critical_files[@]}"; do
        full_path="$SDK_DIR/$file"
        if [ -f "$full_path" ]; then
            echo "✓ $file"
        else
            echo "✗ MISSING: $file"
            all_exist=false
        fi
    done

    if [ "$all_exist" = true ]; then
        echo ""
        echo "✓ Steamworks SDK verified successfully!"
        exit 0
    else
        echo ""
        echo "✗ Steamworks SDK is incomplete"
        exit 1
    fi
else
    echo "⚠ Steamworks SDK not found at: $SDK_DIR"
    echo ""
    echo "To install Steamworks SDK:"
    echo "1. Visit: https://partner.steamgames.com/"
    echo "2. Log in to your Valve partner account"
    echo "3. Download the latest 'Steamworks SDK - Linux' version"
    echo "4. Extract to: $SDK_DIR"
    echo ""
    echo "Example:"
    echo "  mkdir -p $SDK_DIR"
    echo "  unzip SteamworksSDK.zip -d $(dirname "$SDK_DIR")/"
    echo "  mv SteamworksSDK/* $SDK_DIR/"
    echo ""
    exit 1
fi
