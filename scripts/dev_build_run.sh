#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   scripts/dev_build_run.sh bt_trend [Release|Debug]
#   scripts/dev_build_run.sh live_trend [Release|Debug]
#
# Examples:
#   scripts/dev_build_run.sh bt_trend Release
#   scripts/dev_build_run.sh live_trend Debug

TARGET_NAME="${1:-}"
CONFIG="${2:-Release}"

if [[ -z "${TARGET_NAME}" ]]; then
  echo "Error: missing target. Use: bt_trend or live_trend" >&2
  exit 1
fi

if [[ "${TARGET_NAME}" != "bt_trend" && "${TARGET_NAME}" != "live_trend" ]]; then
  echo "Error: invalid target '${TARGET_NAME}'. Use: bt_trend or live_trend" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}/.."
BUILD_DIR="${REPO_ROOT}/build"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Configure if first time (no cache)
if [[ ! -f CMakeCache.txt ]]; then
  cmake ..
fi

# Build (support multi-config and single-config generators)
if cmake --build . --target help >/dev/null 2>&1; then
  cmake --build . --config "${CONFIG}"
else
  cmake --build .
fi

# Resolve binary path for both generator types
BIN_DIR_MC="${BUILD_DIR}/bin/${CONFIG}"
BIN_DIR_SC="${BUILD_DIR}/bin"

if [[ -x "${BIN_DIR_MC}/${TARGET_NAME}" ]]; then
  EXEC_PATH="${BIN_DIR_MC}/${TARGET_NAME}"
elif [[ -x "${BIN_DIR_SC}/${TARGET_NAME}" ]]; then
  EXEC_PATH="${BIN_DIR_SC}/${TARGET_NAME}"
else
  echo "Error: built binary not found for target '${TARGET_NAME}'. Looked in:" >&2
  echo "  ${BIN_DIR_MC}/${TARGET_NAME}" >&2
  echo "  ${BIN_DIR_SC}/${TARGET_NAME}" >&2
  exit 1
fi

echo "Running ${EXEC_PATH}..."
"${EXEC_PATH}"
