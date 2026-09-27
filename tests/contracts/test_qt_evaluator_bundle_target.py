"""Exercise the actual explicit CMake packaging target with owned artifacts."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import unittest
from qt_native_bundle_fixture import BUILD, actual_bundle_inputs


class ActualBundleTarget(unittest.TestCase):
    def test_actual_target_stages_its_complete_inspected_closure(self):
        inventory = BUILD / 'qt-evaluator-bundle-inputs.json'
        destination = BUILD / 'qt-evaluator-bundle'
        cache = (BUILD / 'CMakeCache.txt').read_text()
        for name, wanted in [('QT_EVALUATOR_BUNDLE_INPUTS', inventory),
                             ('QT_EVALUATOR_BUNDLE_DIRECTORY', destination)]:
            found = re.search(r'^' + name + r':(?:FILEPATH|PATH)=(.*)$', cache, re.M)
            self.assertTrue(found is None or found.group(1) == str(wanted),
                            'Refusing to change an existing bundle target configuration')
        self.assertFalse(inventory.exists() or inventory.is_symlink())
        self.assertFalse(destination.exists() or destination.is_symlink())
        self.assertEqual(inventory.parent.resolve(), BUILD.resolve())
        self.assertEqual(destination.parent.resolve(), BUILD.resolve())
        try:
            with inventory.open('x') as stream:
                json.dump(actual_bundle_inputs(), stream)
            completed = subprocess.run(['cmake', '--build', str(BUILD), '--target',
                'qt_evaluator_bundle', '-j2'], capture_output=True, text=True, timeout=600)
            print(completed.stdout)
            print(completed.stderr)
            self.assertEqual(completed.returncode, 0)
            manifest = json.loads((destination / 'qt_evaluator_manifest.json').read_text())
            self.assertEqual(manifest['schema'], 'qt-evaluator-bundle/v1')
            self.assertEqual(manifest['evaluator_build'], 'local-qt-controlled')
            self.assertTrue({'executable', 'engine', 'loader', 'dependency'} <=
                            {row['role'] for row in manifest['artifacts']})
            print('CMAKE_BUNDLE_MANIFEST=' + json.dumps(manifest, sort_keys=True, separators=(',', ':')))
        finally:
            if inventory.is_file() and not inventory.is_symlink(): inventory.unlink()
            if destination.is_dir() and not destination.is_symlink(): shutil.rmtree(destination)
        self.assertFalse(inventory.exists() or destination.exists())


if __name__ == '__main__': unittest.main()
