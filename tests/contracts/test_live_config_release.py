"""Synthetic closed ELF controls: release linkage is checked, never inferred from a label."""
import importlib.util
import sys
from pathlib import Path
from copy import deepcopy
import pytest
import test_release_artifacts as fixtures
ROOT, SHORT = fixtures.ROOT, fixtures.SHORT
sys.path.insert(0,str(ROOT/'apps/tools'))
import live_config_release as release
import qt_evaluator_bundle as bundle

@pytest.fixture
def installed():
    f=fixtures.ReleaseArtifactsContract(methodName='runTest');f.setUp()
    try:
        worker=f.installed/'bin/Release/qt_desk_worker';worker.write_bytes(b'worker')
        f.artifacts['qt_desk_worker']={'source':worker,'install_path':'bin/Release/qt_desk_worker','kind':'desk_worker'}
        f.generate(require_worker=True)
        rows=deepcopy(f.bundle_fixture.artifacts)
        executable=next(r for r in rows if r['role']=='executable');executable['name']='live_config_validate'
        installed_validator=f.installed/'bin/Release/live_config_validate';installed_validator.write_bytes(Path(executable['source']).read_bytes())
        metadata=deepcopy(f.bundle_fixture.metadata);metadata['validator_build']=metadata.pop('evaluator_build')
        # Source metadata is an exact shared Release compiler identity.
        metadata['compiler']=f.build['compiler']
        destination=f.root/'validator'
        bundle.stage_bundle(ROOT/'apps/tools/live_config_validator_manifest.json',rows,metadata,destination,profile=bundle.LIVE_CONFIG_PROFILE)
        yield f,destination
    finally:f.doCleanups()

def args(f,d):return (f.output,f.installed,f.bundle_fixture.destination,d)

def test_closed_validator_sidecar_roundtrip(installed):
    f,d=installed;path=f.root/'sidecar.json'
    value=release.create_manifest(path,*args(f,d))
    assert value['source']['git_sha_short']==SHORT
    assert release.verify_manifest(path,*args(f,d))==value
    with pytest.raises(ValueError):bundle.verify_bundle(d,value['validator']['bundle_sha256'])

@pytest.mark.parametrize('damage',['validator','publisher','engine','sidecar','pending'])
def test_linkage_and_installed_bytes_are_required(installed,damage):
    f,d=installed;path=f.root/'sidecar.json';release.create_manifest(path,*args(f,d))
    if damage in ('validator','publisher','engine'):
        name={'validator':'live_config_validate','publisher':'live_portfolio','engine':'libtrade_ngin.so'}[damage]
        (f.installed/'bin/Release'/name).write_bytes(b'tampered')
    elif damage=='sidecar':path.write_text('{}')
    else:f.rewrite_manifest(lambda m:m.update(integration={'pending_artifacts':['qt_desk_worker']}))
    with pytest.raises(ValueError):release.verify_manifest(path,*args(f,d))
