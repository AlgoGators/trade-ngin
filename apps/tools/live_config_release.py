"""Detached live-config-release/v1 attestation for a verified core Release and validator closure."""
import argparse
from hashlib import sha256
from pathlib import Path
import release_artifacts as core
import qt_evaluator_bundle as bundles


def linked_manifest(core_manifest, bundle, bundle_directory, installed_root):
    if core_manifest['integration'] != {'pending_artifacts': []}:
        raise ValueError('live_config_core_incomplete')
    source=core_manifest['source']
    engine=next(row for row in core_manifest['artifacts'] if row['name']=='libtrade_ngin.so')
    validator=next(row for row in bundle['artifacts'] if row['role']=='executable')
    bundled_engine=next(row for row in bundle['artifacts'] if row['role']=='engine')
    if (bundle['validator_build']!=source['git_sha_short'] or bundle['compiler']!=core_manifest['build']['compiler']
            or bundled_engine['sha256']!=engine['sha256']
            or core.file_sha256(Path(installed_root)/'bin/Release/live_config_validate')!=validator['sha256']):
        raise ValueError('live_config_release_linkage_mismatch')
    value={'schema':'live-config-release/v1','core_manifest_sha256':core_manifest['manifest_sha256'],
        'source':source,'build':core_manifest['build'],'image':core_manifest['image'],
        'validator':{'build':bundle['validator_build'],'executable_sha256':validator['sha256'],
            'engine_sha256':bundled_engine['sha256'],'bundle_sha256':bundle['bundle_sha256'],
            'bundle_manifest_sha256':core.file_sha256(Path(bundle_directory)/bundles.LIVE_CONFIG_PROFILE.manifest),
            'install_path':'live-config-validator-bundle','executable_install_path':'bin/Release/live_config_validate'}}
    value['manifest_sha256']=sha256(core.canonical(value)).hexdigest()
    return value


def verify_inputs(core_path, installed_root, qt_bundle, validator_bundle):
    manifest=core.verify_manifest(core_path,installed_root,qt_bundle)
    raw=core._json(Path(validator_bundle)/bundles.LIVE_CONFIG_PROFILE.manifest)
    bundle=bundles.verify_bundle(validator_bundle,raw['bundle_sha256'],profile=bundles.LIVE_CONFIG_PROFILE)
    return linked_manifest(manifest,bundle,validator_bundle,installed_root)


def create_manifest(output, core_path, installed_root, qt_bundle, validator_bundle):
    result=verify_inputs(core_path,installed_root,qt_bundle,validator_bundle)
    with Path(output).open('xb') as stream:stream.write(core.canonical(result)+b'\n')
    return result


def verify_manifest(path, core_path, installed_root, qt_bundle, validator_bundle):
    expected=verify_inputs(core_path,installed_root,qt_bundle,validator_bundle)
    if core._json(path)!=expected:raise ValueError('live_config_release_mismatch')
    return expected


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--create',action='store_true')
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--core-manifest',type=Path,required=True)
    parser.add_argument('--installed-root',type=Path,required=True)
    parser.add_argument('--qt-bundle',type=Path,required=True)
    parser.add_argument('--validator-bundle',type=Path,required=True)
    args=parser.parse_args()
    fn=create_manifest if args.create else verify_manifest
    print(fn(args.manifest,args.core_manifest,args.installed_root,args.qt_bundle,args.validator_bundle)['manifest_sha256'])

if __name__=='__main__':main()
