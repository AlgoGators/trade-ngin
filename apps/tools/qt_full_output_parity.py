"""Run explicit offline fixture adapters and compare every required output byte.

This harness never invokes a shell, live runner, database or delivery method.
Adapters are trusted local callables supplied by a reviewed test integration.
An adapter's claims about its source/build do not certify an authentic runtime.
The caller must fix the complete output inventory independently of either run.
"""
from hashlib import sha256
from decimal import Decimal
import json
import os
from pathlib import Path
import re
import stat

MAX_INPUT = 32 * 1024 * 1024
MAX_ARTIFACT = 16 * 1024 * 1024
MAX_TOTAL = 128 * 1024 * 1024
REQUIRED_TABLES = {"table:trading." + name for name in
                   ("positions", "executions", "live_results", "equity_curve", "live_run_metadata")}
CASE_KEYS = {"input_bytes", "input_sha256", "fixture_kind", "inventory", "table_schemas",
             "before_source_sha256", "after_source_sha256", "before_build_id", "after_build_id"}


class ParityError(ValueError):
    """Unavailable evidence; messages contain protocol codes, not file contents."""


def _require(condition, code):
    if not condition:
        raise ParityError(code)


def _digest(value):
    return type(value) is str and re.fullmatch(r"[a-f0-9]{64}", value) is not None


def _config(cases):
    _require(type(cases) is dict and set(cases) == {"futures", "equity"}, "both_asset_classes_required")
    validated = {}
    for asset, case in cases.items():
        _require(type(case) is dict and set(case) == CASE_KEYS, "invalid_case")
        _require(type(case["input_bytes"]) is bytes and 0 < len(case["input_bytes"]) <= MAX_INPUT,
                 "invalid_frozen_inputs")
        _require(_digest(case["input_sha256"]) and
                 sha256(case["input_bytes"]).hexdigest() == case["input_sha256"], "input_hash_mismatch")
        _require(case["fixture_kind"] in ("synthetic", "external"), "invalid_fixture_kind")
        for role in ("before", "after"):
            _require(_digest(case[role + "_source_sha256"]), "source_identity_required")
            identity = case[role + "_build_id"]
            _require(type(identity) is str and re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", identity),
                     "invalid_build_identity")
        _require(case["before_build_id"] != case["after_build_id"], "separate_builds_required")
        inventory = case["inventory"]
        _require(type(inventory) is list and 7 <= len(inventory) <= 256 and
                 all(type(name) is str and re.fullmatch(r"(?:table|log|csv):[A-Za-z][A-Za-z0-9_.-]{0,127}", name)
                     for name in inventory), "invalid_inventory")
        _require(len(set(inventory)) == len(inventory) and REQUIRED_TABLES <= set(inventory) and
                 any(name.startswith("log:") for name in inventory) and
                 any(name.startswith("csv:") for name in inventory), "incomplete_inventory")
        schemas = case["table_schemas"]
        tables = {name for name in inventory if name.startswith("table:")}
        _require(type(schemas) is dict and set(schemas) == tables, "complete_table_schemas_required")
        pinned = {}
        for table, schema in schemas.items():
            _require(type(schema) is list and 0 < len(schema) <= 512 and
                     all(type(column) is list and len(column) == 2 and
                         type(column[0]) is str and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,127}", column[0]) and
                         type(column[1]) is str and re.fullmatch(r"[a-z][a-z0-9_ (),.\[\]]{0,127}", column[1])
                         for column in schema), "invalid_table_schema_pin")
            _require(len({column[0] for column in schema}) == len(schema), "duplicate_schema_column")
            pinned[table] = tuple(tuple(column) for column in schema)
        validated[asset] = {**case, "inventory": tuple(inventory), "table_schemas": pinned}
    return validated


def _safe_path(path):
    path = Path(path)
    _require(".." not in path.parts, "unsafe_path")
    path = path.absolute()
    for ancestor in (path, *path.parents):
        try:
            info = ancestor.lstat()
        except FileNotFoundError:
            continue
        _require(not stat.S_ISLNK(info.st_mode) and
                 not (getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 1024)),
                 "unsafe_path")
    return path


def _read(path, limit=MAX_ARTIFACT):
    path = _safe_path(path)
    fd = None
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0) |
                     getattr(os, "O_BINARY", 0))
        info = os.fstat(fd)
        _require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and 0 <= info.st_size <= limit,
                 "invalid_artifact_file")
        with os.fdopen(fd, "rb") as stream:
            fd = None
            raw = stream.read(limit)
            # Detect growth without allocating an extra byte beyond the budget.
            _require(os.fstat(stream.fileno()).st_size == len(raw), "artifact_changed_during_read")
        return raw
    except OSError:
        raise ParityError("artifact_io_error") from None
    finally:
        if fd is not None:
            os.close(fd)


def _table(raw, expected_schema):
    def pairs(items):
        result = {}
        for key, item in items:
            _require(key not in result, "duplicate_table_key")
            result[key] = item
        return result
    def nonfinite(_value):
        raise ParityError("nonfinite_table_value")
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=pairs, parse_constant=nonfinite,
                           parse_float=Decimal)
    except (UnicodeError, ValueError, RecursionError):
        raise ParityError("invalid_table_json") from None
    _require(type(value) is dict and set(value) == {"columns", "types", "rows"}, "invalid_table_schema")
    columns, types, rows = value["columns"], value["types"], value["rows"]
    _require(columns == [column[0] for column in expected_schema] and
             types == [column[1] for column in expected_schema], "table_schema_mismatch")
    _require(type(rows) is list and len(rows) <= 100000 and
             all(type(row) is list and len(row) == len(columns) for row in rows), "incomplete_table_row")


def _preflight(paths, budget):
    total = 0
    for path in paths.values():
        try:
            info = _safe_path(path).stat()
        except OSError:
            raise ParityError("artifact_io_error") from None
        _require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and
                 0 <= info.st_size <= MAX_ARTIFACT, "invalid_artifact_file")
        total += info.st_size
    _require(total <= MAX_TOTAL, "capture_capacity_exceeded")
    _require(total <= budget, "matrix_capacity_exceeded")


def _capture(request, capture, remaining_budget):
    _require(type(capture) is dict and set(capture) ==
             {"source_sha256", "build_id", "input_sha256", "outputs"}, "invalid_capture")
    for key in ("source_sha256", "build_id", "input_sha256"):
        _require(capture[key] == request[key], "capture_identity_mismatch")
    outputs = capture["outputs"]
    _require(type(outputs) is dict and set(outputs) == set(request["inventory"]), "capture_inventory_mismatch")
    root = _safe_path(request["output_directory"])
    paths = {}
    for name, path in outputs.items():
        path = _safe_path(path)
        _require(path.parent == root and path not in paths.values(), "artifact_outside_capture_or_alias")
        paths[name] = path
    _require(set(root.iterdir()) == set(paths.values()), "unlisted_output")
    _preflight(paths, min(MAX_TOTAL, remaining_budget))
    captured = {}
    budget = min(MAX_TOTAL, remaining_budget)
    for name, path in paths.items():
        raw = _read(path, min(MAX_ARTIFACT, budget))
        budget -= len(raw)
        captured[name] = raw
    for name, raw in captured.items():
        if name.startswith("table:"):
            _table(raw, request["table_schemas"][name])
    return paths, captured


def _revalidate(request, paths, captured):
    # Re-read one artifact at a time; never allocate an entire second capture.
    budget = sum(map(len, captured.values()))
    root = _safe_path(request["output_directory"])
    _require(set(root.iterdir()) == set(paths.values()), "unlisted_output")
    _preflight(paths, budget)
    for name, path in paths.items():
        raw = _read(path, min(MAX_ARTIFACT, budget, len(captured[name])))
        budget -= len(raw)
        _require(raw == captured[name], "captured_outputs_mutated")


def run_matrix(directory, cases, before_adapter, after_adapter):
    """Execute both reviewed adapters for both assets in new isolated directories.

    Table artifacts use {columns: [...], types: [...], rows: [[every column's value], ...]}.
    Caller table_schemas must pin every table's exact ordered [name, type] pairs.
    Decimal values should retain their exact exported lexical spelling. No rows,
    columns, whitespace, timestamps, logs or CSV bytes are ignored/normalized.
    The external inventory must include every in-scope output, including empty
    table snapshots; the minimum five financial tables is only a lower bound.
    No existing directory is erased, reused or overwritten. Partial captures
    remain available for investigation after a refusal.
    """
    cases = _config(cases)
    _require(callable(before_adapter) and callable(after_adapter), "adapters_required")
    directory = _safe_path(directory)
    try:
        directory.mkdir()
    except OSError:
        raise ParityError("capture_directory_not_new") from None
    results, frozen = {}, []
    total_bytes = 0
    for asset_class, config in cases.items():
        case_directory = directory / asset_class
        case_directory.mkdir()
        input_path = case_directory / "input.bin"
        input_path.write_bytes(config["input_bytes"])
        captures, capture_paths, requests = {}, {}, {}
        for role, adapter in (("before", before_adapter), ("after", after_adapter)):
            side = case_directory / role
            side.mkdir()
            build, output = side / "build", side / "output"
            build.mkdir()
            output.mkdir()
            request = {"asset_class": asset_class, "role": role,
                       "input_path": input_path, "input_sha256": config["input_sha256"],
                       "source_sha256": config[role + "_source_sha256"],
                       "build_id": config[role + "_build_id"],
                       "build_directory": build, "output_directory": output,
                       "inventory": tuple(config["inventory"]),
                       "table_schemas": dict(config["table_schemas"])}
            # A separate dict is given to the adapter so it cannot rewrite the
            # expected identities/path inventory held by the harness.
            capture = adapter({**request, "table_schemas": dict(request["table_schemas"])})
            requests[role] = request
            capture_paths[role], captures[role] = _capture(request, capture, MAX_TOTAL - total_bytes)
            total_bytes += sum(map(len, captures[role].values()))
            _require(total_bytes <= MAX_TOTAL, "matrix_capacity_exceeded")
        frozen.append((input_path, config["input_bytes"], requests, capture_paths, captures))
        _require(_read(input_path, MAX_INPUT) == config["input_bytes"], "frozen_input_mutated")
        for role in ("before", "after"):
            _revalidate(requests[role], capture_paths[role], captures[role])
        artifacts = {}
        differences = []
        for identity in config["inventory"]:
            previous, current = captures["before"][identity], captures["after"][identity]
            artifacts[identity] = {role: {"sha256": sha256(captures[role][identity]).hexdigest(),
                                         "byte_count": len(captures[role][identity])}
                                   for role in ("before", "after")}
            if previous != current:
                differences.append({"artifact": identity, "kind": "bytes_differ"})
        results[asset_class] = {"comparison_status": "mismatch" if differences else "pass",
                               "differences": differences, "artifacts": artifacts,
                               "input_sha256": config["input_sha256"], "fixture_kind": config["fixture_kind"],
                               "inventory_sha256": sha256(json.dumps(config["inventory"], separators=(",", ":")).encode()).hexdigest(),
                               "table_schemas": config["table_schemas"],
                               "table_schemas_sha256": sha256(json.dumps(config["table_schemas"], sort_keys=True,
                                                           separators=(",", ":")).encode()).hexdigest(),
                               "sources": {role: config[role + "_source_sha256"] for role in ("before", "after")},
                               "builds": {role: config[role + "_build_id"] for role in ("before", "after")}}
    # Later asset adapters must not alter an earlier case after comparison.
    for input_path, input_bytes, requests, paths, captures in frozen:
        _require(_read(input_path, MAX_INPUT) == input_bytes, "frozen_input_mutated")
        for role in ("before", "after"):
            request = requests[role]
            _revalidate(request, paths[role], captures[role])
    return {"schema_version": "qt-full-output-parity/v1",
            "comparison_status": "pass" if all(c["comparison_status"] == "pass" for c in results.values()) else "mismatch",
            "cases": results,
            "authentic_runtime_certification": {"status": "unavailable",
                                                "reason": "independent_runtime_provenance_required"}}
