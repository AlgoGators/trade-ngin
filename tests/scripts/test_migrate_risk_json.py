#!/usr/bin/env python3
"""scripts/migrate_risk_json.py, driven end to end.

Its own ctest entry rather than a case inside the gtest binary (T-6a lead ruling 16), so a
machine without python3 fails this entry loudly instead of silently skipping the test of
the one thing on the migration path that is not C++.

The migration is run once, by hand, on a production box, against files nobody reads again
afterwards. A script that quietly gets a value wrong there leaves a book gating on the
wrong number for as long as nobody notices. So the cases below check what the operator
cannot: that a dry run writes nothing, that a second run is a no-op, that the rollback copy
is never overwritten, that the printed diff never carries the database password, that every
schema-1 value -- including the ones that resolved through risk_defaults and appear in no
book's own file -- lands literally in the book's risk.json, and that the three things the
script must refuse are refused rather than guessed.

The complementary check lives in C++: TrackedTemplateResolvedRiskConfig.* loads the tracked
config_template/, which this script migrated, through ConfigLoader::load and asserts the
resolved values per book. That is the loader judging the script's real output.

Run directly (python3 tests/scripts/test_migrate_risk_json.py) or through ctest.
"""

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(REPO_ROOT, "scripts", "migrate_risk_json.py")


def import_script():
    spec = importlib.util.spec_from_file_location("migrate_risk_json", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

# The database password the script must never echo, not even as diff context.
PASSWORD_SENTINEL = "s3cr3t-must-never-be-printed"


def schema1_defaults():
    return {
        "database": {"host": "h", "port": "5432", "username": "u",
                     "password": PASSWORD_SENTINEL, "name": "n", "num_connections": 5},
        "execution": {"commission_rate": 0.0005, "slippage_bps": 1.0,
                      "position_limit_backtest": 1000.0, "position_limit_live": 500.0},
        "optimization": {"tau": 1.0, "cost_penalty_scalar": 50.0,
                         "asymmetric_risk_buffer": 0.1, "max_iterations": 100,
                         "convergence_threshold": 1e-06, "use_buffering": True,
                         "buffer_size_factor": 0.01},
        "backtest": {"lookback_years": 2, "store_trade_details": True},
        "live": {"historical_days": 730},
        "strategy_defaults": {"max_strategy_allocation": 1.0, "min_strategy_allocation": 0.1,
                              "use_optimization": True, "use_risk_management": True,
                              "carver_buffer_floor": 0.5},
        # The trap: BASE and EQUITY_MR carry no max_correlation of their own and resolve
        # 0.7 from here, which is indistinguishable from RiskConfig's struct default.
        "risk_defaults": {"confidence_level": 0.99, "lookback_period": 252,
                          "max_correlation": 0.7},
    }


EMAIL = {"smtp_host": "smtp.test.com", "smtp_port": 587, "username": "u", "password": "p",
         "from_email": "f@test.com", "to_emails": ["a@test.com"]}


def strategies(sid, stype):
    return {sid: {"enabled_backtest": True, "enabled_live": True, "default_allocation": 1.0,
                  "type": stype, "config": {"weight": 0.03, "risk_target": 0.2}}}


# The three shipped books as they were at schema 1, values included, so this file carries
# the "before" as well as the "after".
BOOKS = {
    "conservative": {
        "portfolio": {"portfolio_id": "CONSERVATIVE_PORTFOLIO", "initial_capital": 500000.0,
                      "reserve_capital_pct": 0.10,
                      "strategies": strategies("TREND_FOLLOWING", "TrendFollowingStrategy")},
        "risk": {"_description": "Risk configuration for CONSERVATIVE_PORTFOLIO",
                 "var_limit": 0.25, "jump_risk_limit": 0.05, "max_correlation": 0.85,
                 "max_gross_leverage": 4.0, "max_net_leverage": 2.0,
                 "max_drawdown": 0.3, "max_leverage": 2.0},
        "expected": {"var_limit": 0.25, "jump_risk_limit": 0.05, "max_correlation": 0.85,
                     "max_gross_leverage": 4.0, "max_net_leverage": 2.0,
                     "confidence_level": 0.99, "lookback_period": 252},
        "use_optimization": True,
    },
    "base": {
        "portfolio": {"portfolio_id": "BASE_PORTFOLIO", "initial_capital": 500000.0,
                      "reserve_capital_pct": 0.10,
                      "strategies": strategies("TREND_FOLLOWING", "TrendFollowingStrategy")},
        "risk": {"var_limit": 0.15, "jump_risk_limit": 0.10,
                 "max_gross_leverage": 4.0, "max_net_leverage": 2.0,
                 "max_drawdown": 0.4, "max_leverage": 4.0},
        "expected": {"var_limit": 0.15, "jump_risk_limit": 0.10, "max_correlation": 0.7,
                     "max_gross_leverage": 4.0, "max_net_leverage": 2.0,
                     "confidence_level": 0.99, "lookback_period": 252},
        "use_optimization": True,
    },
    "equity_mr": {
        "portfolio": {"portfolio_id": "EQUITY_MR_PORTFOLIO", "initial_capital": 100000.0,
                      "reserve_capital_pct": 0.10,
                      "strategies": strategies("MEAN_REVERSION", "MeanReversionStrategy")},
        "risk": {"var_limit": 0.25, "jump_risk_limit": 0.08,
                 "max_gross_leverage": 1, "max_net_leverage": 1,
                 "max_drawdown": 0.3, "max_leverage": 1},
        "expected": {"var_limit": 0.25, "jump_risk_limit": 0.08, "max_correlation": 0.7,
                     "max_gross_leverage": 1, "max_net_leverage": 1,
                     "confidence_level": 0.99, "lookback_period": 252},
        # All-equity book: the runners hard-code the optimizer off, so the migration
        # writes false rather than the resolved true.
        "use_optimization": False,
    },
}


def write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(json.dumps(obj, indent=2) + "\n")


def read_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def snapshot(root):
    files = {}
    for dirpath, _, names in os.walk(root):
        for name in names:
            path = os.path.join(dirpath, name)
            with open(path, "r", encoding="utf-8") as handle:
                files[os.path.relpath(path, root)] = handle.read()
    return files


class MigrateRiskJsonTest(unittest.TestCase):
    def setUp(self):
        self.assertTrue(os.path.isfile(SCRIPT), "migrate_risk_json.py not found at " + SCRIPT)
        self.tmp = tempfile.mkdtemp(prefix="trade_ngin_migration_")
        self.config = os.path.join(self.tmp, "config")
        write_json(os.path.join(self.config, "defaults.json"), schema1_defaults())
        for name, book in BOOKS.items():
            base = os.path.join(self.config, "portfolios", name)
            write_json(os.path.join(base, "portfolio.json"), book["portfolio"])
            write_json(os.path.join(base, "risk.json"), book["risk"])
            write_json(os.path.join(base, "email.json"), EMAIL)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_script(self, *args):
        done = subprocess.run([sys.executable, SCRIPT] + list(args),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True)
        return done.returncode, done.stdout

    # ----- the dry run -----

    def test_dry_run_exits_ten_and_changes_no_byte(self):
        before = snapshot(self.config)
        code, out = self.run_script(self.config)
        self.assertEqual(code, 10, out)
        self.assertIn("Dry run", out)
        self.assertEqual(snapshot(self.config), before, "a dry run must not write")

    def test_the_diff_never_prints_the_password(self):
        code, out = self.run_script(self.config)
        self.assertEqual(code, 10, out)
        self.assertNotIn(PASSWORD_SENTINEL, out,
                         "the migration diff printed the database password as context")

    def test_a_password_line_inside_a_diff_hunk_is_redacted(self):
        # The end-to-end case above only proves the password was far enough from a change
        # to stay out of the hunk. This proves the redaction itself: the deployed
        # defaults.json is 4-space indented, so a migration re-indents it and every line,
        # password included, becomes diff context.
        redact = import_script().redact
        line = '  "password": "%s",' % PASSWORD_SENTINEL
        self.assertEqual(redact(line), '  "password": "<redacted>",')
        self.assertNotIn(PASSWORD_SENTINEL, redact("+" + line))
        self.assertEqual(redact('  "name": "prod_db",'), '  "name": "prod_db",')

    def test_the_dry_run_names_the_source_of_every_resolved_value(self):
        code, out = self.run_script(self.config)
        self.assertEqual(code, 10, out)
        # The two books whose max_correlation came from the deleted global have to say so:
        # a migration that moves a value without naming where it came from is unreviewable.
        self.assertIn("base: max_correlation      = 0.7       <- risk_defaults", out)
        self.assertIn("equity_mr: max_correlation      = 0.7       <- risk_defaults", out)
        self.assertIn("conservative: max_correlation      = 0.85      <- conservative/risk.json",
                      out)

    # ----- writing -----

    def test_in_place_writes_every_schema1_value_literally_per_book(self):
        code, out = self.run_script(self.config, "--in-place")
        self.assertEqual(code, 0, out)
        for name, book in BOOKS.items():
            risk = read_json(os.path.join(self.config, "portfolios", name, "risk.json"))
            self.assertEqual(risk["schema"], 2, name)
            self.assertEqual(len(risk["modules"]), 1, name)
            module = risk["modules"][0]
            self.assertEqual(module["type"], "carver", name)
            for field, value in book["expected"].items():
                self.assertEqual(module[field], value, "%s: modules[0].%s" % (name, field))
                self.assertEqual(risk["risk_reporting"][field], value,
                                 "%s: risk_reporting.%s must mirror the gate" % (name, field))
            self.assertEqual(module["lookback_unit"], "dates", name)
            self.assertEqual(module["min_gate_dates"], 21, name)
            self.assertEqual(module["missing_symbol_policy"], "ignore", name)
            self.assertTrue(module["_missing_symbol_policy_reason"],
                            "%s: the fail-open policy must say why" % name)
            self.assertEqual(risk["max_drawdown"], book["risk"]["max_drawdown"], name)
            self.assertEqual(risk["max_leverage"], book["risk"]["max_leverage"], name)

            portfolio = read_json(os.path.join(self.config, "portfolios", name,
                                               "portfolio.json"))
            self.assertEqual(portfolio["use_optimization"], book["use_optimization"], name)

        defaults = read_json(os.path.join(self.config, "defaults.json"))
        self.assertNotIn("risk_defaults", defaults)
        self.assertNotIn("use_optimization", defaults["strategy_defaults"])
        self.assertNotIn("use_risk_management", defaults["strategy_defaults"])
        # Everything else in defaults.json is untouched, down to the number literals.
        original = schema1_defaults()
        for key in ("database", "execution", "optimization", "backtest", "live"):
            self.assertEqual(defaults[key], original[key], key)

    def test_the_rollback_copies_hold_the_schema1_files(self):
        code, out = self.run_script(self.config, "--in-place")
        self.assertEqual(code, 0, out)
        bak = read_json(os.path.join(self.config, "portfolios", "base",
                                     "risk.json.schema1.bak"))
        self.assertEqual(bak, BOOKS["base"]["risk"])

    def test_numbers_keep_the_text_the_file_spelled_them_with(self):
        # Written as text, not through json.dumps: 0.10 and 4.0 are exactly how the
        # deployed files spell them, and a migration that re-renders them to 0.1 and 4
        # produces a diff in which the line that actually moved is invisible.
        path = os.path.join(self.config, "portfolios", "base", "risk.json")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write('{\n  "var_limit": 0.15,\n  "jump_risk_limit": 0.10,\n'
                         '  "max_gross_leverage": 4.0,\n  "max_net_leverage": 2.0,\n'
                         '  "max_drawdown": 0.40,\n  "max_leverage": 4.0\n}\n')
        code, out = self.run_script(self.config, "--in-place")
        self.assertEqual(code, 0, out)
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn('"jump_risk_limit": 0.10', text)
        self.assertIn('"max_drawdown": 0.40', text)
        with open(os.path.join(self.config, "portfolios", "equity_mr", "risk.json"),
                  encoding="utf-8") as handle:
            equity = handle.read()
        self.assertIn('"max_gross_leverage": 1,', equity)

    def test_a_second_in_place_run_writes_nothing_and_exits_zero(self):
        self.assertEqual(self.run_script(self.config, "--in-place")[0], 0)
        after_first = snapshot(self.config)
        code, out = self.run_script(self.config, "--in-place")
        self.assertEqual(code, 0, out)
        self.assertIn("already schema 2", out)
        self.assertEqual(snapshot(self.config), after_first, "a second run must be a no-op")

    def test_refuses_to_overwrite_an_existing_rollback_copy(self):
        self.assertEqual(self.run_script(self.config, "--in-place")[0], 0)
        # Put the schema-1 files back without removing their .bak copies: the next run
        # would destroy the only copy of the originals.
        for relative in ("defaults.json", os.path.join("portfolios", "base", "risk.json")):
            path = os.path.join(self.config, relative)
            shutil.copy(path + ".schema1.bak", path)
        code, out = self.run_script(self.config, "--in-place")
        self.assertEqual(code, 2, out)
        self.assertIn("rollback copy", out)

    def test_out_writes_a_copy_and_leaves_the_source_alone(self):
        before = snapshot(self.config)
        out_dir = os.path.join(self.tmp, "migrated")
        code, out = self.run_script(self.config, "--out", out_dir)
        self.assertEqual(code, 0, out)
        self.assertEqual(snapshot(self.config), before, "--out must not touch the source")
        risk = read_json(os.path.join(out_dir, "portfolios", "conservative", "risk.json"))
        self.assertEqual(risk["modules"][0]["var_limit"], 0.25)

    def test_refuses_a_non_empty_out_directory(self):
        out_dir = os.path.join(self.tmp, "out")
        os.makedirs(out_dir)
        with open(os.path.join(out_dir, "something.txt"), "w", encoding="utf-8") as handle:
            handle.write("already here")
        code, out = self.run_script(self.config, "--out", out_dir)
        self.assertEqual(code, 2, out)
        self.assertIn("not empty", out)

    # ----- the three refusals -----

    def test_refuses_use_risk_management_false_rather_than_inventing_a_ruling(self):
        defaults = schema1_defaults()
        defaults["strategy_defaults"]["use_risk_management"] = False
        write_json(os.path.join(self.config, "defaults.json"), defaults)
        code, out = self.run_script(self.config)
        self.assertEqual(code, 3, out)
        self.assertIn("will not invent a ruling", out)

    def test_refuses_a_risk_json_with_no_max_leverage(self):
        path = os.path.join(self.config, "portfolios", "base", "risk.json")
        risk = read_json(path)
        del risk["max_leverage"]
        write_json(path, risk)
        code, out = self.run_script(self.config)
        self.assertEqual(code, 3, out)
        self.assertIn("will not write a default into a file", out)

    def test_refuses_a_book_that_mixes_equity_and_futures_strategies(self):
        path = os.path.join(self.config, "portfolios", "equity_mr", "portfolio.json")
        portfolio = read_json(path)
        portfolio["strategies"]["TREND_FOLLOWING"] = {
            "enabled_live": True, "type": "TrendFollowingStrategy", "default_allocation": 0.5}
        write_json(path, portfolio)
        code, out = self.run_script(self.config)
        self.assertEqual(code, 3, out)
        self.assertIn("split the book", out)

    # ----- T-6b-fix F2: a schema-2 file that still says "bars" -----

    def _migrate_then_say_bars(self):
        """A tree migrated before T-6b commit 9: schema 2, lookback_unit "bars"."""
        self.assertEqual(self.run_script(self.config, "--in-place")[0], 0)
        paths = [os.path.join(self.config, "portfolios", n, "risk.json") for n in BOOKS]
        for path in paths:
            with open(path, "r", encoding="utf-8") as handle:
                text = handle.read()
            self.assertIn('"lookback_unit": "dates"', text)
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text.replace('"lookback_unit": "dates"', '"lookback_unit": "bars"'))
        return paths

    def test_a_schema2_bars_file_is_upgraded_to_dates_and_nothing_else(self):
        paths = self._migrate_then_say_bars()
        before = {p: open(p, encoding="utf-8").read() for p in paths}
        code, out = self.run_script(self.config, "--in-place")
        self.assertEqual(code, 0, out)
        for path in paths:
            with open(path, "r", encoding="utf-8") as handle:
                after = handle.read()
            # Byte for byte: only the one value changed, every other character is the operator's.
            self.assertEqual(after, before[path].replace('"lookback_unit": "bars"',
                                                         '"lookback_unit": "dates"'), path)
            # Its rollback copy is the "bars" file, and the schema-1 rollback is untouched.
            with open(path + ".bars.bak", "r", encoding="utf-8") as handle:
                self.assertEqual(handle.read(), before[path], path)
            self.assertTrue(os.path.exists(path + ".schema1.bak"), path)
        self.assertIn('lookback_unit "bars" -> "dates"', out)

    def test_the_bars_upgrade_is_a_dry_run_first_and_idempotent_after(self):
        self._migrate_then_say_bars()
        before = snapshot(self.config)
        code, out = self.run_script(self.config)
        self.assertEqual(code, 10, out)
        self.assertEqual(snapshot(self.config), before, "a dry run changes no byte")
        self.assertIn("(lookback_unit bars)", out)
        self.assertEqual(self.run_script(self.config, "--in-place")[0], 0)
        after_first = snapshot(self.config)
        code, out = self.run_script(self.config, "--in-place")
        self.assertEqual(code, 0, out)
        self.assertIn("Nothing to migrate", out)
        self.assertEqual(snapshot(self.config), after_first)

    # ----- T-6b-fix F4: a book the template assigns `none` -----

    def test_a_carver_written_for_a_book_the_template_assigns_none_prints_a_note(self):
        # config_template/portfolios/equity_mr/risk.json is `none` (HD, 2026-09-18); the
        # script still writes a carver there and must say so, naming the ruling it will not copy.
        code, out = self.run_script(self.config)
        self.assertEqual(code, 10, out)
        notes = [line for line in out.splitlines() if "NOTE:" in line]
        self.assertEqual(len(notes), 1, out)
        self.assertIn("equity_mr", notes[0])
        self.assertIn("_ruled_by HD, _ruled_on 2026-09-18", notes[0])
        self.assertIn("config_template/README.md", notes[0])

    def test_the_note_is_repeated_on_a_migrated_file_that_still_gates(self):
        # The production case: a file migrated in T-6a (schema 2, carver, "bars") is upgraded to
        # "dates" -- and still gates a book HD ruled `none`.
        self._migrate_then_say_bars()
        code, out = self.run_script(self.config, "--in-place")
        self.assertEqual(code, 0, out)
        self.assertEqual(sum("NOTE: equity_mr" in line for line in out.splitlines()), 1, out)
        # ...and on every later run that finds it unchanged.
        code, out = self.run_script(self.config, "--in-place")
        self.assertEqual(sum("NOTE: equity_mr" in line for line in out.splitlines()), 1, out)

    def test_no_note_once_the_ruling_is_copied(self):
        self.assertEqual(self.run_script(self.config, "--in-place")[0], 0)
        path = os.path.join(self.config, "portfolios", "equity_mr", "risk.json")
        risk = read_json(path)
        risk["modules"] = [{"id": "no_portfolio_risk", "type": "none", "_reason": "ruled",
                            "_ruled_by": "HD", "_ruled_on": "2026-09-18"}]
        write_json(path, risk)
        code, out = self.run_script(self.config, "--in-place")
        self.assertEqual(code, 0, out)
        self.assertNotIn("NOTE:", out)

    def test_refuses_a_schema1_book_when_risk_defaults_is_already_gone(self):
        defaults = schema1_defaults()
        del defaults["risk_defaults"]
        write_json(os.path.join(self.config, "defaults.json"), defaults)
        code, out = self.run_script(self.config)
        self.assertEqual(code, 3, out)
        self.assertIn("cannot tell whether values came from a deleted risk_defaults", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
