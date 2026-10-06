"""Resolve native test artifacts without depending on a developer machine path."""
import os
from pathlib import Path
import re


_ROOT = Path(__file__).resolve().parents[1]


def _absolute_environment(name, default):
    value = Path(os.environ.get(name, str(default)))
    if not value.is_absolute():
        raise ValueError(f"{name} must be an absolute path")
    return value


def build_dir():
    return _absolute_environment("TRADE_NGIN_TEST_BUILD_DIR", _ROOT / "build/qt-prod")


def artifact_dir():
    return _absolute_environment(
        "TRADE_NGIN_TEST_ARTIFACT_DIR", build_dir() / "bin/Release")


def artifact(name):
    if not isinstance(name, str) or not name or Path(name).name != name or name in {".", ".."}:
        raise ValueError("invalid test artifact name")
    return artifact_dir() / name


def build_identity():
    header = build_dir() / "include/trade_ngin/git_version.hpp"
    text = header.read_text(encoding="utf-8")
    match = re.search(r'^#define TRADE_NGIN_GIT_SHA "([^"]+)"$', text, re.MULTILINE)
    if not match:
        raise ValueError("selected build has no TRADE_NGIN_GIT_SHA identity")
    return match.group(1)
