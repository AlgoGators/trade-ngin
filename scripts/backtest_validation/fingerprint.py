"""Canonical hashes of a resolved modular config; prints hashes only."""

import argparse
import copy
import hashlib
import json
from pathlib import Path


def _digest(files):
    encoded = json.dumps(files, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=False, allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def fingerprints(config_dir):
    root = Path(config_dir)
    files = {}
    for path in sorted(root.rglob("*.json")):
        files[path.relative_to(root).as_posix()] = json.loads(path.read_text(encoding="utf-8"))
    if "defaults.json" not in files:
        raise ValueError("config directory lacks defaults.json")
    normalized = copy.deepcopy(files)
    defaults = normalized["defaults.json"]
    defaults.pop("database", None)  # scratch connection may change across isolated clones
    backtest = defaults.get("backtest", {})
    backtest.pop("frozen_end_date", None)
    backtest.pop("lookback_years", None)
    backtest.pop("start_date", None)
    backtest.pop("end_date", None)
    return {"config_sha256": _digest(files),
            "non_window_config_sha256": _digest(normalized),
            "json_files": sorted(files)}


def main():
    parser = argparse.ArgumentParser(description="Hash resolved trade-ngin config JSON")
    parser.add_argument("config_dir", help="runner's ./config directory")
    args = parser.parse_args()
    print(json.dumps(fingerprints(args.config_dir), indent=2))


if __name__ == "__main__":
    main()
