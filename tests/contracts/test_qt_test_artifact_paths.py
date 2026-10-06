"""Portable native-test artifact location contract."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "tests/qt_test_artifacts.py"


def load_module():
    spec = importlib.util.spec_from_file_location("qt_test_artifacts", MODULE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class QtTestArtifactPaths(unittest.TestCase):
    def test_python_tests_do_not_embed_a_developer_build_path(self):
        forbidden = "/home/devcontainers/qt-validation-20260921"
        offenders = []
        for path in (ROOT / "tests").rglob("*.py"):
            if path == Path(__file__):
                continue
            if forbidden in path.read_text(encoding="utf-8"):
                offenders.append(path.relative_to(ROOT).as_posix())
        self.assertEqual(offenders, [], "developer build paths must use the portable resolver")

    def test_defaults_are_repository_relative_release_paths(self):
        with patch.dict(os.environ, {}, clear=True):
            paths = load_module()
            self.assertEqual(paths.build_dir(), ROOT / "build/qt-prod")
            self.assertEqual(paths.artifact_dir(), ROOT / "build/qt-prod/bin/Release")
            self.assertEqual(paths.artifact("qt_evaluator"),
                             ROOT / "build/qt-prod/bin/Release/qt_evaluator")

    def test_absolute_environment_overrides_are_honored(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            environment = {
                "TRADE_NGIN_TEST_BUILD_DIR": str(root / "build"),
                "TRADE_NGIN_TEST_ARTIFACT_DIR": str(root / "artifacts"),
            }
            with patch.dict(os.environ, environment, clear=True):
                paths = load_module()
                self.assertEqual(paths.build_dir(), root / "build")
                self.assertEqual(paths.artifact_dir(), root / "artifacts")
                self.assertEqual(paths.artifact("qt_desk_run"), root / "artifacts/qt_desk_run")

    def test_build_identity_comes_from_the_selected_build_header(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            header = root / "include/trade_ngin/git_version.hpp"
            header.parent.mkdir(parents=True)
            header.write_text('#pragma once\n#define TRADE_NGIN_GIT_SHA "a982e429"\n',
                              encoding="utf-8")
            with patch.dict(os.environ, {"TRADE_NGIN_TEST_BUILD_DIR": str(root)}, clear=True):
                paths = load_module()
                self.assertEqual(paths.build_identity(), "a982e429")

    def test_relative_overrides_and_non_leaf_artifacts_fail_closed(self):
        with patch.dict(os.environ, {"TRADE_NGIN_TEST_BUILD_DIR": "relative"}, clear=True):
            paths = load_module()
            with self.assertRaisesRegex(ValueError, "absolute"):
                paths.build_dir()
        with patch.dict(os.environ, {}, clear=True):
            paths = load_module()
            for name in ("", ".", "../qt_evaluator", "nested/qt_evaluator"):
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, "artifact"):
                    paths.artifact(name)

    def test_cli_contract_imports_from_its_documented_working_directory(self):
        completed = subprocess.run(
            [sys.executable, "-B", "-c", "import test_qt_evaluator_cli"],
            cwd=Path(__file__).parent, capture_output=True, text=True,
            env={"PATH": os.environ.get("PATH", "")})
        self.assertEqual(completed.returncode, 0, completed.stderr)


if __name__ == "__main__":
    unittest.main()
