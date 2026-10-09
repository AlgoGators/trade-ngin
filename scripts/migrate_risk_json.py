#!/usr/bin/env python3
"""Migrate a config directory from risk schema 1 to risk schema 2 (T-6 commit 7).

Schema 1 resolved a book's gating values out of three files with two layers of silent
defaults, and switched the whole risk layer on a `use_risk_management` boolean. Schema 2
has no defaults: every gating value is written literally in the book's own risk.json,
the modules the book runs are named, and a book that runs no risk layer says so with a
`none` module carrying who ruled it and when. The loader REFUSES a schema-1 risk.json,
so this has to be run once per config directory.

Runbook (see also config_template/README.md):

    python3 scripts/migrate_risk_json.py config             # dry run: read the diff
    python3 scripts/migrate_risk_json.py config --in-place   # write it

Each changed file is first copied to <file>.schema1.bak; those .bak files are the
rollback for going back to a pre-commit-7 build.

    python3 scripts/migrate_risk_json.py config --out DIR    # write a migrated copy

A schema-2 file written before T-6b commit 9 says "lookback_unit": "bars". The Carver
window has been capped at lookback_period distinct DATES since that commit and the loader
refuses "bars", so the script upgrades such a file to "dates" (the only change it makes to
a schema-2 file), keeping a <file>.bars.bak rollback copy.

Exit codes
    0   nothing to do (already schema 2 with "dates"), or written
    2   refused before reading content: usage, an existing .bak, a non-empty --out
    3   refused on content: the file does not say enough to migrate it safely
    10  dry run with changes pending

What it will not do: invent a value. A book whose risk.json has no max_leverage, or
whose use_risk_management resolves to false, is refused with the reason, because writing
a default into a file is exactly the failure schema 2 exists to end.

Every printed diff redacts the value of any key named "password": defaults.json carries
the database password, and a re-indented file would otherwise print it as context.
"""

import argparse
import difflib
import json
import os
import re
import shutil
import sys

# include/trade_ngin/risk/risk_manager.hpp's RiskConfig member initialisers: the last
# layer schema 1 fell back to when neither the book nor risk_defaults named a value.
STRUCT_DEFAULTS = {
    "var_limit": "0.15",
    "jump_risk_limit": "0.10",
    "max_correlation": "0.7",
    "max_gross_leverage": "4.0",
    "max_net_leverage": "2.0",
    "confidence_level": "0.99",
    "lookback_period": "252",
}
GATING_FIELDS = list(STRUCT_DEFAULTS)

# include/trade_ngin/strategy/equity_strategy_builder.hpp:25
EQUITY_STRATEGY_TYPES = {"MeanReversionStrategy"}

MISSING_SYMBOL_REASON = (
    "Schema-1 behaviour, migrated literally: risk_manager.cpp:55-58 gates the book on the "
    "subset of its symbols present in the window (T-4 §10). Changing it is its own "
    "measured commit."
)
REPORTING_NOTE = (
    "RA-01: the live runners' snapshot RiskManager reads this block through "
    "AppConfig::risk_config; on day one it mirrors the gate (HD Q5)."
)
USE_OPTIMIZATION_NOTE = (
    "false: the equity runners hard-code the optimizer off "
    "(live_equity_mean_reversion.cpp, bt_equity_mean_reversion.cpp; HD 2026-09-01) and "
    "refuse to start if this says true."
)

MIN_GATE_DATES = 21
BAK_SUFFIX = ".schema1.bak"
# The rollback copy of a schema-2 file whose lookback_unit is upgraded from "bars" to "dates".
# A different suffix, because a file migrated from schema 1 already has its .schema1.bak.
BARS_BAK_SUFFIX = ".bars.bak"
LOOKBACK_BARS = re.compile(r'("lookback_unit"\s*:\s*)"bars"')


# The tracked templates beside this script: what each book is ASSIGNED in the repository.
TEMPLATE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                            "config_template", "portfolios")


def template_none_ruling(name):
    """(ruled_by, ruled_on) when config_template assigns book `name` a lone `none`, else None."""
    path = os.path.join(TEMPLATE_DIR, name, "risk.json")
    try:
        with open(path, "r", encoding="utf-8") as handle:
            modules = json.load(handle).get("modules", [])
    except (OSError, ValueError):
        return None
    if len(modules) == 1 and isinstance(modules[0], dict) and modules[0].get("type") == "none":
        return modules[0].get("_ruled_by", "?"), modules[0].get("_ruled_on", "?")
    return None


def note_if_template_says_none(name, log):
    """The script writes a carver for every book and will not invent a ruling. When the tracked
    template assigns this book `none`, the deployed file and the template now disagree, and only
    a person can copy the ruling across (T-6b INTERIM ADVERSARIAL D-1)."""
    ruling = template_none_ruling(name)
    if ruling:
        log("  NOTE: %s: this file keeps a carver module, but config_template/portfolios/%s/"
            "risk.json assigns this book `none` (_ruled_by %s, _ruled_on %s). The script will "
            "not invent that ruling: copy the none module into modules by hand (the runbook "
            "step in config_template/README.md)." % (name, name, ruling[0], ruling[1]))


class Refused(Exception):
    """Content the script will not migrate without a human deciding something."""


class Raw:
    """A number kept as the text the file spells it with.

    json.dumps would re-render 0.10 as 0.1 and 4.0 as 4.0 -> 4.0 but 1e-06 as 1e-06,
    and a diff full of re-rendered numbers hides the one line that actually moved.
    """

    __slots__ = ("text",)

    def __init__(self, text):
        self.text = text

    def __eq__(self, other):
        return isinstance(other, Raw) and other.text == self.text

    def __repr__(self):
        return "Raw(%s)" % self.text


def load(path):
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    return text, json.loads(text, parse_float=Raw, parse_int=Raw)


def detect_indent(text):
    """The file's own indentation, so a migrated file does not re-indent wholesale."""
    for line in text.splitlines()[1:]:
        stripped = line.lstrip(" ")
        if stripped and stripped != line:
            return len(line) - len(stripped)
    return 2


def dumps(obj, indent):
    placeholders = {}

    def encode(value):
        if isinstance(value, Raw):
            key = "@@raw%d@@" % len(placeholders)
            placeholders[key] = value.text
            return key
        raise TypeError(repr(value))

    text = json.dumps(obj, indent=indent, default=encode, ensure_ascii=False)
    for key, literal in placeholders.items():
        text = text.replace('"%s"' % key, literal)
    return collapse_numeric_arrays(text) + "\n"


_NUMBER_ARRAY = re.compile(
    r"\[\s*\n((?:[ \t]*-?[0-9][^\n,\]]*,[ \t]*\n)*[ \t]*-?[0-9][^\n,\]]*[ \t]*\n)[ \t]*\]")
_ARRAY_OF_ARRAYS = re.compile(
    r"\[\s*\n((?:[ \t]*\[[^\n\[\]]*\],[ \t]*\n)*[ \t]*\[[^\n\[\]]*\][ \t]*\n)[ \t]*\]")


def collapse_numeric_arrays(text):
    """Put arrays of numbers back on one line, the way the config files write them.

    json.dumps(indent=...) explodes [[1, 1], [2, 1.03]] over sixteen lines. The values
    would be identical, but the migration's whole point is a diff a human reads before
    writing it, and sixteen unchanged lines per array is how a real change gets missed.
    Only arrays of plain numbers (fdm, ema_windows) are collapsed; a list of strings, one
    per line, is left as the file already had it.
    """

    def join(match, limit=None):
        items = [line.strip().rstrip(",") for line in match.group(1).strip().split("\n")]
        joined = "[" + ", ".join(i for i in items if i) + "]"
        if limit is not None and len(joined) > limit:
            return match.group(0)
        return joined

    previous = None
    while previous != text:
        previous = text
        text = _NUMBER_ARRAY.sub(join, text)
    previous = None
    while previous != text:
        previous = text
        text = _ARRAY_OF_ARRAYS.sub(lambda m: join(m, 120), text)
    return text


PASSWORD_LINE = re.compile(r'("password"\s*:\s*)".*"')


def redact(line):
    return PASSWORD_LINE.sub(r'\1"<redacted>"', line)


def print_diff(path, before, after, labels=("schema 1", "schema 2")):
    diff = difflib.unified_diff(
        before.splitlines(True), after.splitlines(True),
        fromfile="%s (%s)" % (path, labels[0]), tofile="%s (%s)" % (path, labels[1]),
    )
    for line in diff:
        sys.stdout.write(redact(line.rstrip("\n")) + "\n")


def deep_merge(base, override):
    out = dict(base)
    for key, value in override.items():
        if key in out and isinstance(out[key], dict) and isinstance(value, dict):
            out[key] = deep_merge(out[key], value)
        else:
            out[key] = value
    return out


def comments_of(obj):
    return {k: v for k, v in obj.items() if k.startswith("_")}


def find_key(node, key, path=""):
    """Every place `key` occurs, as a dotted path. Paths only, never values."""
    hits = []
    if isinstance(node, dict):
        for name, value in node.items():
            child = name if not path else path + "." + name
            if name == key:
                hits.append((child, value))
            hits.extend(find_key(value, key, child))
    elif isinstance(node, list):
        for i, value in enumerate(node):
            hits.extend(find_key(value, key, "%s[%d]" % (path, i)))
    return hits


def is_false(value):
    return value is False


def migrate_portfolio(name, risk_text, risk, portfolio_text, portfolio, defaults,
                      defaults_has_risk_defaults, log):
    """Return (new_risk_text, new_portfolio_text) or (None, None) when already schema 2."""
    if "schema" in risk:
        schema = risk["schema"]
        if not isinstance(schema, Raw) or schema.text != "2":
            raise Refused("%s/risk.json has \"schema\": %s; only schema 2 exists"
                          % (name, getattr(schema, "text", schema)))
        stale = [f for f in GATING_FIELDS if f in risk]
        if stale:
            raise Refused("%s/risk.json is schema 2 but still carries flat gating keys (%s); "
                          "the loader would reject it -- fix it by hand"
                          % (name, ", ".join(stale)))
        if "use_optimization" not in portfolio:
            raise Refused("%s/risk.json is schema 2 but portfolio.json has no top-level "
                          "use_optimization" % name)
        # The one change a schema-2 file can need: "bars" -> "dates" on its carver modules.
        # Text-level, so every other byte of the operator's file (spacing, key order, numbers
        # spelled as written) is untouched; the reparse proves nothing else moved.
        upgraded, count = LOOKBACK_BARS.subn(r'\1"dates"', risk_text)
        if count:
            before, after = json.loads(risk_text), json.loads(upgraded)
            for m in before.get("modules", []):
                if isinstance(m, dict) and m.get("lookback_unit") == "bars":
                    m["lookback_unit"] = "dates"
            if before != after:
                raise Refused("%s/risk.json: upgrading lookback_unit would change more than that "
                              "key -- fix it by hand" % name)
            log("  %s: schema 2; lookback_unit \"bars\" -> \"dates\" on %d module(s) (the "
                "window has been date-keyed since T-6b commit 9)" % (name, count))
            if any(isinstance(m, dict) and m.get("type") == "carver"
                   for m in after.get("modules", [])):
                note_if_template_says_none(name, log)
            return upgraded, portfolio_text
        log("  %s: already schema 2, nothing to do" % name)
        if any(isinstance(m, dict) and m.get("type") == "carver"
               for m in json.loads(risk_text).get("modules", [])):
            note_if_template_says_none(name, log)
        return None, None

    # 1. use_risk_management: a false anywhere is a decision the script will not invent
    #    the attribution for.
    for scope, blob in (("defaults.json", defaults), ("%s/portfolio.json" % name, portfolio)):
        for where, value in find_key(blob, "use_risk_management"):
            if is_false(value):
                raise Refused(
                    "%s: %s is false. Schema 2 needs an explicit none module with _reason, "
                    "_ruled_by and _ruled_on; the script will not invent a ruling -- write it "
                    "by hand." % (scope, where))

    # 2. Resolve the seven gating values with the loader's own precedence.
    if not defaults_has_risk_defaults:
        raise Refused(
            "%s/risk.json is schema 1 but defaults.json has no risk_defaults block: cannot tell "
            "whether values came from a deleted risk_defaults; restore defaults.json from its "
            "%s" % (name, BAK_SUFFIX))
    risk_defaults = deep_merge(defaults.get("risk_defaults", {}),
                               portfolio.get("risk_defaults", {}))
    resolved = {}
    for field in GATING_FIELDS:
        if field in risk:
            resolved[field] = risk[field]
            source = "%s/risk.json" % name
        elif field in risk_defaults:
            resolved[field] = risk_defaults[field]
            source = "risk_defaults"
        else:
            resolved[field] = Raw(STRUCT_DEFAULTS[field])
            source = "RiskConfig struct default"
        log("  %s: %-20s = %-8s  <- %s"
            % (name, field, getattr(resolved[field], "text", resolved[field]), source))

    for key in ("max_drawdown", "max_leverage"):
        if key not in risk:
            raise Refused(
                "%s/risk.json has no %s. Schema 2 requires it (schema 1 silently fell back to "
                "0.4 / 4.0), and the script will not write a default into a file." % (name, key))

    for dropped in ("corr_shock_threshold", "jump_shock_threshold", "version", "capital"):
        if dropped in risk:
            log("  %s: dropping %s (no reader)" % (name, dropped))
    if "risk" in portfolio:
        log("  %s: portfolio.json has a \"risk\" key, which today's loader discards "
            "(risk.json replaces it wholesale); it is NOT used" % name)

    # 3. The new risk.json: always a carver module (the only gate schema 1 had).
    note_if_template_says_none(name, log)
    new_risk = comments_of(risk)
    new_risk["schema"] = Raw("2")
    module = {"id": "carver", "type": "carver"}
    for field in GATING_FIELDS:
        module[field] = resolved[field]
    module["lookback_unit"] = "dates"
    module["min_gate_dates"] = Raw(str(MIN_GATE_DATES))
    module["missing_symbol_policy"] = "ignore"
    module["_missing_symbol_policy_reason"] = MISSING_SYMBOL_REASON
    new_risk["modules"] = [module]
    reporting = {"type": "carver", "window": "all_bars"}
    for field in GATING_FIELDS:
        reporting[field] = resolved[field]
    reporting["_note"] = REPORTING_NOTE
    new_risk["risk_reporting"] = reporting
    new_risk["max_drawdown"] = risk["max_drawdown"]
    new_risk["max_leverage"] = risk["max_leverage"]

    # 4. use_optimization moves to portfolio.json's top level.
    strategy_defaults = portfolio.get("strategy_defaults", {})
    if "use_optimization" in strategy_defaults:
        effective = strategy_defaults["use_optimization"]
    else:
        effective = defaults.get("strategy_defaults", {}).get("use_optimization", True)
    strategies = portfolio.get("strategies", {})
    enabled_types = []
    for sid, definition in strategies.items():
        if not isinstance(definition, dict):
            continue
        if definition.get("enabled_backtest") is True or definition.get("enabled_live") is True:
            enabled_types.append(definition.get("type", ""))
    equity = [t for t in enabled_types if t in EQUITY_STRATEGY_TYPES]
    futures = [t for t in enabled_types if t not in EQUITY_STRATEGY_TYPES]
    equity_book = bool(equity) and not futures
    if equity and futures:
        raise Refused(
            "%s/portfolio.json enables both equity (%s) and non-equity (%s) strategies; the "
            "equity runners hard-code the optimizer off and the futures runners do not, so the "
            "book's single use_optimization cannot be resolved -- split the book."
            % (name, ", ".join(sorted(set(equity))), ", ".join(sorted(set(futures)))))

    new_portfolio = dict(portfolio)
    new_portfolio.pop("risk_defaults", None)
    if "strategy_defaults" in new_portfolio:
        cleaned = {k: v for k, v in new_portfolio["strategy_defaults"].items()
                   if k not in ("use_optimization", "use_risk_management")}
        if cleaned:
            new_portfolio["strategy_defaults"] = cleaned
        else:
            new_portfolio.pop("strategy_defaults")
    if equity_book:
        new_portfolio["use_optimization"] = False
        new_portfolio["_use_optimization_note"] = USE_OPTIMIZATION_NOTE
        log("  %s: use_optimization = false (equity book; the runners hard-code it off)" % name)
    else:
        new_portfolio["use_optimization"] = bool(effective)
        log("  %s: use_optimization = %s" % (name, str(bool(effective)).lower()))

    return (dumps(new_risk, detect_indent(risk_text)),
            dumps(new_portfolio, detect_indent(portfolio_text)))


def migrate_defaults(defaults_text, defaults, log):
    new = dict(defaults)
    touched = False
    if "risk_defaults" in new:
        new.pop("risk_defaults")
        touched = True
        log("  defaults.json: dropping risk_defaults (every value is now written per book)")
    if "strategy_defaults" in new and isinstance(new["strategy_defaults"], dict):
        cleaned = {k: v for k, v in new["strategy_defaults"].items()
                   if k not in ("use_optimization", "use_risk_management")}
        if cleaned != new["strategy_defaults"]:
            new["strategy_defaults"] = cleaned
            touched = True
            log("  defaults.json: dropping strategy_defaults.use_optimization / "
                "use_risk_management")
    if not touched:
        return None
    return dumps(new, detect_indent(defaults_text))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Migrate a config directory from risk schema 1 to schema 2.")
    parser.add_argument("config_dir")
    parser.add_argument("--in-place", action="store_true",
                        help="write the migrated files, keeping a %s of each" % BAK_SUFFIX)
    parser.add_argument("--out", metavar="DIR",
                        help="write a migrated copy of the whole config directory into DIR")
    args = parser.parse_args(argv)

    if args.in_place and args.out:
        print("refused: --in-place and --out are alternatives", file=sys.stderr)
        return 2
    config_dir = args.config_dir
    if not os.path.isdir(config_dir):
        print("refused: %s is not a directory" % config_dir, file=sys.stderr)
        return 2
    defaults_path = os.path.join(config_dir, "defaults.json")
    if not os.path.isfile(defaults_path):
        print("refused: %s has no defaults.json" % config_dir, file=sys.stderr)
        return 2
    if args.out and os.path.isdir(args.out) and os.listdir(args.out):
        print("refused: --out directory %s is not empty" % args.out, file=sys.stderr)
        return 2

    portfolios_dir = os.path.join(config_dir, "portfolios")
    names = sorted(n for n in os.listdir(portfolios_dir)
                   if os.path.isdir(os.path.join(portfolios_dir, n))) \
        if os.path.isdir(portfolios_dir) else []

    log = print
    log("Reading %s" % config_dir)
    defaults_text, defaults = load(defaults_path)
    defaults_has_risk_defaults = "risk_defaults" in defaults

    written = {}  # path -> new text
    upgrades = set()  # schema-2 risk.json files whose only change is lookback_unit
    try:
        for name in names:
            pdir = os.path.join(portfolios_dir, name)
            risk_path = os.path.join(pdir, "risk.json")
            portfolio_path = os.path.join(pdir, "portfolio.json")
            if not os.path.isfile(risk_path) or not os.path.isfile(portfolio_path):
                log("  %s: no risk.json/portfolio.json pair, skipped" % name)
                continue
            risk_text, risk = load(risk_path)
            portfolio_text, portfolio = load(portfolio_path)
            new_risk, new_portfolio = migrate_portfolio(
                name, risk_text, risk, portfolio_text, portfolio, defaults,
                defaults_has_risk_defaults, log)
            if new_risk is None:
                continue
            if "schema" in risk:
                upgrades.add(risk_path)
            if new_risk != risk_text:
                written[risk_path] = (risk_text, new_risk)
            if new_portfolio != portfolio_text:
                written[portfolio_path] = (portfolio_text, new_portfolio)
        new_defaults = migrate_defaults(defaults_text, defaults, log)
        if new_defaults is not None and new_defaults != defaults_text:
            written[defaults_path] = (defaults_text, new_defaults)
    except Refused as why:
        print("refused: %s" % why, file=sys.stderr)
        return 3

    if not written:
        log("Nothing to migrate.")
        if args.out:
            _copy_tree(config_dir, args.out)
        return 0

    log("")
    for path in sorted(written):
        before, after = written[path]
        print_diff(path, before, after,
                   ("lookback_unit bars", "lookback_unit dates") if path in upgrades
                   else ("schema 1", "schema 2"))

    if args.out:
        _copy_tree(config_dir, args.out)
        for path, (_, after) in written.items():
            target = os.path.join(args.out, os.path.relpath(path, config_dir))
            with open(target, "w", encoding="utf-8") as handle:
                handle.write(after)
        log("")
        log("Wrote a migrated copy to %s" % args.out)
        return 0

    if args.in_place:
        # The .bak check is here, not at the top: a SECOND --in-place on an
        # already-migrated tree writes nothing and exits 0 (idempotence), while a run that
        # would overwrite an existing rollback copy is refused.
        def bak(path):
            return path + (BARS_BAK_SUFFIX if path in upgrades else BAK_SUFFIX)
        for path in written:
            if os.path.exists(bak(path)):
                print("refused: %s already exists; this run would overwrite the rollback copy"
                      % bak(path), file=sys.stderr)
                return 2
        for path, (before, after) in sorted(written.items()):
            shutil.copy2(path, bak(path))
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(after)
        log("")
        log("Wrote %d file(s) in place; the %s copies are the rollback."
            % (len(written), " / ".join(sorted({bak(p)[len(p):] for p in written}))))
        return 0

    log("")
    log("Dry run: %d file(s) would change. Re-run with --in-place to write them."
        % len(written))
    return 10


def _copy_tree(src, dst):
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst)


if __name__ == "__main__":
    sys.exit(main())
