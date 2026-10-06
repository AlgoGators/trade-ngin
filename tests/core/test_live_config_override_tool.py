"""Real process contract: success, native hash bytes, export and refusal/redaction."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

VALIDATOR = os.environ.get("LIVE_CONFIG_VALIDATE", "/tmp/issue88-live-config-validate")
ROOT = Path(__file__).resolve().parents[2]

class NativeValidationTool(unittest.TestCase):
    def run_tool(self, payload=b"", args=()):
        proc = subprocess.run([VALIDATOR, *args], input=payload, capture_output=True, check=False)
        self.assertEqual(proc.stderr, b"")
        self.assertEqual(len(proc.stdout.splitlines()), 1)
        return proc, json.loads(proc.stdout)

    def write_schema2_defaults(self, config):
        defaults = json.loads((ROOT/"config_template/defaults.json").read_text())
        defaults.pop("database", None)
        defaults.pop("risk_defaults", None)
        defaults["strategy_defaults"].pop("use_risk_management", None)
        defaults["strategy_defaults"].pop("use_optimization", None)
        (config/"defaults.json").write_text(json.dumps(defaults))

    def export(self, portfolio="conservative"):
        with tempfile.TemporaryDirectory() as temp:
            config = Path(temp)
            book = config/"portfolios"/portfolio
            book.mkdir(parents=True)
            self.write_schema2_defaults(config)
            for filename in ("portfolio.json", "risk.json"):
                (book/filename).write_bytes((ROOT/"config_template/portfolios"/portfolio/filename).read_bytes())
            proc, snapshot = self.run_tool(args=("--export-base", "--config-root", temp, "--portfolio", portfolio))
        self.assertEqual(proc.returncode, 0, snapshot)
        self.assertEqual(snapshot["snapshot_version"], 2)
        return proc.stdout.strip(), snapshot

    def test_export_and_valid_request_hash_native_bytes(self):
        raw, base = self.export()
        request = {"schema":"live-config-validation/v1", "base_snapshot":base,
                   "changes":{"/optimization/cost_penalty_scalar":12.75}}
        proc, result = self.run_tool(json.dumps(request).encode())
        self.assertEqual(proc.returncode, 0, result)
        self.assertEqual(result["base_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(result["effective_snapshot"]["max_drawdown"], .3)
        self.assertEqual(result["effective_snapshot"]["max_leverage"], 2)
        self.assertEqual(result["effective_snapshot"]["optimization"]["cost_penalty_scalar"], 12.75)
        # Independent simple-values fixture bytes: sorted keys, retained native floats.
        canonical = json.dumps(result["effective_snapshot"], sort_keys=True, separators=(",",":"), ensure_ascii=False).encode()
        self.assertEqual(result["effective_sha256"], hashlib.sha256(canonical).hexdigest())

    def test_export_equity_without_credentials_or_email_file(self):
        _, base = self.export("equity_mr")
        self.assertNotIn("database",base)
        self.assertNotIn("email",base)
        with tempfile.TemporaryDirectory() as temp:
            config = Path(temp)
            book = config/"portfolios"/"book"
            book.mkdir(parents=True)
            self.write_schema2_defaults(config)
            for filename in ("portfolio.json","risk.json"):
                (book/filename).write_bytes((ROOT/"config_template/portfolios/equity_mr"/filename).read_bytes())
            proc,result=self.run_tool(args=("--export-base","--config-root",temp,"--portfolio","book"))
            self.assertEqual(proc.returncode,0,result)
            self.assertEqual(result["risk"]["schema"],2)

    def test_wire_duplicate_nonfinite_depth_oversize_and_redaction(self):
        bad = [b'{"changes":{},"changes":{}}', b'{"nested":{"a":1,"a":2}}',
               b'{"x":NaN}',b'{"x":1e999}',b" "*(1024*1024+1),
               b"["*80+b"0"+b"]"*80]
        for payload in bad:
            proc, result = self.run_tool(payload)
            self.assertEqual(proc.returncode,2,result)
            self.assertIn("error",result)
        _,base=self.export()
        request={"schema":"live-config-validation/v1","base_snapshot":base,
                 "changes":{"/database/password":"never-echo-this-value"}}
        proc,result=self.run_tool(json.dumps(request).encode())
        self.assertEqual(proc.returncode,2)
        self.assertNotIn("never-echo-this-value",proc.stdout.decode())
        self.assertEqual(result["error"]["code"],"live_config_path_denied")

    def test_protocol_has_no_paths_and_cli_refuses_unknown_or_traversal(self):
        for args in (("--config-root","/tmp"),("--actor","123"),
                     ("--export-base","--config-root","/tmp","--portfolio","../book")):
            proc,result=self.run_tool(args=args)
            self.assertEqual(proc.returncode,2,result)
        _,base=self.export()
        request={"schema":"live-config-validation/v1","base_snapshot":base,"changes":{"/optimization/tau":1.2},"config_root":"/tmp"}
        proc,result=self.run_tool(json.dumps(request).encode())
        self.assertEqual(proc.returncode,2,result)

if __name__ == "__main__":
    unittest.main()
