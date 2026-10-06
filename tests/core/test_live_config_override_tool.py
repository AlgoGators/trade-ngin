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

    def test_unmodified_schema2_templates_export(self):
        for portfolio in ("base", "conservative", "equity_mr"):
            with self.subTest(portfolio=portfolio):
                proc, snapshot = self.run_tool(args=("--export-base", "--config-root", str(ROOT/"config_template"), "--portfolio", portfolio))
                self.assertEqual(proc.returncode, 0, snapshot)
                self.assertEqual(snapshot["snapshot_version"], 2)
                self.assertEqual(snapshot["risk"]["schema"], 2)

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

    def test_actual_consumer_refusals(self):
        _,base=self.export()
        selected=[key for key,value in base["strategies"].items() if value.get("enabled_live",False)]
        for changes in ({"/strategy_defaults/fdm":[[1,9.0]]},):
            proc,result=self.run_tool(json.dumps({"schema":"live-config-validation/v1","base_snapshot":base,"changes":changes}).encode())
            self.assertEqual(proc.returncode,2,result)
        for key in selected:
            base["strategies"][key]["config"]={"vol_lookback_short":32,"carver_buffer_floor":.5,"carver_buffer_position_factor":0.0}
        for changes in ({"/strategy_defaults/carver_buffer_floor":.8},
                        {"/strategy_defaults/carver_buffer_position_factor":.8},
                        {f"/strategies/{selected[0]}/config/vol_lookback_short":2147483647}):
            proc,result=self.run_tool(json.dumps({"schema":"live-config-validation/v1","base_snapshot":base,"changes":changes}).encode())
            self.assertEqual(proc.returncode,2,result)
        base["strategies"]["OFF"]={"type":"TrendFollowingStrategy","enabled_live":False,"config":{"risk_target":.2}}
        base["sleeve_risk_modules"]["OFF"]=[{"id":"off_scale","type":"constant_scale","scale":.8,"every_lap":False}]
        for changes in ({"/sleeve_risk_modules/OFF/0/scale":.7},{"/optimization/tau":1.2}):
            proc,result=self.run_tool(json.dumps({"schema":"live-config-validation/v1","base_snapshot":base,"changes":changes}).encode())
            self.assertEqual(proc.returncode,2,result)

    def test_export_attributed_noncarver_roundtrip_and_tuning(self):
        with tempfile.TemporaryDirectory() as temp:
            config=Path(temp); book=config/"portfolios"/"book"; book.mkdir(parents=True)
            self.write_schema2_defaults(config)
            (book/"portfolio.json").write_bytes((ROOT/"config_template/portfolios/conservative/portfolio.json").read_bytes())
            risk=json.loads((ROOT/"config_template/portfolios/conservative/risk.json").read_text())
            risk["modules"]=[{"id":"scale","type":"constant_scale","scale":.8,"every_lap":False},
                             {"id":"warn","type":"warn","condition":{"kind":"lap_at_least","threshold":3},"reason":"fixture"}]
            risk["_ruled_by"]="unit test"; risk["_ruled_on"]="2026-10-06"
            (book/"risk.json").write_text(json.dumps(risk))
            proc,base=self.run_tool(args=("--export-base","--config-root",temp,"--portfolio","book"))
            self.assertEqual(proc.returncode,0,base)
        self.assertEqual(base["risk"]["_ruled_by"],"unit test")
        self.assertEqual(base["risk"]["_ruled_on"],"2026-10-06")
        proc,result=self.run_tool(json.dumps({"schema":"live-config-validation/v1","base_snapshot":base,
                                             "changes":{"/risk/modules/0/scale":.7}}).encode())
        self.assertEqual(proc.returncode,0,result)
        effective=result["effective_snapshot"]
        self.assertEqual(effective["risk"]["modules"][0]["scale"],.7)
        self.assertEqual(effective["risk"]["_ruled_by"],"unit test")
        self.assertEqual(effective["risk"]["_ruled_on"],"2026-10-06")
        self.assertEqual(effective["risk"]["modules"][1],base["risk"]["modules"][1])

    def test_explicit_baseline_validation_keeps_ordinary_noop_rejected(self):
        raw,base=self.export()
        request={"schema":"live-config-baseline-validation/v1","base_snapshot":base}
        proc,result=self.run_tool(json.dumps(request).encode())
        self.assertEqual(proc.returncode,0,result)
        self.assertEqual(result["schema"],"live-config-baseline-validation/v1")
        self.assertEqual(result["effective_snapshot"],base)
        self.assertEqual(result["base_sha256"],hashlib.sha256(raw).hexdigest())
        self.assertEqual(result["effective_sha256"],result["base_sha256"])
        self.assertEqual(result["changed_paths"],[])
        for changes in ({},{"/optimization/tau":base["optimization"]["tau"]}):
            proc,result=self.run_tool(json.dumps({"schema":"live-config-validation/v1",
                "base_snapshot":base,"changes":changes}).encode())
            self.assertEqual(proc.returncode,2,result)
            self.assertEqual(result["error"]["code"],"live_config_noop")

    def test_baseline_protocol_refuses_extra_keys_bad_base_and_unsafe_wire(self):
        _,base=self.export()
        request={"schema":"live-config-baseline-validation/v1","base_snapshot":base}
        bad=[dict(request,changes={}),dict(request,actor="never-echo-reset-secret"),
             dict(request,config_root="never-echo-reset-secret"),
             {"schema":"live-config-baseline-validation/v1"},
             dict(request,base_snapshot=dict(base,snapshot_version=1))]
        for value in bad:
            proc,result=self.run_tool(json.dumps(value).encode())
            self.assertEqual(proc.returncode,2,result)
            self.assertNotIn("never-echo-reset-secret",proc.stdout.decode())
        for payload in (b'{"schema":"live-config-baseline-validation/v1","base_snapshot":{},"base_snapshot":{}}',
                        b'{"schema":"live-config-baseline-validation/v1","base_snapshot":{"x":1e999}}',
                        b'{"schema":"live-config-baseline-validation/v1","base_snapshot":'+b'['*80+b'0'+b']'*80+b'}',
                        json.dumps(request).encode()+b' '*(1024*1024)):
            proc,result=self.run_tool(payload)
            self.assertEqual(proc.returncode,2,result)

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
