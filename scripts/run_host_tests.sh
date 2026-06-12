#!/usr/bin/env bash
# Configure, build and run the host unit tests for lib/dap.
set -euo pipefail
APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$APP_DIR/build/host_tests"
cmake -S "$APP_DIR/tests/host" -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$BUILD" --parallel >/dev/null
"$BUILD/dap_tests"
