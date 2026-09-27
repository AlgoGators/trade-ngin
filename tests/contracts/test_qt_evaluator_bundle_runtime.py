"""Actual closed ELF bundle through the reviewed bounded API transport."""
from contextlib import contextmanager
import errno
import fcntl
from hashlib import sha256
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from qt_native_bundle_fixture import ROOT, BUILD_ID, stage_actual_bundle
sys.path.insert(0, str(ROOT.parent / "algolens-qt/algolens-api"))
from algolens.infrastructure.portfolio.qt_evaluator_bundle import QtEvaluatorBundle, QtEvaluatorBundleUnavailable
from algolens.infrastructure.portfolio.qt_evaluator_process import QtEvaluatorProcess, QtEvaluatorUnavailable
from algolens.infrastructure.portfolio.qt_evaluator_client import QtEvaluatorClient


@contextmanager
def altered_byte(path):
    path.chmod(0o600)
    with path.open("r+b") as stream:
        stream.seek(-1,2); offset=stream.tell(); old=stream.read(1)
        stream.seek(offset);stream.write(bytes([old[0]^1]));stream.flush()
    try: yield
    finally:
        with path.open("r+b") as stream: stream.seek(offset);stream.write(old)
        path.chmod(0o400)


class ActualBundleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.owned = tempfile.TemporaryDirectory(prefix="qt-native-bundle-gate-")
        cls.directory = Path(cls.owned.name) / "bundle"
        cls.manifest = stage_actual_bundle(cls.directory)
        cls.pin = next(row["sha256"] for row in cls.manifest["artifacts"] if row["role"]=="executable")
        cls.request = json.loads((ROOT / "tests/contracts/qt-eval-v1.json").read_text())["selected_book"]
        print("BUNDLE_SHA256="+cls.manifest["bundle_sha256"])
        print("BUNDLE_ARTIFACTS="+str(len(cls.manifest["artifacts"])))
        print("BUNDLE_MANIFEST="+json.dumps(cls.manifest,sort_keys=True,separators=(",",":")))

    @classmethod
    def tearDownClass(cls): cls.owned.cleanup()

    def bundle(self):
        return QtEvaluatorBundle(self.directory,self.manifest["bundle_sha256"],self.pin,BUILD_ID)

    def process(self):
        return QtEvaluatorProcess(self.directory / "bin/qt_evaluator",self.pin,BUILD_ID,
            bundle_directory=self.directory,expected_bundle_sha256=self.manifest["bundle_sha256"])

    def test_actual_selected_book_through_bundle_and_semantic_admission(self):
        result = QtEvaluatorClient(self.process()).evaluate(self.request)
        self.assertTrue(result.available)
        self.assertEqual(result.evidence["selected_costs"]["total_exact"],"0.02")
        self.assertEqual(result.book_digest,"170528e9e3a72c56773ceeba05ed1762eb4a35ff1a35d0af8633e35039ee99df")

    def test_actual_loader_resolves_only_sealed_closure_and_all_artifacts_are_sealed(self):
        with self.bundle().launch() as launch:
            self.assertEqual(len(launch.pass_fds),len(self.manifest["artifacts"]))
            self.assertEqual(len(set(launch.pass_fds)),len(launch.pass_fds))
            for descriptor in launch.pass_fds:
                expected=fcntl.F_SEAL_WRITE|fcntl.F_SEAL_GROW|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_SEAL
                self.assertEqual(fcntl.fcntl(descriptor,fcntl.F_GET_SEALS)&expected,expected)
                with self.assertRaises(OSError) as refused: os.pwrite(descriptor,b"x",0)
                self.assertEqual(refused.exception.errno,errno.EPERM)
            listed=subprocess.run([*launch.argv[:-1],"--list",launch.argv[-1]],
                capture_output=True,text=True,timeout=15,pass_fds=launch.pass_fds,
                env={"PATH":"/usr/bin:/bin","LC_ALL":"C","TZ":"UTC"})
            self.assertEqual(listed.returncode,0,listed.stderr)
            self.assertEqual(listed.stderr,"")
            self.assertNotIn("/home/devcontainers",listed.stdout)
            for line in listed.stdout.splitlines():
                if "=>" in line:
                    resolved=line.split("=>",1)[1].strip().split()[0]
                    self.assertTrue(resolved.startswith("/proc/self/fd/"),line)
            # After admission, replacing the deployment core cannot alter this
            # invocation's sealed native engine code.
            with altered_byte(self.directory / "lib/libtrade_ngin.so"):
                with tempfile.TemporaryDirectory(prefix="qt-native-run-") as cwd:
                    completed=subprocess.run(launch.argv,input=json.dumps(self.request),capture_output=True,
                        text=True,timeout=15,pass_fds=launch.pass_fds,cwd=cwd,
                        env={"PATH":"/usr/bin:/bin","TZ":"UTC"})
                    self.assertEqual(completed.returncode,0,completed.stderr)
                    self.assertEqual(completed.stderr,"")
                    self.assertEqual(json.loads(completed.stdout)["completeness"],"complete")
                    self.assertEqual(list(Path(cwd).iterdir()),[])
        for descriptor in launch.pass_fds:
            with self.assertRaises(OSError) as closed: os.fstat(descriptor)
            self.assertEqual(closed.exception.errno,errno.EBADF)

    def test_private_crypto_replacement_after_launch_cannot_change_loaded_bytes(self):
        original_snapshot = QtEvaluatorBundle.snapshot
        original_spawn = subprocess.Popen
        private = []
        captured = {}

        @contextmanager
        def capture_snapshot(bundle):
            with original_snapshot(bundle) as snapshot:
                private.append(snapshot.directory)
                yield snapshot

        def replace_private_crypto_then_spawn(argv, **kwargs):
            crypto = private[-1] / "lib/libcrypto.so.3"
            crypto.chmod(0o600)
            crypto.write_bytes(crypto.read_bytes()+b"owned post-admission dependency race")
            captured["modified_digest"] = sha256(crypto.read_bytes()).hexdigest()
            captured["sealed_digests"] = []
            for descriptor in kwargs["pass_fds"]:
                size=os.fstat(descriptor).st_size
                captured["sealed_digests"].append(sha256(os.pread(descriptor,size,0)).hexdigest())
            listed=original_spawn([*argv[:-1],"--list",argv[-1]],
                stdout=subprocess.PIPE,stderr=subprocess.PIPE,pass_fds=kwargs["pass_fds"],
                env=kwargs["env"],cwd=kwargs["cwd"])
            listing,error=listed.communicate(timeout=15)
            self.assertEqual(listed.returncode,0,error.decode())
            captured["listing"]=listing.decode()
            captured["spawned"]=True
            return original_spawn(argv,**kwargs)

        with patch.object(QtEvaluatorBundle,"snapshot",capture_snapshot), patch(
                "algolens.infrastructure.portfolio.qt_evaluator_process.subprocess.Popen",
                side_effect=replace_private_crypto_then_spawn):
            result=QtEvaluatorClient(self.process()).evaluate(self.request)
        self.assertTrue(result.available, result.unavailable_reasons)
        self.assertTrue(captured["spawned"],"Race must reach the actual native loader and evaluator")
        expected=next(row["sha256"] for row in self.manifest["artifacts"] if row["name"]=="libcrypto.so.3")
        self.assertNotEqual(captured["modified_digest"],expected)
        self.assertIn(expected,captured["sealed_digests"],"Crypto must use byte-pinned sealed backing after admission")
        self.assertNotIn(str(private[-1]),captured["listing"],"No native dependency may reopen a mutable private path")
        self.assertFalse(private[-1].exists())

    def test_dependency_changed_during_sealing_refuses_and_closes_complete_owned_state(self):
        original_snapshot=QtEvaluatorBundle.snapshot
        original_memfd=os.memfd_create
        private=[];descriptors=[]

        @contextmanager
        def changed_snapshot(bundle):
            with original_snapshot(bundle) as snapshot:
                private.append(snapshot.directory)
                crypto=snapshot.directory/"lib/libcrypto.so.3"
                crypto.chmod(0o600)
                crypto.write_bytes(crypto.read_bytes()+b"owned copy-time dependency change")
                yield snapshot

        def record_memfd(*args,**kwargs):
            descriptor=original_memfd(*args,**kwargs)
            descriptors.append(descriptor)
            return descriptor

        with patch.object(QtEvaluatorBundle,"snapshot",changed_snapshot), patch(
                "algolens.infrastructure.portfolio.qt_evaluator_bundle.os.memfd_create",side_effect=record_memfd):
            with self.assertRaises(QtEvaluatorBundleUnavailable):
                with self.bundle().launch(): self.fail("Changed dependency admitted")
        self.assertTrue(descriptors,"Failure must occur after actual owned descriptors are created")
        self.assertLessEqual(len(descriptors),len(self.manifest["artifacts"]))
        for descriptor in descriptors:
            with self.assertRaises(OSError) as closed: os.fstat(descriptor)
            self.assertEqual(closed.exception.errno,errno.EBADF)
        self.assertFalse(private[-1].exists())

    def test_changed_crypto_or_core_with_same_binary_refuses_before_spawn(self):
        for name in ["libcrypto.so.3","libtrade_ngin.so"]:
            with altered_byte(self.directory / "lib" / name):
                self.assertEqual(sha256((self.directory / "bin/qt_evaluator").read_bytes()).hexdigest(),self.pin)
                with patch("algolens.infrastructure.portfolio.qt_evaluator_process.subprocess.Popen") as spawn:
                    with self.assertRaisesRegex(QtEvaluatorUnavailable,"evaluator_bundle_unavailable"):
                        self.process().run(self.request)
                    spawn.assert_not_called()


if __name__ == "__main__": unittest.main()
