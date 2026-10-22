"""Command line interface for issue #113 curve validation."""

import argparse
import json
import sys

from .curve import CurveError, compare_prefix, read_curve


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
    args = parser.parse_args()
    try:
        result = compare_prefix(
            read_curve(args.short, args.short_run, args.portfolio),
            read_curve(args.long, args.long_run, args.portfolio), args.end)
    except (CurveError, OSError) as exc:
        print(json.dumps({"status": "invalid", "error": str(exc)}), file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())
