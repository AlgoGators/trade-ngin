"""Manifest-backed offline analysis cannot bypass the causality gate."""

import csv
import datetime as dt
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.backtest_validation.workflow import (  # noqa: E402
    WorkflowError,
    analyze,
    load_matrix,
    offline_selection,
)


class WorkflowTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.dates = [dt.date(2020, 12, 1) + dt.timedelta(days=i) for i in range(20)] + [
            dt.date(2021, 1, 1) + dt.timedelta(days=i) for i in range(20)]
        self.runs = []
        for trial, offset in (("A", 0.001), ("B", -0.001)):
            path = self.root / (trial + ".csv")
            equity = 100.0
            with path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.writer(stream)
                writer.writerow(("run_id", "portfolio_id", "timestamp", "equity"))
                writer.writerow((trial, "P", "2020-11-30T00:00:00.000000Z", "100"))
                for i, date in enumerate(self.dates):
                    equity *= 1 + offset + (0.004 if i % 2 else -0.002)
                    writer.writerow((trial, "P", date.isoformat() + "T00:00:00.000000Z",
                                     format(equity, ".10f")))
            self.runs.append({"trial_id": trial, "run_id": trial, "portfolio_id": "P",
                              "curve_csv": path.name,
                              "curve_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                              "warmup_last_timestamp": "2020-11-30T00:00:00.000000Z",
                              "scored_start": "2020-12-01", "scored_end": "2021-01-20",
                              "window_start": "2019-01-20", "window_end": "2021-01-20",
                              "lookback_years": 2,
                              "config_sha256": ("d" if trial == "A" else "e") * 64,
                              "non_window_config_sha256": "b" * 64,
                              "runner_sha256": "c" * 64})
            log = self.root / (trial + ".log")
            log.write_text("Calculated warmup days from strategies: 0, total available days: 40\n",
                           encoding="utf-8")
            self.runs[-1].update({"warmup_days": 0, "warmup_log": log.name,
                                  "warmup_log_sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        gate_path = self.root / "C.csv"
        gate_path.write_text((self.root / "A.csv").read_text(encoding="utf-8").replace(
            "A,P,", "C,P,") + "C,P,2022-01-20T00:00:00.000000Z,110\n", encoding="utf-8")
        self.runs.append({"trial_id": "CAUSALITY_LONG", "role": "causality_only",
                          "run_id": "C", "portfolio_id": "P", "curve_csv": "C.csv",
                          "curve_sha256": hashlib.sha256(gate_path.read_bytes()).hexdigest(),
                          "window_start": "2019-01-20", "window_end": "2022-01-20",
                          "lookback_years": 3,
                          "config_sha256": "f" * 64,
                          "non_window_config_sha256": "b" * 64,
                          "runner_sha256": "c" * 64})
        self.manifest = {"issue": 113, "snapshot_utc": "2026-10-22T00:00:00Z",
                         "engine_sha": "a" * 40, "trial_ledger_complete": True,
                         "portfolio_id": "P",
                         "window_protocol": "frozen_end_date_plus_lookback_years",
                         "analysis": {"periods_per_year": 252, "confidence": 0.95,
                                      "resamples": 100, "seed": 42,
                                      "block_lengths": [1, 2, 4], "cscv_groups": 10},
                         "runs": self.runs}
        self.gate = {"status": "pass", "short_run": "A", "long_run": "C",
                     "portfolio_id": "P", "end_date": "2021-01-20",
                     "matched_rows": 41, "first_difference": None,
                     "short_sha256": self.runs[0]["curve_sha256"],
                     "long_sha256": self.runs[2]["curve_sha256"]}

    def test_matrix_has_real_shared_dates_and_hashes(self):
        dates, matrix, missing = load_matrix(self.manifest, self.root)
        self.assertEqual(len(dates), 40)
        self.assertEqual(sorted(matrix), ["A", "B"])
        self.assertEqual(missing, [])

    def test_changed_export_fails_checksum(self):
        path = self.root / "B.csv"
        path.write_text(path.read_text(encoding="utf-8") + "\n", encoding="utf-8")
        with self.assertRaisesRegex(WorkflowError, "checksum"):
            load_matrix(self.manifest, self.root)

    def test_missing_date_fails_without_silent_intersection(self):
        path = self.root / "B.csv"
        lines = path.read_text(encoding="utf-8").splitlines()
        path.write_text("\n".join(lines[:3] + lines[4:]) + "\n", encoding="utf-8")
        self.manifest["runs"][1]["curve_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        with self.assertRaisesRegex(WorkflowError, "date index"):
            load_matrix(self.manifest, self.root)

    def test_other_portfolio_is_not_a_conservative_trial(self):
        self.runs[1]["portfolio_id"] = "OTHER"
        path = self.root / "B.csv"
        path.write_text(path.read_text(encoding="utf-8").replace("B,P,", "B,OTHER,"),
                        encoding="utf-8")
        self.runs[1]["curve_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        with self.assertRaisesRegex(WorkflowError, "portfolio"):
            analyze(self.manifest, self.root, self.gate, fixed_trial="A")

    def test_string_false_cannot_unlock_dsr(self):
        self.manifest["trial_ledger_complete"] = "false"
        with self.assertRaisesRegex(WorkflowError, "trial_ledger_complete"):
            analyze(self.manifest, self.root, self.gate, fixed_trial="A")

    def test_late_self_declared_warmup_base_is_rejected_by_runner_count(self):
        for run in self.runs[:2]:
            run["warmup_last_timestamp"] = "2020-12-05T00:00:00.000000Z"
            run["scored_start"] = "2020-12-06"
        with self.assertRaisesRegex(WorkflowError, "warm-up"):
            load_matrix(self.manifest, self.root)

    def test_analysis_refuses_failed_causality(self):
        self.gate["status"] = "fail"
        with self.assertRaisesRegex(WorkflowError, "causality"):
            analyze(self.manifest, self.root, self.gate, fixed_trial="A")

    def test_analysis_recomputes_gate_instead_of_trusting_status_field(self):
        self.gate["long_run"] = "B"
        self.gate["long_sha256"] = self.runs[1]["curve_sha256"]
        with self.assertRaisesRegex(WorkflowError, "causality"):
            analyze(self.manifest, self.root, self.gate, fixed_trial="A")

    def test_analysis_rejects_non_matching_window_starts(self):
        self.runs[2]["window_start"] = "2019-01-21"
        with self.assertRaisesRegex(WorkflowError, "window"):
            analyze(self.manifest, self.root, self.gate, fixed_trial="A")

    def test_analysis_rejects_same_run_on_both_sides_of_gate(self):
        self.gate["long_run"] = "A"
        self.gate["long_sha256"] = self.gate["short_sha256"]
        with self.assertRaisesRegex(WorkflowError, "causality"):
            analyze(self.manifest, self.root, self.gate, fixed_trial="A")

    def test_analysis_rejects_changed_non_window_config(self):
        self.runs[2]["non_window_config_sha256"] = "d" * 64
        with self.assertRaisesRegex(WorkflowError, "config"):
            analyze(self.manifest, self.root, self.gate, fixed_trial="A")

    def test_analysis_rejects_truncated_long_run(self):
        path = self.root / "C.csv"
        lines = path.read_text(encoding="utf-8").splitlines()
        lines[-1] = "C,P,2021-01-21T00:00:00.000000Z,110"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        new_hash = hashlib.sha256(path.read_bytes()).hexdigest()
        self.runs[2]["curve_sha256"] = self.gate["long_sha256"] = new_hash
        with self.assertRaisesRegex(WorkflowError, "coverage"):
            analyze(self.manifest, self.root, self.gate, fixed_trial="A")

    def test_duplicate_run_id_with_distinct_config_and_export_is_valid(self):
        self.runs[1]["run_id"] = "A"
        path = self.root / "B.csv"
        path.write_text(path.read_text(encoding="utf-8").replace("B,P,", "A,P,"),
                        encoding="utf-8")
        self.runs[1]["curve_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        dates, matrix, missing = load_matrix(self.manifest, self.root)
        self.assertEqual(len(dates), 40)
        self.assertEqual(sorted(matrix), ["A", "B"])
        self.assertEqual(missing, [])
        # The gate still selects the fixed export by hash, despite the shared run ID.
        self.assertEqual(analyze(self.manifest, self.root, self.gate, "A")["scored_rows"], 40)

    def test_same_config_cannot_be_counted_as_two_trials(self):
        self.runs[1]["config_sha256"] = self.runs[0]["config_sha256"]
        with self.assertRaisesRegex(WorkflowError, "duplicate config"):
            load_matrix(self.manifest, self.root)

    def test_analysis_marks_incomplete_ledger_without_dsr(self):
        self.manifest["trial_ledger_complete"] = False
        result = analyze(self.manifest, self.root, self.gate, fixed_trial="A")
        self.assertIsNone(result["selection_risk"]["dsr"])
        self.assertEqual(result["selection_risk"]["trial_count"], 2)
        self.assertIn("2020", result["calendar_years"])
        self.assertIn("2021", result["calendar_years"])

    def test_historical_trial_without_export_is_counted(self):
        self.manifest["runs"].append({"trial_id": "OLD_SWEEP",
                                      "role": "trial", "portfolio_id": "P",
                                      "missing_reason": "historical output was not archived"})
        self.manifest["trial_ledger_complete"] = False
        result = analyze(self.manifest, self.root, self.gate, fixed_trial="A")
        self.assertEqual(result["selection_risk"]["trial_count"], 3)
        self.assertIsNone(result["selection_risk"]["dsr"])
        self.assertEqual(result["selection_risk"]["pbo"]["scope"],
                         "two_trial_demonstration")

    def test_offline_selection_uses_prior_year_only(self):
        dates = [dt.date(2020, 1, 1), dt.date(2020, 1, 2),
                 dt.date(2021, 1, 1), dt.date(2021, 1, 2)]
        matrix = {"A": [0.04, 0.02, -0.03, -0.02],
                  "B": [-0.03, -0.02, 0.04, 0.02]}
        result = offline_selection(dates, matrix, "A")
        self.assertEqual(result[0]["selected_trial"], "A")
        self.assertEqual(result[0]["test_year"], 2021)
        self.assertLess(result[0]["selected_total_return"], 0)

    def test_cli_analysis_uses_manifest_and_archived_gate(self):
        manifest_path = self.root / "manifest.json"
        gate_path = self.root / "gate.json"
        manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")
        gate_path.write_text(json.dumps(self.gate), encoding="utf-8")
        proc = subprocess.run(
            [sys.executable, "-m", "scripts.backtest_validation", "analyze",
             "--manifest", str(manifest_path), "--gate", str(gate_path),
             "--fixed-trial", "A"], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        report = json.loads(proc.stdout)
        self.assertEqual(report["scored_rows"], 40)
        self.assertEqual(report["selection_risk"]["pbo"]["combinations"], 252)
        self.assertEqual(report["selection_risk"]["pbo"]["scope"],
                         "two_trial_demonstration")
        self.assertEqual(report["selection_risk"]["dsr_scope"],
                         "two_trial_demonstration")


if __name__ == "__main__":
    unittest.main()
