#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
    echo "此脚本必须在 macOS 主机执行。Windows 请使用 tools/windows 下的 PowerShell 7 脚本。" >&2
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE_DIR="$ROOT_DIR/apps/macos"
OUTPUT_DIR="$ROOT_DIR/out/macos"
CPN_CORE_LIBRARY_DIR="${CPN_CORE_LIBRARY_DIR:-$ROOT_DIR/out/core/dev}"
export CPN_CORE_LIBRARY_DIR
METADATA="$PACKAGE_DIR/AppMetadata.plist"
APP_EXECUTABLE="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$METADATA")"
BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$METADATA")"
APP_BUNDLE="$OUTPUT_DIR/$APP_EXECUTABLE.app"
STAGING_BUNDLE="$OUTPUT_DIR/.$APP_EXECUTABLE-staging.app"
LIFECYCLE="$OUTPUT_DIR/AppLifecycle"
MODE=run
if [[ $# -gt 0 ]]; then
    case "$1" in
        run|--debug|--logs|--telemetry|--verify|--build-only) MODE="$1"; shift ;;
    esac
fi

mkdir -p "$OUTPUT_DIR"
if [[ ! -x "$LIFECYCLE" || "$ROOT_DIR/script/AppLifecycle.swift" -nt "$LIFECYCLE" ]]; then
    swiftc "$ROOT_DIR/script/AppLifecycle.swift" -o "$LIFECYCLE"
fi

if [[ "${CPN_CORE_PREBUILT:-0}" != 1 ]]; then
    cmake -S "$ROOT_DIR" -B "$CPN_CORE_LIBRARY_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
    cmake --build "$CPN_CORE_LIBRARY_DIR" --target cpn_core
fi
if [[ ! -f "$CPN_CORE_LIBRARY_DIR/libcpn_core.a" ]]; then
    echo "没有找到共享 Core 静态库：$CPN_CORE_LIBRARY_DIR/libcpn_core.a" >&2
    exit 1
fi
swift build --package-path "$PACKAGE_DIR"
BIN_DIR="$(swift build --package-path "$PACKAGE_DIR" --show-bin-path)"
"$LIFECYCLE" quit "$APP_BUNDLE"
rm -rf "$STAGING_BUNDLE"
mkdir -p "$STAGING_BUNDLE/Contents/MacOS" "$STAGING_BUNDLE/Contents/Resources"
cp "$BIN_DIR/$APP_EXECUTABLE" "$STAGING_BUNDLE/Contents/MacOS/$APP_EXECUTABLE"
cp "$METADATA" "$STAGING_BUNDLE/Contents/Info.plist"
chmod +x "$STAGING_BUNDLE/Contents/MacOS/$APP_EXECUTABLE"

# SwiftPM 生成的 Bundle.module accessor 首选 Bundle.main.resourceURL 下的资源 bundle。
shopt -s nullglob
for resource in "$BIN_DIR"/*.bundle; do
    ditto "$resource" "$STAGING_BUNDLE/Contents/Resources/$(basename "$resource")"
done
shopt -u nullglob
plutil -lint "$STAGING_BUNDLE/Contents/Info.plist"
codesign --force --sign - "$STAGING_BUNDLE"
rm -rf "$APP_BUNDLE"
mv "$STAGING_BUNDLE" "$APP_BUNDLE"

case "$MODE" in
    --build-only) echo "已构建：$APP_BUNDLE" ;;
    --debug) lldb -- "$APP_BUNDLE/Contents/MacOS/$APP_EXECUTABLE" "$@" ;;
    run) /usr/bin/open -n "$APP_BUNDLE" --args "$@" ;;
    --verify)
        /usr/bin/open -n "$APP_BUNDLE" --args "$@"
        "$LIFECYCLE" verify "$APP_BUNDLE"
        ;;
    --logs)
        /usr/bin/open -n "$APP_BUNDLE" --args "$@"
        /usr/bin/log stream --info --style compact --predicate "process == \"$APP_EXECUTABLE\""
        ;;
    --telemetry)
        /usr/bin/open -n "$APP_BUNDLE" --args "$@"
        /usr/bin/log stream --info --style compact --predicate "subsystem == \"$BUNDLE_ID\""
        ;;
esac
