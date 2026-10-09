"""Call one DeskService RPC the way AlgoLens does (scripts/qt_e2e.sh).

    docker exec -e PYTHONPATH=/opt/rpc/app engine-rpc \
        /opt/rpc/venv/bin/python /app/scripts/qt_grpc_call.py RunDesk '{"audit_id": 7, ...}'

Sends the x-algogators-api-version header from the generated algogators.versions (override it
with QT_API_VERSION, e.g. "desk=2.0.0", to see a refusal). Prints the reply as one JSON line.
Target: RPC_TARGET (default 127.0.0.1:50051; QT_DESK_AGENT is still read for one release).
"""

import json
import os
import sys

from google.protobuf.json_format import MessageToDict, ParseDict

from algogators import desk_pb2, desk_pb2_grpc
from algogators.versions import API_VERSIONS
from algogators_rpc.client import versioned_channel
from algogators_rpc.versioning import parse_header

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
    target = (os.environ.get("RPC_TARGET") or os.environ.get("QT_DESK_AGENT")
              or "127.0.0.1:50051")
    override = os.environ.get("QT_API_VERSION")
    versions = parse_header(override) if override else API_VERSIONS
    with versioned_channel(target, versions) as channel:
        stub = desk_pb2_grpc.DeskServiceStub(channel)
        reply = getattr(stub, rpc)(request, timeout=30)
    print(json.dumps(MessageToDict(reply, preserving_proto_field_name=True)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
