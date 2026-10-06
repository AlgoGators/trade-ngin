"""Exercise the actual offline evaluator process against synthetic snapshots."""
import json
from datetime import date, timedelta
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.qt_test_artifacts import artifact, build_identity

EXECUTABLE = artifact("qt_evaluator")
FIXTURE = Path(__file__).with_name('qt-eval-v1.json')


class QtEvaluatorCliContract(unittest.TestCase):
    def fixture_request(self, name):
        request = json.loads(FIXTURE.read_text())[name]
        if request.get('evaluator_build') == 'local-qt-controlled':
            request['evaluator_build'] = build_identity()
        return request

    def request(self):
        return self.fixture_request('selected_book')

    def test_fixture_is_bound_to_the_compiled_reviewed_build(self):
        self.assertEqual(self.request()['evaluator_build'], build_identity())

    def invoke(self, data):
        self.assertTrue(EXECUTABLE.is_file(), 'Actual qt_evaluator executable must be built')
        raw = data if isinstance(data, str) else json.dumps(data, separators=(',', ':'))
        with tempfile.TemporaryDirectory(prefix='qt-cli-contract-') as directory:
            result = subprocess.run([str(EXECUTABLE)], input=raw, text=True,
                                    capture_output=True, timeout=15, cwd=directory,
                                    env={'PATH': '/usr/local/bin:/usr/bin:/bin', 'TZ': 'UTC'})
            self.assertEqual(result.stderr, '', 'The structured CLI must keep stderr empty')
            self.assertEqual(list(Path(directory).iterdir()), [], 'The evaluator must not create log files')
            return result

    def test_selected_five_one_reaches_actual_response_without_optimizer(self):
        result = self.invoke(self.request())
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout)
        self.assertEqual(value['completeness'], 'complete')
        self.assertEqual([row['quantity_exact'] for row in value['evaluated_book']], ['5', '1'])
        self.assertEqual(value['evaluated_book_digest'], '170528e9e3a72c56773ceeba05ed1762eb4a35ff1a35d0af8633e35039ee99df')
        self.assertEqual(value['optimizer']['status'], 'disabled')
        self.assertEqual(value['selected_costs']['total_exact'], '0.02')
        self.assertEqual(value['selected_risk']['evaluated_book_digest'], value['evaluated_book_digest'])
        self.assertEqual(value['selected_costs']['evaluated_book_digest'], value['evaluated_book_digest'])

    def test_risk_diagnostics_carry_units_and_actual_snapshot_source(self):
        result = self.invoke(self.request())
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout)
        for metric in value['selected_risk']['metrics']:
            self.assertIn('unit', metric)
            self.assertEqual(metric['unit'], 'ratio')
            self.assertEqual(metric['source_id'], 'synthetic-market-v1')

    def test_fractional_equity_survives_actual_cli(self):
        request = self.request()
        request['proposal']['quantities'][0]['quantity_exact'] = '5.125'
        for row in request['component_cost_inputs']:
            row['calculation_increment_exact'] = '0.00000001'
            row['cash_cost_per_increment_exact'] = '0.00000001'
        result = self.invoke(request)
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout)
        self.assertEqual(value['evaluated_book'][0]['quantity_exact'], '5.125')
        self.assertEqual(value['selected_costs']['total_exact'], '2.125')

    def test_missing_history_is_not_a_pass_or_an_override(self):
        request = self.request()
        request['risk_inputs']['closes'] = []
        result = self.invoke(request)
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout)
        self.assertNotEqual(value['completeness'], 'complete')
        self.assertEqual(value['selected_risk']['status'], 'unavailable')
        self.assertIsNone(value['selected_risk']['passed'])

    def test_invalid_duplicate_trailing_build_and_oversized_inputs_fail(self):
        bad_build = self.request()
        bad_build['evaluator_build'] = 'not-this-binary'
        for data in ['', '{"schema":"qt-eval/v1","schema":"qt-eval/v1"}',
                     json.dumps(self.request()) + '{}', bad_build, ' ' * (8 * 1024 * 1024 + 1)]:
            with self.subTest(kind=type(data).__name__, length=len(data)):
                result = self.invoke(data)
                self.assertNotEqual(result.returncode, 0)
                value = json.loads(result.stdout)
                self.assertIn('error', value)
                self.assertNotIn('completeness', value)

    def test_diagnostic_advice_does_not_replace_the_evaluated_candidate(self):
        request = self.fixture_request('draft_diagnostic')
        # The optimizer needs 21 observations. The parser fixture has only
        # three, so supply a complete declared synthetic history for this
        # real-kernel positive case, consistently for both evaluation stages.
        stamps = [(date(2026, 9, 5) + timedelta(days=index)).isoformat() + 'T12:00:00Z'
                  for index in range(21)]
        closes = [{'instrument': {'instrument_type': 'EQUITY', 'symbol': 'SYN'},
                   'timestamp': stamp, 'close': '100' if index % 2 == 0 else '110'}
                  for index, stamp in enumerate(stamps)]
        for inputs in (request['risk_inputs'], request['optimizer_inputs']):
            inputs['expected_observation_times'] = stamps
            inputs['closes'] = closes
        result = self.invoke(request)
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout)
        self.assertEqual(value['evaluated_book'][0]['quantity_exact'], '5')
        self.assertEqual(value['optimizer']['status'], 'evaluated', value['diagnostics'])
        self.assertEqual(value['optimizer']['evaluated_book_digest'], value['evaluated_book_digest'])
        metrics = {row['code']: row['value_diagnostic'] for row in value['selected_risk']['metrics']}
        breach = next(row for row in value['selected_risk']['breaches'] if row['code'] == 'portfolio_var')
        self.assertEqual(breach['actual_diagnostic'], metrics['portfolio_var_gate'])
        self.assertGreater(float(breach['actual_diagnostic']), float(breach['limit_diagnostic']))


if __name__ == '__main__':
    unittest.main(verbosity=2)
