#!/usr/bin/env bash
set -euo pipefail

worker="${QT_WORKER_BINARY:-/app/build/bin/Release/qt_desk_worker}"
connection_file="${QT_WORKER_CONNECTION_FILE:-/run/secrets/qt_worker_connection}"
prepare_tool="${QT_PREPARE_TOOL:-/app/build/bin/Release/qt_desk_prepare_sources}"
run_tool="${QT_RUN_TOOL:-/app/build/bin/Release/qt_desk_run}"
healthcheck=false
if [[ "${1:-}" == "--healthcheck" && "$#" -eq 1 ]]; then
    healthcheck=true
elif [[ "$#" -ne 0 ]]; then
    echo '{"schema":"qt-desk-worker/v1","status":"invalid_arguments"}' >&2
    exit 64
fi

if [[ "${worker}" != /* || ! -x "${worker}" ]]; then
    echo '{"schema":"qt-desk-worker/v1","status":"worker_unavailable"}' >&2
    exit 66
fi
if [[ "${connection_file}" != /* || ! -f "${connection_file}" || ! -r "${connection_file}" ]]; then
    echo '{"schema":"qt-desk-worker/v1","status":"connection_file_unavailable"}' >&2
    exit 66
fi
if [[ "${healthcheck}" == false && ("${prepare_tool}" != /* || "${run_tool}" != /*) ]]; then
    echo '{"schema":"qt-desk-worker/v1","status":"tool_path_invalid"}' >&2
    exit 64
fi

exec 9<"${connection_file}"
if [[ "${healthcheck}" == true ]]; then
    exec "${worker}" --connection-fd 9 --healthcheck
fi
exec "${worker}" --connection-fd 9 --prepare-tool "${prepare_tool}" --run-tool "${run_tool}"
