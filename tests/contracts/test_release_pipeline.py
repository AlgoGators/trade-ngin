"""Guards for the immutable build-once production release chain."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import unittest

from tests.qt_test_artifacts import build_dir


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/ci-cd-pipeline.yml"
DOCKERFILE = ROOT / "Dockerfile"
TOOLS_CMAKE = ROOT / "apps/tools/CMakeLists.txt"
WORKER_CMAKE = ROOT / "apps/tools/qt_desk_worker.cmake"


class ReleasePipelineContract(unittest.TestCase):
    def setUp(self):
        self.workflow = WORKFLOW.read_text(encoding="utf-8")
        self.image_job = self.workflow.split("  image-generation:", 1)[1].split(
            "  schema-ownership-guard:", 1)[0]
        self.dockerfile = DOCKERFILE.read_text(encoding="utf-8")

    def test_image_job_hashes_the_tested_tree_without_recompiling(self):
        self.assertNotIn("cmake -S", self.image_job)
        self.assertNotIn("cmake --build", self.image_job)
        self.assertIn("--create --release-root build/qt-prod", self.image_job)
        self.assertIn("canonical-release-${{ github.sha }}", self.image_job)

    def test_security_scan_precedes_prod_latest_promotion(self):
        scan = self.image_job.index("- name: Scan image with Trivy")
        promote = self.image_job.index("- name: Promote scanned prod digest to latest")
        self.assertLess(scan, promote)
        self.assertIn("--exit-code 1", self.image_job[scan:promote])
        self.assertIn("if: github.event_name == 'push' && github.ref_name == 'prod'",
                      self.image_job[promote:])

    def test_runtime_image_only_packages_the_canonical_release(self):
        self.assertIn("ARG RUNTIME_BASE_IMAGE\nFROM ${RUNTIME_BASE_IMAGE}", self.dockerfile)
        self.assertIn("COPY build/qt-prod/bin/Release/", self.dockerfile)
        self.assertNotIn("cmake ", self.dockerfile)
        self.assertNotIn("make ", self.dockerfile)
        self.assertNotIn("sed ", self.dockerfile)

    def test_runtime_packages_and_arrow_source_are_immutable_inputs(self):
        self.assertIn("ARG APACHE_ARROW_APT_SOURCE_SHA256", self.dockerfile)
        self.assertIn("sha256sum -c -", self.dockerfile)
        self.assertIn("ARG TRADE_NGIN_RUNTIME_APT_PACKAGES", self.dockerfile)
        self.assertIn("package_missing_exact_version", self.dockerfile)
        self.assertIn(
            '--build-arg APACHE_ARROW_APT_SOURCE_SHA256="$APACHE_ARROW_APT_SOURCE_SHA256"',
            self.image_job)
        self.assertIn(
            '--build-arg TRADE_NGIN_RUNTIME_APT_PACKAGES="$TRADE_NGIN_RUNTIME_APT_PACKAGES"',
            self.image_job)

    def test_prod_deploy_trigger_remains_exact(self):
        deploy = self.workflow.split("  deploy-to-ec2:", 1)[1]
        self.assertIn("if: github.ref_name == 'prod' && github.event_name == 'push'", deploy)
        self.assertIn("EXPECTED_DIGEST='${{ needs.image-generation.outputs.image_digest }}'",
                      deploy)
        self.assertIn("test \"$RUNNING_IMAGE_ID\" = \"$EXPECTED_IMAGE_ID\"", deploy)

    def test_worker_target_is_mandatory_release_input(self):
        tools = TOOLS_CMAKE.read_text(encoding="utf-8")
        worker = WORKER_CMAKE.read_text(encoding="utf-8")
        include = 'include("${CMAKE_CURRENT_LIST_DIR}/qt_desk_worker.cmake")'
        self.assertIn(include, tools)
        self.assertLess(tools.index(include), tools.index("function(trade_ngin_add_release_artifacts_target)"))
        self.assertIn("set(QT_DESK_WORKER_TARGET qt_desk_worker)", worker)
        self.assertIn("if(NOT TARGET qt_desk_worker)", tools)
        self.assertIn("qt_desk_worker)", tools)
        self.assertIn("--require-worker", self.image_job)

    @unittest.skipUnless(os.environ.get("TRADE_NGIN_TEST_BUILD_DIR"),
                         "configured-build contract needs TRADE_NGIN_TEST_BUILD_DIR")
    def test_configured_release_input_and_target_graph_require_the_worker(self):
        """A directory-scope regression must fail on configured output, not source text."""
        configured = build_dir()
        inputs = json.loads(
            (configured / "release-artifacts-inputs-Release.json").read_text(
                encoding="utf-8"))
        self.assertIs(inputs["require_worker"], True)
        self.assertEqual(inputs["artifacts"]["qt_desk_worker"], {
            "source": str(configured / "bin/Release/qt_desk_worker"),
            "install_path": "bin/Release/qt_desk_worker",
            "kind": "desk_worker",
        })

        make_graph = configured / "CMakeFiles/Makefile2"
        if make_graph.exists():
            graph = make_graph.read_text(encoding="utf-8")
            release = graph.split("CMakeFiles/release_artifacts.dir/all:", 1)[1].split(
                ".PHONY : CMakeFiles/release_artifacts.dir/all", 1)[0]
            self.assertIn("apps/tools/CMakeFiles/qt_desk_worker.dir/all", release)

            recipe = (configured / "CMakeFiles/release_artifacts.dir/build.make").read_text()
            command = re.search(r"(\S+/release_artifacts\.py) --create", recipe)
            self.assertIsNotNone(command)
            result = subprocess.run([sys.executable, command.group(1), "--help"],
                                    capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
