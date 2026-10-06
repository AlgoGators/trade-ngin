"""Pure packaging checks: never load a native library or invoke an evaluator."""
import copy
from hashlib import sha256
import importlib.util
import json
from pathlib import Path, PurePosixPath
from dataclasses import replace
from unittest.mock import patch
import tempfile
import unittest
import struct
import sys


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'apps/tools/qt_evaluator_bundle.py'
CONTRACT = ROOT / 'apps/tools/qt_evaluator_manifest.json'
API_TOOL = ROOT.parent / 'algolens-qt/algolens-api/algolens/infrastructure/portfolio/qt_evaluator_bundle.py'


def synthetic_elf(name, needed, runpath=None):
    """Minimal metadata-only ELF; it has no machine instructions and cannot run."""
    data = bytearray(2048)
    data[:8] = b'\x7fELF\x02\x01\x01\x00'
    struct.pack_into('<HH', data, 16, 3, 62)
    struct.pack_into('<Q', data, 32, 64)
    executable = name == 'qt_evaluator'
    struct.pack_into('<HH', data, 54, 56, 3 if executable else 2)
    strings = bytearray(b'\0')
    tags = []

    def string(tag, value):
        tags.append((tag, len(strings)))
        strings.extend(value.encode('ascii') + b'\0')

    for value in needed:
        string(1, value)
    if not executable:
        string(14, name)
    if runpath:
        string(29, runpath)
    tags.extend([(5, 0x400200), (10, len(strings)), (0, 0)])
    struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0x400000, 0, len(data), len(data), 4096)
    struct.pack_into('<IIQQQQQQ', data, 120, 2, 4, 1024, 0x400400, 0, len(tags)*16, len(tags)*16, 8)
    if executable:
        interpreter = b'/lib64/ld-linux-x86-64.so.2\0'
        data[384:384+len(interpreter)] = interpreter
        struct.pack_into('<IIQQQQQQ', data, 176, 3, 4, 384, 0x400180, 0,
                         len(interpreter), len(interpreter), 1)
    data[512:512+len(strings)] = strings
    for index, (tag, value) in enumerate(tags):
        struct.pack_into('<qQ', data, 1024+index*16, tag, value)
    return bytes(data)


class QtEvaluatorBundleContract(unittest.TestCase):
    def setUp(self):
        # An explicit assertion makes the absent E6 implementation a RED
        # requirement rather than an accidental import/dependency error.
        self.assertTrue(TOOL.is_file(), 'E6 pure bundle generator is not implemented')
        spec = importlib.util.spec_from_file_location('qt_evaluator_bundle', TOOL)
        self.tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.tool)
        self.temporary = tempfile.TemporaryDirectory(prefix='qt-bundle-pure-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sources = self.root / 'sources'
        self.sources.mkdir()
        self.destination = self.root / 'bundle'
        self.metadata = {
            'evaluator_build': 'synthetic-build',
            'compiler': {'id': 'GNU', 'version': '14.2.0'},
            'abi': {'platform': 'linux', 'machine': 'x86_64', 'elf_class': 'ELF64',
                    'cxx_standard': '20'},
        }
        # Metadata-only ELF has no executable code. Real loader inspection
        # remains a separate guarded integration gate.
        content = {
            'qt_evaluator': (b'synthetic cli', ['libtrade_ngin.so', 'libc.so.6']),
            'libtrade_ngin.so': (b'synthetic own engine', ['libcrypto.so.3', 'libc.so.6']),
            'libcrypto.so.3': (b'synthetic crypto', ['libc.so.6']),
            'libc.so.6': (b'synthetic libc', ['ld-linux-x86-64.so.2']),
            'ld-linux-x86-64.so.2': (b'synthetic loader', []),
        }
        self.artifacts = []
        for name, (data, needed) in content.items():
            path = self.sources / name
            path.write_bytes(synthetic_elf(name, needed) + data)
            role = ('executable' if name == 'qt_evaluator' else
                    'engine' if name == 'libtrade_ngin.so' else
                    'loader' if name == 'ld-linux-x86-64.so.2' else 'dependency')
            self.artifacts.append({'name': name, 'source': str(path), 'role': role})

    def stage(self, **changes):
        return self.tool.stage_bundle(CONTRACT, changes.get('artifacts', self.artifacts),
                                      changes.get('metadata', self.metadata),
                                      changes.get('destination', self.destination))

    def test_engine_change_changes_bundle_with_unchanged_executable(self):
        first = self.stage()
        self.tool.verify_bundle(self.destination, first['bundle_sha256'])
        core = self.sources / 'libtrade_ngin.so'
        core.write_bytes(core.read_bytes() + b'changed own engine')
        second = self.stage(destination=self.root / 'second')
        self.assertNotEqual(first['bundle_sha256'], second['bundle_sha256'])
        old_cli = next(row for row in first['artifacts'] if row['role'] == 'executable')
        new_cli = next(row for row in second['artifacts'] if row['role'] == 'executable')
        self.assertEqual(old_cli['sha256'], new_cli['sha256'])

    def test_closed_mapping_is_required_before_any_destination_write(self):
        cases = [self.artifacts[:-1],
                 [row for row in self.artifacts if row['name'] != 'libcrypto.so.3']]
        for artifacts in cases:
            with self.subTest(missing=set(row['name'] for row in self.artifacts) -
                              set(row['name'] for row in artifacts)):
                with self.assertRaises(ValueError):
                    self.stage(artifacts=artifacts)
                self.assertFalse(self.destination.exists())

    def test_duplicate_unsafe_and_disconnected_mapping_refused(self):
        duplicate = self.artifacts + [dict(self.artifacts[1])]
        unsafe = copy.deepcopy(self.artifacts)
        unsafe[1]['name'] = '../libtrade_ngin.so'
        unrelated = self.artifacts + [{'name': 'unused.so', 'source': self.artifacts[1]['source'],
                                     'role': 'dependency'}]
        for artifacts in [duplicate, unsafe, unrelated]:
            with self.subTest(mapping=artifacts):
                with self.assertRaises(ValueError):
                    self.stage(artifacts=artifacts)
                self.assertFalse(self.destination.exists())

    def test_manifest_is_canonical_and_independent_of_input_order(self):
        first = self.stage()
        second = self.stage(artifacts=list(reversed(self.artifacts)),
                            destination=self.root / 'second')
        self.assertEqual(first, second)
        self.assertEqual((self.destination / 'qt_evaluator_manifest.json').read_bytes(),
                         (self.root / 'second/qt_evaluator_manifest.json').read_bytes())
        self.assertEqual(first['wire_schema'], 'qt-eval/v1')
        self.assertEqual(first['evaluator_build'], 'synthetic-build')
        for row in first['artifacts']:
            self.assertEqual(row['sha256'], sha256((self.destination / row['path']).read_bytes()).hexdigest())
        self.assertNotIn(str(self.sources), json.dumps(first))

    def test_library_missing_modified_and_unexpected_file_refused(self):
        manifest = self.stage()
        library = self.destination / 'lib/libtrade_ngin.so'
        old = library.read_bytes()
        library.chmod(0o600)
        library.write_bytes(b'changed staged library')
        with self.assertRaises(ValueError):
            self.tool.verify_bundle(self.destination, manifest['bundle_sha256'])
        library.unlink()
        with self.assertRaises(ValueError):
            self.tool.verify_bundle(self.destination, manifest['bundle_sha256'])
        library.write_bytes(old)
        (self.destination / 'lib/unexpected.so').write_bytes(b'extra')
        with self.assertRaises(ValueError):
            self.tool.verify_bundle(self.destination, manifest['bundle_sha256'])

    def test_manifest_edit_and_wrong_pin_refused(self):
        manifest = self.stage()
        with self.assertRaises(ValueError):
            self.tool.verify_bundle(self.destination, '0' * 64)
        manifest['compiler']['version'] = 'changed'
        (self.destination / 'qt_evaluator_manifest.json').write_text(json.dumps(manifest))
        with self.assertRaises(ValueError):
            self.tool.verify_bundle(self.destination, manifest['bundle_sha256'])

    def test_existing_destination_refused(self):
        self.destination.mkdir()
        sentinel = self.destination / 'unrelated'
        sentinel.write_bytes(b'preserve')
        with self.assertRaises(ValueError):
            self.stage()
        self.assertEqual(sentinel.read_bytes(), b'preserve')

    def test_actual_elf_dependency_graph_cannot_be_replaced_by_caller_names(self):
        core = self.sources / 'libtrade_ngin.so'
        core.write_bytes(synthetic_elf('libtrade_ngin.so', ['unresolved.so']))
        with self.assertRaises(ValueError):
            self.stage()
        self.assertFalse(self.destination.exists())

    def test_absolute_elf_dependency_refused(self):
        core = self.sources / 'libtrade_ngin.so'
        core.write_bytes(synthetic_elf('libtrade_ngin.so', ['/mutable/libcrypto.so.3']))
        with self.assertRaises(ValueError):
            self.stage()
        self.assertFalse(self.destination.exists())

    def api(self):
        self.assertTrue(API_TOOL.is_file(), 'E6 bundle admission is not implemented')
        spec = importlib.util.spec_from_file_location('pure_api_bundle', API_TOOL)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return module

    def bundle(self, api, manifest):
        executable = next(row for row in manifest['artifacts'] if row['role'] == 'executable')
        return api.QtEvaluatorBundle(self.destination, manifest['bundle_sha256'],
                                     executable['sha256'], 'synthetic-build')

    def test_api_admission_snapshots_complete_bytes_before_source_overwrite(self):
        manifest = self.stage()
        api = self.api()
        bundle = self.bundle(api, manifest)
        with bundle.snapshot() as snapshot:
            self.assertEqual(snapshot.manifest, manifest)
            private = snapshot.directory
            self.assertNotEqual(private, self.destination)
            source = self.destination / 'lib/libtrade_ngin.so'
            source.chmod(0o600)
            source.write_bytes(b'changed after snapshot')
            frozen = private / 'lib/libtrade_ngin.so'
            self.assertEqual(sha256(frozen.read_bytes()).hexdigest(),
                             next(row['sha256'] for row in manifest['artifacts'] if row['role'] == 'engine'))
        self.assertFalse(private.exists())

    def test_api_modified_core_dependency_loader_and_wrong_executable_pin_refused(self):
        manifest = self.stage()
        api = self.api()
        for name in ['libtrade_ngin.so', 'libcrypto.so.3', 'ld-linux-x86-64.so.2']:
            path = self.destination / 'lib' / name
            original = path.read_bytes()
            path.chmod(0o600)
            path.write_bytes(original + b'changed')
            with self.subTest(name=name), self.assertRaises(api.QtEvaluatorBundleUnavailable):
                with self.bundle(api, manifest).snapshot():
                    self.fail('modified native artifact admitted')
            path.write_bytes(original)
        wrong = api.QtEvaluatorBundle(self.destination, manifest['bundle_sha256'],
                                      '0' * 64, 'synthetic-build')
        with self.assertRaises(api.QtEvaluatorBundleUnavailable):
            with wrong.snapshot():
                self.fail('wrong executable admitted')

    def test_api_launch_command_has_no_ambient_runpath_or_cache_fallback(self):
        for name in ['qt_evaluator', 'libtrade_ngin.so']:
            path = self.sources / name
            row = next(row for row in self.artifacts if row['name'] == name)
            needed = self.tool.read_elf(path.read_bytes())['needed']
            path.write_bytes(synthetic_elf(name, needed, '/mutable/build:/ambient/lib'))
        manifest = self.stage()
        api = self.api()
        with self.bundle(api, manifest).snapshot() as snapshot:
            # Pure command construction: no descriptors opened or child spawned.
            snapshot = replace(snapshot, directory=PurePosixPath('/private-owned-qt'))
            descriptors = {row['name']:101+index for index,row in enumerate(manifest['artifacts'])}
            command = snapshot.command(descriptors)
            self.assertEqual(command[:4], ('/usr/bin/unshare', '-Urn', '--',
                f"/proc/self/fd/{descriptors['ld-linux-x86-64.so.2']}"))
            self.assertIn('--inhibit-cache', command)
            self.assertEqual(command[command.index('--library-path') + 1], '')
            inhibit = command[command.index('--inhibit-rpath') + 1].split(':')
            self.assertIn('', inhibit)  # main object's name is empty
            self.assertIn(f"/proc/self/fd/{descriptors['libtrade_ngin.so']}", inhibit)
            for row in manifest['artifacts']:
                if row['role'] != 'executable':
                    self.assertIn(row['name'], inhibit)
                    self.assertIn(f"/proc/self/fd/{descriptors[row['name']]}", inhibit)
            self.assertNotIn('/mutable/build', command)
            preloaded=command[command.index('--preload')+1].split(':')
            self.assertEqual(set(preloaded), {f"/proc/self/fd/{descriptors[row['name']]}"
                for row in manifest['artifacts'] if row['role'] not in {'executable','loader'}})
            self.assertEqual(command[-1],f"/proc/self/fd/{descriptors['qt_evaluator']}")
            self.assertNotIn(str(snapshot.directory), ' '.join(command))

    def test_api_missing_library_symlink_and_path_escape_refused(self):
        manifest = self.stage()
        api = self.api()
        path = self.destination / 'lib/libcrypto.so.3'
        original = path.read_bytes()
        path.chmod(0o600)
        path.unlink()
        with self.assertRaises(api.QtEvaluatorBundleUnavailable):
            with self.bundle(api, manifest).snapshot():
                self.fail('missing dependency admitted')
        path.write_bytes(original)
        # Windows does not grant symlink creation by default; simulate the
        # filesystem predicate and exercise the same refusal without privileges.
        original_predicate = Path.is_symlink
        with patch.object(Path, 'is_symlink', autospec=True,
                          side_effect=lambda item: item == path or original_predicate(item)):
            with self.assertRaises(api.QtEvaluatorBundleUnavailable):
                with self.bundle(api, manifest).snapshot():
                    self.fail('symlinked dependency admitted')
        forged = copy.deepcopy(manifest)
        forged['artifacts'][0]['path'] = '../outside'
        unsigned = {key: value for key, value in forged.items() if key != 'bundle_sha256'}
        forged['bundle_sha256'] = sha256(self.tool._canonical(unsigned)).hexdigest()
        (self.destination / 'qt_evaluator_manifest.json').write_bytes(self.tool._canonical(forged))
        with self.assertRaises(api.QtEvaluatorBundleUnavailable):
            with self.bundle(api, forged).snapshot():
                self.fail('escaped path admitted')

    def test_api_wrong_build_and_runtime_platform_refused(self):
        manifest = self.stage()
        api = self.api()
        executable = next(row for row in manifest['artifacts'] if row['role'] == 'executable')
        wrong = api.QtEvaluatorBundle(self.destination, manifest['bundle_sha256'],
                                      executable['sha256'], 'wrong-build')
        with self.assertRaises(api.QtEvaluatorBundleUnavailable):
            with wrong.snapshot():
                self.fail('wrong build admitted')
        with patch.object(api.os, 'name', 'nt'):
            with self.assertRaises(api.QtEvaluatorBundleUnavailable):
                with self.bundle(api, manifest).launch():
                    self.fail('unsupported platform admitted')

    def test_elf_audit_dependency_cannot_escape_closed_graph(self):
        data = bytearray(synthetic_elf('libtrade_ngin.so', ['libcrypto.so.3', 'libc.so.6']))
        # DT_AUDIT is an additional loader code source, not DT_NEEDED.
        size = struct.unpack_from('<Q', data, 152)[0]
        struct.pack_into('<qQ', data, 1024 + size - 16, 0x6ffffefc, 1)
        struct.pack_into('<qQ', data, 1024 + size, 0, 0)
        struct.pack_into('<QQ', data, 152, size + 16, size + 16)
        (self.sources / 'libtrade_ngin.so').write_bytes(data)
        with self.assertRaises(ValueError):
            self.stage()

    def test_api_private_snapshot_mutation_is_detected_before_sealing(self):
        # Sealing is separately checked in Linux transport integration. Here
        # validate the private closure validator refuses an overwritten loader.
        manifest = self.stage()
        api = self.api()
        with self.bundle(api, manifest).snapshot() as snapshot:
            loader = snapshot.directory / 'lib/ld-linux-x86-64.so.2'
            loader.chmod(0o600)
            loader.write_bytes(b'changed private loader')
            with self.assertRaises(ValueError):
                api.verify_bundle(snapshot.directory, manifest['bundle_sha256'])


if __name__ == '__main__':
    unittest.main()
