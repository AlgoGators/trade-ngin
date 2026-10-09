"""SubprocessRunner and the spawned command line, with a real fake engine script under flock.
POSIX only (flock(1) and sh); skipped elsewhere."""

import os
import shutil
import stat
import time

import pytest

from conftest import PDIR, PID, make_row
from algogators_rpc.services.desk.config import CommandSettings
from algogators_rpc.services.desk.jobs import JobManager, SubprocessRunner, engine_argv, tail

posix_with_flock = pytest.mark.skipif(os.name != "posix" or shutil.which("flock") is None,
                                      reason="needs flock(1) and sh")


def test_engine_argv_shape():
    s = CommandSettings(lock_dir="/tmp/qt-locks", engine_binary="/bin/eng")
    assert engine_argv(s, "desk", PID, PDIR, "2026-10-07", 9) == [
        "flock", f"/tmp/qt-locks/{PID}.lock", "/bin/eng", "--desk", "--portfolio-config", PDIR,
        "--date", "2026-10-07", "--audit-id", "9"]
    with pytest.raises(ValueError):
        engine_argv(s, "model", PID, PDIR, "2026-10-07", 9)


def test_tail_keeps_the_last_lines():
    assert tail("".join(f"{i}\n" for i in range(50)), 3) == ["47", "48", "49"]
    assert tail("") == []


def fake_engine(tmp_path, body):
    path = tmp_path / "fake_engine.sh"
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IEXEC)
    return str(path)


@posix_with_flock
def test_real_spawn_records_the_command_line(tmp_path, cstore):
    out = tmp_path / "argv.txt"
    binary = fake_engine(tmp_path, f'pwd > {out}\necho "$@" >> {out}\necho bye >&2\nexit 3\n')
    cwd = tmp_path / "app"
    cwd.mkdir()
    s = CommandSettings(engine_binary=binary, engine_cwd=str(cwd),
                        lock_dir=str(tmp_path / "locks"), job_timeout_s=20)
    cstore.add(make_row(5, "save", status="running"))
    jobs = JobManager(cstore, s)
    jobs.submit(jobs.engine_job("desk", PID, PDIR, "2026-10-07", 5, "save"))
    jobs.wait_idle()
    lines = out.read_text().splitlines()
    assert lines == [str(cwd), f"--desk --portfolio-config {PDIR} --date 2026-10-07 --audit-id 5"]
    assert (tmp_path / "locks" / f"{PID}.lock").exists()
    row = cstore.rows[5]
    assert row.status == "failed"
    assert row.message == "engine exited 3 without recording an outcome: bye"


@posix_with_flock
def test_real_timeout_kills_the_process_group(tmp_path):
    binary = fake_engine(tmp_path, "sleep 30\n")
    argv = ["flock", str(tmp_path / "x.lock"), binary]
    started = time.monotonic()
    res = SubprocessRunner().run(argv, str(tmp_path), timeout=0.5)
    assert res.timed_out and time.monotonic() - started < 10


def test_missing_binary_is_an_error_not_a_crash(tmp_path):
    res = SubprocessRunner().run([str(tmp_path / "nope")], str(tmp_path), timeout=5)
    assert res.rc is None and res.error


@posix_with_flock
def test_lock_is_shared_with_another_holder(tmp_path):
    # The cron model run holds the same lock: the engine waits for it.
    lock = tmp_path / f"{PID}.lock"
    import subprocess
    holder = subprocess.Popen(["flock", str(lock), "sleep", "1"])
    time.sleep(0.2)
    binary = fake_engine(tmp_path, "exit 0\n")
    started = time.monotonic()
    res = SubprocessRunner().run(["flock", str(lock), binary], str(tmp_path), timeout=10)
    holder.wait()
    assert res.rc == 0 and time.monotonic() - started >= 0.5


