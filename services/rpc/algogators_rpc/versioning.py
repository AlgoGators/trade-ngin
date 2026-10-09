"""The API version header (docs/design/rpc.md, "Versioning").

Every call carries gRPC metadata

    x-algogators-api-version: <api>=<major>.<minor>.<patch>[,<api>=<semver>...]

The server accepts a call when the header names the called API with the same major version as
its own; minor and patch may differ in either direction. Anything else is refused with
FAILED_PRECONDITION and the message from `check`.
"""

from __future__ import annotations

import re
from typing import Dict, Mapping, Optional, Tuple

HEADER = "x-algogators-api-version"

_SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
_API = re.compile(r"^[a-z][a-z0-9_]*$")


class VersionError(ValueError):
    """A version or header value that does not parse."""


def parse_semver(value: str) -> Tuple[int, int, int]:
    match = _SEMVER.match(value.strip())
    if not match:
        raise VersionError(f"'{value}' is not <major>.<minor>.<patch>")
    return int(match.group(1)), int(match.group(2)), int(match.group(3))


def parse_header(value: str) -> Dict[str, str]:
    """'desk=1.0.0, other=2.1.0' -> {'desk': '1.0.0', 'other': '2.1.0'}."""
    out: Dict[str, str] = {}
    for part in value.split(","):
        part = part.strip()
        if not part:
            continue
        api, sep, version = part.partition("=")
        api, version = api.strip(), version.strip()
        if not sep or not _API.match(api):
            raise VersionError(f"'{part}' is not <api>=<major>.<minor>.<patch>")
        parse_semver(version)
        if api in out and out[api] != version:
            raise VersionError(f"API '{api}' is named twice with different versions")
        out[api] = version
    if not out:
        raise VersionError("empty value")
    return out


def format_header(versions: Mapping[str, str]) -> str:
    """{'desk': '1.0.0'} -> 'desk=1.0.0' (sorted by API, so the value is stable)."""
    return ",".join(f"{api}={versions[api]}" for api in sorted(versions))


def check(api: str, server_version: str, header_value: Optional[str]) -> Optional[str]:
    """None when the call is accepted, else the refusal message (safe to send to the client)."""
    speaks = f"this server speaks {api}={server_version}"
    if header_value is None or not header_value.strip():
        return (f"missing {HEADER} metadata: {speaks}; send "
                f"'{HEADER}: {api}=<major>.<minor>.<patch>'")
    try:
        sent = parse_header(header_value)
    except VersionError as exc:
        return f"malformed {HEADER} '{header_value}': {exc}; {speaks}"
    client_version = sent.get(api)
    if client_version is None:
        return f"{HEADER} '{header_value}' names no version for API '{api}': {speaks}"
    if parse_semver(client_version)[0] != parse_semver(server_version)[0]:
        return (f"API version mismatch for '{api}': client speaks {api}={client_version}, "
                f"server speaks {api}={server_version}; the major versions must match")
    return None
