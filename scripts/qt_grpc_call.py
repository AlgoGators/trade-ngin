"""Call one DeskService RPC the way AlgoLens does (scripts/qt_e2e.sh).

    docker exec -e PYTHONPATH=/opt/desk-agent/app desk-agent \
        /opt/desk-agent/venv/bin/python /app/scripts/qt_grpc_call.py RunDesk '{"audit_id": 7, ...}'

Prints the reply as one JSON line. Target: QT_DESK_AGENT (default 127.0.0.1:50051).
"""

import json
import os
import sys

import grpc
from google.protobuf.json_format import MessageToDict, ParseDict

from qt.v1 import desk_pb2, desk_pb2_grpc

REQUEST = {
    "RunDesk": desk_pb2.RunDeskRequest,
    "RequestOverride": desk_pb2.OverrideRequest,
    "RecordDecision": desk_pb2.DecisionRequest,
    "Publish": desk_pb2.PublishRequest,
    "GetRunStatus": desk_pb2.RunStatusRequest,
}


def main() -> int:
    rpc, body = sys.argv[1], json.loads(sys.argv[2] if len(sys.argv) > 2 else "{}")
    request = ParseDict(body, REQUEST[rpc]())
    target = os.environ.get("QT_DESK_AGENT", "127.0.0.1:50051")
    with grpc.insecure_channel(target) as channel:
        stub = desk_pb2_grpc.DeskServiceStub(channel)
        reply = getattr(stub, rpc)(request, timeout=30)
    print(json.dumps(MessageToDict(reply, preserving_proto_field_name=True)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
