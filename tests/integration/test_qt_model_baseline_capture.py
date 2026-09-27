"""Capture adapter contracts; pure cases never execute a MODEL binary.

The separately selected owned-PG case will exercise the real compiled runner.
Synthetic fixture evidence does not certify authentic before/after parity.
"""
from hashlib import sha256
import importlib.util
import json
import os
import uuid
from copy import deepcopy
from pathlib import Path

import pytest

ROOT = Path(__file__).parents[2]
MODULE = ROOT / "apps/tools/qt_model_baseline_capture.py"


@pytest.fixture
def adapter():
    assert MODULE.is_file(), "actual MODEL baseline capture adapter is missing"
    spec = importlib.util.spec_from_file_location("qt_model_baseline_capture", MODULE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("day", ["", "2026-02-30", "2026-9-26", "2026-09-26 --send-email", "2026-09-26x"])
def test_strict_day_refuses_email_and_invalid_dates(adapter, day):
    with pytest.raises(adapter.CaptureError, match="invalid_day"):
        adapter.validate_day(day)


def test_exact_historical_day_preserved(adapter):
    assert adapter.validate_day("2026-09-25") == "2026-09-25"


@pytest.mark.parametrize("dsn", ["", "host=127.0.0.1 dbname=algolens_test_x user=postgres",
    "host=/tmp/other/socket dbname=algolens_test_x user=postgres",
    "host=/tmp/algolens-repair-pg-123/socket dbname=production user=postgres",
    "host=/tmp/algolens-repair-pg-123/socket dbname=algolens_test_x user=postgres options=evil",
    "host=/tmp/algolens-repair-pg-123/socket dbname=algolens_test_x user=postgres password=secret",
    "host=/tmp/algolens-repair-pg-123/socket dbname=algolens_test_x user=postgres host=other"])
def test_external_or_ambiguous_database_refused(adapter, dsn):
    with pytest.raises(adapter.CaptureError, match="invalid_owned_dsn"):
        adapter.owned_database(dsn)


def test_owned_socket_public_placeholder_parsed(adapter):
    dsn = "host=/tmp/algolens-repair-pg-ab12/socket port=5432 dbname=algolens_test_ab12 user=postgres password=synthetic-test-only connect_timeout=2"
    assert adapter.owned_database(dsn) == {
        "host": "/tmp/algolens-repair-pg-ab12/socket", "port": "5432",
        "dbname": "algolens_test_ab12", "user": "postgres",
        "password": "synthetic-test-only", "connect_timeout": "2"}


def test_runner_socket_uri_encodes_owned_path_without_external_fallback(adapter):
    values = adapter.owned_database("host=/tmp/algolens-repair-pg-ab12/socket dbname=algolens_test_ab12 user=postgres")
    config = adapter.runner_database_config(values)
    assert config["host"] == "%2Ftmp%2Falgolens-repair-pg-ab12%2Fsocket"
    assert config["password"] == "synthetic-test-only"
    assert config["name"] == "algolens_test_ab12" and config["username"] == "postgres"


def test_libpq_parser_reads_generated_uri_as_owned_socket(adapter):
    from psycopg2.extensions import parse_dsn
    values = adapter.owned_database("host=/tmp/algolens-repair-pg-ab12/socket dbname=algolens_test_ab12 user=postgres")
    config = adapter.runner_database_config(values)
    uri = "postgresql://" + config["username"] + ":" + config["password"] + "@" + config["host"] + ":" + config["port"] + "/" + config["name"]
    assert parse_dsn(uri)["host"] == values["host"]
    assert parse_dsn(uri)["password"] == "synthetic-test-only"
    assert adapter.owned_connection_parameters(values)["password"] == "synthetic-test-only"


def test_child_environment_has_no_inherited_credentials_or_runtime_activation(adapter, monkeypatch, tmp_path):
    monkeypatch.setenv("PGSERVICE", "production")
    monkeypatch.setenv("QT_RUNTIME_CONTROL_ENABLED", "true")
    monkeypatch.setenv("DB_PASSWORD", "must-not-read")
    env = adapter.child_environment(tmp_path / "libguard.so", tmp_path, tmp_path / "temp")
    assert set(env) == {"PATH", "TZ", "LC_ALL", "TMPDIR", "LD_PRELOAD", "LD_LIBRARY_PATH", "QT_EMAIL_DELIVERY_ENABLED"}
    assert env["QT_EMAIL_DELIVERY_ENABLED"] == "false" and env["TZ"] == "UTC"
    assert "PGSERVICE" not in env and "QT_RUNTIME_CONTROL_ENABLED" not in env


def test_lossless_table_export_retains_every_value_and_duplicate_row(adapter):
    columns = [["amount", "numeric(30,8)"], ["note", "text"], ["nothing", "text"]]
    rows = [["1.23000000", " raw\ntext ", None], ["1.23000000", " raw\ntext ", None]]
    raw = adapter.table_bytes(columns, rows)
    value = json.loads(raw)
    assert value == {"columns": ["amount", "note", "nothing"],
                     "types": ["numeric(30,8)", "text", "text"], "rows": rows}
    assert b"1.23000000" in raw


def test_incomplete_table_row_refused(adapter):
    with pytest.raises(adapter.CaptureError, match="incomplete_table_row"):
        adapter.table_bytes([["a", "text"], ["b", "text"]], [["only-one"]])


def test_raw_capture_differences_are_not_normalized_into_parity(adapter):
    before = {"table:trading.positions": b"same", "log:stdout": b"2026-01-01 uuid-A\n"}
    after = {"table:trading.positions": b"same", "log:stdout": b"2026-01-02 uuid-B\n"}
    result = adapter.characterize(before, after)
    assert result["byte_identical"] is False
    assert result["different_outputs"] == ["log:stdout"]
    assert result["authentic_runtime_certification"] == "unavailable"


def test_missing_capture_output_is_refused(adapter):
    with pytest.raises(adapter.CaptureError, match="capture_inventory_mismatch"):
        adapter.characterize({"log:a": b"x"}, {})


@pytest.mark.parametrize('change', ['clock', 'identity', 'whitespace', 'missing'])
def test_reference_repeatability_never_normalizes_real_raw_differences(adapter, change):
    # Raw-byte comparison contract only. No fabricated MODEL emitter executes.
    before = {'table:trading.positions': b'{"id":"recorded-A","clock":"2026-09-26"}',
              'log:runner': b'complete raw log\n'}
    after = dict(before)
    if change == 'clock': after['table:trading.positions'] = before['table:trading.positions'].replace(b'2026-09-26', b'2026-09-27')
    elif change == 'identity': after['table:trading.positions'] = before['table:trading.positions'].replace(b'recorded-A', b'recorded-B')
    elif change == 'whitespace': after['log:runner'] += b' '
    else: del after['log:runner']
    with pytest.raises(adapter.CaptureError, match='reference_outputs_differ|capture_inventory_mismatch'):
        adapter.require_reference_repeatability(before, after)


def test_reference_repeatability_reports_exact_equal_raw_outputs_without_authentic_claim(adapter):
    raw = {'table:trading.positions': b'all columns\n', 'log:runner': b'raw\n', 'csv:positions': b'header\n'}
    result = adapter.require_reference_repeatability(raw, raw.copy())
    assert result['byte_identical'] is True and result['normalization'] == 'none'
    assert result['authentic_runtime_certification'] == 'unavailable'


def test_deterministic_public_fixture_is_explicit_and_keeps_catalog_controls_visible():
    control = ROOT / 'tests/fixtures/qt_model_baseline/deterministic-controls.sql'
    assert control.is_file(), 'owned deterministic fixture provider is missing'
    sql = control.read_text()
    assert 'CREATE FUNCTION trading.gen_random_uuid' in sql
    assert 'CREATE FUNCTION trading.now' in sql and 'CREATE FUNCTION trading.clock_timestamp' in sql
    assert 'CREATE SEQUENCE trading.qt_capture_uuid_seq' in sql
    assert 'search_path' in sql and 'pg_catalog' in sql
    assert '2026-09-26T12:00:00Z' in sql
    assert 'CREATE OR REPLACE' not in sql and 'qt_action_grants' not in sql


def test_raw_output_scan_refuses_unknown_file_and_escape(adapter, tmp_path):
    (tmp_path / "unexpected.txt").write_bytes(b"extra")
    with pytest.raises(adapter.CaptureError, match="unlisted_runner_output"):
        adapter.collect_runner_files(tmp_path, "live_trend", "BASE_PORTFOLIO", ["2026-09-25_positions.csv"])


def test_raw_output_scan_collects_every_logger_part_and_exact_csv(adapter, tmp_path):
    logs = tmp_path / "logs"; logs.mkdir()
    outputs = tmp_path / "apps/strategies/results/BASE_PORTFOLIO"; outputs.mkdir(parents=True)
    for part in (1, 2):
        (logs / f"live_trend_20260926_120000_part{part}.log").write_bytes(b"log" + bytes([part]))
    (outputs / "2026-09-25_positions.csv").write_bytes(b"header\nES,2.00\n")
    result = adapter.collect_runner_files(tmp_path, "live_trend", "BASE_PORTFOLIO", ["2026-09-25_positions.csv"])
    assert len(result) == 3
    assert result["csv:2026-09-25_positions.csv"] == b"header\nES,2.00\n"
    assert sorted(v for k, v in result.items() if k.startswith("log:")) == [b"log\x01", b"log\x02"]


def test_raw_output_scan_requires_expected_csv_even_when_runner_exit_zero(adapter, tmp_path):
    (tmp_path / "logs").mkdir()
    (tmp_path / "logs/live_trend_20260926_120000_part1.log").write_bytes(b"success")
    with pytest.raises(adapter.CaptureError, match="missing_expected_csv"):
        adapter.collect_runner_files(tmp_path, "live_trend", "BASE_PORTFOLIO", ["2026-09-25_positions.csv"])


def test_preflight_bounds_total_before_reading_artifact_bytes(adapter, monkeypatch, tmp_path):
    logs = tmp_path / "logs"; logs.mkdir()
    (logs / "live_trend_20260926_120000_part1.log").write_bytes(b"1234")
    (logs / "live_trend_20260926_120000_part2.log").write_bytes(b"1234")
    monkeypatch.setattr(adapter, "MAX_TOTAL", 7)
    with pytest.raises(adapter.CaptureError, match="capture_capacity_exceeded"):
        adapter.collect_runner_files(tmp_path, "live_trend", "BASE_PORTFOLIO", [])


def test_public_fixture_has_full_independent_table_pins(adapter):
    schemas = json.loads((ROOT / "tests/fixtures/qt_model_baseline/table-schemas.json").read_text())
    assert adapter.REQUIRED_TABLES <= set(schemas)
    assert len(schemas) == 29
    assert len(schemas["trading.live_results"]) == 46
    assert len(schemas["trading.executions"]) == 17
    assert schemas["trading.positions"][-1] == ["qt_proposal_revision", "uuid"]
    assert dict(schemas["trading.positions"])["quantity"] == "numeric(20,8)"
    assert schemas["trading.qt_model_seed_publications"][:2] == [["publication_id", "uuid"], ["attempt_id", "uuid"]]


def test_real_legacy_report_reader_has_explicit_disabled_fixture_scope():
    fixture = ROOT / "tests/fixtures/qt_model_baseline"
    original = ROOT.parents[0] / "algolens-qt/algolens-api/migrations/003_qt_decision_workflow.sql"
    copied = fixture / "api-003-qt-decision-workflow.sql"
    assert copied.is_file(), "actual governance schema required by real report reader is missing"
    assert copied.read_bytes() == original.read_bytes()
    scope = (fixture / "legacy-report-scope.sql").read_text()
    assert "('BASE_PORTFOLIO',false,1)" in scope
    assert "('CONSERVATIVE_PORTFOLIO',false,1)" in scope
    assert "qt_action_grants" not in scope and "qt_decisions" not in scope
    schemas = json.loads((fixture / "table-schemas.json").read_text())
    assert schemas["auth.users"] == [["id", "bigint"], ["role", "text"]]
    assert schemas["trading.qt_workflow_capabilities"][:3] == [
        ["book_id", "text"], ["enabled", "boolean"], ["version", "bigint"]]
    assert len(schemas["trading.qt_decisions"]) == 17


def test_actual_metadata_loader_positional_contract_is_complete():
    schemas = json.loads((ROOT / "tests/fixtures/qt_model_baseline/table-schemas.json").read_text())
    columns = schemas["metadata.contract_metadata"]
    assert len(columns) == 21
    assert [columns[i][0] for i in (2, 8, 10, 12, 19)] == [
        "Name", "Asset Type", "Contract Size", "Minimum Price Fluctuation", "Contract Months"]


@pytest.mark.parametrize("code,guard", [(0, False), (1, False), (86, True), (-15, False)])
def test_native_exit_is_durable_and_delivery_guard_is_distinct(adapter, tmp_path, code, guard):
    adapter.record_native_exit(tmp_path, code)
    evidence = json.loads((tmp_path / "native-exit.json").read_text())
    assert evidence == {"native_exit": code, "delivery_guard_exit": guard}


def test_request_refuses_unbound_schema_or_substituted_target_before_launch(adapter):
    with pytest.raises(adapter.CaptureError, match="invalid_request"):
        adapter.validate_request({"binary": "qt_desk_storage_probe"})
    value = dict.fromkeys(adapter.REQUEST_KEYS)
    value.update(schema_version="qt-model-baseline-capture/v1", fixture_kind="synthetic", target="qt_desk_storage_probe")
    with pytest.raises(adapter.CaptureError, match="actual_model_target_required"):
        adapter.validate_request(value)


@pytest.fixture
def guard_request(tmp_path):
    """ELF-prefix bytes test request guards only; never executable MODEL output."""
    sources = ["apps/strategies/live_portfolio_runner.cpp", "apps/strategies/live_portfolio_runner.hpp",
               "apps/strategies/CMakeLists.txt", "include/trade_ngin/core/holidays.json",
               "tests/integration/qt_no_delivery_guard.cpp", "tests/fixtures/qt_model_baseline/base.sql"]
    request = {"schema_version": "qt-model-baseline-capture/v1", "fixture_kind": "synthetic", "target": "live_portfolio",
        "day": "2026-09-25", "source_root": str(ROOT.absolute()),
        "source_files": {name: sha256((ROOT / name).read_bytes()).hexdigest() for name in sources},
        "build_id": "guard-unit-only", "artifacts": {}, "schema_files": [],
        "table_schemas": json.loads((ROOT / "tests/fixtures/qt_model_baseline/table-schemas.json").read_text()),
        "expected_csv": ["2026-09-25_positions.csv"], "expected_sequences": [],
        "required_postconditions": [{"table": "trading.positions", "equals": {"portfolio_type": "system"}, "minimum": 1}]}
    for key, name in (("binary", "live_portfolio"), ("library", "libtrade_ngin.so"), ("guard", "libqt_no_delivery_guard.so")):
        path = tmp_path / name; path.write_bytes(b"\x7fELFunit-guard-only-" + key.encode())
        request["artifacts"][key] = {"path": str(path.absolute()), "sha256": sha256(path.read_bytes()).hexdigest()}
    fixture = json.loads((ROOT / "tests/fixtures/qt_model_baseline/config.json").read_text())
    request["config_files"] = {"defaults.json": fixture["defaults"], **{
        "portfolios/base/" + name + ".json": fixture[name] for name in ("portfolio", "risk", "email")}}
    request["schema_files"] = [{"path": str((ROOT / sources[-1]).absolute()), "sha256": request["source_files"][sources[-1]]}]
    request["required_postconditions"] = [{"table": table, "equals": equals, "minimum": 1} for table, equals in (
        ("trading.positions", {"portfolio_id": "BASE_PORTFOLIO", "date": "2026-09-25", "portfolio_type": "system"}),
        ("trading.positions", {"portfolio_id": "BASE_PORTFOLIO", "date": "2026-09-25", "portfolio_type": "qt_proposal"}),
        ("trading.signals", {"portfolio_id": "BASE_PORTFOLIO"}),
        ("trading.executions", {"portfolio_id": "BASE_PORTFOLIO", "portfolio_type": "system"}),
        ("trading.live_results", {"portfolio_id": "BASE_PORTFOLIO", "portfolio_type": "system"}),
        ("trading.equity_curve", {"portfolio_id": "BASE_PORTFOLIO", "portfolio_type": "system"}),
        ("trading.qt_model_seed_publications", {"portfolio_id": "BASE_PORTFOLIO", "source_day": "2026-09-25"}),
        ("trading.live_run_metadata", {"portfolio_id": "BASE_PORTFOLIO"}),
        ("trading.run_inputs", {"portfolio_id": "BASE_PORTFOLIO"}),
        ("trading.risk_limits", {"portfolio_id": "BASE_PORTFOLIO"}))]
    return request


def test_guard_request_can_be_inspected_without_loading_or_executing_binary(adapter, guard_request):
    paths = adapter.validate_request(guard_request)
    assert paths["binary"].name == "live_portfolio"


@pytest.fixture
def deterministic_request(guard_request, tmp_path):
    request = deepcopy(guard_request)
    sources = tmp_path / 'source'; sources.mkdir()
    clock_relative = 'tests/integration/qt_model_capture_clock.cpp'
    sql_relative = 'tests/fixtures/qt_model_baseline/deterministic-controls.sql'
    originals = {name: ROOT / name for name in request['source_files']}
    originals.update({clock_relative: ROOT / clock_relative, sql_relative: ROOT / sql_relative})
    for relative, original in originals.items():
        target = sources / relative; target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(original.read_bytes())
    request['source_root'] = str(sources)
    request['source_files'] = {name: sha256((sources / name).read_bytes()).hexdigest() for name in originals}
    request['schema_files'] = [{
        'path': str(sources / Path(item['path']).relative_to(ROOT)), 'sha256': item['sha256']}
        for item in request['schema_files']]
    request['schema_files'].append({'path': str(sources / sql_relative), 'sha256': request['source_files'][sql_relative]})
    clock = tmp_path / 'libqt_model_capture_clock.so'; clock.write_bytes(b'\x7fELFclock-unit-only-never-executed')
    request['artifacts']['clock'] = {'path': str(clock), 'sha256': sha256(clock.read_bytes()).hexdigest()}
    request.update(schema_version='qt-model-baseline-capture/v2', determinism={
        'schema_version': 'qt-model-capture-controls/v1', 'epoch_unix_seconds': '1790424000',
        'epoch_utc': '2026-09-26T12:00:00Z', 'clock_source': clock_relative, 'sql_source': sql_relative})
    return request


def test_explicit_v2_controls_are_inspectable_without_executing_model(adapter, deterministic_request):
    paths = adapter.validate_request(deterministic_request)
    assert paths['clock'].name == 'libqt_model_capture_clock.so'


@pytest.mark.parametrize('damage', ['epoch_missing', 'epoch_mismatch', 'unbound_clock_source',
    'clock_hash', 'missing_sql', 'sql_order', 'extra_control'])
def test_v2_clock_and_fixture_controls_fail_closed_before_launch(adapter, deterministic_request, damage):
    request = deterministic_request
    if damage == 'epoch_missing': del request['determinism']['epoch_unix_seconds']
    elif damage == 'epoch_mismatch': request['determinism']['epoch_utc'] = '2026-09-27T12:00:00Z'
    elif damage == 'unbound_clock_source': del request['source_files'][request['determinism']['clock_source']]
    elif damage == 'clock_hash': request['artifacts']['clock']['sha256'] = '0' * 64
    elif damage == 'missing_sql': request['schema_files'].pop()
    elif damage == 'sql_order': request['schema_files'].reverse()
    elif damage == 'extra_control': request['determinism']['runtime_activation'] = True
    with pytest.raises(adapter.CaptureError): adapter.validate_request(request)


def test_clock_environment_is_opt_in_explicit_and_not_inherited(adapter, tmp_path, monkeypatch):
    monkeypatch.setenv('QT_MODEL_CAPTURE_EPOCH', 'untrusted-parent-value')
    plain = adapter.child_environment(tmp_path / 'guard', tmp_path, tmp_path)
    assert 'QT_MODEL_CAPTURE_EPOCH' not in plain
    controlled = adapter.child_environment(tmp_path / 'guard', tmp_path, tmp_path,
        clock=tmp_path / 'clock', epoch='1790424000')
    assert controlled['LD_PRELOAD'] == str(tmp_path / 'guard') + ' ' + str(tmp_path / 'clock')
    assert controlled['QT_MODEL_CAPTURE_EPOCH'] == '1790424000'
    with pytest.raises(adapter.CaptureError):
        adapter.child_environment(tmp_path / 'guard', tmp_path, tmp_path, clock=tmp_path / 'clock')


def test_delivery_guard_source_is_bound_before_any_launch(adapter, guard_request):
    del guard_request["source_files"]["tests/integration/qt_no_delivery_guard.cpp"]
    with pytest.raises(adapter.CaptureError, match="source_identity_required"):
        adapter.validate_request(guard_request)


@pytest.mark.parametrize("damage,code", [
    ("source", "artifact_hash_mismatch"), ("library", "artifact_hash_mismatch"),
    ("schema", "schema_source_identity_required"), ("database", "database_override_refused"),
    ("email", "inert_email_config_required"), ("table", "complete_table_inventory_required"),
    ("sql", "invalid_postcondition"), ("extra_config", "complete_public_config_required"),
    ("weakened_output", "complete_postconditions_required")])
def test_source_config_schema_and_sql_guard_refusal_before_launch(adapter, guard_request, damage, code):
    value = deepcopy(guard_request)
    if damage == "source": value["source_files"]["apps/strategies/live_portfolio_runner.cpp"] = "0" * 64
    if damage == "library": value["artifacts"]["library"]["sha256"] = "0" * 64
    if damage == "schema": value["schema_files"][0]["sha256"] = "0" * 64
    if damage == "database": value["config_files"]["portfolios/base/portfolio.json"]["database"] = {"host": "production"}
    if damage == "email": value["config_files"]["portfolios/base/email.json"]["to_emails"] = ["someone@example.com"]
    if damage == "table": value["table_schemas"].pop("trading.signals")
    if damage == "sql": value["required_postconditions"] = [{"sql": "SELECT dangerous_function()", "expected": []}]
    if damage == "extra_config": value["config_files"]["dotenv"] = {}
    if damage == "weakened_output": value["required_postconditions"] = [{"table": "trading.positions", "equals": {"portfolio_type": "system"}, "minimum": 1}]
    with pytest.raises(adapter.CaptureError, match=code):
        adapter.validate_request(value)


def test_owned_pg_actual_model_capture(adapter, tmp_path):
    """Only explicitly selected after root reviews request/guard/launch.

    The standard wrapper owns cleanup. No fake MODEL emitter substitutes for it.
    """
    dsn = os.environ.get("ALGOLENS_TEST_DB")
    if not dsn:
        pytest.skip("explicit owned-PG gate required; pure tests never invoke MODEL")
    gate = ROOT.parents[1] / "docs/repairs/2026-09-26-hemdutt-issue-completion/MODEL-CAPTURE-REQUEST-2.json"
    assert gate.is_file(), "root-reviewed explicit compiled request is required"
    value = json.loads(gate.read_text())
    output = gate.parent / ("model-baseline-capture-" + uuid.uuid4().hex)
    result = adapter.capture(value, dsn, output)
    assert result["actual_entry"] == "run_live_portfolio" and result["target"] == "live_portfolio"
    assert result["parity"] == "not_evaluated" and result["normalization"] == "none"
    assert result["authentic_runtime_certification"] == "unavailable"
    expected = {"table:" + table for table in value["table_schemas"]}
    assert expected <= set(result["outputs"])
    assert result["initial_outputs"].keys() == expected | {"log:sequence-state.json", "log:database-catalog.json"}
    assert {"log:stdout.raw", "log:stderr.raw", "csv:2026-09-25_positions.csv"} <= set(result["outputs"])
    for metadata in result["outputs"].values():
        raw = Path(metadata["path"]).read_bytes()
        assert len(raw) == metadata["bytes"] and sha256(raw).hexdigest() == metadata["sha256"]
    print("ACTUAL_MODEL_CAPTURE=" + json.dumps(result, sort_keys=True))


def _recreate_exact_owned_reference_database(adapter, dsn):
    """Test-only reset inside the wrapper's sole owned no-network PG container.

    Same database/socket identity keeps actual runner logs comparable. Refuse
    extra databases or concurrent sessions; never terminate other connections.
    The wrapper owns this entire server and removes it after the selected test.
    """
    import psycopg2
    from psycopg2 import sql
    adapter.isolation()
    values = adapter.owned_database(dsn)
    parameters = adapter.owned_connection_parameters(values)
    parameters['dbname'] = 'postgres'
    connection = psycopg2.connect(**parameters)
    try:
        connection.autocommit = True
        with connection.cursor() as cur:
            cur.execute('SELECT current_database(),inet_server_addr(),current_user')
            adapter.require(cur.fetchone() == ('postgres', None, 'postgres'), 'external_database_refused')
            cur.execute('SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname')
            adapter.require([row[0] for row in cur.fetchall()] == sorted(['postgres', values['dbname']]),
                'nonexclusive_reference_server_refused')
            cur.execute('SELECT count(*) FROM pg_stat_activity WHERE datname=%s', (values['dbname'],))
            adapter.require(cur.fetchone() == (0,), 'active_reference_database_refused')
            cur.execute(sql.SQL('DROP DATABASE {}').format(sql.Identifier(values['dbname'])))
            cur.execute(sql.SQL('CREATE DATABASE {} OWNER postgres TEMPLATE template1').format(sql.Identifier(values['dbname'])))
    finally:
        connection.close()


def _reference_raw(adapter, result):
    """Read every captured byte unchanged, with exact manifest integrity."""
    outputs = {}
    for phase, inventory in (('initial', result['initial_outputs']), ('final', result['outputs'])):
        for name, item in inventory.items():
            raw = adapter.bounded_read(item['path'])
            assert len(raw) == item['bytes'] and sha256(raw).hexdigest() == item['sha256']
            outputs[phase + ':' + name] = raw
    return outputs


@pytest.mark.parametrize('request_name', ['MODEL-REFERENCE-UNCONTROLLED-REQUEST-5.json',
    'MODEL-REFERENCE-CONTROLLED-REQUEST-5.json',
    'MODEL-REFERENCE-UNCONTROLLED-REQUEST-6.json',
    'MODEL-REFERENCE-CONTROLLED-REQUEST-6.json'])
def test_owned_pg_actual_reference_repeatability(adapter, request_name):
    """Selected explicitly only after root reviews each concrete request.

    The uncontrolled reference pair is expected to expose actual volatile raw
    outputs as a RED assertion, not a passing expected-failure substitute.
    Controlled captures must satisfy the exact same complete raw assertion.
    """
    dsn = os.environ.get('ALGOLENS_TEST_DB')
    if not dsn:
        pytest.skip('explicit independently reviewed owned-PG gate required')
    gate = ROOT.parents[1] / 'docs/repairs/2026-09-26-hemdutt-issue-completion' / request_name
    assert gate.is_file(), 'root-reviewed source/artifact/input request is required'
    request = json.loads(gate.read_text())
    adapter.validate_request(request)
    results = []
    for index in range(2):
        if index:
            _recreate_exact_owned_reference_database(adapter, dsn)
        directory = gate.parent / ('model-reference-' + uuid.uuid4().hex)
        result = adapter.capture(request, dsn, directory)
        assert result['exit_code'] == 0 and result['actual_entry'] == 'run_live_portfolio'
        assert result['normalization'] == 'none' and result['authentic_runtime_certification'] == 'unavailable'
        results.append(result)
    before, after = [_reference_raw(adapter, result) for result in results]
    if set(before) == set(after):
        characterized = adapter.characterize(before, after)
    else:
        # Retain the failed comparison before the strict assertion rejects it.
        # A changed logger filename is an actual inventory difference.
        characterized = {'byte_identical': False, 'error': 'capture_inventory_mismatch',
            'only_before': sorted(set(before) - set(after)), 'only_after': sorted(set(after) - set(before)),
            'different_outputs': sorted(name for name in before.keys() & after.keys() if before[name] != after[name]),
            'normalization': 'none', 'authentic_runtime_certification': 'unavailable'}
    pair = {'schema_version': 'qt-model-reference-pair/v1', 'request_file': str(gate),
        'request_file_sha256': sha256(gate.read_bytes()).hexdigest(), 'captures': results,
        'raw_comparison': characterized, 'fresh_database_instances': 2,
        'same_owned_database_and_socket_identity': True}
    pair_path = gate.parent / ('model-reference-pair-' + uuid.uuid4().hex + '.json')
    pair_path.write_text(json.dumps(pair, indent=2, sort_keys=True))
    print('ACTUAL_MODEL_REFERENCE_PAIR=' + str(pair_path))
    adapter.require_reference_repeatability(before, after)


@pytest.mark.parametrize('request_name', ['MODEL-REFERENCE-CONTROLLED-REQUEST-5.json',
    'MODEL-REFERENCE-CONTROLLED-REQUEST-6.json'])
def test_owned_pg_compiled_clock_boundary(adapter, tmp_path, request_name):
    """Inspect the actual pinned test-clock ELF before either MODEL pair."""
    import subprocess
    import sys
    if not os.environ.get('ALGOLENS_TEST_DB'):
        pytest.skip('explicit independently reviewed owned-PG gate required')
    gate = ROOT.parents[1] / 'docs/repairs/2026-09-26-hemdutt-issue-completion' / request_name
    assert gate.is_file(), 'concrete reviewed clock artifact request is required'
    request = json.loads(gate.read_text())
    paths = adapter.validate_request(request)
    environment = adapter.child_environment(paths['guard'], paths['library'].parent, tmp_path,
        clock=paths['clock'], epoch=adapter.CONTROL_EPOCH)
    program = '''import ctypes,json,time
class Timeval(ctypes.Structure):
    _fields_=[('tv_sec',ctypes.c_long),('tv_usec',ctypes.c_long)]
lib=ctypes.CDLL(None)
before=time.monotonic()
time.sleep(.02)
assert time.monotonic()>before
assert time.time()==1790424000
assert time.clock_gettime(time.CLOCK_REALTIME)==1790424000
value=Timeval()
assert lib.gettimeofday(ctypes.byref(value),None)==0
assert (value.tv_sec,value.tv_usec)==(1790424000,0)
assert lib.clearenv()==0
assert time.time()==1790424000
print(json.dumps({'realtime_frozen':True,'monotonic_advances':True,'cached_before_clearenv':True},sort_keys=True))
'''
    result = subprocess.run([sys.executable, '-c', program], env=environment,
        capture_output=True, timeout=10)
    assert result.returncode == 0 and not result.stderr
    assert json.loads(result.stdout) == {'realtime_frozen': True, 'monotonic_advances': True,
        'cached_before_clearenv': True}
    for invalid in (None, '', '1790424000junk', '0000000000'):
        altered = dict(environment)
        if invalid is None:
            altered.pop('QT_MODEL_CAPTURE_EPOCH')
        else:
            altered['QT_MODEL_CAPTURE_EPOCH'] = invalid
        refused = subprocess.run([sys.executable, '-c', 'raise AssertionError("clock control admitted")'],
            env=altered, capture_output=True, timeout=10)
        assert refused.returncode == 87


@pytest.mark.parametrize('request_name', ['MODEL-REFERENCE-CONTROLLED-REQUEST-5.json',
    'MODEL-REFERENCE-CONTROLLED-REQUEST-6.json'])
def test_owned_pg_deterministic_catalog_preflight(adapter, request_name):
    """Exercise PG16 catalog/control binding before any real MODEL invocation.

    Exact request selection is updated at the next coherent reviewed boundary.
    This gate installs only the public owned fixture and reads its catalog.
    """
    import psycopg2
    dsn = os.environ.get('ALGOLENS_TEST_DB')
    if not dsn:
        pytest.skip('explicit independently reviewed owned-PG gate required')
    gate = ROOT.parents[1] / 'docs/repairs/2026-09-26-hemdutt-issue-completion' / request_name
    assert gate.is_file(), 'fresh reviewed fixture/source/artifact request is required'
    request = json.loads(gate.read_text())
    adapter.isolation()
    values = adapter.owned_database(dsn)
    adapter.validate_request(request)
    connection = psycopg2.connect(**adapter.owned_connection_parameters(values))
    try:
        connection.autocommit = True
        with connection.cursor() as cur:
            cur.execute('SELECT current_database(),inet_server_addr()')
            assert cur.fetchone() == (values['dbname'], None)
            cur.execute("SELECT count(*) FROM pg_namespace WHERE nspname NOT IN ('public','pg_catalog','information_schema') AND nspname NOT LIKE 'pg_toast%%' AND nspname NOT LIKE 'pg_temp%%'")
            assert cur.fetchone() == (0,)
            cur.execute("SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='public' AND c.relkind IN ('r','p','S','v','m')")
            assert cur.fetchone() == (0,)
            for item in request['schema_files']:
                cur.execute(adapter.bounded_read(item['path']).decode('utf-8'))
        captured = adapter.database_snapshot(connection, request['table_schemas'],
            request['expected_sequences'], controlled=True)
        catalog = json.loads(captured['log:database-catalog.json'])
        assert catalog['session_settings'] == [['trading, pg_catalog']]
        assert [values['dbname'], '*', ['search_path=trading, pg_catalog']] in catalog['database_settings']
        defaults = {tuple(row[:3]): row[-1] for row in catalog['columns']}
        assert defaults['trading', 'qt_model_seed_publications', 'created_at'] == 'now()'
        assert defaults['trading', 'qt_storage_capabilities', 'installed_at'] == 'now()'
        with connection.cursor() as cur:
            cur.execute("SELECT now()::text,clock_timestamp()::text,gen_random_uuid()::text")
            assert cur.fetchone() == ('2026-09-26 12:00:00+00', '2026-09-26 12:00:00+00',
                'c0decafe-0000-4000-8000-000000000001')
    finally:
        connection.close()
