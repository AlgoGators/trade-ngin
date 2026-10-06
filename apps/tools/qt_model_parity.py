"""Offline MODEL evidence capture/comparison; never execute a producer.

The caller supplies one opaque bars-and-run-settings artifact and two explicit
MODEL output files. Reviewed external provenance is a separate caller-pinned
input; synthetic equality or a file's own authenticity claim cannot certify
real data. Safe capture uses Linux descriptor-relative no-follow operations.
"""
import argparse
from datetime import date
from hashlib import sha256
import json
import os
from pathlib import Path
import re
import stat

MAX_INPUT = 32 * 1024 * 1024
MAX_OUTPUT = 8 * 1024 * 1024
MAX_MANIFEST = 1024 * 1024
MAX_ROWS = 10000
KEY_FIELDS = ("portfolio_id", "strategy_id", "strategy_name", "date", "symbol", "portfolio_type")
META_FIELDS = {"fixture_kind", "input_scope", "source_id", "source_version", "source_day",
               "producer_id", "before_build_id", "after_build_id"}
OUTPUT_FIELDS = {"schema_version", "input_sha256", "producer_id", "build_id", "source_id",
                 "source_version", "source_day", "rows"}
FILES = {"input": ("input.bin", MAX_INPUT), "before": ("before.json", MAX_OUTPUT), "after": ("after.json", MAX_OUTPUT)}
DIGEST = re.compile(r"[0-9a-f]{64}\Z")
QUANTITY = re.compile(r"-?(?:0|[1-9][0-9]{0,19})(?:\.[0-9]{0,7}[1-9])?\Z")


class ParityError(ValueError):
    """Stable unavailable-evidence code; never disclose underlying paths."""


def _require(condition, code):
    if not condition:
        raise ParityError(code)


def _closed(value, keys, code="invalid_closed_object"):
    _require(type(value) is dict and set(value) == set(keys), code)
    return value


def _identity(value):
    _require(type(value) is str and 0 < len(value) <= 256 and value == value.strip()
             and not any(ord(c) < 32 or ord(c) == 127 for c in value), "invalid_identity")
    try:
        value.encode("utf-8", "strict")
    except UnicodeError:
        raise ParityError("invalid_identity") from None
    return value


def _day(value):
    _require(type(value) is str and re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", value), "invalid_date")
    try:
        _require(date.fromisoformat(value).isoformat() == value, "invalid_date")
    except ValueError:
        raise ParityError("invalid_date") from None
    return value


def _digest(value):
    _require(type(value) is str and DIGEST.fullmatch(value), "invalid_digest")
    return value


def _pairs(pairs):
    value = {}
    for key, item in pairs:
        _require(key not in value, "duplicate_json_key")
        value[key] = item
    return value


def _json(raw):
    def nonfinite(_):
        raise ParityError("nonfinite_json")
    try:
        return json.loads(raw.decode("utf-8", "strict"), object_pairs_hook=_pairs, parse_constant=nonfinite)
    except ParityError:
        raise
    except (UnicodeError, ValueError, RecursionError):
        raise ParityError("invalid_json") from None


def _encoded(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")


def _absolute(path):
    path = Path(path)
    _require(".." not in path.parts, "path_traversal")
    return path.absolute()


def _directory_fd(path):
    """Anchor each ancestor to an opened directory; no symlink is followed."""
    _require(os.open in os.supports_dir_fd and hasattr(os, "O_NOFOLLOW") and hasattr(os, "O_DIRECTORY"),
             "unsupported_safe_filesystem")
    path = _absolute(path)
    _require(path.anchor == "/" and len(path.parts) <= 128, "unsupported_safe_path")
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    fd = None
    try:
        fd = os.open(path.anchor, flags)
        for name in path.parts[1:]:
            following = os.open(name, flags, dir_fd=fd)
            os.close(fd)
            fd = following
        return fd
    except OSError:
        if fd is not None:
            os.close(fd)
        raise ParityError("unsafe_or_missing_directory") from None


def _read_at(directory_fd, filename, limit):
    fd = None
    try:
        fd = os.open(filename, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK, dir_fd=directory_fd)
        info = os.fstat(fd)
        _require(stat.S_ISREG(info.st_mode) and 0 < info.st_size <= limit, "invalid_file_size_or_type")
        with os.fdopen(fd, "rb") as stream:
            fd = None
            raw = stream.read(limit + 1)
        _require(0 < len(raw) <= limit, "invalid_file_size_or_type")
        return raw
    except OSError:
        raise ParityError("unsafe_or_missing_file") from None
    finally:
        if fd is not None:
            os.close(fd)


def _read(path, limit):
    path = _absolute(path)
    parent_fd = _directory_fd(path.parent)
    try:
        return _read_at(parent_fd, path.name, limit)
    finally:
        os.close(parent_fd)


def _metadata(value):
    _closed(value, META_FIELDS)
    _require(value["fixture_kind"] in ("synthetic", "external"), "invalid_fixture_kind")
    _require(value["input_scope"] == "bars_and_run_settings", "invalid_input_scope")
    for name in META_FIELDS - {"fixture_kind", "input_scope", "source_day"}:
        _identity(value[name])
    _day(value["source_day"])
    return dict(value)


def _output(raw, metadata, input_digest, role):
    value = _closed(_json(raw), OUTPUT_FIELDS)
    _require(value["schema_version"] == "qt-model-output/v1", "unsupported_output_schema")
    _digest(value["input_sha256"])
    _require(value["input_sha256"] == input_digest, "output_input_mismatch")
    for name in ("producer_id", "source_id", "source_version", "source_day"):
        _require(value[name] == metadata[name], "output_identity_mismatch")
    _require(value["build_id"] == metadata[role + "_build_id"], "output_build_mismatch")
    _require(type(value["rows"]) is list and len(value["rows"]) <= MAX_ROWS, "invalid_row_count")
    rows = {}
    for row in value["rows"]:
        _closed(row, {"key", "quantity_exact"})
        key = _closed(row["key"], KEY_FIELDS)
        for field in KEY_FIELDS:
            (_day if field == "date" else _identity)(key[field])
        _require(key["portfolio_type"] == "system", "wrong_model_stream")
        _require(key["date"] == metadata["source_day"], "model_key_day_mismatch")
        quantity = row["quantity_exact"]
        _require(type(quantity) is str and QUANTITY.fullmatch(quantity) and quantity != "-0", "invalid_exact_quantity")
        integer, _, fraction = quantity.lstrip("-").partition(".")
        scaled = int(integer) * 100000000 + int((fraction + "00000000")[:8])
        if quantity.startswith("-"):
            scaled = -scaled
        _require(-(1 << 63) <= scaled <= (1 << 63) - 1, "exact_quantity_out_of_range")
        identity = tuple(key[field] for field in KEY_FIELDS)
        _require(identity not in rows, "duplicate_model_key")
        rows[identity] = quantity
    return rows


def capture(input_path, before_path, after_path, directory, metadata):
    """Copy explicit verified inputs into a newly created exclusive bundle."""
    metadata = _metadata(metadata)
    supplied = {"input": _read(input_path, MAX_INPUT), "before": _read(before_path, MAX_OUTPUT),
                "after": _read(after_path, MAX_OUTPUT)}
    input_digest = sha256(supplied["input"]).hexdigest()
    for role in ("before", "after"):
        _output(supplied[role], metadata, input_digest, role)
    artifacts = {role: {"file": FILES[role][0], "sha256": sha256(raw).hexdigest(), "byte_count": len(raw)}
                 for role, raw in supplied.items()}
    manifest = {"schema_version": "qt-model-parity-capture/v1", **metadata, "artifacts": artifacts}
    directory = _absolute(directory)
    parent_fd = _directory_fd(directory.parent)
    capture_fd = None
    try:
        os.mkdir(directory.name, mode=0o700, dir_fd=parent_fd)
        capture_fd = os.open(directory.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=parent_fd)
        # No overwrite or cleanup of a caller's existing tree. An interrupted
        # exclusive capture is incomplete and cannot pass comparison.
        for name, raw in [(FILES[role][0], supplied[role]) for role in FILES] + [("manifest.json", _encoded(manifest))]:
            fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                         0o600, dir_fd=capture_fd)
            with os.fdopen(fd, "wb") as output:
                output.write(raw)
    except OSError:
        raise ParityError("capture_not_exclusive_or_io_error") from None
    finally:
        if capture_fd is not None:
            os.close(capture_fd)
        os.close(parent_fd)
    return directory / "manifest.json"


def _reviewed_provenance(manifest, path, digest):
    _require(path is not None and digest is not None, "incomplete_reviewed_provenance")
    _digest(digest)
    _require(manifest["fixture_kind"] == "external", "reviewed_provenance_requires_external_capture")
    raw = _read(path, MAX_MANIFEST)
    _require(sha256(raw).hexdigest() == digest, "reviewed_provenance_hash_mismatch")
    keys = (META_FIELDS - {"fixture_kind"}) | {"schema_version", "evidence_kind", "review_reference", "artifacts"}
    value = _closed(_json(raw), keys)
    _require(value["schema_version"] == "qt-model-parity-provenance/v1"
             and value["evidence_kind"] == "reviewed_external", "unsupported_provenance_schema")
    _identity(value["review_reference"])
    for name in META_FIELDS - {"fixture_kind"}:
        _require(value[name] == manifest[name], "reviewed_provenance_identity_mismatch")
    _closed(value["artifacts"], FILES)
    for role in FILES:
        _digest(value["artifacts"][role])
        _require(value["artifacts"][role] == manifest["artifacts"][role]["sha256"], "reviewed_provenance_artifact_mismatch")
    return digest


def compare(manifest_path, *, reviewed_provenance=None, reviewed_provenance_sha256=None):
    """Revalidate all bytes; compare complete full-key MODEL quantities."""
    path = _absolute(manifest_path)
    directory_fd = _directory_fd(path.parent)
    try:
        manifest_raw = _read_at(directory_fd, path.name, MAX_MANIFEST)
        value = _closed(_json(manifest_raw), META_FIELDS | {"schema_version", "artifacts"})
        _require(value["schema_version"] == "qt-model-parity-capture/v1", "unsupported_capture_schema")
        metadata = _metadata({name: value[name] for name in META_FIELDS})
        _closed(value["artifacts"], FILES)
        raw = {}
        for role, (filename, limit) in FILES.items():
            artifact = _closed(value["artifacts"][role], {"file", "sha256", "byte_count"})
            _require(artifact["file"] == filename, "invalid_artifact_filename")
            _digest(artifact["sha256"])
            _require(type(artifact["byte_count"]) is int and 0 < artifact["byte_count"] <= limit, "invalid_artifact_byte_count")
            raw[role] = _read_at(directory_fd, filename, limit)
            _require(len(raw[role]) == artifact["byte_count"] and sha256(raw[role]).hexdigest() == artifact["sha256"],
                     "artifact_hash_or_size_mismatch")
    finally:
        os.close(directory_fd)
    input_digest = value["artifacts"]["input"]["sha256"]
    before = _output(raw["before"], metadata, input_digest, "before")
    after = _output(raw["after"], metadata, input_digest, "after")
    provenance_digest = None
    if reviewed_provenance is not None or reviewed_provenance_sha256 is not None:
        provenance_digest = _reviewed_provenance(value, reviewed_provenance, reviewed_provenance_sha256)
    differences = []
    for identity in sorted(set(before) | set(after)):
        prior, current = before.get(identity), after.get(identity)
        if prior == current:
            continue
        differences.append({"kind": "missing_before" if prior is None else "missing_after" if current is None else "quantity",
            "key": dict(zip(KEY_FIELDS, identity)), "before_quantity_exact": prior, "after_quantity_exact": current})
    real = {"status": "unavailable", "reason_code": "synthetic_fixture" if metadata["fixture_kind"] == "synthetic"
            else "missing_reviewed_external_pin"}
    if provenance_digest:
        real = {"status": "unavailable", "reason_code": "model_output_mismatch"} if differences else {
            "status": "externally_reviewed_comparison_pass", "scope": "offline_reviewed_evidence_only"}
    return {"schema_version": "qt-model-parity-verdict/v1", "comparison_status": "mismatch" if differences else "pass",
            "row_count_before": len(before), "row_count_after": len(after), "differences": differences,
            "input_sha256": input_digest, "capture_manifest_sha256": sha256(manifest_raw).hexdigest(),
            "reviewed_provenance_sha256": provenance_digest, "real_certification": real}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="operation", required=True)
    capture_parser = commands.add_parser("capture")
    for name in ("input", "before", "after", "metadata", "directory"):
        capture_parser.add_argument("--" + name, required=True, type=Path)
    compare_parser = commands.add_parser("compare")
    compare_parser.add_argument("--manifest", required=True, type=Path)
    compare_parser.add_argument("--reviewed-provenance", type=Path)
    compare_parser.add_argument("--reviewed-provenance-sha256")
    args = parser.parse_args(argv)
    try:
        if args.operation == "capture":
            manifest = capture(args.input, args.before, args.after, args.directory, _json(_read(args.metadata, MAX_MANIFEST)))
            result = {"schema_version": "qt-model-parity-capture-result/v1", "manifest": str(manifest), "capture_status": "captured"}
            code = 0
        else:
            result = compare(args.manifest, reviewed_provenance=args.reviewed_provenance,
                             reviewed_provenance_sha256=args.reviewed_provenance_sha256)
            code = 1 if result["comparison_status"] == "mismatch" else 0
    except ParityError as error:
        result = {"schema_version": "qt-model-parity-verdict/v1", "comparison_status": "unavailable", "error_code": str(error),
                  "real_certification": {"status": "unavailable", "reason_code": "invalid_evidence"}}
        code = 2
    print(_encoded(result).decode("utf-8"))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
