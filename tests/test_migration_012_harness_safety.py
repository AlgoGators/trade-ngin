"""Safety boundaries for the disposable migration-012 Docker harness.

The harness must only remove the specific container created by its own
``docker run``.  These tests execute the real shell script and replace only the
Docker command with a narrowly scoped fake that records destructive actions.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "migrations" / "test_012_position_overrides_portfolio_scope.sh"


class Migration012HarnessSafetyTests(unittest.TestCase):
    def run_harness(self, scenario: str, container_name: str) -> list[str]:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            actions = temporary_path / "docker-actions.log"
            fake_docker = temporary_path / "docker"
            fake_docker.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env bash
                    set -u
                    printf '%s\\n' "$*" >> "$FAKE_DOCKER_LOG"
                    case "$1" in
                      info) exit 0 ;;
                      run)
                        if [ "$FAKE_DOCKER_SCENARIO" = "collision" ]; then
                          exit 125
                        fi
                        if [ "$FAKE_DOCKER_SCENARIO" = "failed-create-with-stdout" ]; then
                          printf '%s\\n' 'unowned-output-from-failed-run'
                          exit 125
                        fi
                        printf '%s\\n' 'owned-container-id'
                        exit 0
                        ;;
                      exec)
                        # The readiness probe succeeds so the harness reaches
                        # its own fixture path, which then fails immediately.
                        case " $* " in
                          *" SELECT 1 "*) exit 0 ;;
                        esac
                        exit 1
                        ;;
                      rm) exit 0 ;;
                    esac
                    exit 1
                    """
                ),
                encoding="utf-8",
            )
            fake_docker.chmod(0o755)
            environment = os.environ | {
                "CONTAINER_NAME": container_name,
                "FAKE_DOCKER_LOG": str(actions),
                "FAKE_DOCKER_SCENARIO": scenario,
                "PATH": f"{temporary}{os.pathsep}{os.environ['PATH']}",
            }
            completed = subprocess.run(
                ["bash", str(HARNESS)],
                cwd=ROOT,
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=10,
                check=False,
            )
            self.assertNotEqual(completed.returncode, 0)
            return actions.read_text(encoding="utf-8").splitlines()

    def test_failed_container_create_never_removes_the_requested_name(self):
        """A colliding name belongs to somebody else, even if run fails."""
        actions = self.run_harness("collision", "preexisting-container")

        self.assertNotIn("rm -f preexisting-container", actions)

    def test_failed_container_create_with_stdout_never_removes_that_stdout(self):
        """Only a successful Docker run can establish cleanup ownership."""
        actions = self.run_harness("failed-create-with-stdout", "requested-name")

        self.assertNotIn("rm -f unowned-output-from-failed-run", actions)

    def test_fixture_failure_removes_the_id_returned_by_docker_run(self):
        """Cleanup owns Docker's returned ID, not a mutable requested name."""
        actions = self.run_harness("fixture-failure", "requested-container-name")

        self.assertIn("rm -f owned-container-id", actions)
        self.assertNotIn("rm -f requested-container-name", actions)


if __name__ == "__main__":
    unittest.main()
