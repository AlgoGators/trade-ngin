"""Pure offline evidence tests: no child process, database or network."""
from copy import deepcopy
from hashlib import sha256
import importlib.util
import json
from pathlib import Path

import pytest

SPEC = importlib.util.spec_from_file_location("qt_model_parity", Path(__file__).resolve().parents[2] / "apps/tools/qt_model_parity.py")
parity = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(parity)


def encoded(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode()


def key(owner="alpha", symbol="SYN"):
    return {"portfolio_id": "BOOK", "strategy_id": "engine-one", "strategy_name": owner,
            "date": "2026-09-25", "symbol": symbol, "portfolio_type": "system"}


@pytest.fixture
def evidence(tmp_path):
    metadata = {"fixture_kind": "synthetic", "input_scope": "bars_and_run_settings",
                "source_id": "offline-synthetic-bars-and-settings", "source_version": "fixture/v1",
                "source_day": "2026-09-25", "producer_id": "synthetic-model/v1",
                "before_build_id": "before-build/v1", "after_build_id": "after-build/v2"}
    raw = b'{"bars":[{"close":"101.25"}],"run_settings":{"risk_version":"v1"}}'
    before = {"schema_version": "qt-model-output/v1", "input_sha256": sha256(raw).hexdigest(),
              "producer_id": metadata["producer_id"], "build_id": metadata["before_build_id"],
              "source_id": metadata["source_id"], "source_version": metadata["source_version"],
              "source_day": metadata["source_day"], "rows": [
                  {"key": key(), "quantity_exact": "1.25"},
                  {"key": key("beta"), "quantity_exact": "-1.25"},
                  {"key": key("zero", "ZERO"), "quantity_exact": "0"}]}
    after = deepcopy(before)
    after["build_id"] = metadata["after_build_id"]
    after["rows"].reverse()
    paths = [tmp_path / name for name in ("source.bin", "before-supplied.json", "after-supplied.json")]
    for path, data in zip(paths, (raw, encoded(before), encoded(after))):
        path.write_bytes(data)
    return {"paths": paths, "before": before, "after": after, "metadata": metadata,
            "directory": tmp_path / "capture", "tmp": tmp_path, "raw": raw}


def captured(evidence):
    return parity.capture(*evidence["paths"], evidence["directory"], evidence["metadata"])


def provenance(evidence, manifest):
    value = {"schema_version": "qt-model-parity-provenance/v1", "evidence_kind": "reviewed_external",
             "review_reference": "explicit-human-review-2026-09-26",
             **{k: v for k, v in evidence["metadata"].items() if k != "fixture_kind"},
             "artifacts": {name: item["sha256"] for name, item in json.loads(manifest.read_bytes())["artifacts"].items()}}
    path = evidence["tmp"] / "reviewed-provenance.json"
    path.write_bytes(encoded(value))
    return path, sha256(path.read_bytes()).hexdigest()


def test_same_complete_model_book_compares_exactly_without_real_claim(evidence):
    manifest = captured(evidence)
    result = parity.compare(manifest)
    assert result["comparison_status"] == "pass" and result["differences"] == []
    assert result["row_count_before"] == result["row_count_after"] == 3
    assert result["real_certification"]["status"] == "unavailable"
    assert result["real_certification"]["reason_code"] == "synthetic_fixture"
    assert result["reviewed_provenance_sha256"] is None
    for source, name in zip(evidence["paths"], ("input.bin", "before.json", "after.json")):
        assert source.read_bytes() == (manifest.parent / name).read_bytes()


def test_complete_full_key_and_quantity_differences_never_net_away(evidence):
    after = evidence["after"]
    after["rows"] = [{"key": key("alpha"), "quantity_exact": "2.5"},
                     {"key": key("gamma"), "quantity_exact": "-2.5"},
                     {"key": key("zero", "ZERO"), "quantity_exact": "0"}]
    evidence["paths"][2].write_bytes(encoded(after))
    result = parity.compare(captured(evidence))
    assert result["comparison_status"] == "mismatch"
    assert {item["kind"] for item in result["differences"]} == {"quantity", "missing_after", "missing_before"}
    assert {item["key"]["strategy_name"] for item in result["differences"]} == {"alpha", "beta", "gamma"}
    assert result["real_certification"]["status"] == "unavailable"


@pytest.mark.parametrize("mutation", ["input_hash", "source_version", "producer", "build", "day", "duplicate_row",
    "float", "scientific", "precision", "trailing_zero", "negative_zero", "wrong_stream", "bad_date", "missing_key", "unknown_field"])
def test_invalid_supplied_evidence_refuses_before_capture_writes(evidence, mutation):
    value = evidence["after"]
    if mutation == "input_hash": value["input_sha256"] = "0" * 64
    elif mutation == "source_version": value["source_version"] = "wrong/v2"
    elif mutation == "producer": value["producer_id"] = "other-model/v1"
    elif mutation == "build": value["build_id"] = "unexpected-build"
    elif mutation == "day": value["source_day"] = "2026-09-26"
    elif mutation == "duplicate_row": value["rows"].append(deepcopy(value["rows"][0]))
    elif mutation == "float": value["rows"][0]["quantity_exact"] = 1.25
    elif mutation == "scientific": value["rows"][0]["quantity_exact"] = "1e0"
    elif mutation == "precision": value["rows"][0]["quantity_exact"] = "1.000000001"
    elif mutation == "trailing_zero": value["rows"][0]["quantity_exact"] = "1.250"
    elif mutation == "negative_zero": value["rows"][0]["quantity_exact"] = "-0"
    elif mutation == "wrong_stream": value["rows"][0]["key"]["portfolio_type"] = "qt"
    elif mutation == "bad_date": value["rows"][0]["key"]["date"] = "2026-02-30"
    elif mutation == "missing_key": del value["rows"][0]["key"]["strategy_id"]
    elif mutation == "unknown_field": value["trusted_real"] = True
    evidence["paths"][2].write_bytes(encoded(value))
    with pytest.raises(parity.ParityError): captured(evidence)
    assert not evidence["directory"].exists()


def test_duplicate_json_keys_and_nonfinite_numbers_refuse(evidence):
    for raw in (b'{"schema_version":"qt-model-output/v1","schema_version":"qt-model-output/v1"}', b'{"rows":NaN}'):
        evidence["paths"][2].write_bytes(raw)
        with pytest.raises(parity.ParityError): captured(evidence)
        assert not evidence["directory"].exists()


def test_capture_refuses_existing_destination_and_does_not_change_any_byte(evidence):
    manifest = captured(evidence)
    before = {p.name: p.read_bytes() for p in manifest.parent.iterdir()}
    with pytest.raises(parity.ParityError): captured(evidence)
    assert {p.name: p.read_bytes() for p in manifest.parent.iterdir()} == before


@pytest.mark.parametrize("target", ["input", "before", "after", "manifest_hash", "filename", "missing_file"])
def test_missing_tampered_and_traversing_capture_evidence_refuses(evidence, target):
    manifest = captured(evidence)
    value = json.loads(manifest.read_bytes())
    if target in {"input", "before", "after"}:
        path = manifest.parent / value["artifacts"][target]["file"]
        path.write_bytes(path.read_bytes() + b" ")
    elif target == "manifest_hash":
        value["artifacts"]["input"]["sha256"] = "0" * 64
        manifest.write_bytes(encoded(value))
    elif target == "filename":
        value["artifacts"]["input"]["file"] = "../source.bin"
        manifest.write_bytes(encoded(value))
    else: (manifest.parent / "after.json").unlink()
    with pytest.raises(parity.ParityError): parity.compare(manifest)


def test_symlink_inputs_destination_and_parent_are_refused(evidence):
    alias = evidence["tmp"] / "alias-input"
    alias.symlink_to(evidence["paths"][0])
    with pytest.raises(parity.ParityError): parity.capture(alias, *evidence["paths"][1:], evidence["directory"], evidence["metadata"])
    parent = evidence["tmp"] / "alias-parent"
    parent.symlink_to(evidence["tmp"], target_is_directory=True)
    with pytest.raises(parity.ParityError): parity.capture(*evidence["paths"], parent / "new-capture", evidence["metadata"])
    assert not evidence["directory"].exists() and not (evidence["tmp"] / "new-capture").exists()


def test_external_claim_alone_never_creates_real_certification(evidence):
    evidence["metadata"]["fixture_kind"] = "external"
    result = parity.compare(captured(evidence))
    assert result["comparison_status"] == "pass"
    assert result["real_certification"]["status"] == "unavailable"
    assert result["real_certification"]["reason_code"] == "missing_reviewed_external_pin"


def test_separately_pinned_reviewed_evidence_is_scoped_and_content_bound(evidence):
    evidence["metadata"]["fixture_kind"] = "external"
    manifest = captured(evidence)
    path, digest = provenance(evidence, manifest)
    result = parity.compare(manifest, reviewed_provenance=path, reviewed_provenance_sha256=digest)
    assert result["real_certification"] == {"status": "externally_reviewed_comparison_pass", "scope": "offline_reviewed_evidence_only"}
    assert result["reviewed_provenance_sha256"] == digest
    path.write_bytes(path.read_bytes() + b" ")
    with pytest.raises(parity.ParityError): parity.compare(manifest, reviewed_provenance=path, reviewed_provenance_sha256=digest)


@pytest.mark.parametrize("mutation", ["producer", "input", "no_pin", "no_file", "synthetic"])
def test_unbound_external_provenance_cannot_create_authentic_claim(evidence, mutation):
    evidence["metadata"]["fixture_kind"] = "external" if mutation != "synthetic" else "synthetic"
    manifest = captured(evidence)
    path, digest = provenance(evidence, manifest)
    value = json.loads(path.read_bytes())
    if mutation == "producer": value["producer_id"] = "wrong/v1"
    if mutation == "input": value["artifacts"]["input"] = "0" * 64
    path.write_bytes(encoded(value));digest = sha256(path.read_bytes()).hexdigest()
    with pytest.raises(parity.ParityError): parity.compare(manifest,
        reviewed_provenance=None if mutation == "no_file" else path,
        reviewed_provenance_sha256=None if mutation == "no_pin" else digest)


def test_cli_emits_machine_verdict_and_distinguishes_mismatch_from_invalid(evidence, capsys):
    manifest = captured(evidence)
    assert parity.main(["compare", "--manifest", str(manifest)]) == 0
    assert json.loads(capsys.readouterr().out)["comparison_status"] == "pass"
    (manifest.parent / "input.bin").write_bytes(b"tampered")
    assert parity.main(["compare", "--manifest", str(manifest)]) == 2
    refused = json.loads(capsys.readouterr().out)
    assert refused["comparison_status"] == "unavailable" and refused["real_certification"]["status"] == "unavailable"
    assert "error_code" in refused and str(evidence["tmp"]) not in json.dumps(refused)


def test_cli_capture_then_quantity_mismatch_has_exit_one(evidence, capsys):
    after = evidence["after"]
    after["rows"][0]["quantity_exact"] = "7"
    evidence["paths"][2].write_bytes(encoded(after))
    metadata = evidence["tmp"] / "metadata.json"
    metadata.write_bytes(encoded(evidence["metadata"]))
    assert parity.main(["capture", "--input", str(evidence["paths"][0]), "--before", str(evidence["paths"][1]),
        "--after", str(evidence["paths"][2]), "--metadata", str(metadata), "--directory", str(evidence["directory"])]) == 0
    manifest = json.loads(capsys.readouterr().out)["manifest"]
    assert parity.main(["compare", "--manifest", manifest]) == 1
    assert json.loads(capsys.readouterr().out)["comparison_status"] == "mismatch"


def test_even_reviewed_external_pin_cannot_certify_a_quantity_mismatch(evidence):
    evidence["metadata"]["fixture_kind"] = "external"
    evidence["after"]["rows"][0]["quantity_exact"] = "7"
    evidence["paths"][2].write_bytes(encoded(evidence["after"]))
    manifest = captured(evidence)
    path, digest = provenance(evidence, manifest)
    result = parity.compare(manifest, reviewed_provenance=path, reviewed_provenance_sha256=digest)
    assert result["comparison_status"] == "mismatch"
    assert result["real_certification"] == {"status": "unavailable", "reason_code": "model_output_mismatch"}


def test_bounds_missing_metadata_and_unsafe_files_fail_without_fd_leaks(evidence, monkeypatch):
    import os
    count_before = len(list(Path("/proc/self/fd").iterdir()))
    metadata = deepcopy(evidence["metadata"])
    del metadata["source_version"]
    with pytest.raises(parity.ParityError): parity.capture(*evidence["paths"], evidence["directory"], metadata)
    with monkeypatch.context() as scoped:
        scoped.setattr(parity, "MAX_INPUT", 8)
        with pytest.raises(parity.ParityError): captured(evidence)
    pipe = evidence["tmp"] / "not-a-regular-file"
    os.mkfifo(pipe)
    with pytest.raises(parity.ParityError): parity.capture(pipe, *evidence["paths"][1:], evidence["directory"], evidence["metadata"])
    manifest = captured(evidence)
    for _ in range(4):
        with pytest.raises(parity.ParityError): captured(evidence)
    (manifest.parent / "after.json").unlink()
    (manifest.parent / "after.json").symlink_to(evidence["paths"][2])
    with pytest.raises(parity.ParityError): parity.compare(manifest)
    assert len(list(Path("/proc/self/fd").iterdir())) == count_before


def test_lexical_traversal_is_rejected_even_when_existing_target_is_valid(evidence):
    traversing = evidence["tmp"] / "unused" / ".." / evidence["paths"][0].name
    with pytest.raises(parity.ParityError): parity.capture(traversing, *evidence["paths"][1:], evidence["directory"], evidence["metadata"])
    assert not evidence["directory"].exists()


@pytest.mark.parametrize("quantity", ["92233720368.54775807", "-92233720368.54775808"])
def test_shared_decimal8_signed_int64_exact_boundaries_are_accepted(evidence, quantity):
    for role, index in (("before", 1), ("after", 2)):
        for row in evidence[role]["rows"]:
            if row["key"]["strategy_name"] == "alpha": row["quantity_exact"] = quantity
        evidence["paths"][index].write_bytes(encoded(evidence[role]))
    assert parity.compare(captured(evidence))["comparison_status"] == "pass"


@pytest.mark.parametrize("quantity", ["92233720368.54775808", "-92233720368.54775809"])
def test_one_scaled_unit_outside_shared_decimal8_range_is_refused(evidence, quantity):
    evidence["after"]["rows"][0]["quantity_exact"] = quantity
    evidence["paths"][2].write_bytes(encoded(evidence["after"]))
    with pytest.raises(parity.ParityError): captured(evidence)
    assert not evidence["directory"].exists()


def test_matching_output_rows_on_wrong_valid_day_cannot_create_capture(evidence):
    for role, index in (("before", 1), ("after", 2)):
        for row in evidence[role]["rows"]: row["key"]["date"] = "2026-09-24"
        evidence["paths"][index].write_bytes(encoded(evidence[role]))
    with pytest.raises(parity.ParityError): captured(evidence)
    assert not evidence["directory"].exists()


def test_oversized_json_integer_is_stable_cli_unavailable_not_traceback(evidence, capsys):
    manifest = captured(evidence)
    manifest.write_bytes(b'{"invalid_integer":' + b'9' * 5000 + b'}')
    assert parity.main(["compare", "--manifest", str(manifest)]) == 2
    result = json.loads(capsys.readouterr().out)
    assert result["comparison_status"] == "unavailable" and result["error_code"] == "invalid_json"
    assert result["real_certification"]["status"] == "unavailable"
