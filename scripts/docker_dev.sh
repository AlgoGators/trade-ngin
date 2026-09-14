#!/usr/bin/env bash
set -euo pipefail

# Build and run trade_ngin inside a container that carries the third-party
# dependencies. Use this when the host does not have Arrow / libpqxx / Eigen
# installed natively (scripts/dev_build_run.sh covers the native case).
#
# Usage:
#   scripts/docker_dev.sh image                 Build the dependency image (once)
#   scripts/docker_dev.sh build [TARGET]        Build a CMake target (default: all)
#   scripts/docker_dev.sh test [GTEST_FILTER]   Build and run the test suite
#   scripts/docker_dev.sh run TARGET [ARGS...]  Build and run an executable
#
# Examples:
#   scripts/docker_dev.sh image
#   scripts/docker_dev.sh test 'SpreadStrategyTest.*'
#   scripts/docker_dev.sh run bt_spread GC.v.0 SI.v.0 --years 2
#
# `run` mounts config.json read-only into the container working directory, so
# database-backed apps pick up the same credentials as a native run.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

IMAGE="trade-ngin-deps"
BUILD_VOLUME="trade-ngin-build"
LIB_PATH="/build/bin/Release"

docker_run() {
    # MSYS_NO_PATHCONV stops Git Bash on Windows from rewriting container paths.
    MSYS_NO_PATHCONV=1 docker run --rm "$@"
}

ensure_image() {
    if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
        echo "Dependency image '${IMAGE}' not found; building it now..." >&2
        build_image
    fi
}

build_image() {
    docker build -f "${REPO_ROOT}/docker/Dockerfile.devdeps" -t "${IMAGE}" "${REPO_ROOT}/docker"
}

ensure_volume() {
    docker volume inspect "${BUILD_VOLUME}" >/dev/null 2>&1 || docker volume create "${BUILD_VOLUME}" >/dev/null
}

# Compile a target. Extra args after the target are passed to `cmake --build`.
compile() {
    local target="${1:-}"
    ensure_image
    ensure_volume

    local build_args=()
    [[ -n "${target}" && "${target}" != "all" ]] && build_args=(--target "${target}")

    docker_run \
        -v "${REPO_ROOT}:/src:ro" \
        -v "${BUILD_VOLUME}:/build" \
        -v "${REPO_ROOT}/docker/build-in-container.sh:/build.sh:ro" \
        "${IMAGE}" bash /build.sh "${build_args[@]}"
}

case "${1:-}" in
    image)
        build_image
        ;;

    build)
        compile "${2:-all}"
        ;;

    test)
        compile trade_ngin_tests
        filter="${2:-*}"
        docker_run \
            -v "${BUILD_VOLUME}:/build" \
            "${IMAGE}" bash -c \
            "cd /build && LD_LIBRARY_PATH=${LIB_PATH} ${LIB_PATH}/trade_ngin_tests --gtest_filter='${filter}'"
        ;;

    run)
        target="${2:-}"
        if [[ -z "${target}" ]]; then
            echo "Error: 'run' needs a target, e.g. scripts/docker_dev.sh run bt_spread" >&2
            exit 1
        fi
        shift 2
        compile "${target}"

        config_mount=()
        if [[ -f "${REPO_ROOT}/config.json" ]]; then
            config_mount=(-v "${REPO_ROOT}/config.json:/seed-config.json:ro")
        else
            echo "Warning: no config.json at repo root; database-backed apps will fail." >&2
        fi

        # The app expects ./config.json and writes ./logs, so give it a writable cwd.
        docker_run \
            -v "${BUILD_VOLUME}:/build" \
            "${config_mount[@]}" \
            "${IMAGE}" bash -c \
            'mkdir -p /wd && cd /wd && [ -f /seed-config.json ] && cp /seed-config.json config.json;
             export LD_LIBRARY_PATH='"${LIB_PATH}"';
             exec '"${LIB_PATH}/${target}"' "$@"' _ "$@"
        ;;

    *)
        sed -n '4,21p' "${BASH_SOURCE[0]}" | sed 's|^# \{0,1\}||'
        exit 1
        ;;
esac
