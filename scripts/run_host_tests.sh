#!/usr/bin/env bash
# Configure, build and run the host unit tests for lib/dap.
# Set SANITIZE=1 to build with AddressSanitizer.
set -euo pipefail
APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if [ "${SANITIZE:-0}" = "1" ]; then
	BUILD="$APP_DIR/build/host_tests_asan"
	cmake -S "$APP_DIR/tests/host" -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_CXX_FLAGS="-fsanitize=address" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" >/dev/null
else
	BUILD="$APP_DIR/build/host_tests"
	cmake -S "$APP_DIR/tests/host" -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug >/dev/null
fi
cmake --build "$BUILD" --parallel >/dev/null
"$BUILD/dap_tests"
