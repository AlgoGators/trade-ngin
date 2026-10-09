"""Map a portfolio_id to its config directory under <TRADING_CONFIG_DIR>/portfolios/.

The engine takes `--portfolio-config <dir>`, the directory name, not the id. The mapping is found
by scanning every <dir>/portfolio.json for `"portfolio_id"`. No match, or more than one, is a
refusal: the agent never guesses which book to run (ruling 24).
"""

from __future__ import annotations

import json
from pathlib import Path


class PortfolioConfigError(RuntimeError):
    """No unique config directory for the portfolio. The message is safe to record."""


def portfolios_root(config_dir: str) -> Path:
    return Path(config_dir) / "portfolios"


def resolve_portfolio_dir(config_dir: str, portfolio_id: str) -> str:
    root = portfolios_root(config_dir)
    if not root.is_dir():
        raise PortfolioConfigError(f"no portfolio config directory {root}")
    matches = []
    for path in sorted(root.glob("*/portfolio.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if isinstance(data, dict) and data.get("portfolio_id") == portfolio_id:
            matches.append(path.parent.name)
    if not matches:
        raise PortfolioConfigError(
            f"no portfolio config under {root} has portfolio_id {portfolio_id}")
    if len(matches) > 1:
        raise PortfolioConfigError(
            f"portfolio_id {portfolio_id} is claimed by more than one config directory under "
            f"{root}: {', '.join(matches)}")
    return matches[0]
