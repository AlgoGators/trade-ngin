"""Issue #113: deterministic checks for exported scratch-run curves."""

import csv
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.backtest_validation.curve import (  # noqa: E402
    CurveError,
    compare_prefix,
    read_curve,
    scored_returns,
)


def csv_curve(path, run_id, values, portfolio="CONSERVATIVE_PORTFOLIO"):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("run_id", "portfolio_id", "timestamp", "equity"))
        for day, equity in values:
            writer.writerow((run_id, portfolio, day + "T00:00:00.000000Z", equity))


class CurveValidationTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.short = self.root / "short.csv"
        self.long = self.root / "long.csv"
        self.base = [("2020-01-01", "100.000000"),
                     ("2020-01-02", "100.100000"),
                     ("2020-01-03", "100.100000")]

    def load(self, path, run_id):
        return read_curve(path, run_id, "CONSERVATIVE_PORTFOLIO")

    def test_exact_prefix_passes_and_later_rows_are_ignored(self):
        csv_curve(self.short, "A", self.base)
        csv_curve(self.long, "B", self.base + [("2020-01-04", "102")])
        result = compare_prefix(self.load(self.short, "A"),
                                self.load(self.long, "B"), "2020-01-03")
        self.assertEqual(result["matched_rows"], 3)
        self.assertEqual(result["status"], "pass")

    def test_one_stored_decimal_digit_fails_without_tolerance(self):
        csv_curve(self.short, "A", self.base)
        csv_curve(self.long, "B", self.base[:1] +
                  [("2020-01-02", "100.100001")] + self.base[2:])
        result = compare_prefix(self.load(self.short, "A"),
                                self.load(self.long, "B"), "2020-01-03")
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["first_difference"]["timestamp"],
                         "2020-01-02T00:00:00.000000Z")
        self.assertEqual(result["first_difference"]["short_equity"], "100.100000")

    def test_missing_prefix_row_fails(self):
        csv_curve(self.short, "A", self.base)
        csv_curve(self.long, "B", self.base[:1] + self.base[2:])
        result = compare_prefix(self.load(self.short, "A"),
                                self.load(self.long, "B"), "2020-01-03")
        self.assertEqual(result["status"], "fail")
        self.assertIsNone(result["first_difference"]["long_equity"])

    def test_duplicate_and_invalid_rows_are_rejected(self):
        for values in (self.base + [self.base[-1]],
                       [("2020-01-01", "0")],
                       [("2020-01-01", "NaN")]):
            with self.subTest(values=values):
                csv_curve(self.short, "A", values)
                with self.assertRaises(CurveError):
                    self.load(self.short, "A")

    def test_initial_sentinel_duplicate_with_ids_is_preserved(self):
        with self.short.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(("id", "run_id", "portfolio_id", "timestamp", "equity"))
            writer.writerow((1, "A", "CONSERVATIVE_PORTFOLIO", "2020-01-01T00:00:00Z", 100))
            writer.writerow((2, "A", "CONSERVATIVE_PORTFOLIO", "2020-01-01T00:00:00Z", 100))
            writer.writerow((3, "A", "CONSERVATIVE_PORTFOLIO", "2020-01-02T00:00:00Z", 101))
        curve = self.load(self.short, "A")
        self.assertEqual(len(curve), 3)
        out = scored_returns(curve, "2020-01-01T00:00:00.000000Z",
                             "2020-01-02", "2020-01-02", warmup_last_row=2)
        self.assertAlmostEqual(out[0][1], 0.01)
        with self.assertRaises(CurveError):
            scored_returns(curve, "2020-01-01T00:00:00.000000Z",
                           "2020-01-02", "2020-01-02")

    def test_causality_detects_missing_initial_sentinel_occurrence(self):
        with self.short.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(("id", "run_id", "portfolio_id", "timestamp", "equity"))
            writer.writerow((1, "A", "CONSERVATIVE_PORTFOLIO", "2020-01-01T00:00:00Z", 100))
            writer.writerow((2, "A", "CONSERVATIVE_PORTFOLIO", "2020-01-01T00:00:00Z", 100))
        csv_curve(self.long, "B", [("2020-01-01", "100")])
        result = compare_prefix(self.load(self.short, "A"), self.load(self.long, "B"),
                                "2020-01-01")
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["first_difference"]["occurrence"], 2)

    def test_scored_returns_use_last_warmup_equity_as_base(self):
        csv_curve(self.short, "A", self.base)
        out = scored_returns(self.load(self.short, "A"),
                             "2020-01-01T00:00:00.000000Z",
                             "2020-01-02", "2020-01-03")
        self.assertEqual([round(x[1], 6) for x in out], [0.001, 0.0])

    def test_scored_returns_reject_ambiguous_base(self):
        csv_curve(self.short, "A", self.base)
        with self.assertRaises(CurveError):
            scored_returns(self.load(self.short, "A"),
                           "2020-01-01T00:00:00.000000Z",
                           "2020-01-03", "2020-01-03")

    def test_cli_exits_nonzero_on_causality_failure(self):
        csv_curve(self.short, "A", self.base)
        csv_curve(self.long, "B", self.base[:1] +
                  [("2020-01-02", "99")] + self.base[2:])
        proc = subprocess.run(
            [sys.executable, "-m", "scripts.backtest_validation", "compare",
             "--short", str(self.short), "--long", str(self.long),
             "--short-run", "A", "--long-run", "B",
             "--portfolio", "CONSERVATIVE_PORTFOLIO", "--end", "2020-01-03"],
            cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["status"], "fail")


if __name__ == "__main__":
    unittest.main()
