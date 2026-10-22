"""Command line interface for issue #113 curve validation."""

import argparse
import hashlib
import json
import sys
from pathlib import Path

from .curve import CurveError, compare_prefix, read_curve
from .workflow import WorkflowError, analyze


def main():
    parser = argparse.ArgumentParser(description="Read-only backtest validation")
    sub = parser.add_subparsers(dest="command", required=True)
    compare = sub.add_parser("compare", help="exact causality prefix comparison")
    compare.add_argument("--short", required=True)
    compare.add_argument("--long", required=True)
    compare.add_argument("--short-run", required=True)
    compare.add_argument("--long-run", required=True)
    compare.add_argument("--portfolio", required=True)
    compare.add_argument("--end", required=True, help="inclusive YYYY-MM-DD")
    report = sub.add_parser("analyze", help="analyze verified scratch exports")
    report.add_argument("--manifest", required=True)
    report.add_argument("--gate", required=True, help="JSON output from compare")
    report.add_argument("--fixed-trial", required=True)
    args = parser.parse_args()
    if args.command == "analyze":
        try:
            manifest_path = Path(args.manifest)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            gate = json.loads(Path(args.gate).read_text(encoding="utf-8"))
            result = analyze(manifest, manifest_path.parent, gate, args.fixed_trial)
        except (WorkflowError, CurveError, OSError, ValueError, KeyError) as exc:
            print(json.dumps({"status": "invalid", "error": str(exc)}), file=sys.stderr)
            return 2
        print(json.dumps(result, indent=2, allow_nan=False))
        return 0
    try:
        result = compare_prefix(
            read_curve(args.short, args.short_run, args.portfolio),
            read_curve(args.long, args.long_run, args.portfolio), args.end)
        result["short_sha256"] = hashlib.sha256(Path(args.short).read_bytes()).hexdigest()
        result["long_sha256"] = hashlib.sha256(Path(args.long).read_bytes()).hexdigest()
    except (CurveError, OSError) as exc:
        print(json.dumps({"status": "invalid", "error": str(exc)}), file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())
