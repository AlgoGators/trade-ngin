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
