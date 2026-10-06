"""Offline all-output harness regressions; stdlib unittest also runs in pytest."""
from hashlib import sha256
import importlib.util
import json
import os
from copy import deepcopy
from pathlib import Path
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[2] / "apps/tools/qt_full_output_parity.py"


def load_tool():
    if not TOOL.exists():
        return None
    spec = importlib.util.spec_from_file_location("qt_full_output_parity", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


TABLES = ("trading.positions", "trading.executions", "trading.live_results",
          "trading.equity_curve", "trading.live_run_metadata")
INVENTORY = ["table:" + table for table in TABLES] + ["log:runner", "csv:positions"]


def cases():
    return {asset: {
        "input_bytes": (asset + " frozen synthetic bars and settings").encode(),
        "input_sha256": sha256((asset + " frozen synthetic bars and settings").encode()).hexdigest(),
        "fixture_kind": "synthetic", "inventory": list(INVENTORY),
        "table_schemas": {name: [["strategy_name", "text"], ["quantity", "numeric"], ["metric", "numeric"]]
                          for name in INVENTORY if name.startswith("table:")},
        "before_source_sha256": "1" * 64, "after_source_sha256": "2" * 64,
        "before_build_id": "before-" + asset, "after_build_id": "after-" + asset,
    } for asset in ("futures", "equity")}


def adapter(change=None, observed=None):
    def produce(request):
        if observed is not None:
            observed.append(request)
        assert not list(request["build_directory"].iterdir())
        assert not list(request["output_directory"].iterdir())
        assert sha256(request["input_path"].read_bytes()).hexdigest() == request["input_sha256"]
        (request["build_directory"] / "synthetic-build-marker").write_text("fixture adapter only")
        outputs = {}
        for index, identity in enumerate(request["inventory"]):
            path = request["output_directory"] / (str(index) + ".bin")
            if identity.startswith("table:"):
                data = json.dumps({"columns": ["strategy_name", "quantity", "metric"],
                                   "types": ["text", "numeric", "numeric"],
                                   "rows": [["alpha", "1.25", "9.125"],
                                            ["beta", "-1.25", "8.5"]]}).encode()
            elif identity.startswith("log:"):
                data = b"calculation completed\n"
            else:
                data = b"symbol,quantity\nA,1.25\n"
            if change:
                data = change(request, identity, data)
            path.write_bytes(data)
            outputs[identity] = path
        return {"source_sha256": request["source_sha256"], "build_id": request["build_id"],
                "input_sha256": request["input_sha256"], "outputs": outputs}
    return produce


class FullOutputParityTests(unittest.TestCase):
    def setUp(self):
        self.parity = load_tool()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name) / "new-capture"

    def run_matrix(self, config=None, before=None, after=None):
        self.assertIsNotNone(self.parity, "full-output parity harness is missing")
        return self.parity.run_matrix(self.work, config or cases(), before or adapter(), after or adapter())

    def test_futures_and_equity_full_outputs_match_without_authentic_certification(self):
        observed = []
        result = self.run_matrix(before=adapter(observed=observed), after=adapter(observed=observed))
        self.assertEqual(result["comparison_status"], "pass")
        self.assertEqual(set(result["cases"]), {"futures", "equity"})
        self.assertEqual(len({str(r["build_directory"]) for r in observed}), 4)
        for case in result["cases"].values():
            self.assertEqual(case["differences"], [])
            self.assertEqual(set(case["artifacts"]), set(INVENTORY))
        self.assertEqual(result["authentic_runtime_certification"]["status"], "unavailable")

    def test_every_table_field_log_and_csv_difference_is_visible(self):
        for identity in INVENTORY:
            with self.subTest(identity=identity):
                def change(request, name, raw):
                    if request["asset_class"] != "equity" or name != identity:
                        return raw
                    if name.startswith("table:"):
                        table = json.loads(raw)
                        table["rows"][1][2] = "8.500000001"
                        return json.dumps(table).encode()
                    return raw + b" "
                work = self.work / str(INVENTORY.index(identity))
                self.work.mkdir(exist_ok=True)
                result = self.parity.run_matrix(work, cases(), adapter(), adapter(change))
                self.assertEqual(result["comparison_status"], "mismatch")
                self.assertEqual(result["cases"]["equity"]["differences"],
                                 [{"artifact": identity, "kind": "bytes_differ"}])

    def test_invalid_cases_fail_before_adapters_or_files(self):
        variants = []
        value = cases(); del value["equity"]; variants.append(value)
        value = cases(); value["equity"]["inventory"].remove("table:trading.executions"); variants.append(value)
        value = cases(); value["equity"]["inventory"].remove("log:runner"); variants.append(value)
        value = cases(); value["equity"]["inventory"].append("csv:positions"); variants.append(value)
        value = cases(); value["equity"]["inventory"].append("table:../../secret"); variants.append(value)
        value = cases(); value["equity"]["input_sha256"] = "0" * 64; variants.append(value)
        value = cases(); value["equity"]["before_source_sha256"] = "unknown"; variants.append(value)
        value = cases(); value["equity"]["after_build_id"] = value["equity"]["before_build_id"]; variants.append(value)
        value = cases(); value["equity"]["trusted_real"] = True; variants.append(value)
        for config in variants:
            with self.subTest(config=config):
                observed = []
                with self.assertRaises(self.parity.ParityError):
                    self.run_matrix(config, adapter(observed=observed))
                self.assertFalse(self.work.exists())
                self.assertEqual(observed, [])

    def test_invalid_adapter_capture_cannot_pass(self):
        for mutation in ("source", "build", "input", "missing", "extra", "escape", "unlisted", "alias", "table"):
            with self.subTest(mutation=mutation):
                def bad(request):
                    capture = adapter()(request)
                    if mutation in ("source", "build", "input"):
                        capture[{"source": "source_sha256", "build": "build_id", "input": "input_sha256"}[mutation]] = "wrong"
                    elif mutation == "missing": del capture["outputs"]["csv:positions"]
                    elif mutation == "extra": capture["outputs"]["csv:extra"] = capture["outputs"]["csv:positions"]
                    elif mutation == "escape": capture["outputs"]["log:runner"] = request["input_path"]
                    elif mutation == "unlisted": (request["output_directory"] / "unobserved.log").write_text("hidden")
                    elif mutation == "alias": capture["outputs"]["log:runner"] = capture["outputs"]["csv:positions"]
                    else: capture["outputs"]["table:trading.positions"].write_text('{"columns":["a","a"],"rows":[[1]]}')
                    return capture
                work = Path(self.tmp.name) / ("bad-" + mutation)
                with self.assertRaises(self.parity.ParityError):
                    self.parity.run_matrix(work, cases(), bad, adapter())

    def test_frozen_input_and_before_outputs_cannot_change_during_after_run(self):
        for target in ("input", "before"):
            with self.subTest(target=target):
                seen = []
                def after(request):
                    capture = adapter()(request)
                    if target == "input": request["input_path"].write_bytes(b"changed inputs")
                    else: next(seen[0]["output_directory"].iterdir()).write_bytes(b"changed before")
                    return capture
                with self.assertRaises(self.parity.ParityError):
                    self.parity.run_matrix(Path(self.tmp.name) / target, cases(), adapter(observed=seen), after)

    def test_existing_capture_is_never_rebuilt_or_overwritten(self):
        self.work.mkdir()
        marker = self.work / "original"
        marker.write_bytes(b"retain me")
        with self.assertRaises(self.parity.ParityError):
            self.run_matrix()
        self.assertEqual(marker.read_bytes(), b"retain me")

    def test_external_input_claim_still_cannot_certify_runtime(self):
        config = cases()
        for case in config.values(): case["fixture_kind"] = "external"
        result = self.run_matrix(config)
        self.assertEqual(result["comparison_status"], "pass")
        self.assertEqual(result["authentic_runtime_certification"]["status"], "unavailable")

    def test_equity_adapter_cannot_mutate_already_compared_futures_artifacts(self):
        seen = []
        def after(request):
            capture = adapter()(request)
            if request["asset_class"] == "equity":
                next(seen[0]["output_directory"].iterdir()).write_bytes(b"cross-case mutation")
            return capture
        with self.assertRaises(self.parity.ParityError):
            self.run_matrix(before=adapter(observed=seen), after=after)

    def test_quiet_run_can_have_empty_logs_and_csv_bytes(self):
        def empty(_request, name, raw):
            return b"" if name.startswith(("log:", "csv:")) else raw
        result = self.run_matrix(before=adapter(empty), after=adapter(empty))
        self.assertEqual(result["comparison_status"], "pass")
        self.assertEqual(result["cases"]["equity"]["artifacts"]["log:runner"]["after"]["byte_count"], 0)

    def test_bad_table_data_cannot_be_masked_by_equal_bytes(self):
        for raw in (b'{"columns":["a"],"rows":[[NaN]]}',
                    b'{"columns":["a"],"rows":[[1]],"rows":[]}',
                    b'{"columns":["a","b"],"rows":[[1]]}',
                    b'{"columns":["a"],"rows":[[{}]],"unknown":0}'):
            with self.subTest(raw=raw):
                def bad(_request, name, current):
                    return raw if name.startswith("table:") else current
                with self.assertRaises(self.parity.ParityError):
                    self.parity.run_matrix(Path(self.tmp.name) / sha256(raw).hexdigest(),
                                           cases(), adapter(bad), adapter(bad))

    def test_inventory_additional_tables_must_also_be_present_and_match(self):
        config = cases()
        for case in config.values(): case["inventory"].append("table:trading.run_inputs")
        for case in config.values(): case["table_schemas"]["table:trading.run_inputs"] = deepcopy(case["table_schemas"]["table:trading.positions"])
        def change(request, name, raw):
            return raw + b" " if request["role"] == "after" and name == "table:trading.run_inputs" else raw
        result = self.run_matrix(config, adapter(), adapter(change))
        self.assertEqual(result["comparison_status"], "mismatch")
        for case in result["cases"].values():
            self.assertEqual(case["differences"], [{"artifact": "table:trading.run_inputs", "kind": "bytes_differ"}])

    def test_table_order_columns_and_zero_rows_are_not_normalized(self):
        for mutation in ("order", "column", "empty"):
            with self.subTest(mutation=mutation):
                def change(_request, name, raw):
                    if name != "table:trading.positions": return raw
                    table = json.loads(raw)
                    if mutation == "order": table["rows"].reverse()
                    elif mutation == "column": table["columns"][2] = "another_metric"
                    else: table["rows"] = []
                    return json.dumps(table).encode()
                if mutation == "column":
                    with self.assertRaisesRegex(self.parity.ParityError, "table_schema_mismatch"):
                        self.parity.run_matrix(Path(self.tmp.name) / mutation, cases(), adapter(), adapter(change))
                else:
                    result = self.parity.run_matrix(Path(self.tmp.name) / mutation,
                                                   cases(), adapter(), adapter(change))
                    self.assertEqual(result["comparison_status"], "mismatch")

    def test_hardlinked_and_oversized_artifacts_refuse(self):
        for mutation in ("hardlink", "oversized"):
            with self.subTest(mutation=mutation):
                def bad(request):
                    capture = adapter()(request)
                    path = capture["outputs"]["log:runner"]
                    if mutation == "hardlink": os.link(path, request["build_directory"] / "alias")
                    else:
                        with path.open("r+b") as stream: stream.truncate(self.parity.MAX_ARTIFACT + 1)
                    return capture
                with self.assertRaises(self.parity.ParityError):
                    self.parity.run_matrix(Path(self.tmp.name) / mutation, cases(), bad, adapter())

    def test_traversal_capture_path_refuses_before_writing(self):
        with self.assertRaises(self.parity.ParityError):
            self.parity.run_matrix(Path(self.tmp.name) / "child" / ".." / "capture",
                                   cases(), adapter(), adapter())
        self.assertFalse((Path(self.tmp.name) / "capture").exists())

    def test_independent_schema_is_required_before_any_adapter_or_write(self):
        self.assertIn("table_schemas", self.parity.CASE_KEYS)
        for mutation in ("missing", "additional", "malformed", "duplicate", "wrong_type", "extra"):
            with self.subTest(mutation=mutation):
                config = cases()
                schema = {name: [["strategy_name", "text"], ["quantity", "numeric"], ["metric", "numeric"]]
                          for name in INVENTORY if name.startswith("table:")}
                for case in config.values(): case["table_schemas"] = deepcopy(schema)
                target = config["equity"]["table_schemas"]
                if mutation == "missing": target.pop("table:trading.positions")
                elif mutation == "additional": config["equity"]["inventory"].append("table:trading.additional")
                elif mutation == "malformed": target["table:trading.positions"][0] = ["strategy_name"]
                elif mutation == "duplicate": target["table:trading.positions"][1][0] = "strategy_name"
                elif mutation == "wrong_type": target["table:trading.positions"][1][1] = None
                else: target["table:trading.unlisted"] = [["a", "text"]]
                observed = []
                with self.assertRaises(self.parity.ParityError):
                    self.parity.run_matrix(self.work, config, adapter(observed=observed), adapter())
                self.assertFalse(self.work.exists())
                self.assertEqual(observed, [])

    def test_equal_schema_omissions_changes_and_types_on_both_sides_refuse(self):
        for mutation in ("omit", "rename", "reorder", "add", "type"):
            with self.subTest(mutation=mutation):
                def change(_request, name, raw):
                    if not name.startswith("table:"): return raw
                    table = json.loads(raw)
                    if mutation == "omit":
                        table["columns"] = table["columns"][:1]
                        table["types"] = table.get("types", ["text"])[:1]
                        table["rows"] = [row[:1] for row in table["rows"]]
                    elif mutation == "rename": table["columns"][1] = "omitted_quantity"
                    elif mutation == "reorder": table["columns"].reverse()
                    elif mutation == "add":
                        table["columns"].append("unreviewed")
                        table["types"] = table.get("types", ["text", "numeric", "numeric"]) + ["text"]
                        table["rows"] = [row + ["x"] for row in table["rows"]]
                    else: table["types"] = ["text", "text", "numeric"]
                    return json.dumps(table).encode()
                with self.assertRaises(self.parity.ParityError):
                    self.parity.run_matrix(Path(self.tmp.name) / mutation, cases(), adapter(change), adapter(change))

    def test_capture_and_remaining_matrix_budgets_preflight_before_body_reads(self):
        table_size = len(json.dumps({"columns": ["strategy_name", "quantity", "metric"],
                                    "types": ["text", "numeric", "numeric"],
                                    "rows": [["alpha", "1.25", "9.125"], ["beta", "-1.25", "8.5"]]}).encode())
        capture_size = 5 * table_size + len(b"calculation completed\n") + len(b"symbol,quantity\nA,1.25\n")
        for cap, expected_reads in ((50, 0), (capture_size + 1, 7)):
            with self.subTest(cap=cap):
                read = self.parity._read
                observed = []
                def watched(path, *args, **kwargs):
                    if path.parent.name == "output": observed.append(path)
                    return read(path, *args, **kwargs)
                self.parity._read = watched
                self.parity.MAX_TOTAL = cap
                try:
                    with self.assertRaises(self.parity.ParityError):
                        self.parity.run_matrix(Path(self.tmp.name) / str(cap), cases(), adapter(), adapter())
                    self.assertEqual(len(observed), expected_reads)
                finally:
                    self.parity._read = read
                    self.parity.MAX_TOTAL = 128 * 1024 * 1024

    def test_complete_schema_pins_are_immutable_and_bound_into_evidence(self):
        config = cases()
        expected = deepcopy(config["futures"]["table_schemas"])
        def producer(request):
            # The caller-owned expectation must survive request-map edits.
            request["table_schemas"]["table:trading.positions"] = (("wrong", "text"),)
            return adapter()(request)
        result = self.run_matrix(config, producer, producer)
        for case in result["cases"].values():
            self.assertEqual(case["table_schemas_sha256"],
                             sha256(json.dumps(expected, sort_keys=True, separators=(",", ":")).encode()).hexdigest())
            self.assertEqual(case["table_schemas"]["table:trading.positions"],
                             tuple(tuple(column) for column in expected["table:trading.positions"]))

    def test_exact_matrix_budget_and_empty_artifacts_pass(self):
        def empty(_request, name, raw):
            return b"" if name.startswith(("log:", "csv:")) else raw
        baseline = self.run_matrix(before=adapter(empty), after=adapter(empty))
        budget = sum(item[role]["byte_count"] for case in baseline["cases"].values()
                     for item in case["artifacts"].values() for role in ("before", "after"))
        self.parity.MAX_TOTAL = budget
        result = self.parity.run_matrix(Path(self.tmp.name) / "exact-budget", cases(), adapter(empty), adapter(empty))
        self.assertEqual(result["comparison_status"], "pass")

    def test_growth_after_preflight_stays_within_remaining_budget(self):
        baseline = self.run_matrix()
        budget = sum(item["before"]["byte_count"] for item in baseline["cases"]["futures"]["artifacts"].values())
        self.parity.MAX_TOTAL = budget
        read = self.parity._read
        consumed = []
        mutated = []
        def growing(path, limit):
            if path.parent.name == "output" and not mutated:
                path.write_bytes(path.read_bytes() + b" ")
                mutated.append(path)
            raw = read(path, limit)
            if path.parent.name == "output": consumed.append(len(raw))
            return raw
        self.parity._read = growing
        with self.assertRaises(self.parity.ParityError):
            self.parity.run_matrix(Path(self.tmp.name) / "growing", cases(), adapter(), adapter())
        self.assertLessEqual(sum(consumed), budget)
        self.assertEqual(len(mutated), 1)

    def test_final_revalidation_preflights_growth_before_reading_bodies(self):
        original = self.parity._revalidate
        read = self.parity._read
        observed = []
        mutated = []
        def watched(path, *args, **kwargs):
            observed.append(path)
            return read(path, *args, **kwargs)
        def growing(request, paths, captured):
            if not mutated:
                path = paths["log:runner"]
                path.write_bytes(path.read_bytes() + b" ")
                mutated.append(path)
                self.parity._read = watched
            return original(request, paths, captured)
        self.parity._revalidate = growing
        with self.assertRaises(self.parity.ParityError):
            self.run_matrix()
        self.assertEqual(observed, [])


if __name__ == "__main__":
    unittest.main()
