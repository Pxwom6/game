#!/usr/bin/env bash
#
# build_macos.sh — package GRAVITON as a native macOS application.
#
#   tools/build_macos.sh          build dist/GRAVITON.app
#   tools/build_macos.sh --dmg    also build dist/GRAVITON-<version>.dmg
#
# The result is a self-contained bundle: SDL2 is copied inside it and the
# executable is repointed at the copy, so the app runs on a machine that has
# never heard of Homebrew. The binary is universal (arm64 + x86_64) whenever
# the available SDL2 is, and the bundle is ad-hoc signed, which Apple silicon
# requires before it will run anything at all.
#
# Everything here uses tools that ship with macOS and Xcode's command line
# tools: clang, lipo, otool, install_name_tool, codesign, iconutil, hdiutil.
set -euo pipefail

APP_NAME="GRAVITON"
BUNDLE_ID="com.github.pxwom6.graviton"
VERSION="${GV_VERSION:-1.0.0}"
BUILD_NUMBER="${GV_BUILD_NUMBER:-1}"
COPYRIGHT="Released under the MIT licence."

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/macos"
DIST="$ROOT/dist"
APP="$DIST/$APP_NAME.app"
CONTENTS="$APP/Contents"
MACOS_DIR="$CONTENTS/MacOS"
RES_DIR="$CONTENTS/Resources"
FW_DIR="$CONTENTS/Frameworks"

WANT_DMG=0
for arg in "$@"; do
    case "$arg" in
    --dmg) WANT_DMG=1 ;;
    -h | --help)
        sed -n '3,15p' "${BASH_SOURCE[0]}"
        exit 0
        ;;
    *)
        echo "unknown option: $arg" >&2
        exit 2
        ;;
    esac
done

die() {
    echo "error: $*" >&2
    exit 1
}

step() { printf '\n==> %s\n' "$*"; }

[ "$(uname -s)" = "Darwin" ] || die "this script builds a macOS bundle and must be run on macOS.
       On other systems, 'make' builds the game normally."

for tool in clang lipo otool install_name_tool codesign iconutil; do
    command -v "$tool" >/dev/null 2>&1 ||
        die "'$tool' not found. Install Xcode's command line tools: xcode-select --install"
done

# ------------------------------------------------------------------- find SDL2
#
# Two shapes are supported: the framework from libsdl.org (universal, and what
# a release build wants) and a Homebrew dylib (usually single-architecture).

SDL_KIND=""
SDL_SRC=""
SDL_CFLAGS=""
SDL_LIBS=""

find_sdl() {
    local fw
    for fw in "$ROOT/Frameworks/SDL2.framework" \
        "$HOME/Library/Frameworks/SDL2.framework" \
        "/Library/Frameworks/SDL2.framework"; do
        if [ -d "$fw" ]; then
            SDL_KIND="framework"
            SDL_SRC="$fw"
            SDL_CFLAGS="-F$(dirname "$fw") -I$fw/Headers"
            SDL_LIBS="-F$(dirname "$fw") -framework SDL2"
            return 0
        fi
    done

    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl2 2>/dev/null; then
        local libdir dylib
        libdir="$(pkg-config --variable=libdir sdl2)"
        dylib="$libdir/libSDL2-2.0.0.dylib"
        if [ -f "$dylib" ]; then
            SDL_KIND="dylib"
            SDL_SRC="$dylib"
            SDL_CFLAGS="$(pkg-config --cflags sdl2)"
            SDL_LIBS="$(pkg-config --libs sdl2)"
            return 0
        fi
    fi

    if command -v sdl2-config >/dev/null 2>&1; then
        local prefix dylib
        prefix="$(sdl2-config --prefix)"
        dylib="$prefix/lib/libSDL2-2.0.0.dylib"
        if [ -f "$dylib" ]; then
            SDL_KIND="dylib"
            SDL_SRC="$dylib"
            SDL_CFLAGS="$(sdl2-config --cflags)"
            SDL_LIBS="$(sdl2-config --libs)"
            return 0
        fi
    fi

    return 1
}

step "Locating SDL2"
find_sdl || die "SDL2 was not found.

  For a universal build, download the official macOS runtime and put
  SDL2.framework in /Library/Frameworks:
      https://github.com/libsdl-org/SDL/releases  (SDL2-2.x.y.dmg)

  Or install it with Homebrew, which builds for this Mac only:
      brew install sdl2"

echo "    $SDL_KIND: $SDL_SRC"

# ----------------------------------------------------------- choose the arches
#
# A universal binary cannot be linked against a single-architecture SDL2, so
# the build narrows to whatever the available library actually contains.

sdl_binary() {
    if [ "$SDL_KIND" = "framework" ]; then
        echo "$SDL_SRC/Versions/A/SDL2"
    else
        echo "$SDL_SRC"
    fi
}

SDL_ARCHS="$(lipo -archs "$(sdl_binary)" 2>/dev/null || echo "")"
[ -n "$SDL_ARCHS" ] || die "could not read the architectures of $(sdl_binary)"

ARCH_FLAGS=""
for arch in arm64 x86_64; do
    case " $SDL_ARCHS " in
    *" $arch "*) ARCH_FLAGS="$ARCH_FLAGS -arch $arch" ;;
    esac
done
[ -n "$ARCH_FLAGS" ] || die "SDL2 contains none of arm64/x86_64 (found: $SDL_ARCHS)"

echo "    SDL2 architectures: $SDL_ARCHS"
echo "    building:$ARCH_FLAGS"
if [ "$SDL_ARCHS" = "arm64" ] || [ "$SDL_ARCHS" = "x86_64" ]; then
    echo "    note: this SDL2 is single-architecture, so the app will be too."
    echo "          Use the SDL2.framework from libsdl.org for a universal build."
fi

# --------------------------------------------------------------------- compile
step "Building the game"
rm -rf "$BUILD" "$APP"
mkdir -p "$BUILD"

make -C "$ROOT" --no-print-directory \
    BUILD="$BUILD" \
    ARCHS="$ARCH_FLAGS" \
    SDL_CFLAGS="$SDL_CFLAGS" \
    SDL_LIBS="$SDL_LIBS" \
    EXTRA_LDFLAGS="-Wl,-rpath,@executable_path/../Frameworks" \
    all

BIN="$BUILD/graviton"
[ -x "$BIN" ] || die "the build produced no executable at $BIN"

# --------------------------------------------------------------- test the build
#
# The bundle is only worth assembling if the code in it is sound, so the same
# suite CI runs is run here against the binaries actually being shipped.
if [ "${GV_SKIP_TESTS:-0}" != "1" ]; then
    step "Running the test suite"
    make -C "$ROOT" --no-print-directory BUILD="$BUILD" ARCHS="$ARCH_FLAGS" test
fi

# ------------------------------------------------------------------ the bundle
step "Assembling $APP_NAME.app"
mkdir -p "$MACOS_DIR" "$RES_DIR" "$FW_DIR"
cp "$BIN" "$MACOS_DIR/graviton"
chmod +x "$MACOS_DIR/graviton"

cat >"$CONTENTS/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleExecutable</key>
	<string>graviton</string>
	<key>CFBundleIconFile</key>
	<string>$APP_NAME</string>
	<key>CFBundleIdentifier</key>
	<string>$BUNDLE_ID</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>$APP_NAME</string>
	<key>CFBundleDisplayName</key>
	<string>$APP_NAME</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>$VERSION</string>
	<key>CFBundleVersion</key>
	<string>$BUILD_NUMBER</string>
	<key>LSApplicationCategoryType</key>
	<string>public.app-category.action-games</string>
	<key>LSMinimumSystemVersion</key>
	<string>11.0</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSSupportsAutomaticGraphicsSwitching</key>
	<true/>
	<key>NSHumanReadableCopyright</key>
	<string>$COPYRIGHT</string>
</dict>
</plist>
PLIST

printf 'APPL????' >"$CONTENTS/PkgInfo"

# ------------------------------------------------------------------- the icon
step "Drawing the icon"
ICONSET="$BUILD/$APP_NAME.iconset"
rm -rf "$ICONSET"
mkdir -p "$ICONSET"

make -C "$ROOT" --no-print-directory \
    BUILD="$BUILD" ARCHS="$ARCH_FLAGS" \
    SDL_CFLAGS="$SDL_CFLAGS" SDL_LIBS="$SDL_LIBS" \
    "$BUILD/gv_icon"

# The generator links SDL2 from wherever it was found, which is still on this
# machine, so it runs fine before any bundling happens.
"$BUILD/gv_icon" "$ICONSET"
iconutil --convert icns --output "$RES_DIR/$APP_NAME.icns" "$ICONSET"

# ------------------------------------------------------------------ bundle SDL2
step "Bundling SDL2"
if [ "$SDL_KIND" = "framework" ]; then
    cp -R "$SDL_SRC" "$FW_DIR/"
    # Headers are dead weight in a shipped bundle. The licence stays: SDL2 is
    # redistributed here, and its terms travel with it.
    rm -rf "$FW_DIR/SDL2.framework/Headers" \
        "$FW_DIR/SDL2.framework/Versions/A/Headers" 2>/dev/null || true
    SDL_INSIDE="$FW_DIR/SDL2.framework/Versions/A/SDL2"
    # A framework is signed as a bundle, not as the Mach-O file inside it.
    SDL_SIGN="$FW_DIR/SDL2.framework"
    NEW_ID="@rpath/SDL2.framework/Versions/A/SDL2"
else
    cp "$SDL_SRC" "$FW_DIR/libSDL2-2.0.0.dylib"
    SDL_INSIDE="$FW_DIR/libSDL2-2.0.0.dylib"
    SDL_SIGN="$SDL_INSIDE"
    NEW_ID="@rpath/libSDL2-2.0.0.dylib"
fi

chmod u+w "$SDL_INSIDE"
OLD_ID="$(otool -D "$SDL_INSIDE" | tail -n 1)"
install_name_tool -id "$NEW_ID" "$SDL_INSIDE"

# Repoint the executable at the copy inside the bundle. `otool -L` puts the
# file being inspected on the first line, hence the tail: without it a bundle
# path that happened to contain "SDL2" would be mistaken for a dependency.
CURRENT="$(otool -L "$MACOS_DIR/graviton" | tail -n +2 | awk '/SDL2/ {print $1; exit}')"
if [ -n "$CURRENT" ] && [ "$CURRENT" != "$NEW_ID" ]; then
    install_name_tool -change "$CURRENT" "$NEW_ID" "$MACOS_DIR/graviton"
    echo "    $CURRENT -> $NEW_ID"
fi
# Belt and braces: if the linker recorded something other than the install name
# the library now advertises, retarget that too.
if [ -n "$OLD_ID" ] && [ "$OLD_ID" != "$CURRENT" ] && [ "$OLD_ID" != "$NEW_ID" ]; then
    install_name_tool -change "$OLD_ID" "$NEW_ID" "$MACOS_DIR/graviton" 2>/dev/null || true
fi

# ------------------------------------------------------------------- verify it
step "Checking the bundle is self-contained"
LEAKED="$(otool -L "$MACOS_DIR/graviton" | tail -n +2 | awk '{print $1}' |
    grep -Ev '^(/usr/lib/|/System/Library/|@rpath/|@executable_path/|@loader_path/)' || true)"
if [ -n "$LEAKED" ]; then
    echo "$LEAKED" >&2
    die "the executable still references libraries outside the bundle (listed above).
       Those paths will not exist on another Mac."
fi
otool -L "$MACOS_DIR/graviton" | sed 's/^/    /'

echo "    architectures: $(lipo -archs "$MACOS_DIR/graviton")"

# ---------------------------------------------------------------------- sign it
#
# Ad-hoc ("-") rather than with a Developer ID: there is no certificate here,
# and Apple silicon refuses to run an unsigned binary outright. Inner code is
# signed first — a bundle's signature covers what it contains, so re-signing a
# nested library afterwards would invalidate the outer signature.
step "Signing"
SIGN_ID="${GV_CODESIGN_IDENTITY:--}"
codesign --force --timestamp=none --sign "$SIGN_ID" "$SDL_SIGN"
codesign --force --timestamp=none --sign "$SIGN_ID" "$MACOS_DIR/graviton"
codesign --force --timestamp=none --sign "$SIGN_ID" "$APP"
codesign --verify --deep --strict "$APP" && echo "    signature verifies"

if [ "$SIGN_ID" = "-" ]; then
    echo "    (ad-hoc signature: Gatekeeper will still ask on first launch)"
fi

SIZE="$(du -sh "$APP" | cut -f1)"
echo
echo "built $APP  ($SIZE)"

# --------------------------------------------------------------------- the dmg
if [ "$WANT_DMG" = "1" ]; then
    step "Building the disk image"
    command -v hdiutil >/dev/null 2>&1 || die "'hdiutil' not found"

    STAGE="$BUILD/dmg"
    DMG="$DIST/$APP_NAME-$VERSION.dmg"
    rm -rf "$STAGE" "$DMG"
    mkdir -p "$STAGE"

    cp -R "$APP" "$STAGE/"
    ln -s /Applications "$STAGE/Applications"

    cat >"$STAGE/Read Me.txt" <<'README'
GRAVITON

Drag GRAVITON to the Applications folder, then launch it from there.

The first launch:
  This build is signed ad-hoc rather than with an Apple Developer ID, so
  macOS will refuse to open it on a double-click. Right-click (or
  Control-click) the app and choose Open, then confirm. macOS remembers the
  choice and every launch after that is normal.

Controls:
  WASD or arrows   thrust            Left mouse (hold)   fire the tether
  Mouse            aim               Right mouse/Space   pulse
  Wheel or Q/E     reel in and out   Shift               focus (slow motion)
  Escape           pause             F11                 fullscreen
  A game controller works throughout, and can be used instead.

Settings and high scores are kept in
  ~/Library/Application Support/Graviton/graviton.cfg
Deleting that file resets the game to its defaults.
README

    hdiutil create \
        -volname "$APP_NAME" \
        -srcfolder "$STAGE" \
        -fs HFS+ \
        -format UDZO \
        -imagekey zlib-level=9 \
        -ov \
        "$DMG" >/dev/null

    echo
    echo "built $DMG  ($(du -sh "$DMG" | cut -f1))"
fi

echo
echo "Done."
