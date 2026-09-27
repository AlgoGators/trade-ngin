"""A build target must package its own actual binary, engine and metadata."""
import copy
import unittest
from test_qt_evaluator_bundle import QtEvaluatorBundleContract, CONTRACT


class BuiltBundleTest(unittest.TestCase):
    setUp = QtEvaluatorBundleContract.setUp

    def stage(self, inputs=None):
        return self.tool.stage_built_bundle(CONTRACT,
            inputs or {'artifacts': self.artifacts, 'metadata': self.metadata},
            self.destination, self.sources / 'qt_evaluator', self.sources / 'libtrade_ngin.so',
            self.metadata['evaluator_build'], self.metadata['compiler']['id'],
            self.metadata['compiler']['version'])

    def test_target_stages_closed_actual_target_artifacts(self):
        result = self.stage()
        self.assertEqual(self.tool.verify_bundle(self.destination, result['bundle_sha256']), result)

    def test_another_binary_engine_or_build_cannot_be_packaged_as_this_target(self):
        for replacement in ['executable', 'engine', 'evaluator_build', 'compiler']:
            with self.subTest(replacement=replacement):
                inputs = copy.deepcopy({'artifacts': self.artifacts, 'metadata': self.metadata})
                if replacement in {'executable', 'engine'}:
                    row = next(row for row in inputs['artifacts'] if row['role'] == replacement)
                    substituted = self.sources / ('other-' + row['name'])
                    substituted.write_bytes((self.sources / row['name']).read_bytes())
                    row['source'] = str(substituted)
                elif replacement == 'evaluator_build':
                    inputs['metadata']['evaluator_build'] = 'other-build'
                else:
                    inputs['metadata']['compiler']['version'] = 'other-compiler'
                with self.assertRaisesRegex(ValueError, 'bundle_build_target_mismatch'):
                    self.stage(inputs)
                self.assertFalse(self.destination.exists())


if __name__ == '__main__': unittest.main()
