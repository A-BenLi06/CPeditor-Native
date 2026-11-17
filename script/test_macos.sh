#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/macos_toolchain.sh"
cpn_macos_toolchain
cpn_prepare_core cpn_checker_helper
cpn_swiftpm test "$@"
