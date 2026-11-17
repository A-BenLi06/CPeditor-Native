#!/usr/bin/env bash
# 构建和测试共同使用同一个 Xcode、SDK 和最低系统版本。

cpn_macos_toolchain() {
    if [[ "$(uname -s)" != Darwin ]]; then
        echo "此入口必须在 macOS 主机执行。Windows 请使用 tools/windows 下的 PowerShell 7 脚本。" >&2
        return 1
    fi
    ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    PACKAGE_DIR="$ROOT_DIR/apps/macos"
    METADATA="$PACKAGE_DIR/AppMetadata.plist"
    CPN_CORE_LIBRARY_DIR="${CPN_CORE_LIBRARY_DIR:-$ROOT_DIR/out/core/dev}"
    if [[ "$CPN_CORE_LIBRARY_DIR" != /* ]]; then
        echo "CPN_CORE_LIBRARY_DIR 必须是绝对路径。" >&2
        return 1
    fi
    export CPN_CORE_LIBRARY_DIR
    local configuration="$ROOT_DIR/script/macos-toolchain.plist"
    local schema required_major actual_major
    CPN_SWIFTPM_BUILD_SYSTEM="$(/usr/libexec/PlistBuddy -c 'Print :SwiftPMBuildSystem' "$configuration")"
    if [[ "$CPN_SWIFTPM_BUILD_SYSTEM" != native && "$CPN_SWIFTPM_BUILD_SYSTEM" != swiftbuild ]]; then
        echo "无法识别 SwiftPM 构建后端。" >&2
        return 1
    fi
    schema="$(/usr/libexec/PlistBuddy -c 'Print :SchemaVersion' "$configuration")"
    required_major="$(/usr/libexec/PlistBuddy -c 'Print :XcodeMajorVersion' "$configuration")"
    if [[ "$schema" != 1 || ! "$required_major" =~ ^[0-9]+$ ]]; then
        echo "无法识别 macOS 工具链配置。" >&2
        return 1
    fi
    DEVELOPER_DIR="${DEVELOPER_DIR:-$(/usr/bin/xcode-select -p)}"
    if [[ -d "$DEVELOPER_DIR/Contents/Developer" ]]; then
        DEVELOPER_DIR="$DEVELOPER_DIR/Contents/Developer"
    fi
    DEVELOPER_DIR="$(cd "$DEVELOPER_DIR" && pwd -P)"
    export DEVELOPER_DIR
    CPN_XCODE_VERSION="$(/usr/bin/xcodebuild -version)"
    actual_major="$(printf '%s\n' "$CPN_XCODE_VERSION" | /usr/bin/awk '/^Xcode / {split($2, parts, "."); print parts[1]}')"
    if [[ "$actual_major" != "$required_major" ]]; then
        echo "需要 Xcode $required_major；当前 DEVELOPER_DIR=$DEVELOPER_DIR，版本：$CPN_XCODE_VERSION" >&2
        return 1
    fi
    CPN_XCODE_TOOLCHAIN="$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain/usr"
    CPN_SWIFT="$(/usr/bin/xcrun --toolchain XcodeDefault --find swift)"
    CPN_SWIFTC="$(/usr/bin/xcrun --toolchain XcodeDefault --find swiftc)"
    CPN_CLANG="$(/usr/bin/xcrun --toolchain XcodeDefault --find clang)"
    CPN_CLANGXX="$(/usr/bin/xcrun --toolchain XcodeDefault --find clang++)"
    CPN_PYTHON="$(/usr/bin/xcrun --toolchain XcodeDefault --find python3)"
    CPN_OTOOL="$(/usr/bin/xcrun --toolchain XcodeDefault --find otool)"
    SDKROOT="$(/usr/bin/xcrun --sdk macosx --toolchain XcodeDefault --show-sdk-path)"
    CPN_SDK_VERSION="$(/usr/bin/xcrun --sdk macosx --toolchain XcodeDefault --show-sdk-version)"
    MACOSX_DEPLOYMENT_TARGET="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$METADATA")"
    if [[ ! "$MACOSX_DEPLOYMENT_TARGET" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
        echo "应用元数据中的最低系统版本无效。" >&2
        return 1
    fi
    # 子构建同样只能找到选定 Xcode 的编译器，不受外部 Swift 或 TOOLCHAINS 影响。
    export TOOLCHAINS=XcodeDefault
    export SDKROOT MACOSX_DEPLOYMENT_TARGET
    export SWIFT_EXEC="$CPN_SWIFTC" CC="$CPN_CLANG" CXX="$CPN_CLANGXX"
    export PATH="$CPN_XCODE_TOOLCHAIN/bin:$PATH"
    export CPN_XCODE_VERSION CPN_XCODE_TOOLCHAIN CPN_SWIFT CPN_SWIFTC CPN_CLANG CPN_CLANGXX CPN_SDK_VERSION CPN_OTOOL CPN_SWIFTPM_BUILD_SYSTEM
    printf '使用 %s；SDK %s；macOS 目标 %s\n' "${CPN_XCODE_VERSION//$'\n'/ }" "$CPN_SDK_VERSION" "$MACOSX_DEPLOYMENT_TARGET"
}

cpn_swiftpm() {
    local operation="$1"; shift
    # Xcode 27.2 的 swiftbuild 后端将已选 SDK 27.2 链接为 sdk=26.0；
    # 使用 SwiftPM 仍支持的 native 后端，让 SDK 选择贯穿编译与链接。
    "$CPN_SWIFT" "$operation" --package-path "$PACKAGE_DIR" --sdk "$SDKROOT" --toolchain "$CPN_XCODE_TOOLCHAIN" \
        --build-system "$CPN_SWIFTPM_BUILD_SYSTEM" "$@"
}

cpn_prepare_core() {
    local reuse=()
    local extra_targets=("$@")
    local clean_args=()
    if [[ "${CPN_CORE_PREBUILT:-0}" == 1 ]]; then
        reuse=(--reuse-core)
    else
        if [[ -f "$CPN_CORE_LIBRARY_DIR/CMakeCache.txt" ]]; then
            local cached_c cached_cxx cached_sysroot cached_target
            cached_c="$(/usr/bin/awk -F= '/^CMAKE_C_COMPILER:[^=]+=/ {print substr($0, index($0, "=") + 1); exit}' "$CPN_CORE_LIBRARY_DIR/CMakeCache.txt")"
            cached_cxx="$(/usr/bin/awk -F= '/^CMAKE_CXX_COMPILER:[^=]+=/ {print substr($0, index($0, "=") + 1); exit}' "$CPN_CORE_LIBRARY_DIR/CMakeCache.txt")"
            cached_sysroot="$(/usr/bin/awk -F= '/^CMAKE_OSX_SYSROOT:[^=]+=/ {print substr($0, index($0, "=") + 1); exit}' "$CPN_CORE_LIBRARY_DIR/CMakeCache.txt")"
            cached_target="$(/usr/bin/awk -F= '/^CMAKE_OSX_DEPLOYMENT_TARGET:[^=]+=/ {print substr($0, index($0, "=") + 1); exit}' "$CPN_CORE_LIBRARY_DIR/CMakeCache.txt")"
            if [[ "$cached_c" != "$CPN_CLANG" || "$cached_cxx" != "$CPN_CLANGXX" || "$cached_sysroot" != "$SDKROOT" || "$cached_target" != "$MACOSX_DEPLOYMENT_TARGET" ]]; then
                rm -rf "$CPN_CORE_LIBRARY_DIR/CMakeCache.txt" "$CPN_CORE_LIBRARY_DIR/CMakeFiles"
                clean_args=(--clean-first)
            fi
        fi
        cmake -S "$ROOT_DIR" -B "$CPN_CORE_LIBRARY_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
            "-DCMAKE_C_COMPILER=$CPN_CLANG" "-DCMAKE_CXX_COMPILER=$CPN_CLANGXX" \
            "-DCMAKE_OSX_SYSROOT=$SDKROOT" "-DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET"
        cmake --build "$CPN_CORE_LIBRARY_DIR" "${clean_args[@]}" --target cpn_core "${extra_targets[@]}"
    fi
    "$CPN_PYTHON" "$ROOT_DIR/script/verify_macos_build.py" --core-directory "$CPN_CORE_LIBRARY_DIR" "${reuse[@]}"
}
