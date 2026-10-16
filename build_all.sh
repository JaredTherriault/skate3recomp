#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${REPO_ROOT}"

JOBS="${JOBS:-2}"

echo "=== [1/2] Building Linux Targets (build) ==="
ninja -C build -j"${JOBS}" skate3 skate3_achievement_tests

if [ -d "build-win" ]; then
    echo "=== [2/2] Building Windows Targets (build-win) ==="
    # Re-run cmake configuration if CMakeLists.txt or sources were modified
    cmake -B build-win >/dev/null 2>&1 || true
    ninja -C build-win -j"${JOBS}" skate3
    if [ -f "build-win/skate3.exe" ] && [ -d "bin/windows" ]; then
        cp -p build-win/skate3.exe bin/windows/skate3.exe
        cp -p build-win/rexruntime.dll bin/windows/rexruntime.dll 2>/dev/null || true
    fi
fi

echo "=== Local build complete (Linux + Windows) ==="
