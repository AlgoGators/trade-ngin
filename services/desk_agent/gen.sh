#!/usr/bin/env sh
# Generate the Python stubs for proto/qt/v1/desk.proto into services/desk_agent/qt/v1/.
# The stubs are build output, not source: they are .gitignored and produced by the Docker
# build and by CI. Needs grpcio-tools (requirements-build.txt).
#
#   services/desk_agent/gen.sh                 # uses python3 on PATH
#   PYTHON=.venv/bin/python services/desk_agent/gen.sh
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
PROTO_DIR=${PROTO_DIR:-$(cd "$HERE/../.." && pwd)/proto}
OUT_DIR=${OUT_DIR:-$HERE}
PYTHON=${PYTHON:-python3}

mkdir -p "$OUT_DIR"
rm -rf "$OUT_DIR/qt"
# grpc_tools.protoc adds its bundled well-known types (google/protobuf/timestamp.proto) itself.
"$PYTHON" -m grpc_tools.protoc \
    -I "$PROTO_DIR" \
    --python_out="$OUT_DIR" \
    --pyi_out="$OUT_DIR" \
    --grpc_python_out="$OUT_DIR" \
    "$PROTO_DIR/qt/v1/desk.proto"
touch "$OUT_DIR/qt/__init__.py" "$OUT_DIR/qt/v1/__init__.py"
echo "generated $OUT_DIR/qt/v1/desk_pb2.py and desk_pb2_grpc.py"
