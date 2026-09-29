"""Owned explicitly simulated S fixture; actual native preview/receipt retained."""
from copy import deepcopy
from concurrent.futures import ThreadPoolExecutor
from datetime import timedelta, timezone
from decimal import Decimal
from hashlib import sha256
from pathlib import Path
import json
import os
import subprocess
import sys
from uuid import UUID

import psycopg2
from psycopg2.extras import Json, RealDictCursor
import pytest

from test_runtime_control_schema import connection
from test_proposal_storage_migration import normalize_positions_shape, apply

from test_qt_desk_storage import ROOT, API, apply_registry_asset_class
sys.path.insert(0, str(API))
from algolens.domain.portfolio.qt_canonical import qt_digest_v1, qt_book_digest_v1
from algolens.infrastructure.portfolio.qt_read_set import (
    canonical_internal_snapshot_bytes, capture_qt_read_set, internal_snapshot_digest)
from algolens.infrastructure.portfolio.qt_workflow_repository import QtTransaction
from algolens.infrastructure.portfolio.qt_evaluation_inputs import canonical_qt_input_bytes, load_qt_evaluation_inputs
from algolens.infrastructure.portfolio.qt_evaluator_client import QtEvaluatorClient
from algolens.infrastructure.portfolio.qt_evaluator_process import QtEvaluatorProcess
from tests.qt_native_evaluator import native_evaluator_configuration

MODEL = "10000000-0000-4000-8000-000000000001"
DRAFT = "20000000-0000-4000-8000-000000000001"
PREVIEW = "30000000-0000-4000-8000-000000000001"
DECISION = "40000000-0000-4000-8000-000000000001"
ATTEMPT = "50000000-0000-4000-8000-000000000001"
OBSERVATION = "60000000-0000-4000-8000-000000000001"
BINARY = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/qt_desk_storage_probe")
EVALUATOR = BINARY.with_name("qt_evaluator")
BOOK = "EQUITY_MR_PORTFOLIO"


def exact(value):
    from algolens.domain.portfolio.position_decimal import canonical_position_decimal8
    return canonical_position_decimal8(Decimal(str(value)))


def utc(value):
    return value.isoformat().replace("+00:00", "Z")


def run(attempt=ATTEMPT, observation=OBSERVATION, *, decision=DECISION):
    assert BINARY.is_file(), "Actual native desk probe must be built"
    result = subprocess.run([str(BINARY), decision, attempt, observation],
        capture_output=True, text=True, timeout=30, env=os.environ.copy())
    return result


def report_proof(conn):
    with conn.cursor() as cur:
        cur.execute("SELECT source_day::text FROM trading.qt_decisions WHERE decision_id=%s",(DECISION,))
        day = cur.fetchone()[0]
    return subprocess.run([str(BINARY),"--report",BOOK,day], capture_output=True,
                          text=True,timeout=30,env=os.environ.copy())


def table_state(conn):
    with conn.cursor() as cur:
        cur.execute("SELECT to_jsonb(p)::text FROM trading.positions p ORDER BY "
                    "portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type")
        positions = cur.fetchall()
        values = []
        for table in ("qt_desk_receipts", "qt_desk_results", "position_overrides",
                      "live_results", "equity_curve"):
            cur.execute("SELECT to_jsonb(t)::text FROM trading." + table + " t ORDER BY to_jsonb(t)::text")
            values.append(cur.fetchall())
    return positions, values


@pytest.fixture()
def desk(connection, request, eod_clock):
    conn = connection
    scenario = getattr(request, "param", "executed")
    assert scenario in ("executed","carried"), "closed MR fixture scenario required"
    ticker = "MES" if scenario == "futures_mes" else "SYN"
    if scenario == "futures_mes": scenario = "futures"
    chosen = (("4",) if scenario == "carried" else ("5","0") if scenario == "zero"
              else ("-0.5",) if scenario == "fractional" else ("5",))
    normalize_positions_shape(conn)
    apply(conn, ROOT / "migrations/015_qt_proposal_positions.sql")
    apply(conn, ROOT / "migrations/016_qt_exact_precision_and_seed_provenance.sql")
    with conn.cursor() as cur:
        cur.execute("DROP SCHEMA IF EXISTS auth CASCADE; CREATE SCHEMA auth;"
                    "CREATE TABLE auth.users(id bigint PRIMARY KEY,role text NOT NULL);"
                    "INSERT INTO auth.users VALUES(1,'general_member'),(2,'general_member'),(3,'admin');"
                    "ALTER TABLE trading.strategy_registry ADD COLUMN updated_at timestamptz NOT NULL DEFAULT now();"
                    "DELETE FROM trading.strategy_registry;"
                    "INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id) VALUES('LIVE_EQUITY_MEAN_REVERSION','LIVE_EQUITY_MEAN_REVERSION','EQUITY_MR_PORTFOLIO');"
                    "INSERT INTO trading.strategy_book_memberships(strategy_id,portfolio_id) VALUES('LIVE_EQUITY_MEAN_REVERSION','EQUITY_MR_PORTFOLIO')")
    apply(conn, API / "migrations/003_qt_decision_workflow.sql")
    apply(conn, API / "migrations/004_qt_governed_sources.sql")
    apply(conn, API / "migrations/005_qt_evaluator_bundle.sql")
    apply_registry_asset_class(conn, "FUTURE" if scenario == "futures" else "EQUITY")
    with conn.cursor() as cur:
        cur.execute("SELECT clock_timestamp()")
        now = cur.fetchone()[0].astimezone(timezone.utc)
    day = now.date().isoformat()
    stamp = utc(now)
    keys = [dict(portfolio_id=BOOK,strategy_id="LIVE_EQUITY_MEAN_REVERSION",strategy_name=name,
                 date=day,symbol=ticker,portfolio_type="qt_proposal")
            for name in ("EQUITY_MEAN_REVERSION",)]
    if scenario.startswith("new"):
        keys = [keys[0],{**keys[0],"symbol":"NEW"}]
        if scenario == "new_foreign": keys[1]["strategy_id"] = "FOREIGN"
        if scenario == "new_unknown_owner": keys[1]["strategy_name"] = "unregistered-component"
        chosen = ("5","3")
    mixed = scenario == "mixed"
    if mixed:
        keys[1] = {**keys[0],"strategy_id":"IMMUTABLE","strategy_name":"held-component","symbol":"IMM"}
        chosen = ("5","8")
        with conn.cursor() as cur:
            cur.execute("INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id,is_active,lifecycle,asset_class) "
                        "VALUES('IMMUTABLE','IMMUTABLE','EQUITY_MR_PORTFOLIO',true,'live','EQUITY')")
            cur.execute("INSERT INTO trading.positions(portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,"
                        "quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update) "
                        "VALUES('EQUITY_MR_PORTFOLIO','IMMUTABLE','held-component',%s,'IMM','qt',8,25,1.25,-0.5,%s)",(day,now))
    seed_rows = []
    proposal_rows = []
    with conn.cursor() as cur:
        for key, quantity in zip(keys[:1] if scenario.startswith("new") or mixed else keys, ("4","2")):
            for stream in ("system", "qt_proposal", "qt"):
                cur.execute("""INSERT INTO trading.positions
                    (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
                     quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
                    VALUES(%s,%s,%s,%s,%s,%s,%s,100,1.25,-0.5,%s)""",
                    (BOOK,"LIVE_EQUITY_MEAN_REVERSION",key["strategy_name"],day,ticker,stream,quantity,now))
            cur.execute("SELECT qt_proposal_revision::text FROM trading.positions WHERE "
                        "portfolio_id=%s AND strategy_name=%s AND date=%s AND portfolio_type='qt_proposal'",
                        (BOOK,key["strategy_name"],day))
            revision = cur.fetchone()[0]
            assert revision is not None
            seed_rows.append(dict(key={**key,"portfolio_type":"system"},
                                  quantity_exact=quantity,average_price_exact="100"))
            proposal_rows.append(dict(key=key,quantity_exact=quantity,average_price_exact="100",
                action="inserted",position_revision=revision,origin_publication_id=MODEL))
        seed_digest = qt_digest_v1({"seed_rows": seed_rows})
        manifest_digest = internal_snapshot_digest("qt-proposal-manifest/v1",{"proposal_rows":proposal_rows})
        cur.execute("""INSERT INTO trading.qt_model_seed_publications
            (publication_id,portfolio_id,strategy_id,source_day,publication_version,
             system_components,seed_digest,proposal_components,proposal_manifest_digest,producer_version)
            VALUES(%s,%s,'LIVE_EQUITY_MEAN_REVERSION',%s,1,%s,%s,%s,%s,'owned-native-desk-fixture')""",
            (MODEL,BOOK,day,Json(seed_rows),seed_digest,Json(proposal_rows),manifest_digest))
        cur.execute("INSERT INTO trading.qt_workflow_capabilities(book_id,enabled,version) VALUES('EQUITY_MR_PORTFOLIO',true,1)")
        cur.execute("INSERT INTO trading.qt_action_grants(user_id,capability,active,version) "
                    "VALUES(1,'qt_submit',true,1),(2,'qt_approve',true,1),(3,'qt_approve',true,1)")
        cur.execute("INSERT INTO trading.qt_approver_allowlist(person_id,display_label,user_id,active,mapping_version) "
                    "VALUES('john_riley','john riley',2,true,1),('xander_robbins','xander robbins',3,true,1)")

    request = json.loads((ROOT / "tests/contracts/qt-eval-v1.json").read_text())["selected_book"]
    request["context"]["slots"] = request["context"]["slots"][:1]
    request["proposal"]["quantities"] = request["proposal"]["quantities"][:1]
    request["component_cost_inputs"] = request["component_cost_inputs"][:1]
    if ticker != "SYN": request = json.loads(json.dumps(request).replace('"SYN"', json.dumps(ticker)))
    asset_type = "FUTURE" if scenario == "futures" else "EQUITY"
    if scenario == "futures":
        request = json.loads(json.dumps(request).replace('"EQUITY"', '"FUTURE"'))
        for rule in request["quantity_rules"]: rule["increment_exact"] = "1"
    if scenario.startswith("new"):
        request["context"]["slots"] = request["context"]["slots"][:1]
        request["proposal"]["quantities"] = request["proposal"]["quantities"][:1]
        request["component_cost_inputs"] = request["component_cost_inputs"][:1]
        slot = deepcopy(request["context"]["slots"][0])
        slot.update(key=keys[1],previous=None)
        slot["instrument"]["symbol"] = "NEW"
        request["context"]["slots"].append(slot)
        request["proposal"]["quantities"].append(dict(key=keys[1],quantity_exact="3"))
        cost = deepcopy(request["component_cost_inputs"][0])
        cost["key"] = keys[1]
        cost["instrument"]["symbol"] = "NEW"
        request["component_cost_inputs"].append(cost)
        rule = deepcopy(request["quantity_rules"][0])
        rule["instrument"]["symbol"] = "NEW"
        request["quantity_rules"].append(rule)
        mark = deepcopy(request["risk_inputs"]["valuations"][0])
        mark["instrument"]["symbol"] = "NEW"
        request["risk_inputs"]["valuations"].append(mark)
        for close in deepcopy(request["risk_inputs"]["closes"]):
            close["instrument"]["symbol"] = "NEW"
            request["risk_inputs"]["closes"].append(close)
        request["risk_config"]["max_correlation"] = "1"
    if mixed:
        request["context"]["slots"][1].update(editable=False)
        request["context"]["slots"][1]["instrument"]["symbol"] = "IMM"
        request["context"]["slots"][1]["previous"].update(symbol="IMM",quantity_exact="8",average_price_exact="25")
        request["proposal"]["quantities"] = request["proposal"]["quantities"][:1]
        request["component_cost_inputs"] = request["component_cost_inputs"][:1]
        for field in ("quantity_rules",):
            item=deepcopy(request[field][0]); item["instrument"]["symbol"]="IMM"; request[field].append(item)
        mark=deepcopy(request["risk_inputs"]["valuations"][0]); mark["instrument"]["symbol"]="IMM"
        request["risk_inputs"]["valuations"].append(mark)
        for close in deepcopy(request["risk_inputs"]["closes"]):
            close["instrument"]["symbol"]="IMM"; request["risk_inputs"]["closes"].append(close)
        request["risk_config"]["max_correlation"]="1"
    request["context"].update(portfolio_id=BOOK,date=day,revision=DRAFT)
    request["proposal"].update(expected_portfolio_id=BOOK,expected_date=day,expected_revision=DRAFT)
    request["risk_inputs"].update(expected_portfolio_id=BOOK,expected_date=day,
                                  expected_revision=DRAFT,valuation_time=stamp)
    for row, key in zip(request["context"]["slots"], keys):
        row["key"] = key
        if row["previous"] is not None:
            row["previous"].update(unrealized_pnl_exact="1.25",realized_pnl_exact="-0.5",last_update=stamp)
    for row, key in zip(request["proposal"]["quantities"], keys):
        row["key"] = key
    for row, quantity in zip(request["proposal"]["quantities"], chosen):
        row["quantity_exact"] = quantity
    for row, key in zip(request["component_cost_inputs"], keys):
        row["key"] = key
        if scenario == "fractional":
            # Explicit approved cost lots cover the quarter-share deltas.
            row["calculation_increment_exact"] = "0.25"
    for row in request["risk_inputs"]["valuations"]:
        row["mark_as_of"] = stamp
    override = scenario == "override"
    allowed_codes = ["gross_leverage","net_leverage"] if override else []
    if override:
        request["risk_config"].update(max_gross_leverage="0.0001",max_net_leverage="0.0001")
    request["context_fingerprint"] = sha256(canonical_qt_input_bytes(request["context"])).hexdigest()
    assert EVALUATOR.is_file()
    configuration = native_evaluator_configuration()
    pin = configuration["expected_sha256"]
    bundle_pin = configuration["expected_bundle_sha256"]
    process = QtEvaluatorProcess(**configuration)
    evaluation = QtEvaluatorClient(process,allowed_override_codes=allowed_codes).evaluate(request)
    assert evaluation.available and evaluation.requires_override == override, evaluation.unavailable_reasons
    diagnostic_request = deepcopy(request)
    diagnostic_request["operation"] = "draft_diagnostic"
    diagnostic_request["optimizer_policy"] = {"enabled":False,"config_source_id":"disabled-v1"}
    for row, quantity in zip(diagnostic_request["proposal"]["quantities"],("4","2")):
        row["quantity_exact"] = quantity
    diagnostic = QtEvaluatorClient(process,allowed_override_codes=allowed_codes).evaluate(diagnostic_request)
    assert diagnostic.available and diagnostic.requires_override == override
    evidence = evaluation.evidence
    evidence["optimizer"] = diagnostic.evidence["optimizer"]
    engine_inputs = {name: deepcopy(request[name]) for name in (
        "risk_config_source_id","risk_inputs","risk_config","quantity_rules","component_cost_inputs")}
    for name in tuple(engine_inputs["risk_inputs"]):
        if name.startswith("expected_"):
            del engine_inputs["risk_inputs"][name]
    engine_inputs.update(optimizer_policy={"enabled":False,"config_source_id":"disabled-v1"},
                         optimizer_inputs=None,optimizer_config=None)
    source_payload = dict(schema_version="qt-inputs/v1",book_id=BOOK,source_day=day,
        model_publication_id=MODEL,instrument_catalog=[dict(key=key,instrument_type=asset_type,editable=not(mixed and index==1))
            for index,key in enumerate(keys)],engine_inputs=engine_inputs)
    source_digest = sha256(canonical_qt_input_bytes(source_payload)).hexdigest()
    selected_rows = [dict(key=key,quantity_exact=quantity,basis_status="preserved_source",
        average_price_exact="100",asset_type=asset_type,editable=True,origin="qt_draft")
        for key,quantity in zip(keys,chosen)]
    if scenario.startswith("new"):
        selected_rows[1].update(basis_status="unfilled",average_price_exact=None)
    if mixed:
        selected_rows[1].update(key={**keys[1],"portfolio_type":"qt"},editable=False,origin="immutable",average_price_exact="25")
    draft_payload = {"selection_rows":selected_rows}
    draft_digest = qt_digest_v1(draft_payload)
    # Capture the real lineage before freezing the immutable draft. The absent
    # draft head is valid at this stage; no placeholder source proof is stored.
    conn.autocommit = False
    with conn.cursor(cursor_factory=RealDictCursor) as cur:
        initial_tx = QtTransaction(cur,BOOK,1)
        initial_tx.lock_authorities([])
        initial_tx.lock_registries(initial_tx.registry_ids_for_book())
        initial_tx.lock_books([])
        initial_tx.lock_mutable(source_day=now.date())
        initial_provenance = initial_tx.read_current_facts()["provenance"]
        assert initial_provenance["status"] == "ready", initial_provenance
    conn.commit()
    conn.autocommit = True
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.qt_source_policies
            (book_id,purpose,enabled,version,producer_id,policy_version,evaluator_build,evaluator_sha256,evaluator_bundle_sha256,allowed_override_codes)
            VALUES('EQUITY_MR_PORTFOLIO','evaluation',true,1,'synthetic-desk','desk-policy-v1','local-qt-controlled',%s,%s,%s),
                  ('EQUITY_MR_PORTFOLIO','execution',true,1,'synthetic-execution','execution-policy-v1',null,null,null,'[]')""",(pin,bundle_pin,Json(allowed_codes)))
        cur.execute("""INSERT INTO trading.qt_evaluation_snapshots
            (book_id,source_day,model_publication_id,producer_id,policy_version,source_version,
             as_of,valid_until,content_digest,payload)
            VALUES('EQUITY_MR_PORTFOLIO',%s,%s,'synthetic-desk','desk-policy-v1','source-v1',%s,%s,%s,%s)""",
            (day,MODEL,now-timedelta(seconds=1),
             now+timedelta(seconds=5) if scenario == "expires" else now+timedelta(hours=1),
             source_digest,Json(source_payload)))
        cur.execute("""INSERT INTO trading.qt_drafts
            (draft_id,book_id,source_day,revision,model_publication_id,model_publication_version,seed_digest,
             source_digest,provenance_digest,draft_digest,selection_payload,created_by,updated_by)
            VALUES(%s,'EQUITY_MR_PORTFOLIO',%s,1,%s,1,%s,%s,%s,%s,%s,1,1)""",
            (DRAFT,day,MODEL,seed_digest,initial_provenance["source_digest"],
             initial_provenance["chain_digest"],draft_digest,Json(draft_payload)))
        cur.execute("INSERT INTO trading.qt_draft_heads VALUES('EQUITY_MR_PORTFOLIO',%s,%s,1)",(day,DRAFT))
        # Legacy inventory is real raw JSON and participates in stale detection.
        cur.execute("INSERT INTO trading.risk_limits(strategy_id,portfolio_id,limits) "
                    "VALUES('LIVE_EQUITY_MEAN_REVERSION','EQUITY_MR_PORTFOLIO',%s)",(Json({"small":1e-7,"value":1.0}),))

    conn.autocommit = False
    with conn.cursor(cursor_factory=RealDictCursor) as cur:
        tx = QtTransaction(cur,BOOK,1)
        tx.lock_authorities([])
        tx.lock_registries(tx.registry_ids_for_book())
        tx.lock_books([])
        tx.lock_mutable(source_day=now.date())
        facts = tx.read_current_facts()
        inputs = load_qt_evaluation_inputs(tx,source_day=now.date(),model_publication_id=MODEL,
                                          checked_at=facts["captured_at"])
        assert inputs.evaluator_bundle_sha256 == bundle_pin, "Real SQL policy must bind the staged dependency closure"
        facts.update(inputs.read_set_overrides)
        read_set = capture_qt_read_set(facts)
        assert read_set.available, "Fixture must have actual proven MODEL and SQL source/accounting"
        read_set_payload = json.loads(canonical_internal_snapshot_bytes(
            "qt-read-set/v1", {name:value for name,value in facts.items() if name != "captured_at"}))
    conn.commit()
    conn.autocommit = True
    preview = dict(schema_version="qt-workflow/v1",book_id=BOOK,source_day=day,preview_id=PREVIEW,
        optimizer_book_digest=diagnostic.book_digest,selected_book_digest=evaluation.book_digest,
        draft_id=DRAFT,draft_revision=1,source_digest=read_set_payload["provenance"]["source_digest"],
        provenance_digest=read_set_payload["provenance"]["chain_digest"],read_set_digest=read_set.digest,
        availability="ready",confirmable=True,requires_override=override,selection_rows=selected_rows,
        unavailable_reasons=[],evaluation=evidence)
    preview["payload_digest"] = qt_digest_v1(preview)
    envelope = dict(schema_version="qt-desk-decision/v1",decision_id=DECISION,preview_id=PREVIEW,
        book_id=BOOK,source_day=day,preview_payload_digest=preview["payload_digest"],
        selected_book_digest=evaluation.book_digest,read_set_digest=read_set.digest)
    fills = [dict(key={**key,"portfolio_type":"qt"},observation_kind="carried" if scenario == "carried" else "executed",
        selected_quantity_exact=quantity,average_price_exact="100" if scenario == "carried" else "101",
        actual_cash_cost_exact="0" if scenario == "carried" else "0.01",
        currency="USD",execution_id=None if scenario == "carried" else "synthetic-execution-"+str(index),accounting_source_id="accounting-v1",
        daily_unrealized_pnl_exact="3",daily_realized_pnl_exact="-1",last_update=stamp)
        for index,(key,quantity) in enumerate(zip(keys,chosen))]
    if mixed:
        fills[1].update(observation_kind="carried",average_price_exact="25",actual_cash_cost_exact="0",execution_id=None)
    results = dict(position_count=len(keys),currency_totals=[dict(currency="USD",
        actual_cash_cost_exact="0" if scenario == "carried" else "0.01" if mixed else exact(Decimal("0.01")*len(keys)),
        daily_unrealized_pnl_exact=exact(3*len(keys)),daily_realized_pnl_exact=exact(-len(keys)))])
    observed = dict(schema_version="qt-execution/v1",decision_id=DECISION,book_id=BOOK,
                    source_day=day,fills=fills,results=results)
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.qt_previews
          (preview_id,book_id,source_day,draft_id,draft_revision,draft_digest,source_digest,
           provenance_digest,read_set_digest,optimizer_book_digest,selected_book_digest,payload_digest,
           payload,read_set_payload,evaluator_build,policy_version,availability,created_by,state)
          VALUES(%s,'EQUITY_MR_PORTFOLIO',%s,%s,1,%s,%s,%s,%s,%s,%s,%s,%s,%s,'local-qt-controlled',
                 'desk-policy-v1','ready',1,'confirmed_decision')""",
          (PREVIEW,day,DRAFT,draft_digest,preview["source_digest"],preview["provenance_digest"],
           read_set.digest,diagnostic.book_digest,evaluation.book_digest,preview["payload_digest"],
           Json(preview),Json(read_set_payload)))
        cur.execute("""INSERT INTO trading.qt_decisions
          (decision_id,preview_id,book_id,source_day,status,model_publication_id,provenance_digest,
           draft_id,draft_revision,selected_book_digest,read_set_digest,workflow_capability_version,
           submitter_grant_version,policy_version,payload,created_by)
          VALUES(%s,%s,'EQUITY_MR_PORTFOLIO',%s,'confirmed_decision',%s,%s,%s,1,%s,%s,1,1,'desk-policy-v1',%s,1)""",
          (DECISION,PREVIEW,day,MODEL,preview["provenance_digest"],DRAFT,evaluation.book_digest,
           read_set.digest,Json(envelope)))
        digest = sha256(canonical_qt_input_bytes(observed)).hexdigest()
        cur.execute("""INSERT INTO trading.qt_execution_observations
          (observation_id,decision_id,producer_id,policy_version,source_version,as_of,valid_until,content_digest,payload)
          VALUES(%s,%s,'synthetic-execution','execution-policy-v1','observed-v1',%s,%s,%s,%s)""",
          (OBSERVATION,DECISION,now-timedelta(seconds=1),
           now+timedelta(seconds=5) if scenario == "observation_expires" else now+timedelta(hours=1),digest,Json(observed)))
        if override:
            cur.execute("INSERT INTO trading.qt_override_requests(request_id,decision_id,eligibility_version) "
                        "VALUES('70000000-0000-4000-8000-000000000001',%s,1)",(DECISION,))
            cur.execute("""INSERT INTO trading.qt_override_approvals
              (approval_id,request_id,person_id,user_id,mapping_version,grant_version) VALUES
              ('80000000-0000-4000-8000-000000000001','70000000-0000-4000-8000-000000000001','john_riley',2,1,1),
              ('80000000-0000-4000-8000-000000000002','70000000-0000-4000-8000-000000000001','xander_robbins',3,1,1)""")
    return conn, observed


def insert_observation(conn, payload, observation_id):
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.qt_execution_observations
          (observation_id,decision_id,producer_id,policy_version,source_version,as_of,valid_until,content_digest,payload)
          SELECT %s,decision_id,producer_id,policy_version,%s,as_of,valid_until,%s,%s
          FROM trading.qt_execution_observations WHERE observation_id=%s""",
          (observation_id,"source-"+observation_id,sha256(canonical_qt_input_bytes(payload)).hexdigest(),
           Json(payload),OBSERVATION))

