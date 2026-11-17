#!/usr/bin/env python3
"""核对实际对象文件、CMake 缓存与原生构建工具链，证据只写入 out。"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def run(*arguments):
    return subprocess.check_output(arguments, text=True, stderr=subprocess.STDOUT).strip()


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def version(value):
    parts = tuple(int(part) for part in value.split("."))
    return parts + (0,) * (3 - len(parts))


def object_versions(path):
    output = run(os.environ["CPN_OTOOL"], "-l", str(path))
    headers = list(re.finditer(r"^.+:\s*$", output, re.MULTILINE))
    result = []
    for index, header in enumerate(headers):
        label = header.group().strip()
        if label.startswith("Archive : "):
            continue
        end = headers[index + 1].start() if index + 1 < len(headers) else len(output)
        commands = output[header.end():end]
        builds = re.findall(r"cmd LC_BUILD_VERSION\s+cmdsize \d+\s+platform (\S+)\s+minos (\S+)\s+sdk (\S+)", commands)
        if len(builds) != 1:
            raise ValueError(f"缺少唯一的 LC_BUILD_VERSION：{label}")
        platform, minimum, sdk = builds[0]
        if platform not in ("1", "MACOS"):
            raise ValueError(f"对象不是 macOS：{label}")
        if version(minimum) != version(os.environ["MACOSX_DEPLOYMENT_TARGET"]):
            raise ValueError(f"最低系统不一致：{label} 为 {minimum}")
        if version(sdk) != version(os.environ["CPN_SDK_VERSION"]):
            raise ValueError(f"SDK 不一致：{label} 为 {sdk}")
        result.append({"object": label, "minos": minimum, "sdk": sdk})
    if not result:
        raise ValueError(f"未找到 Mach-O 对象：{path}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-directory", type=Path, required=True)
    parser.add_argument("--reuse-core", action="store_true")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--receipt", type=Path)
    arguments = parser.parse_args()
    core = arguments.core_directory.resolve()
    archive = core / "libcpn_core.a"
    cache = {}
    for line in (core / "CMakeCache.txt").read_text().splitlines():
        if re.match(r"^[^#/:][^:]*:[^=]+=", line):
            name_type, value = line.split("=", 1)
            cache[name_type.split(":", 1)[0]] = value
    required = {
        "CMAKE_C_COMPILER": os.environ["CPN_CLANG"],
        "CMAKE_CXX_COMPILER": os.environ["CPN_CLANGXX"],
        "CMAKE_OSX_SYSROOT": os.environ["SDKROOT"],
        "CMAKE_OSX_DEPLOYMENT_TARGET": os.environ["MACOSX_DEPLOYMENT_TARGET"],
    }
    for name, expected in required.items():
        if cache.get(name) != expected:
            raise ValueError(f"Core 的 {name}={cache.get(name)!r}，当前要求 {expected!r}；请不带 CPN_CORE_PREBUILT 重新构建")
    toolchain = {
        "developer_directory": os.environ["DEVELOPER_DIR"],
        "xcode": os.environ["CPN_XCODE_VERSION"],
        "swift": run(os.environ["CPN_SWIFT"], "--version"),
        "clang": run(os.environ["CPN_CLANG"], "--version"),
        "sdk_path": os.environ["SDKROOT"],
        "sdk_version": os.environ["CPN_SDK_VERSION"],
        "deployment_target": os.environ["MACOSX_DEPLOYMENT_TARGET"],
        "swiftpm_build_system": os.environ["CPN_SWIFTPM_BUILD_SYSTEM"],
    }
    archive_digest = digest(archive)
    core_receipt = core / "macos-build-receipt.json"
    if arguments.reuse_core:
        previous = json.loads(core_receipt.read_text())
        if previous.get("schema_version") != 1 or previous.get("toolchain") != toolchain or previous.get("archive_sha256") != archive_digest:
            raise ValueError("预构建 Core 的工具链／SDK／目标／归档字节与构建回执不一致；请重新构建")
    result = {
        "schema_version": 1,
        "generated_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "toolchain": toolchain,
        "archive": str(archive),
        "archive_sha256": archive_digest,
        "core_objects": object_versions(archive),
    }
    if arguments.binary:
        result["binary"] = str(arguments.binary.resolve())
        result["binary_sha256"] = digest(arguments.binary)
        result["binary_objects"] = object_versions(arguments.binary)
    if not arguments.reuse_core:
        core_receipt.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    if arguments.receipt:
        arguments.receipt.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print(f"已核对 {len(result['core_objects'])} 个 Core 对象；macOS {toolchain['deployment_target']}，SDK {toolchain['sdk_version']}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"macOS 构建验证失败：{error}", file=sys.stderr)
        sys.exit(1)
