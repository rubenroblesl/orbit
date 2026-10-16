#!/bin/zsh
# SPDX-License-Identifier: GPL-3.0-only
#
# Builds Orbit.app (launcher + runtime + recompiler + toolchain) and Orbit.dmg.
#
#   macos/package.sh [--build-dir DIR] [--textures DIR] [--output DIR] [--no-dmg]
#
#   --build-dir  the CMake build directory of the macOS runtime (default: ../build/runtime)
#   --textures   a custom texture pack folder holding RMG or RMGE01 (bundled into the app and
#                enabled by default); omit for an app without one
#   --output     where Orbit.app and Orbit.dmg go (default: ../dist)
#
# Needs Xcode (swiftc, iconutil) and Rust (cargo). The official CMake and Ninja releases are
# downloaded (pinned SHA-256) and copied into the app; they run on macOS 11 and later, like the
# rest of Orbit on macOS 14. Nothing derived from the game goes into the app: the launcher
# builds that on the user's Mac.

set -euo pipefail

REPO=${0:A:h:h}
BUILD_DIR=${REPO:h}/build/runtime
TEXTURES=""
OUTPUT=${REPO:h}/dist
MAKE_DMG=1
while (( $# )); do
    case $1 in
        --build-dir) BUILD_DIR=${2:A}; shift 2 ;;
        --textures) TEXTURES=${2:A}; shift 2 ;;
        --output) OUTPUT=${2:A}; shift 2 ;;
        --no-dmg) MAKE_DMG=0; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

NAME=Orbit
VERSION=$(<"$REPO/macos/VERSION")
APP="$OUTPUT/$NAME.app"
CONTENTS="$APP/Contents"
GAME_APP="$CONTENTS/Helpers/$NAME Game.app"
STAGE="$OUTPUT/.stage"
TOOLS_CACHE="${XDG_CACHE_HOME:-$HOME/Library/Caches}/orbit-package"

CMAKE_VERSION=3.31.6
CMAKE_URL="https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-macos-universal.tar.gz"
CMAKE_SHA256=330b9514f5112e5ed4fb08b8b05803b776fd9b539a6ae12927d14dcc0ee2ba8d
NINJA_VERSION=1.12.1
NINJA_URL="https://github.com/ninja-build/ninja/releases/download/v$NINJA_VERSION/ninja-mac.zip"
NINJA_SHA256=89a287444b5b3e98f88a945afa50ce937b8ffd1dcc59c555ad9b1baf855298c9
NINJA_LICENSE_URL="https://raw.githubusercontent.com/ninja-build/ninja/v$NINJA_VERSION/COPYING"
NINJA_LICENSE_SHA256=eb7e9ab9690124c5c9f42bdc81383d886a3dede26345b6ed15bbad7caf81f7ea

step() { print -P "%F{cyan}==>%f $1"; }
# rustup's cargo may be outside the default PATH of a non-login shell (Homebrew's rustup, ~/.cargo).
for dir in /opt/homebrew/opt/rustup/bin "$HOME/.cargo/bin"; do
    [[ -d "$dir" ]] && PATH="$dir:$PATH"
done
export PATH

fetch() {  # fetch <url> <sha256> <file>: download once into the cache and check the hash
    local file="$TOOLS_CACHE/$3"
    mkdir -p "$TOOLS_CACHE"
    if [[ ! -f "$file" ]] || [[ "$(shasum -a 256 "$file" | cut -d' ' -f1)" != "$2" ]]; then
        curl -fsSL -o "$file.part" "$1"
        mv "$file.part" "$file"
    fi
    if [[ "$(shasum -a 256 "$file" | cut -d' ' -f1)" != "$2" ]]; then
        echo "checksum mismatch for $1" >&2
        exit 1
    fi
    print -r -- "$file"
}

step "Runtime (NebulaRuntime and the module libraries)"
cmake --build "$BUILD_DIR" --target NebulaRuntime galaxy_ppc_float galaxy_softfloat galaxy_dsp_alu

step "Recompiler"
(cd "$REPO" && MACOSX_DEPLOYMENT_TARGET=14.0 cargo build --release --locked -p nebula-recomp)

step "Launcher"
rm -rf "$STAGE" && mkdir -p "$STAGE"
xcrun swiftc -O -parse-as-library -target arm64-apple-macos14.0 \
    -o "$STAGE/$NAME" "$REPO"/macos/Launcher/*.swift

step "CMake $CMAKE_VERSION and Ninja $NINJA_VERSION"
CMAKE_ARCHIVE=$(fetch "$CMAKE_URL" "$CMAKE_SHA256" "cmake-$CMAKE_VERSION-macos-universal.tar.gz")
NINJA_ARCHIVE=$(fetch "$NINJA_URL" "$NINJA_SHA256" "ninja-$NINJA_VERSION-mac.zip")
NINJA_LICENSE=$(fetch "$NINJA_LICENSE_URL" "$NINJA_LICENSE_SHA256" "ninja-$NINJA_VERSION-COPYING")
tar -xzf "$CMAKE_ARCHIVE" -C "$STAGE"
CMAKE_ROOT_DIR="$STAGE/cmake-$CMAKE_VERSION-macos-universal/CMake.app/Contents"
unzip -q -o "$NINJA_ARCHIVE" -d "$STAGE/ninja"

step "App bundle"
rm -rf "$APP"
mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Resources/Toolchain/bin" "$CONTENTS/Resources/Toolchain/share" \
    "$CONTENTS/Resources/SDK/lib" "$CONTENTS/Resources/Licenses" "$GAME_APP/Contents/MacOS" \
    "$GAME_APP/Contents/Resources"

# Icon
ICONSET="$STAGE/AppIcon.iconset"
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
    sips -z $size $size "$REPO/macos/assets/AppIcon.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
    sips -z $((size * 2)) $((size * 2)) "$REPO/macos/assets/AppIcon.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$CONTENTS/Resources/AppIcon.icns"
cp "$CONTENTS/Resources/AppIcon.icns" "$GAME_APP/Contents/Resources/AppIcon.icns"

plist() {  # plist <file> <executable> <identifier> <name> [extra keys]
    cat > "$1" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>$2</string>
    <key>CFBundleIdentifier</key><string>$3</string>
    <key>CFBundleName</key><string>$4</string>
    <key>CFBundleDisplayName</key><string>$4</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
    $5
</dict>
</plist>
EOF
}
plist "$CONTENTS/Info.plist" "$NAME" dev.orbit.launcher "$NAME" ""
plist "$GAME_APP/Contents/Info.plist" "$NAME" dev.orbit.game "$NAME" \
    "<key>GCSupportsControllerUserInteraction</key><true/><key>LSUIElement</key><false/>"

cp "$STAGE/$NAME" "$CONTENTS/MacOS/$NAME"
cp "$BUILD_DIR/NebulaRuntime" "$GAME_APP/Contents/MacOS/$NAME"

# Recompiler and build tools.
cp "$REPO/target/release/nebula-recomp" "$CONTENTS/Resources/Toolchain/bin/"
cp "$CMAKE_ROOT_DIR/bin/cmake" "$STAGE/ninja/ninja" "$CONTENTS/Resources/Toolchain/bin/"
cp -R "$CMAKE_ROOT_DIR/share/cmake-${CMAKE_VERSION%.*}" "$CONTENTS/Resources/Toolchain/share/"
for unused in Help; do rm -rf "$CONTENTS/Resources/Toolchain/share/cmake-${CMAKE_VERSION%.*}/$unused"; done

# What the game modules compile and link against.
cp -R "$REPO/runtime/include" "$CONTENTS/Resources/SDK/include"
cp "$BUILD_DIR"/libgalaxy_ppc_float.a "$BUILD_DIR"/libgalaxy_softfloat.a "$BUILD_DIR"/libgalaxy_dsp_alu.a \
    "$CONTENTS/Resources/SDK/lib/"

# Licenses of everything shipped.
LICENSES="$CONTENTS/Resources/Licenses"
cp "$REPO/LICENSE" "$LICENSES/Orbit-GPL-3.0.txt"
cp "$REPO/THIRD-PARTY-NOTICES.md" "$LICENSES/THIRD-PARTY-NOTICES.md"
cp "$REPO/docs/licenses/rust-dependencies.txt" "$LICENSES/rust-dependencies.txt"
cp "$REPO/third_party/xxhash/LICENSE" "$LICENSES/xxHash-LICENSE.txt"
cp "$REPO/third_party/stb/LICENSE" "$LICENSES/stb-LICENSE.txt"
cp "$REPO/third_party/sse2neon/LICENSE" "$LICENSES/sse2neon-LICENSE.txt"
DEPS="$BUILD_DIR/_deps"
cp "$DEPS/nebula_glslang-src/LICENSE.txt" "$LICENSES/glslang-LICENSE.txt"
cp -R "$DEPS/nebula_glslang-src/LICENSES" "$LICENSES/glslang-LICENSES"
cp "$DEPS/nebula_spirv_tools-src/LICENSE" "$LICENSES/SPIRV-Tools-LICENSE.txt"
cp "$DEPS/nebula_spirv_headers-src/LICENSE" "$LICENSES/SPIRV-Headers-LICENSE.txt"
cp "$DEPS/nebula_spirv_cross-src/LICENSE" "$LICENSES/SPIRV-Cross-LICENSE.txt"
cp "$CMAKE_ROOT_DIR/doc/cmake-${CMAKE_VERSION%.*}/Copyright.txt" "$LICENSES/CMake-Copyright.txt" 2>/dev/null || \
    cp "$(find "$CMAKE_ROOT_DIR/doc" -name Copyright.txt | head -1)" "$LICENSES/CMake-Copyright.txt"
cp "$NINJA_LICENSE" "$LICENSES/Ninja-LICENSE.txt"

if [[ -n "$TEXTURES" ]]; then
    step "Custom textures from $TEXTURES"
    mkdir -p "$CONTENTS/Resources/Textures"
    for folder in RMG RMGE01; do
        [[ -d "$TEXTURES/$folder" ]] || continue
        # Clones on APFS (no extra space), a plain copy elsewhere.
        cp -cR "$TEXTURES/$folder" "$CONTENTS/Resources/Textures/" 2>/dev/null || \
            cp -R "$TEXTURES/$folder" "$CONTENTS/Resources/Textures/"
    done
    [[ -d "$CONTENTS/Resources/Textures/RMG" || -d "$CONTENTS/Resources/Textures/RMGE01" ]] || \
        { echo "no RMG or RMGE01 folder in $TEXTURES" >&2; exit 1; }
    find "$CONTENTS/Resources/Textures" -name .DS_Store -delete
    cp "$REPO/macos/TEXTURE-CREDITS.md" "$LICENSES/TEXTURE-CREDITS.md"
fi

step "Signing (ad hoc)"
# Inside out: the recompiler (CMake and Ninja keep their publishers' signatures), the game helper,
# then the launcher, whose seal covers them all.
codesign --force --sign - "$CONTENTS/Resources/Toolchain/bin/nebula-recomp"
codesign --force --sign - "$GAME_APP"
codesign --force --sign - "$APP"
codesign --verify --deep --strict "$APP"

if (( MAKE_DMG )); then
    step "Disk image"
    DMG_ROOT="$STAGE/dmg"
    mkdir -p "$DMG_ROOT"
    cp -cR "$APP" "$DMG_ROOT/" 2>/dev/null || cp -R "$APP" "$DMG_ROOT/"
    ln -s /Applications "$DMG_ROOT/Applications"
    cp "$REPO/macos/LEEME.txt" "$DMG_ROOT/LÉEME.txt"
    rm -f "$OUTPUT/$NAME.dmg"
    hdiutil create -volname "$NAME" -srcfolder "$DMG_ROOT" -fs APFS -format ULFO -ov "$OUTPUT/$NAME.dmg" >/dev/null
fi
rm -rf "$STAGE"
if (( MAKE_DMG )); then
    step "Done: $APP and $OUTPUT/$NAME.dmg"
else
    step "Done: $APP"
fi
