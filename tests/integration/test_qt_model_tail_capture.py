"""Separately pinned actual MODEL tail gates. No captured bytes are normalized.

Pure contracts are safe without PG. Each actual gate requires a concrete reviewed
control manifest and the owned, isolated PostgreSQL wrapper.
"""
from hashlib import sha256
from datetime import datetime
import importlib.util
import json
import os
from pathlib import Path
import uuid

import pytest

ALLOWED_SOURCE_DELTA = frozenset({"apps/strategies/live_portfolio_runner.cpp",
    "apps/strategies/live_portfolio_runner.hpp", "include/trade_ngin/apps/book_tail.hpp",
    "src/apps/book_tail.cpp", "CMakeLists.txt"})


def _workspace():
    return next(p for p in Path(__file__).resolve().parents
        if (p / '.review/trade-ngin-qt/apps/tools/qt_model_baseline_capture.py').is_file())


def _load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def adapter():
    return _load_module('qt_model_tail_adapter', _workspace() /
        '.review/trade-ngin-qt/apps/tools/qt_model_baseline_capture.py')


def _pinned_json(adapter, pin):
    assert set(pin) == {'path', 'sha256'}
    raw = adapter.bounded_read(pin['path'])
    assert sha256(raw).hexdigest() == pin['sha256'], 'control_artifact_changed'
    return json.loads(raw)


def _source_delta(before, after, expected):
    actual = {name: {'before': before.get(name), 'after': after.get(name)}
        for name in before.keys() | after.keys() if before.get(name) != after.get(name)}
    assert actual and set(actual) <= ALLOWED_SOURCE_DELTA, 'unapproved_source_change'
    assert actual == expected, 'source_delta_changed'
    assert all(item['after'] is not None for item in actual.values()), 'source_removed'
    return actual


def _matched_requests(before, after, expected):
    assert before['target'] == after['target'], 'variant_changed'
    assert before.keys() == after.keys()
    for name in before.keys() - {'source_files', 'artifacts'}:
        assert before[name] == after[name], 'capture_input_changed:' + name
    _source_delta(before['source_files'], after['source_files'], expected)
    assert before['artifacts'].keys() == after['artifacts'].keys()
    for name, pin in before['artifacts'].items():
        assert pin['path'] == after['artifacts'][name]['path'], 'artifact_path_changed'
        if name not in {'binary', 'library'}:
            assert pin == after['artifacts'][name], 'control_artifact_changed:' + name


def _verified_raw(adapter, request, result):
    assert result['schema_version'] == 'qt-model-baseline-result/v2'
    assert result['actual_entry'] == 'run_live_portfolio' and result['exit_code'] == 0
    assert result['normalization'] == 'none' and result['fixture_kind'] == 'synthetic'
    assert result['authentic_runtime_certification'] == 'unavailable'
    assert result['target'] == request['target'] and result['day'] == request['day']
    assert result['build_id'] == request['build_id'] and result['artifacts'] == request['artifacts']
    assert result['determinism'] == request['determinism']
    assert result['request_sha256'] == sha256(json.dumps(request, sort_keys=True).encode()).hexdigest()
    assert result['source_manifest_sha256'] == sha256(json.dumps(request['source_files'], sort_keys=True).encode()).hexdigest()
    initial_names = {'table:' + name for name in request['table_schemas']} | {
        'log:database-catalog.json', 'log:sequence-state.json'}
    assert set(result['initial_outputs']) == initial_names, 'initial_inventory_changed'
    required_final = initial_names | {'csv:' + name for name in request['expected_csv']} | {
        'log:stdout.raw', 'log:stderr.raw'}
    stamp = datetime.fromisoformat(request['determinism']['epoch_utc'].replace('Z', '+00:00')).strftime('%Y%m%d_%H%M%S')
    prefix = 'live_trend' if request['target'] == 'live_portfolio' else 'live_trend_conservative'
    logger_names = set(result['outputs']) - required_final
    import re
    assert logger_names and all(re.fullmatch('log:' + prefix + '_' + stamp + r'_part[1-9][0-9]*\.log', name)
        for name in logger_names), 'logger_inventory_changed'
    assert required_final <= result['outputs'].keys(), 'final_inventory_changed'
    items = [(phase, name, item) for phase, inventory in
        (('initial', result['initial_outputs']), ('final', result['outputs']))
        for name, item in inventory.items()]
    assert items and all(type(item['bytes']) is int and 0 <= item['bytes'] <= adapter.MAX_FILE
        for _, _, item in items)
    assert sum(item['bytes'] for _, _, item in items) <= adapter.MAX_TOTAL, 'capture_capacity_exceeded'
    directories = {Path(item['path']).parent.parent for _, _, item in items}
    assert len(directories) == 1, 'capture_directory_mismatch'
    directory = directories.pop()
    assert json.loads(adapter.bounded_read(directory / 'native-exit.json')) == {
        'native_exit': 0, 'delivery_guard_exit': False}
    assert json.loads(adapter.bounded_read(directory / 'native-source-boundary.json')) == {
        'source_and_artifacts_verified_after_native': True,
        'source_files': request['source_files'], 'artifacts': request['artifacts']}
    assert json.loads(adapter.bounded_read(directory / 'capture.json')) == result
    assert sha256(adapter.bounded_read(directory / 'loader.raw')).hexdigest() == result['loader_trace_sha256'], 'loader_trace_changed'
    outputs = {}
    seen = set()
    for phase, name, item in items:
        path = Path(item['path'])
        assert path.parent == directory / ('initial' if phase == 'initial' else 'outputs')
        assert path not in seen, 'aliased_capture_artifact'
        seen.add(path)
        raw = adapter.bounded_read(path, item['bytes'])
        assert len(raw) == item['bytes'] and sha256(raw).hexdigest() == item['sha256'], 'raw_artifact_changed'
        outputs[phase + ':' + name] = raw
    return outputs


def _control(adapter, label):
    if not os.environ.get('ALGOLENS_TEST_DB'):
        pytest.skip('explicit root-reviewed owned-PG gate required')
    path = _workspace() / ('docs/repairs/2026-09-26-hemdutt-issue-completion/'
        'MODEL-TAIL-' + label + '-CONTROL.json')
    control = json.loads(adapter.bounded_read(path))
    assert control['schema_version'] == 'qt-model-tail-control/v1'
    assert control['label'] == 'MODEL-TAIL-' + label and control['normalization'] == 'none'
    assert control['authentic_runtime_certification'] == 'unavailable'
    assert control['comparison_scope'] == 'all_raw_initial_and_final_outputs'
    test_raw = adapter.bounded_read(control['test']['path'])
    assert Path(control['test']['path']).resolve() == Path(__file__).resolve()
    assert sha256(test_raw).hexdigest() == control['test']['sha256']
    request = _pinned_json(adapter, control['request'])
    adapter.validate_request(request)
    helper_path = _workspace() / '.review/trade-ngin-qt/tests/integration/test_qt_model_baseline_capture.py'
    assert control['baseline_test']['path'] == str(helper_path)
    assert sha256(adapter.bounded_read(helper_path)).hexdigest() == control['baseline_test']['sha256']
    baseline = _load_module('qt_model_tail_baseline_helpers', helper_path)
    return path, control, request, baseline


@pytest.mark.parametrize('label', ['6C', '7', '7C'])
def test_owned_pg_tail_catalog_preflight(adapter, label):
    _, control, _, baseline = _control(adapter, label)
    baseline.test_owned_pg_deterministic_catalog_preflight(adapter, Path(control['request']['path']).name)


@pytest.mark.parametrize('label', ['6C', '7', '7C'])
def test_owned_pg_tail_clock_boundary(adapter, tmp_path, label):
    _, control, _, baseline = _control(adapter, label)
    baseline.test_owned_pg_compiled_clock_boundary(adapter, tmp_path, Path(control['request']['path']).name)


def test_owned_pg_conservative_reference_baseline(adapter):
    _, control, request, baseline = _control(adapter, '6C')
    assert control['phase'] == 'before' and request['target'] == 'live_portfolio_conservative'
    baseline.test_owned_pg_actual_reference_repeatability(adapter, Path(control['request']['path']).name)


@pytest.mark.parametrize('label', ['7', '7C'])
def test_owned_pg_actual_shared_tail_raw_parity(adapter, label):
    gate, control, request, _ = _control(adapter, label)
    assert control['phase'] == 'after'
    before_request = _pinned_json(adapter, control['before_request'])
    _matched_requests(before_request, request, control['source_delta'])
    pair = _pinned_json(adapter, control['before_pair'])
    assert pair['schema_version'] == 'qt-model-reference-pair/v1'
    assert pair['request_file_sha256'] == control['before_request']['sha256']
    assert pair['raw_comparison']['byte_identical'] is True
    assert pair['fresh_database_instances'] == 2 and pair['same_owned_database_and_socket_identity'] is True
    assert len(pair['captures']) == 2
    before = [_verified_raw(adapter, before_request, item) for item in pair['captures']]
    adapter.require_reference_repeatability(*before)
    values = adapter.owned_database(os.environ['ALGOLENS_TEST_DB'])
    assert {'host': values['host'], 'dbname': values['dbname']} == control['transport'], 'transport_identity_changed'
    result = adapter.capture(request, os.environ['ALGOLENS_TEST_DB'],
        gate.parent / ('model-tail-' + label + '-' + uuid.uuid4().hex))
    after = _verified_raw(adapter, request, result)
    assert result['config_hashes'] == pair['captures'][0]['config_hashes'], 'generated_config_changed'
    comparison = {'byte_identical': False, 'normalization': 'none',
        'only_before': sorted(before[0].keys() - after.keys()),
        'only_after': sorted(after.keys() - before[0].keys()),
        'different_outputs': sorted(k for k in before[0].keys() & after.keys() if before[0][k] != after[k])}
    comparison['byte_identical'] = not any(comparison[k] for k in ('only_before', 'only_after', 'different_outputs'))
    evidence = {'schema_version': 'qt-model-tail-comparison/v1', 'control_file': str(gate),
        'control_file_sha256': sha256(gate.read_bytes()).hexdigest(), 'capture': result,
        'raw_comparison': comparison, 'source_delta': control['source_delta'],
        'authentic_runtime_certification': 'unavailable'}
    output = gate.parent / ('model-tail-comparison-' + label + '-' + uuid.uuid4().hex + '.json')
    with output.open('x') as stream:
        json.dump(evidence, stream, sort_keys=True, indent=2)
    print('ACTUAL_MODEL_TAIL_COMPARISON=' + str(output))
    adapter.require_reference_repeatability(before[0], after)
    adapter.require_reference_repeatability(before[1], after)


def test_source_delta_refuses_extra_runtime_and_wrong_expected_hash():
    before = {'CMakeLists.txt': 'old', 'src/financial.cpp': 'fixed'}
    after = {'CMakeLists.txt': 'new', 'src/financial.cpp': 'changed'}
    with pytest.raises(AssertionError, match='unapproved_source_change'):
        _source_delta(before, after, {})
    with pytest.raises(AssertionError, match='source_delta_changed'):
        _source_delta({'CMakeLists.txt': 'old'}, {'CMakeLists.txt': 'new'}, {})


def test_source_delta_accepts_only_exact_reviewed_new_tail_and_cmake():
    expected = {'CMakeLists.txt': {'before': 'old', 'after': 'new'},
        'src/apps/book_tail.cpp': {'before': None, 'after': 'tail'}}
    assert _source_delta({'CMakeLists.txt': 'old'},
        {'CMakeLists.txt': 'new', 'src/apps/book_tail.cpp': 'tail'}, expected) == expected


@pytest.mark.parametrize('mutation', ['variant', 'fixture', 'clock'])
def test_matched_requests_refuse_changed_variant_inputs_and_control(mutation):
    from copy import deepcopy
    before = {'target': 'live_portfolio', 'config_files': {'fixture': 'same'},
        'source_files': {'CMakeLists.txt': 'old'}, 'artifacts': {
            'binary': {'path': '/fixed/model', 'sha256': 'old'},
            'clock': {'path': '/fixed/clock', 'sha256': 'same'}}}
    after = deepcopy(before)
    after['source_files']['CMakeLists.txt'] = 'new'
    if mutation == 'variant':
        after['target'] = 'live_portfolio_conservative'
    elif mutation == 'fixture':
        after['config_files']['fixture'] = 'changed'
    else:
        after['artifacts']['clock']['sha256'] = 'changed'
    with pytest.raises(AssertionError):
        _matched_requests(before, after, {'CMakeLists.txt': {'before': 'old', 'after': 'new'}})


@pytest.fixture
def retained_evidence(tmp_path):
    request = {'target': 'live_portfolio', 'day': '2026-09-25', 'build_id': 'test-only',
        'artifacts': {}, 'source_files': {}, 'determinism': {'epoch_utc': '2026-09-26T12:00:00Z'},
        'table_schemas': {'trading.positions': []}, 'expected_csv': ['2026-09-25_positions.csv']}
    initial_names = ['table:trading.positions', 'log:database-catalog.json', 'log:sequence-state.json']
    final_names = initial_names + ['csv:2026-09-25_positions.csv', 'log:live_trend_20260926_120000_part1.log',
        'log:stdout.raw', 'log:stderr.raw']
    result = {'schema_version': 'qt-model-baseline-result/v2', 'actual_entry': 'run_live_portfolio',
        'exit_code': 0, 'normalization': 'none', 'fixture_kind': 'synthetic',
        'authentic_runtime_certification': 'unavailable', **{key: request[key] for key in
            ('target', 'day', 'build_id', 'artifacts', 'determinism')},
        'request_sha256': sha256(json.dumps(request, sort_keys=True).encode()).hexdigest(),
        'source_manifest_sha256': sha256(json.dumps({}, sort_keys=True).encode()).hexdigest(),
        'loader_trace_sha256': sha256(b'explicit-loader').hexdigest()}
    for phase, names in [('initial', initial_names), ('outputs', final_names)]:
        directory = tmp_path / phase
        directory.mkdir()
        inventory = {}
        for index, name in enumerate(names):
            path = directory / (str(index) + '.raw')
            path.write_bytes(b'exact')
            inventory[name] = {'path': str(path), 'bytes': 5, 'sha256': sha256(b'exact').hexdigest()}
        result['initial_outputs' if phase == 'initial' else 'outputs'] = inventory
    (tmp_path / 'native-exit.json').write_text(json.dumps({'native_exit': 0, 'delivery_guard_exit': False}))
    (tmp_path / 'native-source-boundary.json').write_text(json.dumps({
        'source_and_artifacts_verified_after_native': True, 'source_files': {}, 'artifacts': {}}))
    (tmp_path / 'loader.raw').write_bytes(b'explicit-loader')
    (tmp_path / 'capture.json').write_text(json.dumps(result))
    return request, result


def test_retained_raw_evidence_passes_without_normalization(adapter, retained_evidence):
    request, result = retained_evidence
    raw = _verified_raw(adapter, request, result)
    assert len(raw) == 10 and set(raw.values()) == {b'exact'}


@pytest.mark.parametrize('mutation', ['loader', 'missing_table', 'raw', 'native', 'source_boundary'])
def test_retained_evidence_refuses_tamper(adapter, retained_evidence, mutation):
    request, result = retained_evidence
    directory = Path(next(iter(result['initial_outputs'].values()))['path']).parent.parent
    if mutation == 'loader':
        (directory / 'loader.raw').write_bytes(b'foreign-loader')
    elif mutation == 'missing_table':
        for inventory in (result['initial_outputs'], result['outputs']):
            inventory.pop('table:trading.positions')
        (directory / 'capture.json').write_text(json.dumps(result))
    elif mutation == 'raw':
        Path(result['outputs']['log:stdout.raw']['path']).write_bytes(b'other')
    elif mutation == 'native':
        (directory / 'native-exit.json').write_text(json.dumps({'native_exit': 86, 'delivery_guard_exit': True}))
    else:
        (directory / 'native-source-boundary.json').write_text('{}')
    with pytest.raises(AssertionError):
        _verified_raw(adapter, request, result)
