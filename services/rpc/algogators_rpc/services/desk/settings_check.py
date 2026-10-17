"""HD D-17: would a live run accept a desk settings change? Asked before AlgoLens saves it.

The engine's qt_settings_check (apps/tools) runs the live runners' own start-up checks on the
portfolio's config files with the overrides merged in, and books nothing. It sits next to the
engine binary and runs in the engine's working directory (it reads ./config).
"""

from __future__ import annotations

import json
import logging
import os
import subprocess
import tempfile
from typing import Any, Callable, Mapping, Tuple

from .config import CommandSettings

log = logging.getLogger("desk.settings_check")

CHECK_BINARY = "qt_settings_check"
TIMEOUT_S = 60


class SettingsCheckUnavailable(RuntimeError):
    """The check could not be run (no binary, a timeout, an answer it could not read)."""


class SettingsChecker:
    def __init__(self, settings: CommandSettings, run: Callable[..., Any] = subprocess.run):
        self._settings = settings
        self._run = run

    def binary(self) -> str:
        return os.path.join(os.path.dirname(self._settings.engine_binary), CHECK_BINARY)

    def check(self, portfolio_dir: str, overrides: Mapping[str, Any]) -> Tuple[bool, str]:
        """(ok, the refusal a run would give)."""
        fd, path = tempfile.mkstemp(prefix="qt-settings-", suffix=".json")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(overrides, f)
            argv = [self.binary(), "--portfolio-config", portfolio_dir, "--overrides-file", path]
            try:
                proc = self._run(argv, cwd=self._settings.engine_cwd, capture_output=True,
                                 text=True, timeout=TIMEOUT_S)
            except (OSError, subprocess.TimeoutExpired) as exc:
                raise SettingsCheckUnavailable(f"the check could not run ({type(exc).__name__})") \
                    from None
        finally:
            try:
                os.unlink(path)
            except OSError:
                pass
        lines = [line for line in (proc.stdout or "").splitlines() if line.strip()]
        try:
            answer = json.loads(lines[-1]) if lines else None
        except ValueError:
            answer = None
        if proc.returncode in (0, 3) and isinstance(answer, dict) and isinstance(answer.get("ok"), bool):
            return answer["ok"], str(answer.get("error") or "")
        if isinstance(answer, dict) and answer.get("error"):
            # Exit 4: the config files themselves do not load; the change cannot be judged.
            raise SettingsCheckUnavailable(str(answer["error"]))
        tail = " ".join((proc.stderr or "").splitlines()[-3:])
        raise SettingsCheckUnavailable(f"the check exited {proc.returncode}: {tail or '(no output)'}")
