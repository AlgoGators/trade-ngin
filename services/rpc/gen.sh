#!/usr/bin/env sh
# Generate the Python stubs for every proto/algogators/*.proto into services/rpc/algogators/,
# plus algogators/versions.py (API_VERSIONS, parsed from each file's version header). The stubs
# are build output, not source: they are .gitignored and produced by the Docker build and by
# CI. Needs grpcio-tools (requirements-build.txt).
#
#   services/rpc/gen.sh                 # uses python3 on PATH
#   PYTHON=.venv/bin/python services/rpc/gen.sh
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
PROTO_DIR=${PROTO_DIR:-$(cd "$HERE/../.." && pwd)/proto}
OUT_DIR=${OUT_DIR:-$HERE}
PYTHON=${PYTHON:-python3}

mkdir -p "$OUT_DIR"
rm -rf "$OUT_DIR/algogators"
# grpc_tools.protoc adds its bundled well-known types (google/protobuf/timestamp.proto) itself.
"$PYTHON" -m grpc_tools.protoc \
    -I "$PROTO_DIR" \
    --python_out="$OUT_DIR" \
    --pyi_out="$OUT_DIR" \
    --grpc_python_out="$OUT_DIR" \
    "$PROTO_DIR"/algogators/*.proto
touch "$OUT_DIR/algogators/__init__.py"
"$PYTHON" "$HERE/gen_versions.py" "$PROTO_DIR/algogators" "$OUT_DIR/algogators/versions.py"
echo "generated $OUT_DIR/algogators/"
