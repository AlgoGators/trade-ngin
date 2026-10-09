import datetime as dt

import grpc
import pytest

from desk_agent.server import build_server
from desk_agent.store import RunFacts, StoreError

UTC = dt.timezone.utc


class FakeStore:
    """The servicer's only DB dependency, scripted per test."""

    def __init__(self):
        self.facts = {}
        self.error = None
        self.calls = []

    def run_facts(self, portfolio_id, date):
        self.calls.append((portfolio_id, date))
        if self.error:
            raise StoreError(self.error)
        return self.facts.get((portfolio_id, date), RunFacts())


@pytest.fixture
def store():
    return FakeStore()


@pytest.fixture
def channel(store):
    server, _, port = build_server(store, "127.0.0.1:0", max_workers=2)
    server.start()
    with grpc.insecure_channel(f"127.0.0.1:{port}") as ch:
        yield ch
    server.stop(grace=None)
