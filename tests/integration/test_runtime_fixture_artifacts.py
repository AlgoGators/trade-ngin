"""Pure tests for generated fixture safety; no PostgreSQL or probe is started."""
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

import test_runtime_control_schema as runtime


def test_exact_fixture_writer_preserves_identical_replay(tmp_path):
    target = tmp_path / "synthetic.json"
    raw = b'{"quantity_exact":"0.125"}\n'
    runtime._write_exact_fixture(target, raw)
    runtime._write_exact_fixture(target, raw)
    assert target.read_bytes() == raw


def test_exact_fixture_writer_refuses_to_overwrite_existing_evidence(tmp_path):
    target = tmp_path / "synthetic.json"
    original = b'{"quantity_exact":"0.125"}\n'
    runtime._write_exact_fixture(target, original)
    with pytest.raises(AssertionError, match="different fixture bytes"):
        runtime._write_exact_fixture(target, b'{"quantity_exact":"9"}\n')
    assert target.read_bytes() == original


class _SnapshotCursor:
    def __init__(self, raw):
        self.raw = raw

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def execute(self, sql):
        self.sql = sql

    def fetchone(self):
        if "portfolio_config" in self.sql:
            return (self.raw,)
        if "strategy_registry" in self.sql:
            return ("synthetic-registry", "synthetic-engine", "synthetic-book", 1)
        raise AssertionError("unexpected synthetic query")

    def fetchall(self):
        return []


def test_generated_publication_artifacts_use_explicit_owned_directory(tmp_path):
    child = {"identity": {"publication_id": "synthetic-publication"}}
    raw = json.dumps(child)
    conn = SimpleNamespace(cursor=lambda: _SnapshotCursor(raw))
    result = SimpleNamespace(stdout="CONFIG_PUBLICATION=" + raw + "\n",
                             args=[__file__])
    evidence = tmp_path / "generated-evidence"
    fixture = runtime._save_required_publication_fixture(
        conn, "synthetic_mode", result, child, evidence=evidence)
    digest = hashlib.sha256(raw.encode("utf-8")).hexdigest()
    assert fixture.parent == evidence
    assert fixture.read_bytes() == raw.encode("utf-8")
    sidecar = fixture.with_name(f"synthetic_mode-{digest[:12]}.provenance.json")
    assert json.loads(sidecar.read_text(encoding="utf-8"))["fixture_sha256"] == digest
    assert set(evidence.iterdir()) == {fixture, sidecar}
    assert Path(__file__).resolve().parents[2] not in fixture.parents
