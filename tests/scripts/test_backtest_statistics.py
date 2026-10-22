"""Known-answer and planted-overfit checks for the offline #113 analysis."""

import math
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.backtest_validation.statistics import (  # noqa: E402
    analysis_summary,
    bootstrap_interval,
    cscv_pbo,
    deflated_sharpe,
    drawdown,
    moving_block_sample,
    probabilistic_sharpe,
    sharpe,
)


class StatisticsTest(unittest.TestCase):
    def test_compounded_return_and_drawdown_known_path(self):
        result = analysis_summary([0.1, -0.1], periods_per_year=2)
        self.assertAlmostEqual(result["annual_return"], -0.01)
        self.assertAlmostEqual(result["max_drawdown"], 0.1)
        self.assertAlmostEqual(result["sharpe"], 0.0)

    def test_undefined_sharpe_is_explicit(self):
        self.assertIsNone(sharpe([0.01]))
        self.assertIsNone(sharpe([0.01] * 20))
        with self.assertRaises(ValueError):
            probabilistic_sharpe([0.01] * 20, 0.0)

    def test_psr_zero_benchmark_and_single_trial_dsr(self):
        returns = [0.01 + 0.004 * math.sin(i * 1.7) for i in range(200)]
        psr = probabilistic_sharpe(returns, 0.0)
        self.assertGreater(psr, 0.95)
        self.assertAlmostEqual(deflated_sharpe(returns, [sharpe(returns)]), psr)

    def test_more_raw_trials_raise_dsr_hurdle(self):
        returns = [0.003 + 0.01 * math.sin(i * 1.7) for i in range(200)]
        dsr_two = deflated_sharpe(returns, [0.0, 0.02])
        dsr_many = deflated_sharpe(returns, [0.0, 0.02] * 20)
        self.assertLess(dsr_many, dsr_two)

    def test_block_sampler_reproducible_and_contiguous(self):
        import random
        values = list(range(8))
        a = moving_block_sample(values, 3, random.Random(42))
        b = moving_block_sample(values, 3, random.Random(42))
        self.assertEqual(a, b)
        self.assertEqual(len(a), len(values))
        for start in range(0, 6, 3):
            self.assertEqual(a[start + 1], (a[start] + 1) % 8)
            self.assertEqual(a[start + 2], (a[start] + 2) % 8)

    def test_bootstrap_interval_is_seeded(self):
        values = [0.01, -0.02, 0.03, -0.01] * 20
        a = bootstrap_interval(values, drawdown, block_length=4, resamples=200, seed=42)
        b = bootstrap_interval(values, drawdown, block_length=4, resamples=200, seed=42)
        self.assertEqual(a, b)
        self.assertLessEqual(a[0], a[1])

    def test_cscv_detects_planted_selection_reversal(self):
        # Each selected in-sample winner becomes the lower-ranked OOS variant.
        a = [sign * x for sign in ([1] * 5 + [-1] * 5)
             for x in (0.01, 0.012)]
        b = [-x for x in a]
        result = cscv_pbo({"A": a, "B": b}, groups=10)
        self.assertEqual(result["combinations"], 252)
        self.assertGreater(result["pbo"], 0.8)

    def test_identical_variants_have_tied_oos_rank(self):
        a = [0.01, -0.005, 0.002, 0.003] * 10
        result = cscv_pbo({"A": a, "B": a}, groups=10)
        self.assertEqual(result["pbo"], 0.0)
        self.assertEqual(result["ties"], 252)


if __name__ == "__main__":
    unittest.main()
