"""Static guards for the immutable build-once production release chain."""
from pathlib import Path
import unittest


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
        self.assertIn('TARGET "${QT_DESK_WORKER_TARGET}"', tools)


if __name__ == "__main__":
    unittest.main()
