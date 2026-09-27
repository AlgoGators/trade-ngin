"""Capture the actual compiled MODEL runner in a reviewed disposable fixture.

No authentic parity assertion, output normalization, shell, delivery or live
activation exists here. Requests explicitly bind every input/source/artifact.
The launcher must be independently reviewed before the first real invocation.
"""
from datetime import date
from hashlib import sha256
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess
import time
from urllib.parse import quote

MAX_FILE = 16 * 1024 * 1024
MAX_TOTAL = 128 * 1024 * 1024
MAX_ROWS = 100000
TARGETS = {"live_portfolio": ("base", "live_trend", "BASE_PORTFOLIO"),
           "live_portfolio_conservative": ("conservative", "live_trend_conservative", "CONSERVATIVE_PORTFOLIO")}
REQUIRED_TABLES = {"trading." + name for name in (
    "positions", "executions", "signals", "live_results", "equity_curve",
    "live_run_metadata", "risk_limits", "run_inputs", "qt_model_seed_publications",
    "strategy_registry", "strategy_book_memberships", "runtime_intents", "runtime_attempts",
    "qt_decisions", "qt_workflow_capabilities")}
SOURCE_ENTRY = "apps/strategies/live_portfolio_runner.cpp"
REQUEST_KEYS = {"schema_version", "fixture_kind", "target", "day", "source_root",
    "source_files", "artifacts", "build_id", "config_files", "schema_files",
    "table_schemas", "expected_csv", "expected_sequences", "required_postconditions"}
CONTROL_CLOCK = "tests/integration/qt_model_capture_clock.cpp"
CONTROL_SQL = "tests/fixtures/qt_model_baseline/deterministic-controls.sql"
CONTROL_EPOCH = "1790424000"
CONTROL_UTC = "2026-09-26T12:00:00Z"


class CaptureError(ValueError):
    """Protocol codes only, never credentials or fixture contents."""


def require(condition, code):
    if not condition:
        raise CaptureError(code)


def validate_day(value):
    require(type(value) is str and re.fullmatch(r"\d{4}-\d{2}-\d{2}", value), "invalid_day")
    try:
        require(date.fromisoformat(value).isoformat() == value, "invalid_day")
    except ValueError:
        raise CaptureError("invalid_day") from None
    return value


def owned_database(dsn):
    require(type(dsn) is str and len(dsn) <= 4096, "invalid_owned_dsn")
    values = {}
    for word in dsn.split():
        require(word.count("=") == 1, "invalid_owned_dsn")
        key, value = word.split("=", 1)
        require(key not in values and key in {"host", "port", "dbname", "user", "password", "connect_timeout"}, "invalid_owned_dsn")
        values[key] = value
    require(re.fullmatch(r"/tmp/algolens-repair-pg-[a-zA-Z0-9_-]+/socket", values.get("host", ""))
        and re.fullmatch(r"algolens_test_[a-zA-Z0-9_]+", values.get("dbname", ""))
        and values.get("user") == "postgres" and values.get("port", "5432") == "5432"
        and values.get("password", "synthetic-test-only") == "synthetic-test-only"
        and values.get("connect_timeout", "2") == "2", "invalid_owned_dsn")
    return values


def child_environment(guard, library_directory, temporary, *, clock=None, epoch=None):
    require((clock is None) == (epoch is None), "incomplete_clock_control")
    require(epoch is None or epoch == CONTROL_EPOCH, "invalid_clock_epoch")
    result = {"PATH": "/usr/local/bin:/usr/bin:/bin", "TZ": "UTC", "LC_ALL": "C",
        "TMPDIR": str(temporary), "LD_PRELOAD": str(guard), "LD_LIBRARY_PATH": str(library_directory),
        "QT_EMAIL_DELIVERY_ENABLED": "false"}
    if clock is not None:
        result["LD_PRELOAD"] += " " + str(clock)
        result["QT_MODEL_CAPTURE_EPOCH"] = epoch
    return result


def runner_database_config(values):
    # Actual DatabaseConfig concatenates a URI authority rather than conninfo.
    # Encode the explicit owned socket host so libpq retains Unix-socket routing.
    return {"host": quote(values["host"], safe=""), "port": "5432", "name": values["dbname"],
            "username": "postgres", "password": "synthetic-test-only", "num_connections": 2}


def owned_connection_parameters(values):
    return {"host": values["host"], "port": 5432, "dbname": values["dbname"], "user": "postgres",
            "password": "synthetic-test-only", "connect_timeout": 2}


def safe_path(value):
    path = Path(value)
    require(path.is_absolute() and ".." not in path.parts, "unsafe_path")
    for ancestor in (path, *path.parents):
        try:
            info = ancestor.lstat()
        except FileNotFoundError:
            continue
        require(not stat.S_ISLNK(info.st_mode) and not (getattr(info, "st_file_attributes", 0) & 1024), "unsafe_path")
    return path


def bounded_read(path, limit=MAX_FILE):
    path = safe_path(path)
    fd = None
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0))
        info = os.fstat(fd)
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_size <= limit, "invalid_artifact")
        with os.fdopen(fd, "rb") as stream:
            fd = None
            raw = stream.read(limit)
            require(os.fstat(stream.fileno()).st_size == len(raw), "artifact_changed")
        return raw
    except OSError:
        raise CaptureError("artifact_unavailable") from None
    finally:
        if fd is not None:
            os.close(fd)


def pinned_file(path, expected, elf=False):
    require(type(expected) is str and re.fullmatch(r"[a-f0-9]{64}", expected), "invalid_hash")
    # ELF libraries can exceed the per-output limit; hash them in bounded chunks.
    path = safe_path(path)
    require(path.is_file() and path.stat().st_nlink == 1, "invalid_artifact")
    h = sha256()
    with path.open("rb") as stream:
        head = stream.read(4)
        if elf:
            require(head == b"\x7fELF", "native_elf_required")
        h.update(head)
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    require(h.hexdigest() == expected, "artifact_hash_mismatch")
    return path


def isolation():
    require(os.name == "posix" and sorted(name for _, name in socket.if_nameindex()) == ["lo"], "isolated_network_required")
    require(not any(line.strip() for line in Path("/proc/net/route").read_text().splitlines()[1:]), "network_routes_refused")
    ipv6 = Path("/proc/net/ipv6_route")
    if ipv6.exists():
        require(all(line.split()[-1] == "lo" for line in ipv6.read_text().splitlines() if line.strip()), "network_routes_refused")
    require(not any(name.startswith("PG") for name in os.environ), "postgres_environment_refused")


def table_bytes(schema, rows):
    require(type(schema) is list and schema and all(type(c) is list and len(c) == 2 for c in schema), "invalid_table_schema")
    require(len(rows) <= MAX_ROWS and all(len(row) == len(schema) and
        all(value is None or type(value) is str for value in row) for row in rows), "incomplete_table_row")
    raw = json.dumps({"columns": [c[0] for c in schema], "types": [c[1] for c in schema],
        "rows": rows}, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    require(len(raw) <= MAX_FILE, "capture_capacity_exceeded")
    return raw


def collect_runner_files(directory, prefix, portfolio, expected_csv):
    directory = safe_path(directory)
    files = {}
    total = 0
    log_pattern = re.compile(r"logs/" + re.escape(prefix) + r"_\d{8}_\d{6}_part\d+\.log")
    csv_prefix = "apps/strategies/results/" + portfolio + "/"
    for path in directory.rglob("*"):
        safe_path(path)
        if path.is_dir():
            continue
        relative = path.relative_to(directory).as_posix()
        if log_pattern.fullmatch(relative):
            key = "log:" + path.name
        elif relative.startswith(csv_prefix) and relative[len(csv_prefix):] in expected_csv:
            key = "csv:" + relative[len(csv_prefix):]
        else:
            raise CaptureError("unlisted_runner_output")
        info = path.stat()
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_size <= MAX_FILE, "invalid_artifact")
        require(key not in files, "artifact_alias")
        files[key] = path
        total += info.st_size
    require(total <= MAX_TOTAL, "capture_capacity_exceeded")
    require({"csv:" + name for name in expected_csv} <= set(files), "missing_expected_csv")
    require(any(name.startswith("log:") for name in files), "missing_runner_log")
    result = {}
    remaining = MAX_TOTAL
    for name, path in sorted(files.items()):
        raw = bounded_read(path, min(MAX_FILE, remaining))
        result[name] = raw
        remaining -= len(raw)
    return result


def characterize(before, after):
    require(set(before) == set(after), "capture_inventory_mismatch")
    different = sorted(name for name in before if before[name] != after[name])
    return {"byte_identical": not different, "different_outputs": different,
            "authentic_runtime_certification": "unavailable", "normalization": "none"}


def require_reference_repeatability(before, after):
    """A failed raw reference pair cannot authorize runner extraction parity."""
    result = characterize(before, after)
    require(result['byte_identical'], 'reference_outputs_differ')
    return result


def record_native_exit(directory, code):
    require(type(code) is int, "invalid_native_exit")
    with (directory / "native-exit.json").open("x", encoding="utf-8") as out:
        json.dump({"native_exit": code, "delivery_guard_exit": code == 86}, out, sort_keys=True)


def validate_request(request):
    require(type(request) is dict, "invalid_request")
    controlled = request.get("schema_version") == "qt-model-baseline-capture/v2"
    require(set(request) == REQUEST_KEYS | ({"determinism"} if controlled else set()), "invalid_request")
    require(request["schema_version"] in {"qt-model-baseline-capture/v1", "qt-model-baseline-capture/v2"}
        and request["fixture_kind"] == "synthetic", "synthetic_fixture_required")
    if controlled:
        require(request["determinism"] == {"schema_version": "qt-model-capture-controls/v1",
            "epoch_unix_seconds": CONTROL_EPOCH, "epoch_utc": CONTROL_UTC,
            "clock_source": CONTROL_CLOCK, "sql_source": CONTROL_SQL}, "invalid_deterministic_controls")
    require(request["target"] in TARGETS, "actual_model_target_required")
    validate_day(request["day"])
    require(re.fullmatch(r"[a-zA-Z0-9_.-]{1,128}", request["build_id"]), "invalid_build_identity")
    root = safe_path(request["source_root"])
    sources = request["source_files"]
    require(type(sources) is dict and 4 <= len(sources) <= 5000 and SOURCE_ENTRY in sources
        and "apps/strategies/live_portfolio_runner.hpp" in sources and "apps/strategies/CMakeLists.txt" in sources
        and "tests/integration/qt_no_delivery_guard.cpp" in sources
        and "include/trade_ngin/core/holidays.json" in sources, "source_identity_required")
    if controlled:
        require(CONTROL_CLOCK in sources and CONTROL_SQL in sources, "control_source_identity_required")
    for relative, digest in sources.items():
        require(type(relative) is str and not Path(relative).is_absolute() and ".." not in Path(relative).parts, "unsafe_source_path")
        pinned_file(root / relative, digest)
    artifacts = request["artifacts"]
    require(type(artifacts) is dict and set(artifacts) == {"binary", "library", "guard"} | ({"clock"} if controlled else set()), "build_artifacts_required")
    paths = {}
    for kind, item in artifacts.items():
        require(type(item) is dict and set(item) == {"path", "sha256"}, "invalid_artifact_identity")
        paths[kind] = pinned_file(item["path"], item["sha256"], elf=True)
    require(paths["binary"].name == request["target"] and paths["library"].name == "libtrade_ngin.so"
        and paths["guard"].name == "libqt_no_delivery_guard.so"
        and len(set(paths.values())) == len(paths) and paths["binary"].parent == paths["library"].parent, "actual_model_artifacts_required")
    if controlled:
        require(paths["clock"].name == "libqt_model_capture_clock.so", "actual_control_artifact_required")
    variant, _, portfolio = TARGETS[request["target"]]
    config = request["config_files"]
    expected_config = {"defaults.json"} | {"portfolios/" + variant + "/" + name
        for name in ("portfolio.json", "risk.json", "email.json")}
    require(type(config) is dict and set(config) == expected_config, "complete_public_config_required")
    for name, value in config.items():
        require(type(value) is dict and "database" not in value, "database_override_refused")
    require(config["portfolios/" + variant + "/portfolio.json"].get("portfolio_id") == portfolio, "portfolio_identity_mismatch")
    require(config["portfolios/" + variant + "/email.json"] == {"smtp_host": "invalid.offline", "smtp_port": 1,
        "username": "synthetic", "password": "synthetic", "from_email": "synthetic@invalid.offline",
        "to_emails": [], "use_tls": False}, "inert_email_config_required")
    require(type(request["schema_files"]) is list and request["schema_files"], "schema_inputs_required")
    for item in request["schema_files"]:
        require(type(item) is dict and set(item) == {"path", "sha256"}, "invalid_schema_input")
        path = safe_path(item["path"])
        require(path.suffix == ".sql" and path.is_relative_to(root) and
            (path.is_relative_to(root / "tests/fixtures/qt_model_baseline") or path.parent == root / "migrations"), "unapproved_schema_input")
        require(sources.get(path.relative_to(root).as_posix()) == item["sha256"], "schema_source_identity_required")
        pinned_file(path, item["sha256"])
    if controlled:
        schema_names = [Path(item["path"]).relative_to(root).as_posix() for item in request["schema_files"]]
        require(schema_names.count(CONTROL_SQL) == 1 and len(schema_names) > 1
            and schema_names[0] == "tests/fixtures/qt_model_baseline/baseline-v2.sql"
            and schema_names[1] == CONTROL_SQL, "control_schema_order_required")
    schemas = request["table_schemas"]
    require(type(schemas) is dict and REQUIRED_TABLES <= set(schemas) and len(schemas) <= 256, "complete_table_inventory_required")
    for table, schema in schemas.items():
        require(re.fullmatch(r"[a-z][a-z0-9_]*\.[a-z][a-z0-9_]*", table) and type(schema) is list and 0 < len(schema) <= 512,
            "invalid_table_schema")
        require(all(type(c) is list and len(c) == 2 and type(c[0]) is str and type(c[1]) is str for c in schema)
            and len({c[0] for c in schema}) == len(schema), "invalid_table_schema")
    require(type(request["expected_csv"]) is list and request["expected_csv"] == [request["day"] + "_positions.csv"], "invalid_csv_inventory")
    require(type(request["expected_sequences"]) is list and len(set(request["expected_sequences"])) == len(request["expected_sequences"]), "invalid_sequence_inventory")
    conditions = request["required_postconditions"]
    require(type(conditions) is list and conditions, "postconditions_required")
    for condition in conditions:
        require(type(condition) is dict and set(condition) == {"table", "equals", "minimum"}
            and condition["table"] in schemas and type(condition["equals"]) is dict
            and type(condition["minimum"]) is int and 1 <= condition["minimum"] <= MAX_ROWS, "invalid_postcondition")
        require(set(condition["equals"]) <= {c[0] for c in schemas[condition["table"]]} and
            all(type(v) is str and len(v) <= 128 for v in condition["equals"].values()), "invalid_postcondition")
    mandatory = [("trading.positions", {"portfolio_id": portfolio, "date": request["day"], "portfolio_type": stream})
                 for stream in ("system", "qt_proposal")]
    mandatory += [("trading." + name, {"portfolio_id": portfolio, "portfolio_type": "system"})
                  for name in ("executions", "live_results", "equity_curve")]
    mandatory += [("trading." + name, {"portfolio_id": portfolio})
                  for name in ("signals", "live_run_metadata", "run_inputs", "risk_limits")]
    mandatory += [("trading.qt_model_seed_publications", {"portfolio_id": portfolio, "source_day": request["day"]})]
    require(all(any(c["table"] == table and all(c["equals"].get(k) == v for k, v in equals.items())
        for c in conditions) for table, equals in mandatory), "complete_postconditions_required")
    return paths


def database_snapshot(connection, pinned_schemas, expected_sequences, budget=MAX_TOTAL, *, controlled=False):
    """Full lexical snapshots with independently supplied exact schema pins."""
    from psycopg2 import sql
    with connection.cursor() as cur:
        cur.execute("SELECT n.nspname,c.relname FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
            "WHERE c.relkind IN ('r','p') AND n.nspname NOT IN ('pg_catalog','information_schema') "
            "AND n.nspname NOT LIKE 'pg_toast%%' ORDER BY 1,2")
        names = [schema + "." + table for schema, table in cur.fetchall()]
        require(set(names) == set(pinned_schemas), "database_inventory_mismatch")
        outputs = {}
        estimated_total = 0
        for name in names:
            schema, table = name.split(".")
            cur.execute("SELECT a.attname,format_type(a.atttypid,a.atttypmod) FROM pg_attribute a "
                "JOIN pg_class c ON c.oid=a.attrelid JOIN pg_namespace n ON n.oid=c.relnamespace "
                "WHERE n.nspname=%s AND c.relname=%s AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum", (schema, table))
            observed = [list(row) for row in cur.fetchall()]
            require(observed == pinned_schemas[name], "database_schema_mismatch")
            columns = sql.SQL(",").join(sql.SQL("{}::text").format(sql.Identifier(c[0])) for c in observed)
            lengths = sql.SQL("+").join(sql.SQL("COALESCE(octet_length({}::text),4)").format(sql.Identifier(c[0])) for c in observed)
            cur.execute(sql.SQL("SELECT count(*),COALESCE(sum({}),0) FROM {}.{}").format(lengths, sql.Identifier(schema), sql.Identifier(table)))
            count, content_size = cur.fetchone()
            # JSON escaping can multiply UTF-8 bytes by six. Bound before fetch.
            estimate = 6 * int(content_size) + count * (len(observed) * 4 + 4) + 65536
            estimated_total += estimate
            require(count <= MAX_ROWS and estimate <= MAX_FILE and estimated_total <= min(MAX_TOTAL, budget), "capture_capacity_exceeded")
            # C collation makes export order independent of fixture DB locale.
            order = sql.SQL(",").join(sql.SQL("{}::text COLLATE \"C\" NULLS FIRST").format(sql.Identifier(c[0])) for c in observed)
            cur.execute(sql.SQL("SELECT {} FROM {}.{} ORDER BY {} LIMIT %s").format(columns,
                sql.Identifier(schema), sql.Identifier(table), order), (MAX_ROWS + 1,))
            rows = [list(row) for row in cur.fetchall()]
            outputs["table:" + name] = table_bytes(observed, rows)
        cur.execute("SELECT schemaname,sequencename FROM pg_sequences WHERE schemaname NOT LIKE 'pg_%%' ORDER BY 1,2")
        sequences = [a + "." + b for a, b in cur.fetchall()]
        require(sequences == sorted(expected_sequences), "sequence_inventory_mismatch")
        sequence_rows = []
        for name in sequences:
            a, b = name.split(".")
            cur.execute(sql.SQL("SELECT last_value::text,is_called::text FROM {}.{}").format(sql.Identifier(a), sql.Identifier(b)))
            sequence_rows.append([name, *cur.fetchone()])
        outputs["log:sequence-state.json"] = json.dumps(sequence_rows, separators=(",", ":")).encode()
        # Retain definitions as evidence; fixture migrations are independently
        # hashed inputs, and no writer is permitted to change their catalog.
        catalog = {}
        queries = {
            "columns": "SELECT n.nspname,c.relname,a.attname,a.attnum,format_type(a.atttypid,a.atttypmod),a.attnotnull,pg_get_expr(d.adbin,d.adrelid) "
                "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace JOIN pg_attribute a ON a.attrelid=c.oid "
                "LEFT JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum WHERE n.nspname IN ('trading','metadata','futures_data','auth') AND a.attnum>0 AND NOT a.attisdropped ORDER BY 1,2,4",
            "constraints": "SELECT n.nspname,c.relname,k.conname,pg_get_constraintdef(k.oid) FROM pg_constraint k JOIN pg_class c ON c.oid=k.conrelid JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname IN ('trading','metadata','futures_data','auth') ORDER BY 1,2,3",
            "functions": "SELECT n.nspname,p.proname,pg_get_function_identity_arguments(p.oid),pg_get_functiondef(p.oid) FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace WHERE n.nspname IN ('trading','metadata','futures_data','auth') ORDER BY 1,2,3",
            "triggers": "SELECT n.nspname,c.relname,t.tgname,t.tgenabled,pg_get_triggerdef(t.oid) FROM pg_trigger t JOIN pg_class c ON c.oid=t.tgrelid JOIN pg_namespace n ON n.oid=c.relnamespace WHERE NOT t.tgisinternal AND n.nspname IN ('trading','metadata','futures_data','auth') ORDER BY 1,2,3"}
        for name, query in queries.items():
            cur.execute(query)
            catalog[name] = [list(row) for row in cur.fetchall()]
        if controlled:
            cur.execute("SELECT current_setting('search_path')")
            catalog["session_settings"] = [list(row) for row in cur.fetchall()]
            # Modern PostgreSQL stores database/role GUCs in pg_db_role_setting.
            # Include database-wide and role-specific settings applying to this
            # connection, plus the session's actual effective search path.
            cur.execute("SELECT COALESCE(d.datname,'*'),COALESCE(r.rolname,'*'),s.setconfig "
                "FROM pg_db_role_setting s LEFT JOIN pg_database d ON d.oid=s.setdatabase "
                "LEFT JOIN pg_roles r ON r.oid=s.setrole "
                "WHERE s.setdatabase IN (0,(SELECT oid FROM pg_database WHERE datname=current_database())) "
                "AND s.setrole IN (0,(SELECT oid FROM pg_roles WHERE rolname=current_user)) ORDER BY 1,2")
            catalog["database_settings"] = [list(row) for row in cur.fetchall()]
        outputs["log:database-catalog.json"] = json.dumps(catalog, separators=(",", ":")).encode()
    require(sum(map(len, outputs.values())) <= min(MAX_TOTAL, budget), "capture_capacity_exceeded")
    return outputs


def capture(request, dsn, directory):
    """Invoke the real binary only after every isolation/input guard passes.

    Complete request/launch source must be reviewed before first integration.
    Partial raw evidence survives failure; no existing directory is removed.
    """
    isolation()
    db_values = owned_database(dsn)
    safe_path(db_values["host"])
    paths = validate_request(request)
    directory = safe_path(directory)
    require(not directory.exists(), "capture_directory_not_new")
    directory.mkdir(mode=0o700)
    working = directory / "runner"; working.mkdir()
    temporary = directory / "temp"; temporary.mkdir()
    evidence = directory / "outputs"; evidence.mkdir()
    variant, prefix, portfolio = TARGETS[request["target"]]
    for relative, original in request["config_files"].items():
        value = json.loads(json.dumps(original))
        if relative == "defaults.json":
            value["database"] = runner_database_config(db_values)
        path = working / "config" / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")
    holiday_relative = "include/trade_ngin/core/holidays.json"
    holiday_raw = bounded_read(Path(request["source_root"]) / holiday_relative)
    holiday_path = working / holiday_relative
    holiday_path.parent.mkdir(parents=True)
    holiday_path.write_bytes(holiday_raw)
    controlled = "clock" in paths
    environment = child_environment(paths["guard"], paths["library"].parent, temporary,
        clock=paths.get("clock"), epoch=CONTROL_EPOCH if controlled else None)
    trace = subprocess.run([str(paths["binary"])], cwd=working,
        env={**environment, "LD_TRACE_LOADED_OBJECTS": "1"}, capture_output=True, timeout=15)
    require(trace.returncode == 0 and not trace.stderr and str(paths["guard"]).encode() in trace.stdout
        and str(paths["library"]).encode() in trace.stdout
        and (not controlled or str(paths["clock"]).encode() in trace.stdout), "delivery_guard_or_library_not_loaded")
    (directory / "loader.raw").write_bytes(trace.stdout)
    (directory / "request.json").write_text(json.dumps(request, indent=2, sort_keys=True), encoding="utf-8")
    import psycopg2
    connection = psycopg2.connect(**owned_connection_parameters(db_values))
    try:
        connection.autocommit = True
        with connection.cursor() as cur:
            cur.execute("SELECT current_database(),inet_server_addr()")
            require(cur.fetchone() == (db_values["dbname"], None), "external_database_refused")
            cur.execute("SELECT count(*) FROM pg_namespace WHERE nspname NOT IN ('public','pg_catalog','information_schema') AND nspname NOT LIKE 'pg_toast%%' AND nspname NOT LIKE 'pg_temp%%'")
            require(cur.fetchone() == (0,), "nonempty_owned_database_refused")
            cur.execute("SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='public' AND c.relkind IN ('r','p','S','v','m')")
            require(cur.fetchone() == (0,), "nonempty_owned_database_refused")
            for item in request["schema_files"]:
                cur.execute(bounded_read(item["path"]).decode("utf-8"))
        initial = database_snapshot(connection, request["table_schemas"], request["expected_sequences"], controlled=controlled)
        (directory / "initial").mkdir()
        initial_manifest = {}
        for index, (name, raw) in enumerate(sorted(initial.items())):
            path = directory / "initial" / (str(index).zfill(3) + ".raw")
            path.write_bytes(raw)
            initial_manifest[name] = {"path": str(path), "bytes": len(raw), "sha256": sha256(raw).hexdigest()}
        (directory / "initial-manifest.json").write_text(json.dumps(initial_manifest, indent=2, sort_keys=True), encoding="utf-8")
        # Only generated public config is permitted beside runner outputs.
        config_raw = {p.relative_to(working).as_posix(): bounded_read(p) for p in (working / "config").rglob("*.json")}
        config_raw[holiday_relative] = holiday_raw
        stdout = directory / "stdout.raw"; stderr = directory / "stderr.raw"
        command = [str(paths["binary"]), request["day"]]
        with stdout.open("xb") as out, stderr.open("xb") as err:
            process = subprocess.Popen(command, cwd=working, env=environment, stdout=out, stderr=err)
            try:
                deadline = time.monotonic() + 180
                while process.poll() is None:
                    generated = [p for p in working.rglob("*") if p.is_file()] + [stdout, stderr]
                    require(all(not p.is_symlink() and p.stat().st_nlink == 1 and p.stat().st_size <= MAX_FILE for p in generated)
                        and sum(p.stat().st_size for p in generated) <= MAX_TOTAL, "capture_capacity_exceeded")
                    require(time.monotonic() < deadline, "actual_model_timeout")
                    time.sleep(.05)
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait(timeout=5)
                record_native_exit(directory, process.returncode)
        validate_request(request)
        (directory / "native-source-boundary.json").write_text(json.dumps({
            "source_and_artifacts_verified_after_native": True,
            "source_files": request["source_files"], "artifacts": request["artifacts"]}, indent=2, sort_keys=True), encoding="utf-8")
        require(process.returncode != 86, "delivery_guard_reached")
        require(process.returncode == 0, "actual_model_run_failed")
        remaining = MAX_TOTAL - sum(map(len, initial.values()))
        final = database_snapshot(connection, request["table_schemas"], request["expected_sequences"], remaining, controlled=controlled)
        require(final["log:database-catalog.json"] == initial["log:database-catalog.json"], "database_catalog_changed")
        with connection.cursor() as cur:
            from psycopg2 import sql
            for condition in request["required_postconditions"]:
                schema, table = condition["table"].split(".")
                clauses = sql.SQL(" AND ").join(sql.SQL("{}::text=%s").format(sql.Identifier(k)) for k in condition["equals"])
                query = sql.SQL("SELECT count(*) FROM {}.{}").format(sql.Identifier(schema), sql.Identifier(table))
                if condition["equals"]:
                    query += sql.SQL(" WHERE ") + clauses
                cur.execute(query, list(condition["equals"].values()))
                require(cur.fetchone()[0] >= condition["minimum"], "actual_model_output_incomplete")
        # Config lives outside file-inventory traversal; verify unchanged first.
        observed_config = {p.relative_to(working).as_posix(): bounded_read(p) for p in (working / "config").rglob("*.json")}
        observed_config[holiday_relative] = bounded_read(holiday_path)
        require(observed_config == config_raw, "config_changed")
        # Traverse all files and validate config separately without deleting it.
        runner_outputs = working / "apps"
        logs = working / "logs"
        require(runner_outputs.is_dir() and logs.is_dir(), "missing_runner_output")
        require(not any(temporary.rglob("*")), "unlisted_runner_temp_output")
        remaining -= sum(map(len, final.values()))
        raw_sizes = stdout.stat().st_size + stderr.stat().st_size
        require(raw_sizes <= remaining, "capture_capacity_exceeded")
        files = _collect_working(working, config_raw, prefix, portfolio, request["expected_csv"], remaining - raw_sizes)
        final.update(files)
        final["log:stdout.raw"] = bounded_read(stdout)
        final["log:stderr.raw"] = bounded_read(stderr)
        require(sum(map(len, initial.values())) + sum(map(len, final.values())) <= MAX_TOTAL, "capture_capacity_exceeded")
        artifacts = {}
        for index, (name, raw) in enumerate(sorted(final.items())):
            path = evidence / (str(index).zfill(3) + ".raw")
            path.write_bytes(raw)
            artifacts[name] = {"path": str(path), "sha256": sha256(raw).hexdigest(), "bytes": len(raw)}
        # Recheck source and binary identities after actual execution.
        validate_request(request)
        result = {"schema_version": "qt-model-baseline-result/v1", "fixture_kind": "synthetic",
            "authentic_runtime_certification": "unavailable", "parity": "not_evaluated", "normalization": "none",
            "actual_entry": "run_live_portfolio", "target": request["target"], "day": request["day"],
            "build_id": request["build_id"], "source_manifest_sha256": sha256(json.dumps(request["source_files"], sort_keys=True).encode()).hexdigest(),
            "request_sha256": sha256(json.dumps(request, sort_keys=True).encode()).hexdigest(),
            "artifacts": request["artifacts"], "loader_trace_sha256": sha256(trace.stdout).hexdigest(),
            "initial_outputs": initial_manifest,
            "config_hashes": {k: sha256(v).hexdigest() for k, v in sorted(config_raw.items())},
            "command": command, "exit_code": process.returncode, "outputs": artifacts}
        if controlled:
            result["schema_version"] = "qt-model-baseline-result/v2"
            result["determinism"] = request["determinism"]
        (directory / "capture.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        return result
    finally:
        connection.close()


def _collect_working(working, config_files, prefix, portfolio, expected_csv, budget=MAX_TOTAL):
    # Same strict traversal as collect_runner_files, with exactly pinned config
    # permitted as input. Never move/delete files just to make an inventory pass.
    files = {}
    total = 0
    pattern = re.compile(r"logs/" + re.escape(prefix) + r"_\d{8}_\d{6}_part\d+\.log")
    for path in working.rglob("*"):
        safe_path(path)
        if path.is_dir():
            continue
        relative = path.relative_to(working).as_posix()
        if relative in config_files:
            require(bounded_read(path) == config_files[relative], "config_changed")
            continue
        if pattern.fullmatch(relative):
            key = "log:" + path.name
        elif relative.startswith("apps/strategies/results/" + portfolio + "/") and path.name in expected_csv:
            require(relative == "apps/strategies/results/" + portfolio + "/" + path.name, "unlisted_runner_output")
            key = "csv:" + path.name
        else:
            raise CaptureError("unlisted_runner_output")
        info = path.stat()
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_size <= MAX_FILE, "invalid_artifact")
        require(key not in files, "artifact_alias")
        files[key] = path; total += info.st_size
    require(total <= min(MAX_TOTAL, budget), "capture_capacity_exceeded")
    require({"csv:" + name for name in expected_csv} <= set(files), "missing_expected_csv")
    require(any(name.startswith("log:") for name in files), "missing_runner_log")
    return {name: bounded_read(path) for name, path in sorted(files.items())}
