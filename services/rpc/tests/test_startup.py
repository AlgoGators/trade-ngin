"""D-39: engine-rpc does not start without both override approvers (QT_APPROVERS)."""

import pytest

from algogators_rpc.registry import Registry
from algogators_rpc.services.desk import service
from algogators_rpc.services.desk.config import ConfigError

ENV = {"DB_HOST": "db", "DB_PORT": "5432", "DB_USER": "u", "DB_PASSWORD": "pw",
       "DB_NAME": "new_algo_data", "QT_CATCHUP_ENABLED": "0"}


@pytest.mark.parametrize("approvers", ["", "vp=vp@x.org", "president=p@x.org", "vp=,president="])
def test_the_server_refuses_to_start_without_both_approvers(approvers):
    env = dict(ENV, **({"QT_APPROVERS": approvers} if approvers else {}))
    with pytest.raises(ConfigError, match="does not start without the approvers"):
        service.register(Registry(), env)


def test_the_server_starts_with_both_approvers():
    env = dict(ENV, QT_APPROVERS="vp=vp@x.org,president=p@x.org")
    assert service.register(Registry(), env) is not None


class FlagStore:
    def __init__(self, flag=None, error=None):
        self.flag, self.error, self.asked = flag, error, []

    def registry_desk_editable(self, portfolio_id):
        self.asked.append(portfolio_id)
        if self.error:
            raise service.StoreError(self.error)
        return self.flag


@pytest.fixture
def desk_config(tmp_path):
    from conftest import PDIR, PID, write_portfolio

    d = write_portfolio(tmp_path)
    (d / "portfolio.json").write_text('{"portfolio_id": "%s", "qt": {"desk_editable": true}}' % PID)
    return str(tmp_path), PDIR, PID


def test_the_server_refuses_to_start_when_the_desk_flags_disagree(desk_config):
    # D-52: portfolio.json says desk-editable, the registry (AlgoLens) says not.
    config_dir, pdir, pid = desk_config
    with pytest.raises(ConfigError, match="desk editing disagrees for " + pid):
        service.check_desk_editable(FlagStore(False), config_dir, [pdir])


@pytest.mark.parametrize("store", [FlagStore(True), FlagStore(None), FlagStore(error="down")])
def test_agreeing_flags_a_missing_row_or_an_unreadable_registry_start(desk_config, store):
    config_dir, pdir, pid = desk_config
    service.check_desk_editable(store, config_dir, [pdir])
    assert store.asked == [pid]
