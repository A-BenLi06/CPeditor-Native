#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/macos_toolchain.sh"
cpn_macos_toolchain
OUTPUT_DIR="$ROOT_DIR/out/macos"
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
# 小型生命周期工具也使用本轮明确选择的编译器和 SDK。
"$CPN_SWIFTC" -sdk "$SDKROOT" -target "$(uname -m)-apple-macosx$MACOSX_DEPLOYMENT_TARGET" "$ROOT_DIR/script/AppLifecycle.swift" -o "$LIFECYCLE"

cpn_prepare_core
cpn_swiftpm build
BIN_DIR="$(cpn_swiftpm build --show-bin-path)"
# 在退出旧应用前拒绝与清单、SDK 不一致的实际产物。
"$CPN_PYTHON" "$ROOT_DIR/script/verify_macos_build.py" --core-directory "$CPN_CORE_LIBRARY_DIR" --reuse-core \
    --binary "$BIN_DIR/$APP_EXECUTABLE" --receipt "$OUTPUT_DIR/build-verification.json"
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
    --debug) /usr/bin/xcrun --toolchain XcodeDefault lldb -- "$APP_BUNDLE/Contents/MacOS/$APP_EXECUTABLE" "$@" ;;
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
