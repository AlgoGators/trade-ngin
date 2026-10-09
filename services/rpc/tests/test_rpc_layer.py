"""The shared layer: registry, version header, a second service next to the desk, settings."""

import logging
import threading

import grpc
import pytest
from google.protobuf import descriptor_pool, descriptor_pb2, wrappers_pb2
from grpc_health.v1 import health_pb2, health_pb2_grpc

from algogators import desk_pb2 as pb
from algogators import desk_pb2_grpc
from algogators.versions import API_VERSIONS
from algogators_rpc import services as service_modules
from algogators_rpc.client import versioned_channel
from algogators_rpc.config import ConfigError, load_server_settings
from algogators_rpc.log import JsonFormatter
from algogators_rpc.registry import BackgroundTask, Registry, RegistryError, Service
from algogators_rpc.server import build_server, run_start_hooks, start_background_tasks
from algogators_rpc.services.desk import LEGACY_SERVICE_NAME, SERVICE_NAME
from algogators_rpc.services.desk.service import build_service
from algogators_rpc.versioning import HEADER, check, format_header, parse_header

DESK = API_VERSIONS["desk"]
STATUS_REQ = pb.RunStatusRequest(portfolio_id="CONSERVATIVE_PORTFOLIO", date="2026-10-07")


def bump(version, part):
    major, minor, patch = (int(x) for x in version.split("."))
    if part == "major":
        return f"{major + 1}.0.0"
    if part == "minor":
        return f"{major}.{minor + 1}.{patch}"
    return f"{major}.{minor}.{patch + 1}"


# -- a second service, defined without generated code, so nothing in desk is touched ----------

def _echo_descriptor():
    fdp = descriptor_pb2.FileDescriptorProto(
        name="algogators/echo_test.proto", package="algogators.echo", syntax="proto3",
        dependency=["google/protobuf/wrappers.proto"])
    svc = fdp.service.add(name="EchoService")
    svc.method.add(name="Echo", input_type=".google.protobuf.StringValue",
                   output_type=".google.protobuf.StringValue")
    pool = descriptor_pool.Default()
    try:
        return pool.FindServiceByName("algogators.echo.EchoService")
    except KeyError:
        return pool.AddSerializedFile(fdp.SerializeToString()).services_by_name["EchoService"]


ECHO_DESCRIPTOR = _echo_descriptor()
ECHO_METHOD = "/algogators.echo.EchoService/Echo"


class EchoServicer:
    def Echo(self, request, context):
        return wrappers_pb2.StringValue(value="echo:" + request.value)


def add_echo(servicer, server):
    server.add_generic_rpc_handlers((grpc.method_handlers_generic_handler(
        "algogators.echo.EchoService",
        {"Echo": grpc.unary_unary_rpc_method_handler(
            servicer.Echo, request_deserializer=wrappers_pb2.StringValue.FromString,
            response_serializer=wrappers_pb2.StringValue.SerializeToString)}),))


def echo_service(**kw):
    base = dict(api="echo", version="3.2.1", descriptor=ECHO_DESCRIPTOR,
                servicer=EchoServicer(), add_to_server=add_echo)
    base.update(kw)
    return Service(**base)


def echo_call(channel, value="hi", metadata=None):
    call = channel.unary_unary(ECHO_METHOD,
                               request_serializer=wrappers_pb2.StringValue.SerializeToString,
                               response_deserializer=wrappers_pb2.StringValue.FromString)
    return call(wrappers_pb2.StringValue(value=value), timeout=5, metadata=metadata)


def desk_status(channel, metadata=None):
    return desk_pb2_grpc.DeskServiceStub(channel).GetRunStatus.with_call(
        STATUS_REQ, timeout=5, metadata=metadata)


# -- version header: pure functions -----------------------------------------------------------

def test_parse_and_format_header():
    assert parse_header(" desk=1.2.3 , other=2.0.0") == {"desk": "1.2.3", "other": "2.0.0"}
    assert format_header({"other": "2.0.0", "desk": "1.2.3"}) == "desk=1.2.3,other=2.0.0"


@pytest.mark.parametrize("value,accepted", [
    ("desk=1.0.0", True), ("desk=1.7.0", True), ("desk=1.0.9", True),
    ("other=9.0.0,desk=1.3.0", True), ("desk=2.0.0", False), ("desk=0.9.0", False),
    (None, False), ("", False), ("other=1.0.0", False), ("desk=1.0", False), ("desk", False)])
def test_check(value, accepted):
    assert (check("desk", "1.0.0", value) is None) == accepted


def test_refusal_messages_name_both_versions():
    msg = check("desk", "1.0.0", "desk=2.0.0")
    assert msg == ("API version mismatch for 'desk': client speaks desk=2.0.0, server speaks "
                   "desk=1.0.0; the major versions must match")
    assert "missing x-algogators-api-version" in check("desk", "1.0.0", None)


# -- version header: on the wire ----------------------------------------------------------------

def test_desk_with_same_version_is_served_and_echoed(raw_channel):
    reply, call = desk_status(raw_channel, metadata=((HEADER, f"desk={DESK}"),))
    assert reply.state == pb.RUN_STATE_NOT_STARTED
    assert (HEADER, f"desk={DESK}") in tuple(call.initial_metadata())


@pytest.mark.parametrize("part", ["minor", "patch"])
def test_newer_minor_or_patch_is_accepted(raw_channel, part):
    reply, _ = desk_status(raw_channel, metadata=((HEADER, f"desk={bump(DESK, part)}"),))
    assert reply.state == pb.RUN_STATE_NOT_STARTED


def test_missing_header_is_refused(raw_channel, store):
    with pytest.raises(grpc.RpcError) as err:
        desk_status(raw_channel)
    assert err.value.code() == grpc.StatusCode.FAILED_PRECONDITION
    assert "missing x-algogators-api-version" in err.value.details()
    assert f"desk={DESK}" in err.value.details()
    assert store.calls == []  # refused before the servicer ran


def test_other_major_is_refused(raw_channel, store):
    other = bump(DESK, "major")
    with pytest.raises(grpc.RpcError) as err:
        desk_status(raw_channel, metadata=((HEADER, f"desk={other}"),))
    assert err.value.code() == grpc.StatusCode.FAILED_PRECONDITION
    assert f"client speaks desk={other}, server speaks desk={DESK}" in err.value.details()
    assert store.calls == []


def test_header_for_another_api_only_is_refused(raw_channel):
    with pytest.raises(grpc.RpcError) as err:
        desk_status(raw_channel, metadata=((HEADER, "other=1.0.0"),))
    assert err.value.code() == grpc.StatusCode.FAILED_PRECONDITION
    assert "names no version for API 'desk'" in err.value.details()


@pytest.mark.parametrize("service", ["", SERVICE_NAME])
def test_health_is_exempt(raw_channel, service):
    reply = health_pb2_grpc.HealthStub(raw_channel).Check(
        health_pb2.HealthCheckRequest(service=service), timeout=5)
    assert reply.status == health_pb2.HealthCheckResponse.SERVING


def test_legacy_name_is_served_without_header(raw_channel):
    """AlgoLens builds before the shared layer call algogators.qt.v1.DeskService, no header."""
    call = raw_channel.unary_unary(f"/{LEGACY_SERVICE_NAME}/GetRunStatus",
                                   request_serializer=pb.RunStatusRequest.SerializeToString,
                                   response_deserializer=pb.RunStatus.FromString)
    assert call(STATUS_REQ, timeout=5).state == pb.RUN_STATE_NOT_STARTED


def test_calls_are_logged_with_code(raw_channel, caplog):
    caplog.set_level(logging.INFO)
    with pytest.raises(grpc.RpcError):
        desk_status(raw_channel, metadata=((HEADER, "desk=99.0.0"),))
    desk_status(raw_channel, metadata=((HEADER, f"desk={DESK}"),))
    lines = [JsonFormatter().format(r) for r in caplog.records if r.getMessage() == "rpc call"]
    assert len(lines) == 2
    assert '"code": "FAILED_PRECONDITION"' in lines[0] and '"api_version": "desk=99.0.0"' in lines[0]
    assert '"code": "OK"' in lines[1]
    assert '"method": "/algogators.desk.DeskService/GetRunStatus"' in lines[1]


# -- registry -------------------------------------------------------------------------------------

def test_registry_refuses_duplicates_and_bad_entries(store):
    reg = Registry()
    reg.register(build_service(store))
    with pytest.raises(RegistryError, match="already registered"):
        reg.register(build_service(store))
    with pytest.raises(RegistryError, match="version"):
        Registry().register(echo_service(version="1.0"))
    with pytest.raises(RegistryError, match="package"):
        Registry().register(echo_service(api="other"))
    with pytest.raises(RegistryError, match="interval"):
        Registry().register(echo_service(
            background_tasks=(BackgroundTask("t", 0, run=lambda: None),)))


def test_registry_lookup(store):
    reg = Registry()
    desk = reg.register(build_service(store))
    assert reg.lookup(SERVICE_NAME) == (desk, False)
    assert reg.lookup(LEGACY_SERVICE_NAME) == (desk, True)
    assert reg.lookup("algogators.nope.Nope") is None
    assert reg.versions() == {"desk": DESK}


def test_second_service_alongside_desk(store):
    """A dummy service registers next to the desk; the desk code is not touched."""
    reg = Registry()
    reg.register(build_service(store))
    reg.register(echo_service())
    server, _, port = build_server(reg, "127.0.0.1:0", max_workers=2)
    server.start()
    try:
        both = {"desk": DESK, "echo": "3.0.0"}
        with versioned_channel(f"127.0.0.1:{port}", both) as ch:
            assert echo_call(ch).value == "echo:hi"
            assert desk_status(ch)[0].state == pb.RUN_STATE_NOT_STARTED
            health = health_pb2_grpc.HealthStub(ch)
            for name in ("", SERVICE_NAME, "algogators.echo.EchoService"):
                assert health.Check(health_pb2.HealthCheckRequest(service=name),
                                    timeout=5).status == health_pb2.HealthCheckResponse.SERVING
        with versioned_channel(f"127.0.0.1:{port}", {"desk": DESK, "echo": "2.9.9"}) as ch:
            with pytest.raises(grpc.RpcError) as err:
                echo_call(ch)
            assert "server speaks echo=3.2.1" in err.value.details()
            assert desk_status(ch)[0].state == pb.RUN_STATE_NOT_STARTED
    finally:
        server.stop(grace=None)


def test_background_tasks_start_hook_then_loop():
    events = []
    ran = threading.Event()

    def run():
        events.append("run")
        ran.set()

    def broken():
        raise RuntimeError("boom")

    reg = Registry()
    reg.register(echo_service(background_tasks=(
        BackgroundTask("echo.tick", 0.01, run=run, on_start=lambda: events.append("start")),
        BackgroundTask("echo.broken", 0.01, run=broken, on_start=broken))))
    run_start_hooks(reg)  # a failing hook is logged, not raised
    assert events == ["start"]
    stop = threading.Event()
    threads = start_background_tasks(reg, stop)
    assert ran.wait(5)
    stop.set()
    for t in threads:
        t.join(5)
        assert not t.is_alive()


# -- settings -----------------------------------------------------------------------------------

def test_server_settings_defaults_and_legacy_names():
    s = load_server_settings({})
    assert (s.listen, s.max_workers, s.services) == ("0.0.0.0:50051", 8, ("desk",))
    assert s.health_listen == "127.0.0.1:50052"
    assert load_server_settings({"RPC_HEALTH_LISTEN": "off"}).health_listen is None
    s = load_server_settings({"DESK_AGENT_LISTEN": "0.0.0.0:6000", "DESK_AGENT_MAX_WORKERS": "2"})
    assert (s.listen, s.max_workers) == ("0.0.0.0:6000", 2)
    s = load_server_settings({"RPC_LISTEN": "127.0.0.1:7000", "DESK_AGENT_LISTEN": "x:1",
                              "RPC_SERVICES": "desk, echo"})
    assert s.listen == "127.0.0.1:7000" and s.services == ("desk", "echo")
    with pytest.raises(ConfigError, match="RPC_MAX_WORKERS"):
        load_server_settings({"RPC_MAX_WORKERS": "0"})


def test_unknown_service_refuses():
    with pytest.raises(ConfigError, match="unknown service"):
        service_modules.load(Registry(), ["desk", "nope"], {})


# -- versions.py comes from the .proto headers -----------------------------------------------------

def test_versions_match_proto_headers():
    import gen_versions
    from pathlib import Path
    protos = Path(__file__).resolve().parents[3] / "proto" / "algogators"
    assert dict(gen_versions.parse(p) for p in sorted(protos.glob("*.proto"))) == API_VERSIONS


@pytest.mark.parametrize("header,error", [
    ("// AlgoGators gRPC API\n// API: other\n// Version: 1.0.0\n// Compatibility: x\n", "file name"),
    ("// AlgoGators gRPC API\n// API: thing\n// Version: 1.0\n// Compatibility: x\n", "Version"),
    ("// API: thing\n", "header block"),
])
def test_gen_versions_rejects_bad_headers(tmp_path, header, error):
    import gen_versions
    p = tmp_path / "thing.proto"
    p.write_text(header + 'syntax = "proto3";\npackage algogators.thing;\n')
    with pytest.raises(SystemExit, match=error):
        gen_versions.parse(p)
