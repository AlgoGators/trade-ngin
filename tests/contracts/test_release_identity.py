"""CMake release identity must fail closed before dependency discovery or compilation."""
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "cmake/TradeNginReleaseIdentity.cmake"
FULL = "a982e429aa33ba97f8c7ec9c462ed962f1233729"
SHORT = "a982e42"


class ReleaseIdentityContract(unittest.TestCase):
    def invoke(self, *, build_type="Release", short=SHORT, full=FULL, dirty="FALSE"):
        with tempfile.TemporaryDirectory(prefix="release-identity-") as directory:
            script = Path(directory) / "verify.cmake"
            script.write_text(
                f'include("{MODULE.as_posix()}")\n'
                f'trade_ngin_validate_release_identity("{build_type}" "{short}" "{full}" "{dirty}")\n',
                encoding="utf-8")
            return subprocess.run(["cmake", "-P", str(script)], capture_output=True,
                                  text=True, timeout=15)

    def test_exact_clean_release_identity_is_accepted(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_release_rejects_mutable_synthetic_and_mismatched_identity(self):
        cases = [
            {"short": "unknown"},
            {"short": "local-qt-controlled"},
            {"short": "synthetic-build"},
            {"full": "not-a-full-sha"},
            {"short": "bbbbbbb"},
            {"dirty": "TRUE"},
        ]
        for changes in cases:
            with self.subTest(changes=changes):
                result = self.invoke(**changes)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("release_identity_invalid", result.stdout + result.stderr)

    def test_nonrelease_controlled_identity_remains_available_to_tests(self):
        result = self.invoke(build_type="Debug", short="local-qt-controlled",
                             full="", dirty="TRUE")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_root_project_validates_identity_before_required_dependencies(self):
        root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        validation = root_cmake.index("trade_ngin_validate_release_identity(")
        first_required_dependency = root_cmake.index("find_package(GTest CONFIG REQUIRED)")
        self.assertLess(validation, first_required_dependency)

    def test_root_only_enforces_checkout_match_for_release(self):
        root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertEqual(root_cmake.count(
            'if(CMAKE_BUILD_TYPE STREQUAL "Release" AND\n'
            '   TRADE_NGIN_GIT_'), 2)


if __name__ == "__main__":
    unittest.main()
