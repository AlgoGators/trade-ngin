import json

import pytest

from algogators_rpc.services.desk.config import ConfigError, load_settings

ENV = {"DB_HOST": "db.internal", "DB_PORT": "5433", "DB_USER": "agent",
       "DB_PASSWORD": "hunter2-long", "DB_NAME": "new_algo_data"}


def test_env_and_password_not_described():
    s = load_settings(ENV)
    assert s.db.conninfo_kwargs()["port"] == "5433"
    assert "hunter2" not in s.db.describe() and "hunter2" not in repr(s.db)


def test_partial_env_refuses():
    with pytest.raises(ConfigError, match="DB_PASSWORD"):
        load_settings({k: v for k, v in ENV.items() if k != "DB_PASSWORD"})


def test_defaults_json_like_the_cpp_runners(tmp_path):
    (tmp_path / "defaults.json").write_text(json.dumps({"database": {
        "host": "h", "port": "5432", "username": "u", "password": "p" * 10, "name": "n"}}))
    s = load_settings({"TRADING_CONFIG_DIR": str(tmp_path)})
    kw = s.db.conninfo_kwargs()
    assert (kw["host"], kw["user"], kw["dbname"]) == ("h", "u", "n")
    assert "p" * 10 not in s.db.describe()


def test_no_credentials_refuses(tmp_path):
    with pytest.raises(ConfigError, match="no database credentials"):
        load_settings({"TRADING_CONFIG_DIR": str(tmp_path)})


def test_incomplete_defaults_json_refuses(tmp_path):
    (tmp_path / "defaults.json").write_text(json.dumps({"database": {"host": "h"}}))
    with pytest.raises(ConfigError, match="missing"):
        load_settings({"TRADING_CONFIG_DIR": str(tmp_path)})
