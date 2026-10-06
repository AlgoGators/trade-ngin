#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
scratch="$(mktemp -d)"
trap 'rm -rf "${scratch}"' EXIT

printf '%s' "host=/run/postgresql dbname=qt_test user=worker password=not-logged" > "${scratch}/connection"
chmod 600 "${scratch}/connection"

cat > "${scratch}/fake-worker" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == --connection-fd ]]
fd="$2"
if [[ "${3:-}" == --healthcheck ]]; then
  [[ -r "/proc/self/fd/${fd}" ]]
  printf '%s\n' '{"schema":"qt-desk-worker/v1","status":"healthy"}'
  exit 0
fi
[[ "$3" == --prepare-tool && "$4" == /opt/pinned/prepare ]]
[[ "$5" == --run-tool && "$6" == /opt/pinned/run ]]
[[ -r "/proc/self/fd/${fd}" ]]
material="$(cat "/proc/self/fd/${fd}")"
[[ "${material}" == *dbname=qt_test* ]]
[[ "${material}" == *password=not-logged* ]]
printf '%s\n' '{"schema":"qt-desk-worker/v1","status":"fake_started"}'
SH
chmod 700 "${scratch}/fake-worker"

output="$(QT_WORKER_BINARY="${scratch}/fake-worker" \
  QT_WORKER_CONNECTION_FILE="${scratch}/connection" \
  QT_PREPARE_TOOL=/opt/pinned/prepare QT_RUN_TOOL=/opt/pinned/run \
  "${repo_root}/scripts/run_qt_desk_worker.sh")"
[[ "${output}" == '{"schema":"qt-desk-worker/v1","status":"fake_started"}' ]]

health="$(QT_WORKER_BINARY="${scratch}/fake-worker" \
  QT_WORKER_CONNECTION_FILE="${scratch}/connection" \
  "${repo_root}/scripts/run_qt_desk_worker.sh" --healthcheck)"
[[ "${health}" == '{"schema":"qt-desk-worker/v1","status":"healthy"}' ]]

if QT_WORKER_BINARY="${scratch}/fake-worker" \
   QT_WORKER_CONNECTION_FILE="${scratch}/missing" \
   QT_PREPARE_TOOL=/opt/pinned/prepare QT_RUN_TOOL=/opt/pinned/run \
   "${repo_root}/scripts/run_qt_desk_worker.sh" >/dev/null 2>&1; then
  echo "missing connection file was accepted" >&2
  exit 1
fi

echo "qt-desk-worker start contract: PASS"
