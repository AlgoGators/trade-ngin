"""Create and verify the closed trade-ngin production Release manifest."""
import argparse
from hashlib import sha256
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re


_DIGEST = re.compile(r"[0-9a-f]{64}\Z")
_IMAGE_DIGEST = re.compile(r"sha256:[0-9a-f]{64}\Z")
_FULL_SHA = re.compile(r"[0-9a-f]{40}\Z")
_SHORT_SHA = re.compile(r"[0-9a-f]{7,40}\Z")
_MAX_FILE = 1024 * 1024 * 1024
_EXPECTED = {
    "qt_evaluator": "evaluator",
    "libtrade_ngin.so": "engine",
    "live_portfolio": "system_publisher",
    "live_portfolio_conservative": "system_publisher",
    "live_equity_mean_reversion": "system_publisher",
    "qt_desk_prepare_sources": "desk_tool",
    "qt_desk_run": "desk_tool",
}
_WORKER = "qt_desk_worker"


def _pairs(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate_json_key")
        value[key] = item
    return value


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=True, allow_nan=False,
                      separators=(",", ":")).encode("ascii")


def _read(path, limit=_MAX_FILE):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError("invalid_release_artifact")
    before = path.stat()
    if not 0 < before.st_size <= limit:
        raise ValueError("invalid_release_artifact")
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    after = path.stat()
    stable = lambda info: (info.st_dev, info.st_ino, info.st_mode, info.st_nlink,
                           info.st_uid, info.st_gid, info.st_size, info.st_mtime_ns,
                           info.st_ctime_ns)
    if len(data) > limit or before.st_size != len(data) or stable(before) != stable(after):
        raise ValueError("release_artifact_changed")
    return data


def file_sha256(path):
    return sha256(_read(path)).hexdigest()


def _json(path):
    return json.loads(_read(path, 4 * 1024 * 1024).decode("utf-8"),
                      object_pairs_hook=_pairs)


def _bundle_tool():
    path = Path(__file__).with_name("qt_evaluator_bundle.py")
    spec = importlib.util.spec_from_file_location("release_qt_evaluator_bundle", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _validate_schema(path):
    schema = _json(path)
    if (schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema" or
            schema.get("properties", {}).get("schema", {}).get("const") !=
            "release-artifacts/v1" or schema.get("additionalProperties") is not False):
        raise ValueError("invalid_release_artifacts_schema")


def _validate_source(source):
    if not isinstance(source, dict) or set(source) != {
            "git_sha_full", "git_sha_short", "dirty"}:
        raise ValueError("release_identity_invalid")
    full = source["git_sha_full"]
    short = source["git_sha_short"]
    if (not isinstance(full, str) or not _FULL_SHA.fullmatch(full) or
            not isinstance(short, str) or not _SHORT_SHA.fullmatch(short) or
            not full.startswith(short) or source["dirty"] is not False):
        raise ValueError("release_identity_invalid")


def _validate_build(build):
    if (not isinstance(build, dict) or set(build) != {
            "build_type", "compiler", "cxx_standard", "toolchain_image_digest",
            "cmake_inputs"} or build["build_type"] != "Release" or
            build["cxx_standard"] != "20" or
            not _IMAGE_DIGEST.fullmatch(str(build["toolchain_image_digest"])) or
            not isinstance(build["compiler"], dict) or
            set(build["compiler"]) != {"id", "version"} or
            any(not isinstance(build["compiler"][key], str) or
                not build["compiler"][key].strip() for key in ("id", "version")) or
            not isinstance(build["cmake_inputs"], list) or
            not build["cmake_inputs"] or
            any(not isinstance(value, str) or not value or len(value) > 512
                for value in build["cmake_inputs"]) or
            build["cmake_inputs"] != sorted(set(build["cmake_inputs"]))):
        raise ValueError("invalid_release_build")


def _install_path(value):
    if not isinstance(value, str) or not value or value.startswith("/"):
        raise ValueError("invalid_release_install_path")
    path = PurePosixPath(value)
    if str(path) != value or any(part in {"", ".", ".."} for part in path.parts):
        raise ValueError("invalid_release_install_path")
    return value


def _artifact_rows(artifacts, require_worker):
    if not isinstance(artifacts, dict):
        raise ValueError("invalid_release_artifacts")
    allowed = set(_EXPECTED) | {_WORKER}
    if not set(_EXPECTED) <= set(artifacts) or not set(artifacts) <= allowed:
        raise ValueError("invalid_release_artifacts")
    if require_worker and _WORKER not in artifacts:
        raise ValueError("release_worker_required")
    rows = []
    for name, value in artifacts.items():
        expected_kind = _EXPECTED.get(name, "desk_worker")
        if (not isinstance(value, dict) or set(value) != {"source", "install_path", "kind"}
                or value["kind"] != expected_kind):
            raise ValueError("invalid_release_artifact")
        data = _read(value["source"])
        rows.append({"name": name, "kind": expected_kind,
                     "install_path": _install_path(value["install_path"]),
                     "size": len(data), "sha256": sha256(data).hexdigest()})
    rows.sort(key=lambda row: row["name"])
    return rows


def _row(rows, name):
    selected = [row for row in rows if row["name"] == name]
    if len(selected) != 1:
        raise ValueError("invalid_release_artifacts")
    return selected[0]


def generate_manifest(*, output, schema, bundle_directory, expected_bundle_sha256,
                      source, build, image_digest, artifacts, require_worker=False):
    output = Path(output)
    if output.exists() or output.is_symlink():
        raise ValueError("release_manifest_exists")
    _validate_schema(schema)
    _validate_source(source)
    _validate_build(build)
    if not isinstance(image_digest, str) or not _IMAGE_DIGEST.fullmatch(image_digest):
        raise ValueError("invalid_release_image_digest")
    rows = _artifact_rows(artifacts, require_worker)
    bundle = _bundle_tool().verify_bundle(bundle_directory, expected_bundle_sha256)
    if bundle["evaluator_build"] != source["git_sha_short"]:
        raise ValueError("evaluator_build_mismatch")
    executable = next(row for row in bundle["artifacts"] if row["role"] == "executable")
    engine = next(row for row in bundle["artifacts"] if row["role"] == "engine")
    if (_row(rows, "qt_evaluator")["sha256"] != executable["sha256"] or
            _row(rows, "libtrade_ngin.so")["sha256"] != engine["sha256"]):
        raise ValueError("evaluator_bundle_artifact_mismatch")
    manifest = {
        "schema": "release-artifacts/v1",
        "source": source,
        "build": build,
        "image": {"digest": image_digest},
        "evaluator": {
            "evaluator_build": bundle["evaluator_build"],
            "evaluator_sha256": executable["sha256"],
            "evaluator_bundle_sha256": bundle["bundle_sha256"],
            "bundle_manifest_sha256": file_sha256(
                Path(bundle_directory) / "qt_evaluator_manifest.json"),
            "install_path": "qt-evaluator-bundle",
        },
        "artifacts": rows,
        "integration": {"pending_artifacts": [] if _WORKER in artifacts else [_WORKER]},
    }
    manifest["manifest_sha256"] = sha256(canonical(manifest)).hexdigest()
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as stream:
        stream.write(canonical(manifest) + b"\n")
    return manifest


def _validate_manifest_shape(manifest):
    if (not isinstance(manifest, dict) or set(manifest) != {
            "schema", "source", "build", "image", "evaluator", "artifacts",
            "integration", "manifest_sha256"} or
            manifest["schema"] != "release-artifacts/v1" or
            not isinstance(manifest["manifest_sha256"], str) or
            not _DIGEST.fullmatch(manifest["manifest_sha256"])):
        raise ValueError("invalid_release_manifest")
    unsigned = {key: value for key, value in manifest.items() if key != "manifest_sha256"}
    if sha256(canonical(unsigned)).hexdigest() != manifest["manifest_sha256"]:
        raise ValueError("release_manifest_digest_mismatch")
    _validate_source(manifest["source"])
    _validate_build(manifest["build"])
    if set(manifest["image"]) != {"digest"} or not _IMAGE_DIGEST.fullmatch(
            str(manifest["image"]["digest"])):
        raise ValueError("invalid_release_image_digest")
    pending = manifest["integration"]
    if pending not in ({"pending_artifacts": []}, {"pending_artifacts": [_WORKER]}):
        raise ValueError("invalid_release_integration")


def verify_manifest(manifest_path, installed_root, bundle_directory):
    manifest = _json(manifest_path)
    _validate_manifest_shape(manifest)
    installed_root = Path(installed_root).resolve(strict=True)
    names = []
    for row in manifest["artifacts"]:
        if (not isinstance(row, dict) or set(row) != {
                "name", "kind", "install_path", "size", "sha256"} or
                type(row["size"]) is not int or not 0 < row["size"] <= _MAX_FILE or
                not isinstance(row["sha256"], str) or not _DIGEST.fullmatch(row["sha256"])):
            raise ValueError("invalid_release_artifact")
        expected_kind = _EXPECTED.get(row["name"],
                                      "desk_worker" if row["name"] == _WORKER else None)
        if expected_kind is None or row["kind"] != expected_kind:
            raise ValueError("invalid_release_artifact")
        names.append(row["name"])
        path = (installed_root / _install_path(row["install_path"])).resolve(strict=True)
        if installed_root not in path.parents:
            raise ValueError("invalid_release_install_path")
        data = _read(path)
        if len(data) != row["size"] or sha256(data).hexdigest() != row["sha256"]:
            raise ValueError("release_artifact_changed")
    if names != sorted(names) or len(names) != len(set(names)):
        raise ValueError("invalid_release_artifacts")
    required = set(_EXPECTED)
    if not required <= set(names) or not set(names) <= required | {_WORKER}:
        raise ValueError("invalid_release_artifacts")
    expected_pending = [] if _WORKER in names else [_WORKER]
    if manifest["integration"] != {"pending_artifacts": expected_pending}:
        raise ValueError("invalid_release_integration")
    evaluator = manifest["evaluator"]
    if (not isinstance(evaluator, dict) or set(evaluator) != {
            "evaluator_build", "evaluator_sha256", "evaluator_bundle_sha256",
            "bundle_manifest_sha256", "install_path"} or
            evaluator["install_path"] != "qt-evaluator-bundle"):
        raise ValueError("invalid_release_evaluator")
    bundle = _bundle_tool().verify_bundle(
        bundle_directory, evaluator["evaluator_bundle_sha256"])
    executable = next(row for row in bundle["artifacts"] if row["role"] == "executable")
    engine = next(row for row in bundle["artifacts"] if row["role"] == "engine")
    if (evaluator["evaluator_build"] != manifest["source"]["git_sha_short"] or
            evaluator["evaluator_build"] != bundle["evaluator_build"] or
            evaluator["evaluator_sha256"] != executable["sha256"] or
            evaluator["bundle_manifest_sha256"] != file_sha256(
                Path(bundle_directory) / "qt_evaluator_manifest.json") or
            _row(manifest["artifacts"], "qt_evaluator")["sha256"] != executable["sha256"] or
            _row(manifest["artifacts"], "libtrade_ngin.so")["sha256"] != engine["sha256"]):
        raise ValueError("release_evaluator_mismatch")
    return manifest


def inputs_from_release_tree(*, release_root, bundle_directory, git_sha_full,
                             git_sha_short, toolchain_image_digest, image_digest,
                             require_worker=False):
    release_root = Path(release_root).resolve(strict=True)
    bundle_manifest = _json(Path(bundle_directory) / "qt_evaluator_manifest.json")
    compiler = bundle_manifest.get("compiler")
    if (not isinstance(compiler, dict) or set(compiler) != {"id", "version"} or
            any(not isinstance(value, str) or not value for value in compiler.values())):
        raise ValueError("invalid_bundle_metadata")
    artifacts = {}
    for name, kind in _EXPECTED.items():
        artifacts[name] = {
            "source": str(release_root / "bin/Release" / name),
            "install_path": f"bin/Release/{name}",
            "kind": kind,
        }
    worker = release_root / "bin/Release" / _WORKER
    if worker.exists() or worker.is_symlink():
        artifacts[_WORKER] = {
            "source": str(worker),
            "install_path": f"bin/Release/{_WORKER}",
            "kind": "desk_worker",
        }
    return {
        "source": {"git_sha_full": git_sha_full, "git_sha_short": git_sha_short,
                   "dirty": False},
        "build": {
            "build_type": "Release",
            "compiler": compiler,
            "cxx_standard": "20",
            "toolchain_image_digest": toolchain_image_digest,
            "cmake_inputs": sorted([
                "CMAKE_BUILD_TYPE=Release",
                "CMAKE_CXX_FLAGS=-O3",
                "CMAKE_CXX_STANDARD=20",
                "NLopt_DIR=/usr/lib/x86_64-linux-gnu/cmake/nlopt",
            ]),
        },
        "image_digest": image_digest,
        "expected_bundle_sha256": bundle_manifest.get("bundle_sha256"),
        "artifacts": artifacts,
        "require_worker": require_worker,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--create", action="store_true")
    mode.add_argument("--verify", action="store_true")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--inputs", type=Path)
    parser.add_argument("--release-root", type=Path)
    parser.add_argument("--schema", type=Path)
    parser.add_argument("--installed-root", type=Path)
    parser.add_argument("--bundle-directory", type=Path, required=True)
    parser.add_argument("--git-sha-full")
    parser.add_argument("--git-sha-short")
    parser.add_argument("--toolchain-image-digest")
    parser.add_argument("--image-digest")
    parser.add_argument("--require-worker", action="store_true")
    args = parser.parse_args()
    if args.create:
        if args.schema is None or args.installed_root is not None:
            parser.error("--create requires --schema and one input source")
        tree_values = [args.git_sha_full, args.git_sha_short,
                       args.toolchain_image_digest, args.image_digest]
        if args.inputs is not None:
            if args.release_root is not None or any(value is not None for value in tree_values):
                parser.error("--inputs and --release-root inputs are mutually exclusive")
            inputs = _json(args.inputs)
        else:
            if args.release_root is None or any(value is None for value in tree_values):
                parser.error("--release-root requires Git, toolchain, and image identities")
            inputs = inputs_from_release_tree(
                release_root=args.release_root, bundle_directory=args.bundle_directory,
                git_sha_full=args.git_sha_full, git_sha_short=args.git_sha_short,
                toolchain_image_digest=args.toolchain_image_digest,
                image_digest=args.image_digest, require_worker=args.require_worker)
        if not isinstance(inputs, dict) or set(inputs) != {
                "source", "build", "image_digest", "expected_bundle_sha256",
                "artifacts", "require_worker"} or type(inputs["require_worker"]) is not bool:
            raise ValueError("invalid_release_inputs")
        expected_bundle_sha256 = inputs["expected_bundle_sha256"]
        if expected_bundle_sha256 == "auto":
            expected_bundle_sha256 = _json(
                args.bundle_directory / "qt_evaluator_manifest.json")["bundle_sha256"]
        manifest = generate_manifest(
            output=args.manifest, schema=args.schema,
            bundle_directory=args.bundle_directory,
            expected_bundle_sha256=expected_bundle_sha256,
            source=inputs["source"], build=inputs["build"],
            image_digest=inputs["image_digest"], artifacts=inputs["artifacts"],
            require_worker=inputs["require_worker"])
    else:
        if (args.installed_root is None or args.inputs is not None or
                args.release_root is not None or args.schema is not None or
                any(value is not None for value in [args.git_sha_full, args.git_sha_short,
                    args.toolchain_image_digest, args.image_digest]) or args.require_worker):
            parser.error("--verify requires --installed-root only")
        manifest = verify_manifest(args.manifest, args.installed_root, args.bundle_directory)
    print(manifest["manifest_sha256"])


if __name__ == "__main__":
    main()
