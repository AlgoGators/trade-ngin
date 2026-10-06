"""Actual native desk transaction against the owned, route-free PostgreSQL."""
from copy import deepcopy
from concurrent.futures import ThreadPoolExecutor
from datetime import timedelta, timezone
from decimal import Decimal
from hashlib import sha256
from pathlib import Path
from tests.qt_test_artifacts import artifact, build_identity
from tests.contracts.qt_native_bundle_fixture import stage_actual_bundle
import atexit
from functools import lru_cache
import json
import os
import subprocess
import sys
import tempfile
from uuid import UUID

import psycopg2
from psycopg2.extras import Json, RealDictCursor
import pytest

from test_runtime_control_schema import connection
from test_proposal_storage_migration import normalize_positions_shape, apply

ROOT = Path(__file__).parents[2]
import algolens
API = Path(algolens.__file__).resolve().parent.parent
from algolens.domain.portfolio.qt_canonical import qt_digest_v1, qt_book_digest_v1
from algolens.infrastructure.portfolio.qt_read_set import (
    canonical_internal_snapshot_bytes, capture_qt_read_set, internal_snapshot_digest)
from algolens.infrastructure.portfolio.qt_workflow_repository import QtTransaction
from algolens.infrastructure.portfolio.qt_evaluation_inputs import canonical_qt_input_bytes, load_qt_evaluation_inputs
from algolens.infrastructure.portfolio.qt_evaluator_client import QtEvaluatorClient
from algolens.infrastructure.portfolio.qt_evaluator_process import QtEvaluatorProcess


@lru_cache(maxsize=1)
def native_evaluator_configuration():
    owned = tempfile.TemporaryDirectory(prefix="qt-owned-native-fixture-")
    atexit.register(owned.cleanup)
    directory = Path(owned.name) / "bundle"
    manifest = stage_actual_bundle(directory)
    executable = next(row for row in manifest["artifacts"] if row["role"] == "executable")
    return dict(executable=directory / "bin/qt_evaluator", expected_sha256=executable["sha256"],
                expected_build=manifest["evaluator_build"], bundle_directory=directory,
                expected_bundle_sha256=manifest["bundle_sha256"])

MODEL = "10000000-0000-4000-8000-000000000001"
DRAFT = "20000000-0000-4000-8000-000000000001"
PREVIEW = "30000000-0000-4000-8000-000000000001"
DECISION = "40000000-0000-4000-8000-000000000001"
ATTEMPT = "50000000-0000-4000-8000-000000000001"
OBSERVATION = "60000000-0000-4000-8000-000000000001"
BINARY = artifact("qt_desk_storage_probe")
EVALUATOR = BINARY.with_name("qt_evaluator")
BOOK = "BOOK"
API_007 = API / "migrations/007_strategy_registry_asset_class.sql"
API_007_SHA256 = "908b03d741d9324f655101de04e738d61af1c93934fcabed8b84c08c0eec4d52"


def apply_registry_asset_class(conn, asset_class):
    """Apply the installed API 007 (hash-pinned) and declare the fixture registry's asset class."""
    assert sha256(API_007.read_bytes()).hexdigest() == API_007_SHA256
    apply(conn, API_007)
    with conn.cursor() as cur:
        cur.execute("UPDATE trading.strategy_registry SET asset_class=%s", (asset_class,))


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
def desk(connection, request):
    conn = connection
    scenario = getattr(request, "param", "executed")
    approval_person = {'override_dominick':'dominick_dupuy', 'override_john':'john_riley',
        'override_eric':'eric_shwartz', 'override_typo':'dominick_dupuoy'}.get(scenario, 'hemdutt_rao')
    approval_user = 1 if scenario == 'override_self' else 2
    approval_count = 1 if scenario == 'override_single' else 2
    if scenario.startswith('override_'): scenario = 'override'
    ticker = "MES" if scenario == "futures_mes" else "SYN"
    if scenario == "futures_mes": scenario = "futures"
    chosen = (("4","2") if scenario in ("carried", "futures_quiet") else ("5","0") if scenario in ("zero", "flat_zero_basis")
              else ("-0.5","2.25") if scenario == "fractional" else ("5","1"))
    if scenario == "futures_quiet": scenario = "futures"
    normalize_positions_shape(conn)
    apply(conn, ROOT / "migrations/015_qt_proposal_positions.sql")
    apply(conn, ROOT / "migrations/016_qt_exact_precision_and_seed_provenance.sql")
    with conn.cursor() as cur:
        cur.execute("DROP SCHEMA IF EXISTS auth CASCADE; CREATE SCHEMA auth;"
                    "CREATE TABLE auth.users(id bigint PRIMARY KEY,role text NOT NULL);"
                    "INSERT INTO auth.users VALUES(1,'general_member'),(2,'general_member'),(3,'admin');"
                    "ALTER TABLE trading.strategy_registry ADD COLUMN updated_at timestamptz NOT NULL DEFAULT now();"
                    "DELETE FROM trading.strategy_registry;"
                    "INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id) VALUES('LIVE_TREND','LIVE_TREND','BOOK');"
                    "INSERT INTO trading.strategy_book_memberships(strategy_id,portfolio_id) VALUES('LIVE_TREND','BOOK')")
    apply(conn, API / "migrations/003_qt_decision_workflow.sql")
    apply(conn, API / "migrations/004_qt_governed_sources.sql")
    apply(conn, API / "migrations/005_qt_evaluator_bundle.sql")
    apply_registry_asset_class(conn, "FUTURE" if scenario == "futures" else "EQUITY")
    with conn.cursor() as cur:
        cur.execute("SELECT clock_timestamp()")
        now = cur.fetchone()[0].astimezone(timezone.utc)
    day = now.date().isoformat()
    stamp = utc(now)
    keys = [dict(portfolio_id=BOOK,strategy_id="LIVE_TREND",strategy_name=name,
                 date=day,symbol=ticker,portfolio_type="qt_proposal")
            for name in ("synthetic-alpha","synthetic-beta")]
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
                        "VALUES('IMMUTABLE','IMMUTABLE','BOOK',true,'live','EQUITY')")
            cur.execute("INSERT INTO trading.positions(portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,"
                        "quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update) "
                        "VALUES('BOOK','IMMUTABLE','held-component',%s,'IMM','qt',8,25,1.25,-0.5,%s)",(day,now))
    seed_rows = []
    proposal_rows = []
    with conn.cursor() as cur:
        for key, quantity in zip(keys[:1] if scenario.startswith("new") or mixed else keys, ("4","0") if scenario == "flat_zero_basis" else ("4","2")):
            basis = "0" if scenario == "flat_zero_basis" and quantity == "0" else "100"
            for stream in ("system", "qt_proposal", "qt"):
                cur.execute("""INSERT INTO trading.positions
                    (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
                     quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
                    VALUES(%s,%s,%s,%s,%s,%s,%s,%s,1.25,-0.5,%s)""",
                    (BOOK,"LIVE_TREND",key["strategy_name"],day,ticker,stream,quantity,basis,now))
            cur.execute("SELECT qt_proposal_revision::text FROM trading.positions WHERE "
                        "portfolio_id=%s AND strategy_name=%s AND date=%s AND portfolio_type='qt_proposal'",
                        (BOOK,key["strategy_name"],day))
            revision = cur.fetchone()[0]
            assert revision is not None
            seed_rows.append(dict(key={**key,"portfolio_type":"system"},
                                  quantity_exact=quantity,average_price_exact=basis))
            proposal_rows.append(dict(key=key,quantity_exact=quantity,average_price_exact=basis,
                action="inserted",position_revision=revision,origin_publication_id=MODEL))
        seed_digest = qt_digest_v1({"seed_rows": seed_rows})
        manifest_digest = internal_snapshot_digest("qt-proposal-manifest/v1",{"proposal_rows":proposal_rows})
        cur.execute("""INSERT INTO trading.qt_model_seed_publications
            (publication_id,portfolio_id,strategy_id,source_day,publication_version,
             system_components,seed_digest,proposal_components,proposal_manifest_digest,producer_version)
            VALUES(%s,%s,'LIVE_TREND',%s,1,%s,%s,%s,%s,'owned-native-desk-fixture')""",
            (MODEL,BOOK,day,Json(seed_rows),seed_digest,Json(proposal_rows),manifest_digest))
        cur.execute("INSERT INTO trading.qt_workflow_capabilities(book_id,enabled,version) VALUES('BOOK',true,1)")
        cur.execute("INSERT INTO trading.qt_action_grants(user_id,capability,active,version) "
                    "VALUES(1,'qt_submit',true,1),(2,'qt_approve',true,1),(3,'qt_approve',true,1)")
        if approval_user == 1:
            cur.execute("INSERT INTO trading.qt_action_grants(user_id,capability,active,version) VALUES(1,'qt_approve',true,1)")
        if approval_person in {'dominick_dupuy', 'eric_shwartz'}:
            # Fixture003 predates corrected009 identity policy. Seed current or
            # historical authority before immutable evidence is created. Preserve
            # mapping/approval uniqueness and FK constraints in every case.
            cur.execute("ALTER TABLE trading.qt_approver_allowlist DROP CONSTRAINT qt_approver_identity")
        cur.execute("INSERT INTO trading.qt_approver_allowlist(person_id,display_label,user_id,active,mapping_version) "
                    "VALUES(%s,%s,%s,true,1),('xander_robbins','xander robbins',3,true,1)",
                    (approval_person,approval_person.replace('_',' '),approval_user))

    request = json.loads((ROOT / "tests/contracts/qt-eval-v1.json").read_text())["selected_book"]
    request["evaluator_build"] = build_identity()
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
            if scenario == "flat_zero_basis" and key["strategy_name"] == "synthetic-beta":
                row["previous"].update(quantity_exact="0", average_price_exact="0")
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
        if name in ("expected_portfolio_id", "expected_date", "expected_revision", "expected_portfolio_type"):
            del engine_inputs["risk_inputs"][name]
    engine_inputs.update(optimizer_policy={"enabled":False,"config_source_id":"disabled-v1"},
                         optimizer_inputs=None,optimizer_config=None)
    source_payload = dict(schema_version="qt-inputs/v1",book_id=BOOK,source_day=day,
        model_publication_id=MODEL,instrument_catalog=[dict(key=key,instrument_type=asset_type,editable=not(mixed and index==1))
            for index,key in enumerate(keys)],engine_inputs=engine_inputs)
    source_digest = sha256(canonical_qt_input_bytes(source_payload)).hexdigest()
    selected_rows = [dict(key=key,quantity_exact=quantity,basis_status="preserved_source",
        average_price_exact="0" if scenario == "flat_zero_basis" and quantity == "0" else "100",asset_type=asset_type,editable=True,origin="qt_draft")
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
        assert initial_provenance["status"] == "ready"
    conn.commit()
    conn.autocommit = True
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.qt_source_policies
            (book_id,purpose,enabled,version,producer_id,policy_version,evaluator_build,evaluator_sha256,evaluator_bundle_sha256,allowed_override_codes)
            VALUES('BOOK','evaluation',true,1,'synthetic-desk','desk-policy-v1',%s,%s,%s,%s),
                  ('BOOK','execution',true,1,'synthetic-execution','execution-policy-v1',null,null,null,'[]')""",(build_identity(),pin,bundle_pin,Json(allowed_codes)))
        cur.execute("""INSERT INTO trading.qt_evaluation_snapshots
            (book_id,source_day,model_publication_id,producer_id,policy_version,source_version,
             as_of,valid_until,content_digest,payload)
            VALUES('BOOK',%s,%s,'synthetic-desk','desk-policy-v1','source-v1',%s,%s,%s,%s)""",
            (day,MODEL,now-timedelta(seconds=1),
             now+timedelta(seconds=5) if scenario == "expires" else now+timedelta(hours=1),
             source_digest,Json(source_payload)))
        cur.execute("""INSERT INTO trading.qt_drafts
            (draft_id,book_id,source_day,revision,model_publication_id,model_publication_version,seed_digest,
             source_digest,provenance_digest,draft_digest,selection_payload,created_by,updated_by)
            VALUES(%s,'BOOK',%s,1,%s,1,%s,%s,%s,%s,%s,1,1)""",
            (DRAFT,day,MODEL,seed_digest,initial_provenance["source_digest"],
             initial_provenance["chain_digest"],draft_digest,Json(draft_payload)))
        cur.execute("INSERT INTO trading.qt_draft_heads VALUES('BOOK',%s,%s,1)",(day,DRAFT))
        # Legacy inventory is real raw JSON and participates in stale detection.
        cur.execute("INSERT INTO trading.risk_limits(strategy_id,portfolio_id,limits) "
                    "VALUES('LIVE_TREND','BOOK',%s)",(Json({"small":1e-7,"value":1.0}),))

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
          VALUES(%s,'BOOK',%s,%s,1,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,
                 'desk-policy-v1','ready',1,'confirmed_decision')""",
          (PREVIEW,day,DRAFT,draft_digest,preview["source_digest"],preview["provenance_digest"],
           read_set.digest,diagnostic.book_digest,evaluation.book_digest,preview["payload_digest"],
           Json(preview),Json(read_set_payload),build_identity()))
        cur.execute("""INSERT INTO trading.qt_decisions
          (decision_id,preview_id,book_id,source_day,status,model_publication_id,provenance_digest,
           draft_id,draft_revision,selected_book_digest,read_set_digest,workflow_capability_version,
           submitter_grant_version,policy_version,payload,created_by)
          VALUES(%s,%s,'BOOK',%s,'confirmed_decision',%s,%s,%s,1,%s,%s,1,1,'desk-policy-v1',%s,1)""",
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
              ('80000000-0000-4000-8000-000000000001','70000000-0000-4000-8000-000000000001',%s,%s,1,1)""",
              (approval_person,approval_user))
            if approval_count == 2:
                cur.execute("""INSERT INTO trading.qt_override_approvals
                  (approval_id,request_id,person_id,user_id,mapping_version,grant_version) VALUES
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


def test_actual_native_publication_and_same_attempt_replay_are_atomic(desk):
    conn, observed = desk
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    assert "DESK_PROCESSED=1" in result.stdout and "REPLAYED=0" in result.stdout
    with conn.cursor() as cur:
        cur.execute("SELECT strategy_name,quantity::text,average_price::text FROM trading.positions "
                    "WHERE portfolio_type='qt' ORDER BY strategy_name")
        assert cur.fetchall() == [("synthetic-alpha","5.00000000","101.00000000"),
                                  ("synthetic-beta","1.00000000","101.00000000")]
        cur.execute("SELECT status,publication_payload FROM trading.qt_desk_receipts")
        status, publication = cur.fetchone()
        assert status == "processed"
        assert publication["schema_version"] == "qt-desk-publication/v1"
        assert [r["quantity_exact"] for r in publication["before_accounting"]] == ["4","2"]
        assert [r["quantity_exact"] for r in publication["after_accounting"]] == ["5","1"]
        cur.execute("SELECT payload FROM trading.qt_desk_results")
        assert cur.fetchone()[0]["results"] == observed["results"]
    before = table_state(conn)
    replay = run()
    assert replay.returncode == 0 and "REPLAYED=1" in replay.stdout
    assert table_state(conn) == before
    conflict = run("50000000-0000-4000-8000-000000000002")
    assert conflict.returncode != 0
    assert table_state(conn) == before


@pytest.mark.parametrize("fact", ["proposal", "accounting", "capability", "grant", "role", "risk"])
def test_actual_current_fact_changes_refuse_without_mutation(desk, fact):
    conn, _ = desk
    changes = {
        "proposal":"UPDATE trading.positions SET quantity=7 WHERE portfolio_type='qt_proposal' AND strategy_name='synthetic-alpha'",
        "accounting":"UPDATE trading.positions SET daily_realized_pnl=7 WHERE portfolio_type='qt' AND strategy_name='synthetic-alpha'",
        "capability":"UPDATE trading.qt_workflow_capabilities SET enabled=false,version=2 WHERE book_id='BOOK'",
        "grant":"UPDATE trading.qt_action_grants SET active=false,version=2 WHERE user_id=1 AND capability='qt_submit'",
        "role":"UPDATE auth.users SET role='external' WHERE id=1",
        "risk":"INSERT INTO trading.risk_limits(strategy_id,portfolio_id,limits) VALUES('LIVE_TREND','BOOK','{}')",
    }
    with conn.cursor() as cur:
        cur.execute(changes[fact])
    before = table_state(conn)
    assert run().returncode != 0
    assert table_state(conn) == before


def test_a_failure_after_first_position_write_rolls_back_every_output(desk):
    conn, _ = desk
    with conn.cursor() as cur:
        cur.execute("""CREATE FUNCTION trading.reject_second_desk() RETURNS trigger LANGUAGE plpgsql AS $$
          BEGIN IF NEW.portfolio_type='qt' AND NEW.strategy_name='synthetic-beta' AND NEW.quantity=1
          THEN RAISE EXCEPTION 'owned second write failure'; END IF; RETURN NEW; END $$;
          CREATE TRIGGER reject_second_desk BEFORE INSERT OR UPDATE ON trading.positions
          FOR EACH ROW EXECUTE FUNCTION trading.reject_second_desk()""")
    before = table_state(conn)
    assert run().returncode != 0
    assert table_state(conn) == before


@pytest.mark.parametrize("desk", ["zero"], indirect=True)
def test_closing_to_zero_persists_complete_key_and_closed_row_mapping(desk):
    conn, _ = desk
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT quantity::text FROM trading.positions "
                    "WHERE portfolio_type='qt' AND strategy_name='synthetic-beta'")
        assert cur.fetchone() == ("0.00000000",)
        cur.execute("SELECT report_eligibility_status,report_reason_codes,row_manifest_digest,publication_payload "
                    "FROM trading.qt_desk_receipts")
        status, reasons, digest, publication = cur.fetchone()
        assert status == "eligible"
        assert reasons == []
        assert isinstance(digest, str) and len(digest) == 64 and set(digest) <= set("0123456789abcdef")
        assert len(publication["after_accounting"]) == 2
        assert publication["after_accounting"][1]["quantity_exact"] == "0"


@pytest.mark.parametrize("desk", ["carried"], indirect=True)
def test_carried_existing_rows_require_no_invented_execution(desk):
    conn, observed = desk
    assert all(row["execution_id"] is None for row in observed["fills"])
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT quantity::text,average_price::text FROM trading.positions "
                    "WHERE portfolio_type='qt' ORDER BY strategy_name")
        assert cur.fetchall() == [("4.00000000","100.00000000"),("2.00000000","100.00000000")]


@pytest.mark.parametrize("change", ["quantity","missing_key","carried_change","execution_id",
                                   "negative_cost","results","missing_time"])
def test_trusted_observation_still_requires_exact_selection_and_real_accounting(desk, change):
    conn, observed = desk
    bad = deepcopy(observed)
    if change == "quantity": bad["fills"][0]["selected_quantity_exact"] = "6"
    elif change == "missing_key":
        bad["fills"].pop()
        bad["results"] = dict(position_count=1,currency_totals=[dict(currency="USD",
            actual_cash_cost_exact="0.01",daily_unrealized_pnl_exact="3",daily_realized_pnl_exact="-1")])
    elif change == "carried_change":
        bad["fills"][0]["observation_kind"] = "carried"
        bad["fills"][0]["execution_id"] = None
    elif change == "execution_id": bad["fills"][0]["execution_id"] = None
    elif change == "negative_cost": bad["fills"][0]["actual_cash_cost_exact"] = "-0.01"
    elif change == "results": bad["results"]["currency_totals"][0]["actual_cash_cost_exact"] = "0"
    elif change == "missing_time": bad["fills"][0]["last_update"] = None
    new_id = "60000000-0000-4000-8000-000000000002"
    insert_observation(conn,bad,new_id)
    before = table_state(conn)
    assert run(observation=new_id).returncode != 0
    assert table_state(conn) == before


def test_two_actual_consumers_of_same_attempt_publish_once(desk):
    conn, _ = desk
    with ThreadPoolExecutor(max_workers=2) as pool:
        outcomes = list(pool.map(lambda _:run(), range(2)))
    assert all(result.returncode == 0 for result in outcomes), [(r.stdout,r.stderr) for r in outcomes]
    assert sorted("REPLAYED=1" in result.stdout for result in outcomes) == [False,True]
    with conn.cursor() as cur:
        for table in ("qt_desk_results","qt_desk_receipts"):
            cur.execute("SELECT count(*) FROM trading."+table)
            assert cur.fetchone() == (1,)
        cur.execute("SELECT count(*) FROM trading.position_overrides")
        assert cur.fetchone() == (2,)


@pytest.mark.parametrize("desk", ["expires"], indirect=True)
def test_real_source_expiry_after_claim_rolls_back_before_commit(desk):
    conn, _ = desk
    with conn.cursor() as cur:
        cur.execute("""CREATE SEQUENCE trading.desk_write_reached;
          CREATE FUNCTION trading.delay_desk_write() RETURNS trigger LANGUAGE plpgsql AS $$
          BEGIN IF NEW.portfolio_type='qt' AND NEW.strategy_name='synthetic-alpha' AND NEW.quantity=5
          THEN PERFORM nextval('trading.desk_write_reached'); PERFORM pg_sleep(6); END IF; RETURN NEW; END $$;
          CREATE TRIGGER delay_desk_write BEFORE INSERT OR UPDATE ON trading.positions
          FOR EACH ROW EXECUTE FUNCTION trading.delay_desk_write()""")
    before = table_state(conn)
    assert run().returncode != 0
    with conn.cursor() as cur:
        cur.execute("SELECT is_called FROM trading.desk_write_reached")
        assert cur.fetchone() == (True,), "Expiry regression must reach the actual position write"
    assert table_state(conn) == before


@pytest.mark.parametrize("fact", ["system","equal_proposal_write","evaluation_policy",
                                  "execution_policy","new_snapshot","new_audit","registry"])
def test_native_authority_inventory_changes_refuse_without_mutation(desk, fact):
    conn, _ = desk
    queries = {
        "system":"UPDATE trading.positions SET quantity=7 WHERE portfolio_type='system' AND strategy_name='synthetic-alpha'",
        "equal_proposal_write":"UPDATE trading.positions SET quantity=quantity WHERE portfolio_type='qt_proposal'",
        "evaluation_policy":"UPDATE trading.qt_source_policies SET version=2 WHERE purpose='evaluation'",
        "execution_policy":"UPDATE trading.qt_source_policies SET enabled=false,version=2 WHERE purpose='execution'",
        "new_snapshot":"INSERT INTO trading.qt_evaluation_snapshots(book_id,source_day,model_publication_id,producer_id,policy_version,source_version,as_of,valid_until,content_digest,payload) SELECT book_id,source_day,model_publication_id,producer_id,policy_version,'source-v2',as_of,valid_until,content_digest,payload FROM trading.qt_evaluation_snapshots",
        "new_audit":"INSERT INTO trading.position_overrides(user_id,source_app,portfolio_id,strategy_id,symbol,before_state,after_state,reason,risk_check_result) VALUES(1,'manual_db_edit','BOOK','LIVE_TREND','SYN','null','null','explicit synthetic stale inventory','{}')",
        "registry":"UPDATE trading.strategy_registry SET is_active=false WHERE id='LIVE_TREND'",
    }
    with conn.cursor() as cur:
        cur.execute(queries[fact])
    before = table_state(conn)
    assert run().returncode != 0
    assert table_state(conn) == before


@pytest.mark.parametrize("desk", ["fractional"], indirect=True)
def test_native_signed_fractional_quantities_are_saved_exactly(desk):
    conn, _ = desk
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT quantity::text FROM trading.positions WHERE portfolio_type='qt' ORDER BY strategy_name")
        assert cur.fetchall() == [("-0.50000000",),("2.25000000",)]


def test_same_attempt_different_observation_refuses_after_publication(desk):
    conn, observed = desk
    assert run().returncode == 0
    other = "60000000-0000-4000-8000-000000000003"
    insert_observation(conn, observed, other)
    before = table_state(conn)
    assert run(observation=other).returncode != 0
    assert table_state(conn) == before


@pytest.mark.parametrize("desk", ["override"], indirect=True)
def test_native_known_breach_requires_actual_current_two_person_quorum(desk):
    conn, _ = desk
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT overrode_risk FROM trading.position_overrides ORDER BY id")
        assert cur.fetchall() == [(True,),(True,)]


@pytest.mark.parametrize("desk", ["override"], indirect=True)
@pytest.mark.parametrize("change", ["mapping","grant","role","mapping_version","grant_version"])
def test_native_confirmed_status_cannot_replace_current_override_eligibility(desk, change):
    conn, _ = desk
    query = {
        "mapping":"UPDATE trading.qt_approver_allowlist SET active=false,mapping_version=2 WHERE user_id=2",
        "grant":"UPDATE trading.qt_action_grants SET active=false,version=2 WHERE user_id=2 AND capability='qt_approve'",
        "role":"UPDATE auth.users SET role='external' WHERE id=2",
        "mapping_version":"UPDATE trading.qt_approver_allowlist SET mapping_version=2 WHERE user_id=2",
        "grant_version":"UPDATE trading.qt_action_grants SET version=2 WHERE user_id=2 AND capability='qt_approve'",
    }[change]
    with conn.cursor() as cur:
        cur.execute(query)
    before = table_state(conn)
    assert run().returncode != 0
    assert table_state(conn) == before


@pytest.mark.parametrize("desk", ["override"], indirect=True)
@pytest.mark.parametrize("approver_role", ["admin", "general_member", "exec_board"])
def test_native_current_approver_roles_do_not_imply_submission_authority(desk, approver_role):
    conn, _ = desk
    with conn.cursor() as cur:
        cur.execute("UPDATE auth.users SET role=%s WHERE id=2", (approver_role,))
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("desk", ["override_dominick"], indirect=True)
def test_native_corrected_dominick_identity_counts_as_current_approver(desk):
    conn, _ = desk
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("desk", ["override_john", "override_eric", "override_typo"], indirect=True)
def test_native_retired_or_misspelled_people_cannot_satisfy_quorum(desk):
    conn, _ = desk
    before = table_state(conn)
    result = run()
    assert result.returncode != 0 and 'qt_desk_unavailable:authorization' in result.stdout
    assert table_state(conn) == before


@pytest.mark.parametrize("desk", ["override_self"], indirect=True)
def test_native_historical_submitter_approval_never_counts(desk):
    conn, _ = desk
    before = table_state(conn)
    result = run()
    assert result.returncode != 0 and 'qt_desk_unavailable:authorization' in result.stdout
    assert table_state(conn) == before


@pytest.mark.parametrize("desk", ["override"], indirect=True)
def test_native_exec_board_cannot_submit_even_with_submit_grant(desk):
    conn, _ = desk
    with conn.cursor() as cur:
        cur.execute("UPDATE auth.users SET role='exec_board' WHERE id=1")
    before = table_state(conn)
    result = run()
    assert result.returncode != 0 and 'qt_desk_unavailable:authorization' in result.stdout
    assert table_state(conn) == before


@pytest.mark.parametrize("desk", ["override_single"], indirect=True)
@pytest.mark.parametrize("person,user", [("hemdutt_rao",3),("xander_robbins",2)])
def test_duplicate_person_or_user_cannot_create_second_approval(desk, person, user):
    conn, _ = desk
    with conn.cursor() as cur:
        with pytest.raises(psycopg2.IntegrityError):
            cur.execute("INSERT INTO trading.qt_override_approvals "
                "(approval_id,request_id,person_id,user_id,mapping_version,grant_version) "
                "VALUES('80000000-0000-4000-8000-000000000002',"
                "'70000000-0000-4000-8000-000000000001',%s,%s,1,1)", (person,user))
    before = table_state(conn)
    result = run()
    assert result.returncode != 0 and 'qt_desk_unavailable:authorization' in result.stdout
    assert table_state(conn) == before


def test_actual_processed_receipt_current_proof_admits_only_linked_unchanged_authority(desk):
    conn, _ = desk
    assert report_proof(conn).returncode != 0, "Enabled workflow requires a processed receipt"
    assert run().returncode == 0
    proof = report_proof(conn)
    assert proof.returncode == 0 and "WORKFLOW_REQUIRED=1" in proof.stdout
    with conn.cursor() as cur:
        cur.execute("UPDATE trading.positions SET daily_realized_pnl=7 WHERE portfolio_type='qt' AND strategy_name='synthetic-alpha'")
    before = table_state(conn)
    assert report_proof(conn).returncode != 0
    assert table_state(conn) == before


def test_explicit_available_disabled_capability_is_only_legacy_report_admission(desk):
    conn, _ = desk
    with conn.cursor() as cur:
        cur.execute("UPDATE trading.qt_workflow_capabilities SET enabled=false,version=2 WHERE book_id='BOOK'")
    proof = report_proof(conn)
    assert proof.returncode == 0 and "WORKFLOW_REQUIRED=0" in proof.stdout
    with conn.cursor() as cur:
        cur.execute("DELETE FROM trading.qt_workflow_capabilities WHERE book_id='BOOK'")
    assert report_proof(conn).returncode != 0


@pytest.mark.parametrize("desk", ["new"], indirect=True)
def test_actual_new_editable_key_uses_observed_basis_and_null_before_audit(desk):
    conn, _ = desk
    with conn.cursor() as cur:
        cur.execute("SELECT count(*) FROM trading.positions WHERE symbol='NEW'")
        assert cur.fetchone() == (0,), "No zero or source row may be invented for the new key"
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT quantity::text,average_price::text FROM trading.positions WHERE symbol='NEW'")
        assert cur.fetchall() == [("3.00000000","101.00000000")]
        cur.execute("SELECT before_state,after_state,risk_check_result FROM trading.position_overrides WHERE symbol='NEW'")
        before, after, reference = cur.fetchone()
        assert before is None
        assert after["quantity_exact"] == "3" and after["average_price_exact"] == "101"
        assert reference["schema_version"] == "qt-desk-audit/v1" and reference["decision_id"] == DECISION
        cur.execute("SELECT report_eligibility_status,report_reason_codes,row_manifest_digest,publication_payload "
                    "FROM trading.qt_desk_receipts")
        status, reasons, digest, publication = cur.fetchone()
        assert status == "eligible"
        assert reasons == []
        assert isinstance(digest, str) and len(digest) == 64 and set(digest) <= set("0123456789abcdef")
        assert len(publication["before_accounting"]) == 1 and len(publication["after_accounting"]) == 2
    assert report_proof(conn).returncode == 0


@pytest.mark.parametrize("desk", ["new_foreign","new_unknown_owner"], indirect=True)
def test_native_typed_catalog_cannot_invent_a_new_component_owner(desk):
    conn, _ = desk
    before = table_state(conn)
    result = run()
    assert result.returncode != 0
    assert "owner_scope" in result.stdout
    assert table_state(conn) == before


def test_mutable_receipt_cannot_rewrite_actual_observed_accounting(desk):
    conn, _ = desk
    assert run().returncode == 0
    with conn.cursor() as cur:
        cur.execute("UPDATE trading.positions SET daily_realized_pnl=7 WHERE portfolio_type='qt' AND strategy_name='synthetic-alpha'")
        cur.execute("UPDATE trading.qt_desk_receipts SET publication_payload=jsonb_set(publication_payload," 
                    "'{after_accounting,0,daily_realized_pnl_exact}', '\"7\"')")
    before = table_state(conn)
    assert report_proof(conn).returncode != 0
    assert run().returncode != 0
    assert table_state(conn) == before


@pytest.mark.parametrize("desk", ["mixed"], indirect=True)
def test_independently_held_immutable_key_is_preserved_with_editable_selection(desk):
    conn, observed = desk
    assert observed["fills"][1]["observation_kind"] == "carried"
    assert observed["fills"][1]["execution_id"] is None
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT strategy_id,symbol,quantity::text,average_price::text FROM trading.positions "
                    "WHERE portfolio_type='qt' ORDER BY strategy_id")
        assert cur.fetchall() == [("IMMUTABLE","IMM","8.00000000","25.00000000"),
                                  ("LIVE_TREND","SYN","5.00000000","101.00000000")]
    assert report_proof(conn).returncode == 0


@pytest.mark.parametrize("desk", ["mixed"], indirect=True)
@pytest.mark.parametrize("change", ["quantity","basis","invented_key"])
def test_immutable_observation_cannot_change_or_invent_held_key(desk, change):
    conn, observed = desk
    bad=deepcopy(observed)
    if change == "quantity": bad["fills"][1]["selected_quantity_exact"]="9"
    elif change == "basis":
        bad["fills"][1].update(average_price_exact="26",observation_kind="executed",
                               execution_id="invented-held-execution",actual_cash_cost_exact="0.01")
        bad["results"]["currency_totals"][0]["actual_cash_cost_exact"]="0.02"
    else: bad["fills"][1]["key"]["symbol"]="UNKNOWN"
    other="60000000-0000-4000-8000-000000000004"
    insert_observation(conn,bad,other)
    before=table_state(conn)
    assert run(observation=other).returncode != 0
    assert table_state(conn) == before


@pytest.mark.parametrize("change", ["missing","disabled","producer","policy_version"])
def test_processed_report_and_replay_require_current_matching_execution_policy(desk, change):
    conn,_=desk
    assert run().returncode == 0
    assert report_proof(conn).returncode == 0
    query={
        "missing":"ALTER TABLE trading.qt_source_policies DISABLE TRIGGER qt_source_policy_no_delete; "
                  "DELETE FROM trading.qt_source_policies WHERE purpose='execution'; "
                  "ALTER TABLE trading.qt_source_policies ENABLE TRIGGER qt_source_policy_no_delete",
        "disabled":"UPDATE trading.qt_source_policies SET enabled=false,version=2 WHERE purpose='execution'",
        "producer":"UPDATE trading.qt_source_policies SET producer_id='other-producer',version=2 WHERE purpose='execution'",
        "policy_version":"UPDATE trading.qt_source_policies SET policy_version='other-policy',version=2 WHERE purpose='execution'",
    }[change]
    with conn.cursor() as cur: cur.execute(query)
    before=table_state(conn)
    assert report_proof(conn).returncode != 0
    assert run().returncode != 0
    assert table_state(conn) == before


@pytest.mark.parametrize("desk", ["observation_expires"], indirect=True)
def test_processed_execution_is_durable_after_original_observation_window(desk):
    conn,_=desk
    result=run()
    assert result.returncode == 0, result.stdout + result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT pg_sleep(GREATEST(0,EXTRACT(EPOCH FROM valid_until-clock_timestamp()))+0.1) "
                    "FROM trading.qt_execution_observations WHERE observation_id=%s",(OBSERVATION,))
    before=table_state(conn)
    assert report_proof(conn).returncode == 0
    assert run().returncode == 0
    assert table_state(conn) == before
