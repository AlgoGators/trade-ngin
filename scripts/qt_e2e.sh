#!/usr/bin/env bash
# QT end-to-end check against QT_E2E_PORTFOLIO (docs/design/qt-contract.md), run on the
# trade-ngin host next to the engine-rpc container. It does what AlgoLens does: writes the
# position_overrides rows and calls the gRPC commands, then reads the books back.
#
#   DB_HOST=... DB_PORT=5432 DB_USER=... DB_PASSWORD=... DB_NAME=new_algo_data scripts/qt_e2e.sh
#
# Steps: model run -> desk save (qt_proposal upsert + 'save' row) -> RunDesk -> qt differs from
# system for the edited symbol, moved_by set, book_source desk -> override request (token row,
# e-mail sent or kept in the row; driven by the 60 s re-drive sweep, no gRPC call) -> a self-approval is refused -> decision by the approver ->
# qt == qt_proposal, book_source override -> publish -> published_by/at set.
#
# position_overrides rows are never deleted, so each run takes the latest date (on or before
# QT_E2E_LAST_DATE) that has no command rows yet, and clears the e2e portfolio's book rows first so
# the date is its first desk day. Only QT_E2E_PORTFOLIO is touched.
set -uo pipefail

PID=QT_E2E_PORTFOLIO
DIR=qt_e2e
AGENT="${QT_AGENT_CONTAINER:-engine-rpc}"
LAST_DATE="${QT_E2E_LAST_DATE:-2026-10-08}"
REQUESTER="${QT_E2E_REQUESTER:-qt-e2e-desk@algogators.com}"
APPROVER="${QT_E2E_APPROVER:-qt-e2e-vp@algogators.com}"
FAILS=0

[ "${DB_NAME:-}" = "new_algo_data" ] || { echo "refusing: DB_NAME must be new_algo_data"; exit 2; }
export PGHOST="${QT_E2E_PGHOST:-$DB_HOST}" PGPORT="${DB_PORT:-5432}" PGUSER="$DB_USER" \
       PGPASSWORD="$DB_PASSWORD" PGDATABASE="$DB_NAME"

q() { psql -X -v ON_ERROR_STOP=1 -Atq -c "$1"; }
call() {
    docker exec -e PYTHONPATH=/opt/rpc/app "$AGENT" /opt/rpc/venv/bin/python \
        /app/scripts/qt_grpc_call.py "$1" "$2"
}
poll() {
    local s
    for _ in $(seq 1 150); do
        s="$(q "SELECT status FROM trading.position_overrides WHERE id = $1")"
        case "$s" in done|refused|failed) echo "$s"; return;; esac
        sleep 4
    done
    echo timeout
}
check() {
    if [ "$1" = "$2" ]; then echo "PASS  $3"; else echo "FAIL  $3: got '$1', want '$2'"; FAILS=$((FAILS + 1)); fi
}
row() { q "SELECT status || ' | ' || COALESCE(message, '') FROM trading.position_overrides WHERE id = $1"; }

# ---- the date and a clean book
D="$(q "SELECT to_char(d, 'YYYY-MM-DD') FROM generate_series(DATE '$LAST_DATE', DATE '$LAST_DATE' - 60, interval '-1 day') AS g(d)
        WHERE NOT EXISTS (SELECT 1 FROM trading.position_overrides WHERE portfolio_id = '$PID' AND date = g.d::date)
        AND extract(isodow FROM g.d) BETWEEN 2 AND 6 LIMIT 1")"
echo "== e2e date $D (first desk day of $PID)"
for t in positions executions live_results equity_curve signals live_run_metadata strategy_trading_days_metadata; do
    q "DELETE FROM trading.$t WHERE portfolio_id = '$PID'"
done

# ---- 1. model run
echo "== 1. model run"
docker exec -e QT_RUN_DATE="$D" "$AGENT" /app/scripts/qt_model_run.sh "$DIR" | tail -n 2
for book in system qt_proposal qt; do
    check "$(q "SELECT count(*) > 0 FROM trading.positions WHERE portfolio_id = '$PID' AND date = '$D' AND portfolio_type = '$book'")" t "model run wrote $book positions"
done
check "$(q "SELECT book_source FROM trading.live_results WHERE portfolio_id = '$PID' AND date = '$D' AND portfolio_type = 'qt'")" model "qt live_results book_source = model"

# ---- 2. desk save, as AlgoLens writes it
echo "== 2. desk save + RunDesk"
read -r SYM FROM_Q <<<"$(q "SELECT symbol, sum(quantity)::int FROM trading.positions WHERE portfolio_id = '$PID' AND date = '$D' AND portfolio_type = 'qt_proposal' GROUP BY symbol ORDER BY abs(sum(quantity)) DESC, symbol LIMIT 1" | tr '|' ' ')"
TO_Q=$(( FROM_Q >= 0 ? FROM_Q + 25 : FROM_Q - 25 ))
echo "   edit $SYM: $FROM_Q -> $TO_Q"
q "UPDATE trading.positions SET quantity = $TO_Q WHERE portfolio_id = '$PID' AND date = '$D' AND portfolio_type = 'qt_proposal' AND symbol = '$SYM'"
SAVE=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, reason, payload) VALUES ('$PID', '$D', 'save', '$REQUESTER', 'e2e: desk edit', '{\"changes\":[{\"symbol\":\"$SYM\",\"from\":$FROM_Q,\"to\":$TO_Q}]}') RETURNING id")
echo "   RunDesk -> $(call RunDesk "{\"portfolio_id\":\"$PID\",\"date\":\"$D\",\"audit_id\":$SAVE,\"requested_by\":\"$REQUESTER\"}")"
check "$(poll "$SAVE")" done "save row $SAVE done ($(row "$SAVE"))"
GIVEN=$(q "SELECT COALESCE(sum(quantity), 0)::int FROM trading.positions WHERE portfolio_id = '$PID' AND date = '$D' AND portfolio_type = 'qt' AND symbol = '$SYM'")
SYSQ=$(q "SELECT COALESCE(sum(quantity), 0)::int FROM trading.positions WHERE portfolio_id = '$PID' AND date = '$D' AND portfolio_type = 'system' AND symbol = '$SYM'")
echo "   $SYM asked $TO_Q, system $SYSQ, qt gave $GIVEN"
check "$([ "$GIVEN" != "$SYSQ" ] && echo t || echo f)" t "qt differs from system for $SYM"
MOVED=$(q "SELECT result->'symbols' @> '[{\"symbol\":\"$SYM\"}]' AND (SELECT bool_and(s ? 'moved_by') FROM jsonb_array_elements(result->'symbols') s) FROM trading.position_overrides WHERE id = $SAVE")
check "$MOVED" t "result has asked/given/moved_by per symbol"
echo "   $SYM moved_by: $(q "SELECT s->>'moved_by' FROM trading.position_overrides, jsonb_array_elements(result->'symbols') s WHERE id = $SAVE AND s->>'symbol' = '$SYM'")"
if [ "$GIVEN" != "0" ]; then
    check "$(q "SELECT count(*) > 0 FROM trading.positions WHERE portfolio_id = '$PID' AND date = '$D' AND portfolio_type = 'qt' AND symbol = '$SYM' AND moved_by IS NOT NULL")" t "positions.moved_by set on the qt row"
fi
check "$(q "SELECT book_source FROM trading.live_results WHERE portfolio_id = '$PID' AND date = '$D' AND portfolio_type = 'qt'")" desk "qt book_source = desk"

# ---- 3. override request
echo "== 3. override request"
REQ=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, reason) VALUES ('$PID', '$D', 'override_request', '$REQUESTER', 'e2e: trade the desk book exactly') RETURNING id")
echo "   no RequestOverride call: the row is picked up by the desk service's 60 s re-drive sweep alone"
check "$(poll "$REQ")" done "override_request row $REQ done ($(row "$REQ"))"
check "$(q "SELECT token_hash IS NOT NULL AND token_expires_at BETWEEN now() + interval '47 hours' AND now() + interval '49 hours' FROM trading.position_overrides WHERE id = $REQ")" t "token_hash stored with a 48 h expiry"
echo "   result: $(q "SELECT (result - 'email')::text || CASE WHEN result ? 'email' THEN ' (+ email kept in the row: subject ' || (result->'email'->>'subject') || ')' ELSE '' END FROM trading.position_overrides WHERE id = $REQ")"
TOKEN=$(q "SELECT substring(result->'email'->>'body' FROM 'token=([A-Za-z0-9_-]+)') FROM trading.position_overrides WHERE id = $REQ")
if [ -n "$TOKEN" ]; then
    HASH=$(printf '%s' "$TOKEN" | sha256sum | cut -d' ' -f1)
    check "$(q "SELECT token_hash = '$HASH' FROM trading.position_overrides WHERE id = $REQ")" t "the link's token hashes to token_hash (AlgoLens' check)"
else
    echo "   (e-mail was sent; the token is only in the inbox)"
    TOKEN=e2e-token-not-available
fi

# ---- 4. decisions
echo "== 4. decisions"
SELF=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, payload, parent_id, approver_role) VALUES ('$PID', '$D', 'override_decision', '$REQUESTER', '{\"approved\":true}', $REQ, 'vp') RETURNING id")
call RecordDecision "{\"audit_id\":$SELF,\"approver\":\"$REQUESTER\",\"approved\":true,\"token\":\"$TOKEN\"}" >/dev/null
check "$(poll "$SELF")" refused "a self-approval is refused ($(row "$SELF"))"
DEC=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, payload, parent_id, approver_role) VALUES ('$PID', '$D', 'override_decision', '$APPROVER', '{\"approved\":true}', $REQ, 'vp') RETURNING id")
echo "   RecordDecision -> $(call RecordDecision "{\"audit_id\":$DEC,\"approver\":\"$APPROVER\",\"approved\":true,\"token\":\"$TOKEN\"}")"
check "$(poll "$DEC")" done "decision row $DEC done ($(row "$DEC"))"
DIFF=$(q "SELECT count(*) FROM (SELECT symbol, sum(quantity) q FROM trading.positions WHERE portfolio_id = '$PID' AND date = '$D' AND portfolio_type = 'qt_proposal' GROUP BY 1) p FULL JOIN (SELECT symbol, sum(quantity) q FROM trading.positions WHERE portfolio_id = '$PID' AND date = '$D' AND portfolio_type = 'qt' GROUP BY 1) g USING (symbol) WHERE COALESCE(p.q, 0) <> COALESCE(g.q, 0)")
check "$DIFF" 0 "qt == qt_proposal on every symbol"
check "$(q "SELECT book_source FROM trading.live_results WHERE portfolio_id = '$PID' AND date = '$D' AND portfolio_type = 'qt'")" override "qt book_source = override"
check "$(q "SELECT (risk_detail->'desk'->>'report_only') FROM trading.live_results WHERE portfolio_id = '$PID' AND date = '$D' AND portfolio_type = 'qt'")" true "the one pass is kept as a report in risk_detail"

# ---- 5. publish
echo "== 5. publish"
PUB=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by) VALUES ('$PID', '$D', 'publish', '$REQUESTER') RETURNING id")
echo "   Publish (no audit_id, found by portfolio and date as AlgoLens calls it) -> $(call Publish "{\"portfolio_id\":\"$PID\",\"date\":\"$D\",\"published_by\":\"$REQUESTER\"}")"
check "$(poll "$PUB")" done "publish row $PUB done ($(row "$PUB"))"
check "$(q "SELECT published_by || ' ' || (published_at IS NOT NULL) FROM trading.live_run_metadata WHERE portfolio_id = '$PID' AND date = '$D'")" "$REQUESTER true" "live_run_metadata.published_by/at set"
echo "   status: $(call GetRunStatus "{\"portfolio_id\":\"$PID\",\"date\":\"$D\"}")"

echo "== $([ "$FAILS" -eq 0 ] && echo "ALL PASSED" || echo "$FAILS FAILED")"
exit "$FAILS"
