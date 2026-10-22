"""Config provenance retains strategy/cost equality across window changes."""

import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.backtest_validation.fingerprint import fingerprints  # noqa: E402


class FingerprintTest(unittest.TestCase):
    def test_window_and_connection_changes_only_affect_full_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "defaults.json").write_text(json.dumps({
                "database": {"name": "scratch_a", "password": "secret"},
                "backtest": {"frozen_end_date": "2020-01-01", "lookback_years": 2},
                "execution": {"slippage_bps": 5}}), encoding="utf-8")
            first = fingerprints(root)
            (root / "defaults.json").write_text(json.dumps({
                "database": {"name": "scratch_b", "password": "other"},
                "backtest": {"frozen_end_date": "2021-01-01", "lookback_years": 3},
                "execution": {"slippage_bps": 5}}), encoding="utf-8")
            second = fingerprints(root)
            self.assertNotEqual(first["config_sha256"], second["config_sha256"])
            self.assertEqual(first["non_window_config_sha256"],
                             second["non_window_config_sha256"])
            text = json.dumps(second)
            self.assertNotIn("other", text)
            (root / "defaults.json").write_text(json.dumps({
                "database": {"name": "scratch_b", "password": "other"},
                "backtest": {"frozen_end_date": "2021-01-01", "lookback_years": 3},
                "execution": {"slippage_bps": 10}}), encoding="utf-8")
            self.assertNotEqual(second["non_window_config_sha256"],
                                fingerprints(root)["non_window_config_sha256"])


if __name__ == "__main__":
    unittest.main()
