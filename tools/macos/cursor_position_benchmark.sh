#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT_DIR/script/macos_toolchain.sh"
cpn_macos_toolchain
OUTPUT_DIR="${1:-$ROOT_DIR/out/macos/cursor-position-perf}"
mkdir -p "$OUTPUT_DIR"
case "$(uname -m)" in
    arm64) CPN_BENCH_ARCH=arm64 ;;
    x86_64) CPN_BENCH_ARCH=x86_64 ;;
    *) echo "不支持的 macOS 主机架构。" >&2; exit 1 ;;
esac
"$CPN_SWIFTC" -O -sdk "$SDKROOT" -target "$CPN_BENCH_ARCH-apple-macos$MACOSX_DEPLOYMENT_TARGET" \
    "$ROOT_DIR/apps/macos/Sources/NativeEditorModel/RawIdentifier.swift" \
    "$ROOT_DIR/apps/macos/Sources/NativeEditorModel/TextCoordinateIndex.swift" \
    "$ROOT_DIR/apps/macos/Sources/NativeEditorModel/SourceDocument.swift" \
    "$ROOT_DIR/apps/macos/Sources/NativeEditorModel/TextLineIndex.swift" \
    "$ROOT_DIR/tools/macos/CursorPositionBenchmark.swift" -o "$OUTPUT_DIR/cursor-position-benchmark"
"$OUTPUT_DIR/cursor-position-benchmark" "$OUTPUT_DIR/benchmark.json"
echo "状态栏局部基准：$OUTPUT_DIR/benchmark.json"
