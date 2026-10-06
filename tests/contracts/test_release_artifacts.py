"""Closed production release manifest for the exact tested native bytes."""
import importlib.util
from hashlib import sha256
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import test_qt_evaluator_bundle as bundle_contract


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "apps/tools/release_artifacts.py"
SCHEMA = ROOT / "apps/tools/release_artifacts_schema.json"
FULL = "a982e429aa33ba97f8c7ec9c462ed962f1233729"
SHORT = "a982e42"
DIGEST_A = "sha256:" + "a" * 64
DIGEST_B = "sha256:" + "b" * 64


def load_tool():
    spec = importlib.util.spec_from_file_location("release_artifacts", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ReleaseArtifactsContract(unittest.TestCase):
    def setUp(self):
        self.bundle_fixture = bundle_contract.QtEvaluatorBundleContract(methodName="runTest")
        self.bundle_fixture.setUp()
        self.addCleanup(self.bundle_fixture.doCleanups)
        self.bundle_fixture.metadata["evaluator_build"] = SHORT
        self.bundle = self.bundle_fixture.stage()
        self.temporary = tempfile.TemporaryDirectory(prefix="release-artifacts-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.installed = self.root / "installed"
        self.output = self.root / "release-artifacts.json"
        self.artifacts = {}
        roles = {
            "qt_evaluator": "evaluator",
            "libtrade_ngin.so": "engine",
            "live_portfolio": "system_publisher",
            "live_portfolio_conservative": "system_publisher",
            "live_equity_mean_reversion": "system_publisher",
            "qt_desk_prepare_sources": "desk_tool",
            "qt_desk_run": "desk_tool",
        }
        bundle_sources = {
            row["name"]: self.bundle_fixture.destination / row["path"]
            for row in self.bundle["artifacts"]
        }
        for name, role in roles.items():
            install_path = f"bin/Release/{name}"
            path = self.installed / install_path
            path.parent.mkdir(parents=True, exist_ok=True)
            if name in bundle_sources:
                path.write_bytes(bundle_sources[name].read_bytes())
            else:
                path.write_bytes(("release-" + name).encode("ascii"))
            self.artifacts[name] = {
                "source": path, "install_path": install_path, "kind": role}
        self.source = {"git_sha_full": FULL, "git_sha_short": SHORT, "dirty": False}
        self.build = {
            "build_type": "Release",
            "compiler": {"id": "GNU", "version": "14.2.0"},
            "cxx_standard": "20",
            "toolchain_image_digest": DIGEST_A,
            "cmake_inputs": ["CMAKE_BUILD_TYPE=Release", "CMAKE_CXX_STANDARD=20"],
        }

    def generate(self, **changes):
        return load_tool().generate_manifest(
            output=self.output,
            schema=SCHEMA,
            bundle_directory=self.bundle_fixture.destination,
            expected_bundle_sha256=self.bundle["bundle_sha256"],
            source=changes.get("source", self.source),
            build=changes.get("build", self.build),
            image_digest=changes.get("image_digest", DIGEST_B),
            artifacts=changes.get("artifacts", self.artifacts),
            require_worker=changes.get("require_worker", False))

    def rewrite_manifest(self, transform):
        manifest = json.loads(self.output.read_text(encoding="utf-8"))
        transform(manifest)
        unsigned = {key: value for key, value in manifest.items()
                    if key != "manifest_sha256"}
        manifest["manifest_sha256"] = sha256(
            load_tool().canonical(unsigned)).hexdigest()
        self.output.write_bytes(load_tool().canonical(manifest) + b"\n")

    def test_manifest_binds_source_toolchain_image_bundle_and_all_current_binaries(self):
        manifest = self.generate()
        self.assertEqual(manifest["schema"], "release-artifacts/v1")
        self.assertEqual(manifest["source"], self.source)
        self.assertEqual(manifest["image"]["digest"], DIGEST_B)
        self.assertEqual(manifest["evaluator"], {
            "evaluator_build": SHORT,
            "evaluator_sha256": next(row["sha256"] for row in self.bundle["artifacts"]
                                     if row["role"] == "executable"),
            "evaluator_bundle_sha256": self.bundle["bundle_sha256"],
            "bundle_manifest_sha256": load_tool().file_sha256(
                self.bundle_fixture.destination / "qt_evaluator_manifest.json"),
            "install_path": "qt-evaluator-bundle",
        })
        self.assertEqual([row["name"] for row in manifest["artifacts"]], sorted(self.artifacts))
        self.assertEqual(manifest["integration"], {"pending_artifacts": ["qt_desk_worker"]})
        raw = self.output.read_bytes()
        self.assertEqual(raw, load_tool().canonical(manifest) + b"\n")
        self.assertEqual(load_tool().verify_manifest(
            self.output, self.installed, self.bundle_fixture.destination), manifest)

    def test_schema_declares_every_nested_object_closed(self):
        schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
        properties = schema["properties"]
        for name in ("source", "build", "image", "evaluator", "integration"):
            with self.subTest(name=name):
                self.assertIs(properties[name]["additionalProperties"], False)
                self.assertEqual(set(properties[name]["required"]),
                                 set(properties[name]["properties"]))
        artifact = properties["artifacts"]["items"]
        self.assertIs(artifact["additionalProperties"], False)
        self.assertEqual(set(artifact["required"]), set(artifact["properties"]))
        self.assertEqual(properties["source"]["properties"]["dirty"], {"const": False})
        self.assertEqual(properties["build"]["properties"]["build_type"],
                         {"const": "Release"})

    def test_worker_hook_closes_the_final_manifest(self):
        worker = self.installed / "bin/Release/qt_desk_worker"
        worker.write_bytes(b"release-worker")
        artifacts = dict(self.artifacts)
        artifacts["qt_desk_worker"] = {
            "source": worker,
            "install_path": "bin/Release/qt_desk_worker",
            "kind": "desk_worker",
        }
        manifest = self.generate(artifacts=artifacts, require_worker=True)
        self.assertEqual(manifest["integration"], {"pending_artifacts": []})
        self.assertEqual(next(row for row in manifest["artifacts"]
                             if row["name"] == "qt_desk_worker")["kind"], "desk_worker")

    def test_wrong_identity_digest_or_bundle_bytes_fail_before_manifest_write(self):
        cases = [
            {"source": {**self.source, "git_sha_short": "local-qt-controlled"}},
            {"source": {**self.source, "dirty": True}},
            {"image_digest": "latest"},
        ]
        for changes in cases:
            with self.subTest(changes=changes):
                with self.assertRaises(ValueError):
                    self.generate(**changes)
                self.assertFalse(self.output.exists())
        evaluator = self.artifacts["qt_evaluator"]["source"]
        evaluator.write_bytes(evaluator.read_bytes() + b"changed")
        with self.assertRaisesRegex(ValueError, "evaluator"):
            self.generate()
        self.assertFalse(self.output.exists())

    def test_installed_byte_change_is_detected(self):
        self.generate()
        binary = self.artifacts["qt_desk_run"]["source"]
        binary.write_bytes(b"different")
        with self.assertRaisesRegex(ValueError, "artifact"):
            load_tool().verify_manifest(
                self.output, self.installed, self.bundle_fixture.destination)

    def test_recomputed_manifest_cannot_change_artifact_roles(self):
        self.generate()
        self.rewrite_manifest(lambda manifest: next(
            row for row in manifest["artifacts"]
            if row["name"] == "live_portfolio").update(kind="evaluator"))
        with self.assertRaisesRegex(ValueError, "artifact"):
            load_tool().verify_manifest(
                self.output, self.installed, self.bundle_fixture.destination)

    def test_recomputed_manifest_cannot_hide_pending_worker(self):
        self.generate()
        self.rewrite_manifest(lambda manifest: manifest["integration"].update(
            pending_artifacts=[]))
        with self.assertRaisesRegex(ValueError, "integration"):
            load_tool().verify_manifest(
                self.output, self.installed, self.bundle_fixture.destination)

    def test_recomputed_manifest_cannot_decouple_engine_from_bundle(self):
        self.generate()
        engine = self.artifacts["libtrade_ngin.so"]["source"]
        engine.write_bytes(b"different-engine")

        def replace_engine_digest(manifest):
            row = next(row for row in manifest["artifacts"]
                       if row["name"] == "libtrade_ngin.so")
            row["size"] = engine.stat().st_size
            row["sha256"] = sha256(engine.read_bytes()).hexdigest()

        self.rewrite_manifest(replace_engine_digest)
        with self.assertRaisesRegex(ValueError, "evaluator"):
            load_tool().verify_manifest(
                self.output, self.installed, self.bundle_fixture.destination)

    def test_cli_creates_manifest_from_closed_json_inputs(self):
        inputs = self.root / "inputs.json"
        value = {
            "source": self.source,
            "build": self.build,
            "image_digest": DIGEST_B,
            "expected_bundle_sha256": self.bundle["bundle_sha256"],
            "artifacts": {name: {**item, "source": str(item["source"])}
                          for name, item in self.artifacts.items()},
            "require_worker": False,
        }
        inputs.write_text(json.dumps(value), encoding="utf-8")
        completed = subprocess.run([
            sys.executable, "-B", str(TOOL), "--create", "--inputs", str(inputs),
            "--manifest", str(self.output), "--schema", str(SCHEMA),
            "--bundle-directory", str(self.bundle_fixture.destination),
        ], capture_output=True, text=True, timeout=15,
           env={"PATH": "/usr/local/bin:/usr/bin:/bin", "PYTHONDONTWRITEBYTECODE": "1"})
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        manifest = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual(completed.stdout.strip(), manifest["manifest_sha256"])

    def test_cli_creates_manifest_directly_from_the_tested_release_tree(self):
        completed = subprocess.run([
            sys.executable, "-B", str(TOOL), "--create",
            "--release-root", str(self.installed),
            "--git-sha-full", FULL, "--git-sha-short", SHORT,
            "--toolchain-image-digest", DIGEST_A, "--image-digest", DIGEST_B,
            "--manifest", str(self.output), "--schema", str(SCHEMA),
            "--bundle-directory", str(self.bundle_fixture.destination),
        ], capture_output=True, text=True, timeout=15,
           env={"PATH": "/usr/local/bin:/usr/bin:/bin", "PYTHONDONTWRITEBYTECODE": "1"})
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        manifest = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual(manifest["source"], self.source)
        self.assertEqual(manifest["build"]["compiler"],
                         self.bundle["compiler"])
        self.assertEqual(manifest["image"]["digest"], DIGEST_B)
        self.assertEqual(load_tool().verify_manifest(
            self.output, self.installed, self.bundle_fixture.destination), manifest)


if __name__ == "__main__":
    unittest.main()
