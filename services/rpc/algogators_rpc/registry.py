"""The service registry: what the shared server runs.

A service module builds one `Service` and calls `Registry.register`. The server then mounts its
servicer, reports its health under its full name, checks the version header against its version
and runs its background tasks. Nothing in the shared layer knows about any one service.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Tuple

from .versioning import VersionError, parse_semver


class RegistryError(RuntimeError):
    """A service that cannot be registered (duplicate, bad version, broken naming rule)."""


@dataclass(frozen=True)
class BackgroundTask:
    """Periodic work owned by a service. `on_start` runs once, before the server accepts calls;
    `run` then repeats every `interval_s` in a daemon thread until shutdown. Exceptions from
    either are logged and swallowed: a background task never stops the server."""

    name: str
    interval_s: float
    run: Callable[[], Any]
    on_start: Optional[Callable[[], Any]] = None


@dataclass(frozen=True)
class Service:
    # The API name: the proto file proto/algogators/<api>.proto, package algogators.<api>.
    api: str
    # The API version, from the generated algogators.versions.API_VERSIONS[api].
    version: str
    # The generated ServiceDescriptor (e.g. desk_pb2.DESCRIPTOR.services_by_name["DeskService"]).
    descriptor: Any
    servicer: Any
    # The generated add_<Name>Servicer_to_server(servicer, server).
    add_to_server: Callable[[Any, Any], None]
    background_tasks: Tuple[BackgroundTask, ...] = ()
    # Earlier full names served by the same servicer, for clients that predate a rename. Calls
    # on them skip the version check (those clients send no header). Unary-unary RPCs only.
    legacy_names: Tuple[str, ...] = ()

    @property
    def full_name(self) -> str:
        return self.descriptor.full_name


class Registry:
    def __init__(self) -> None:
        self._services: Dict[str, Service] = {}
        self._names: Dict[str, Tuple[Service, bool]] = {}

    def register(self, service: Service) -> Service:
        if service.api in self._services:
            raise RegistryError(f"API '{service.api}' is already registered")
        try:
            parse_semver(service.version)
        except VersionError as exc:
            raise RegistryError(f"API '{service.api}': version {exc}") from None
        package = service.descriptor.file.package
        if package != f"algogators.{service.api}":
            raise RegistryError(f"API '{service.api}': package is '{package}', "
                                f"must be 'algogators.{service.api}'")
        for name in (service.full_name, *service.legacy_names):
            if name in self._names:
                raise RegistryError(f"service name '{name}' is already registered")
        for task in service.background_tasks:
            if task.interval_s <= 0:
                raise RegistryError(f"background task '{task.name}': interval must be > 0")
        self._services[service.api] = service
        self._names[service.full_name] = (service, False)
        for name in service.legacy_names:
            self._names[name] = (service, True)
        return service

    def services(self) -> List[Service]:
        return list(self._services.values())

    def lookup(self, full_name: str) -> Optional[Tuple[Service, bool]]:
        """(service, is_legacy_name) for a gRPC service full name, or None."""
        return self._names.get(full_name)

    def versions(self) -> Dict[str, str]:
        return {s.api: s.version for s in self._services.values()}

    def health_names(self) -> List[str]:
        names = []
        for s in self._services.values():
            names.append(s.full_name)
            names.extend(s.legacy_names)
        return names
