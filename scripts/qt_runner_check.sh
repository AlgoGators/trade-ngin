#!/usr/bin/env bash
# QT runner check on a SCRATCH database (never new_algo_data): rulings 16, 17 and 29 against the
# real binary, on the QT_E2E_PORTFOLIO config (desk-editable). It needs market data through
# 2026-10-07 (bars of Thu 10-01 .. Wed 10-07).
#
#   QT_RUN_CMD   runs the engine: "$QT_RUN_CMD <args>" must print a line EXIT=<code>
#   QT_PSQL_CMD  a psql reading SQL from -c, e.g. "psql -X -Atq -v ON_ERROR_STOP=1"
#
# Checks:
#   ruling 17  the model run of an editable portfolio finalises system's Day T-1 from system's own
#              rows (system 10-01 == qt 10-01 once 10-02 has run: the two books were identical);
#   ruling 29  an unpublished day refuses the next run;
#   C7         the daily cutoff: --publish (the desk's approval) freezes the day and sends nothing
#              (publish_source desk, no sent_at); --send-now sends from the stored rows; the model
#              run publishes no day, weekends included; --fallback resets qt to the model's book
#              over a desk save and publishes it (publish_source fallback), a past day without an
#              e-mail (send_skipped); --send refuses an unpublished day; --fallback refuses a desk
#              row. Migration 027 must be applied;
#   ruling 16  a desk flatten of a held symbol stores a 0-quantity qt row priced at the latest
#              known price, with moved_by;
#   hardening  the override replaces the qt day's fills and marks the day last; an approval of a
#              proposal that moved after its snapshot is refused (C1); a published day refuses a second publish and a desk run, and a
#              model re-run leaves its qt and qt_proposal alone (C3).
set -uo pipefail
PID=QT_E2E_PORTFOLIO
DIR=qt_e2e
FAILS=0
q() { $QT_PSQL_CMD -c "$1"; }
run() { $QT_RUN_CMD "$@" | grep -o 'EXIT=[0-9]*' | cut -d= -f2; }
check() {
    if [ "$1" = "$2" ]; then echo "PASS  $3"; else echo "FAIL  $3: got '$1', want '$2'"; FAILS=$((FAILS + 1)); fi
}
publish() {
    local id day="$1"; shift
    id=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by) VALUES ('$PID', '$day', 'publish', 'check@x.org') RETURNING id")
    check "$(run --publish --portfolio-config $DIR --date "$day" --audit-id "$id" "$@")" 0 "approve $day $*"
    LAST_PUB=$id
}
# fallback <day> <requested_by> [--send-now]
fallback() {
    local id day="$1" by="$2"; shift 2
    id=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by) VALUES ('$PID', '$day', 'publish', '$by') RETURNING id")
    check "$(run --fallback --portfolio-config $DIR --date "$day" --audit-id "$id" "$@")" 0 "fallback $day ($by) $*"
    LAST_PUB=$id
}
meta() { q "SELECT COALESCE(published_by, '-') || ' ' || COALESCE(publish_source, '-') || ' ' || (sent_at IS NOT NULL)::text FROM trading.live_run_metadata WHERE portfolio_id = '$PID' AND date = '$1'"; }
book() { q "SELECT md5(string_agg(strategy_name || ':' || symbol || ':' || quantity::text, ',' ORDER BY strategy_name, symbol)) FROM trading.positions WHERE portfolio_id = '$PID' AND date = '$1' AND portfolio_type = '$2'"; }
row() {
    q "SELECT round(daily_pnl::numeric, 4) || ' ' || round(total_pnl::numeric, 4) || ' ' || round(current_portfolio_value::numeric, 4) FROM trading.live_results WHERE portfolio_id = '$PID' AND date = '$1' AND portfolio_type = '$2'"
}

[ "$(q "SELECT current_database()")" != "new_algo_data" ] || { echo "refusing: scratch databases only"; exit 2; }
for t in positions executions live_results equity_curve signals live_run_metadata strategy_trading_days_metadata; do
    q "DELETE FROM trading.$t WHERE portfolio_id = '$PID'"
done

echo "== first desk day and ruling 17"
check "$(run --portfolio-config $DIR --date 2026-10-01)" 0 "model 10-01 (first desk day, from system)"
publish 2026-10-01
check "$(meta 2026-10-01)" "check@x.org desk false" "an approval freezes the day (publish_source desk) and sends nothing"
check "$(q "SELECT status || ' ' || (result->>'emailed') || ' ' || (result ? 'published_at')::text FROM trading.position_overrides WHERE id = $LAST_PUB")" "done false true" "the approval row: done, not e-mailed"
SYS_BEFORE=$(row 2026-10-01 system)
check "$(run --portfolio-config $DIR --date 2026-10-02)" 0 "model 10-02 (from qt)"
SYS_AFTER=$(row 2026-10-01 system)
QT_AFTER=$(row 2026-10-01 qt)
echo "   system 10-01 before: $SYS_BEFORE; after: $SYS_AFTER; qt 10-01: $QT_AFTER"
check "$SYS_AFTER" "$QT_AFTER" "system's Day T-1 finalised like qt's (identical books)"
check "$(q "SELECT round(e.equity::numeric, 4) = round(g.equity::numeric, 4) FROM trading.equity_curve e JOIN trading.equity_curve g USING (portfolio_id, strategy_id, \"timestamp\") WHERE e.portfolio_id = '$PID' AND DATE(e.\"timestamp\") = '2026-10-01' AND e.portfolio_type = 'system' AND g.portfolio_type = 'qt'")" t "system equity 10-01 == qt equity 10-01"

echo "== ruling 29 and the daily cutoff (C7)"
check "$(run --portfolio-config $DIR --date 2026-10-03)" 1 "model 10-03 refuses while 10-02 is unpublished"
publish 2026-10-02 --send-now
check "$(q "SELECT (result->>'email_disabled') || ' ' || (result ? 'published_at')::text FROM trading.position_overrides WHERE id = $LAST_PUB")" "true true" "an approval with --send-now builds the e-mail from the stored rows (e-mail disabled: kept in the row)"
check "$(run --portfolio-config $DIR --date 2026-10-03)" 0 "model 10-03 after the catch-up"
publish 2026-10-03
check "$(run --portfolio-config $DIR --date 2026-10-04)" 0 "model 10-04 (T-1 Saturday)"
check "$(meta 2026-10-04)" "- - false" "the model run publishes no day, a weekend day included"
check "$(run --send --portfolio-config $DIR --date 2026-10-04)" 2 "--send refuses an unpublished day"
check "$(run --portfolio-config $DIR --date 2026-10-05)" 1 "model 10-05 refuses while 10-04 is unpublished"
fallback 2026-10-04 system:fallback-catchup
check "$(meta 2026-10-04)" "system:fallback-catchup fallback false" "a past day's fallback publishes the model's book, no e-mail"
check "$(q "SELECT result->>'send_skipped' FROM trading.position_overrides WHERE id = $LAST_PUB")" "past day" "send_skipped recorded"
check "$(run --portfolio-config $DIR --date 2026-10-05)" 0 "model 10-05"
# a desk save of 10-05 that is never approved: the 10:00 fallback discards it
q "UPDATE trading.positions SET quantity = quantity + 1 WHERE portfolio_id = '$PID' AND date = '2026-10-05' AND portfolio_type = 'qt_proposal' AND symbol = (SELECT min(symbol) FROM trading.positions WHERE portfolio_id = '$PID' AND date = '2026-10-05' AND portfolio_type = 'qt_proposal' AND quantity <> 0)" >/dev/null
SAVE5=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, reason, payload) VALUES ('$PID', '2026-10-05', 'save', 'check@x.org', 'check: never approved', '{}') RETURNING id")
check "$(run --desk --portfolio-config $DIR --date 2026-10-05 --audit-id "$SAVE5")" 0 "desk save 10-05"
check "$(q "SELECT book_source FROM trading.live_results WHERE portfolio_id = '$PID' AND date = '2026-10-05' AND portfolio_type = 'qt'")" desk "10-05 qt is the desk's"
DESKROW=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by) VALUES ('$PID', '2026-10-05', 'publish', 'check@x.org') RETURNING id")
check "$(run --fallback --portfolio-config $DIR --date 2026-10-05 --audit-id "$DESKROW")" 2 "--fallback refuses a desk row"
fallback 2026-10-05 system:fallback-10am --send-now
check "$(meta 2026-10-05)" "system:fallback-10am fallback false" "the 10:00 fallback publishes the model's book (e-mail disabled: no sent_at)"
check "$(book 2026-10-05 qt)" "$(book 2026-10-05 system)" "the fallback reset qt to the model's book over the desk save"
check "$(q "SELECT book_source FROM trading.live_results WHERE portfolio_id = '$PID' AND date = '2026-10-05' AND portfolio_type = 'qt'")" model "10-05 qt book_source model"
check "$(q "SELECT (result->>'email_disabled') FROM trading.position_overrides WHERE id = $LAST_PUB")" true "the fallback built its e-mail from the stored rows"
check "$(run --send --portfolio-config $DIR --date 2026-10-05)" 0 "--send of a published day (e-mail disabled: nothing sent)"
check "$(run --portfolio-config $DIR --date 2026-10-06)" 0 "model 10-06"

echo "== ruling 16"
read -r SLEEVE SYM HELD <<<"$(q "SELECT strategy_name, symbol, quantity FROM trading.positions WHERE portfolio_id = '$PID' AND date = '2026-10-05' AND portfolio_type = 'qt' AND quantity <> 0 ORDER BY abs(quantity) DESC, symbol LIMIT 1" | tr '|' ' ')"
echo "   flatten $SYM ($SLEEVE, held $HELD)"
q "UPDATE trading.positions SET quantity = 0 WHERE portfolio_id = '$PID' AND date = '2026-10-06' AND portfolio_type = 'qt_proposal' AND symbol = '$SYM'"
SAVE=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, reason, payload) VALUES ('$PID', '2026-10-06', 'save', 'check@x.org', 'check: flatten', '{}') RETURNING id")
check "$(run --desk --portfolio-config $DIR --date 2026-10-06 --audit-id "$SAVE")" 0 "desk run 10-06"
echo "   desk run gave $SYM: $(q "SELECT s->>'given' || ' moved_by ' || (s->>'moved_by') FROM trading.position_overrides, jsonb_array_elements(result->'symbols') s WHERE id = $SAVE AND s->>'symbol' = '$SYM'")"
# the loop may keep it (the no-trade buffer); an approved override trades the flatten exactly
# The request as AlgoLens writes it (contract C1): the day's qt_proposal snapshot, sorted by
# (strategy_name, symbol, quantity), and the SHA-256 of its json.dumps(separators=(",", ":")) bytes;
# inserted pending, then e-mailed (running, token, done) as the agent moves it.
SNAP="(SELECT '[' || COALESCE(string_agg(format('{\"strategy_name\":%s,\"symbol\":%s,\"quantity\":%s}', to_json(strategy_name)::text, to_json(symbol)::text, trunc(quantity)::bigint), ',' ORDER BY strategy_name COLLATE \"C\", symbol COLLATE \"C\", trunc(quantity)), '') || ']' AS s FROM trading.positions WHERE portfolio_id = '$PID' AND date = '2026-10-06' AND portfolio_type = 'qt_proposal')"
REQ=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, reason, payload) SELECT '$PID', '2026-10-06', 'override_request', 'check@x.org', 'check: flatten exactly', jsonb_build_object('proposal', s::jsonb, 'proposal_sha256', encode(sha256(convert_to(s, 'UTF8')), 'hex')) FROM $SNAP x RETURNING id")
q "UPDATE trading.position_overrides SET status = 'running', started_at = now() WHERE id = $REQ" >/dev/null
q "UPDATE trading.position_overrides SET status = 'done', finished_at = now(), token_hash = md5(random()::text), token_expires_at = now() + interval '48 hours' WHERE id = $REQ" >/dev/null
DEC=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, payload, parent_id, approver_role) VALUES ('$PID', '2026-10-06', 'override_decision', 'vp@x.org', '{\"approved\":true}', $REQ, 'vp') RETURNING id")
check "$(run --override --portfolio-config $DIR --date 2026-10-06 --audit-id "$DEC")" 0 "override run 10-06"
ZERO=$(q "SELECT count(*) || ' ' || bool_and(quantity = 0)::text || ' ' || bool_and(average_price > 0)::text || ' ' || bool_and(moved_by IS NOT NULL)::text FROM trading.positions WHERE portfolio_id = '$PID' AND date = '2026-10-06' AND portfolio_type = 'qt' AND symbol = '$SYM'")
echo "   qt rows of $SYM: $ZERO (count, all zero, priced, moved_by)"
check "$(echo "$ZERO" | cut -d' ' -f2-)" "true true true" "the close is a zero-quantity qt row, priced, with moved_by"
check "$(q "SELECT count(*) > 0 FROM trading.executions WHERE portfolio_id = '$PID' AND date = '2026-10-06' AND portfolio_type = 'qt' AND symbol = '$SYM' AND order_id LIKE 'qt-%'")" t "the close traded in qt with a qt- order id"

echo "== hardening (contract C1 to C5)"
# item 2: the override replaced the qt day's executions: no qt fill of a symbol the book did not
# trade survives from the model's copy (every non-ROLL qt fill of the day was written by this run)
check "$(q "SELECT count(*) FROM trading.executions e WHERE e.portfolio_id = '$PID' AND e.date = '2026-10-06' AND e.portfolio_type = 'qt' AND e.execution_type <> 'ROLL' AND e.symbol = '$SYM' AND e.side = (SELECT CASE WHEN $HELD > 0 THEN 'BUY' ELSE 'SELL' END)")" 0 "no copied model fill of $SYM in the same direction survives the override"
check "$(q "SELECT book_source FROM trading.live_results WHERE portfolio_id = '$PID' AND date = '2026-10-06' AND portfolio_type = 'qt'")" override "the override marked its day last"
# C1: a request whose proposal moved after it was snapshotted cannot be approved (a second
# decision on the first request is refused by migration 025's unique index, and by the engine)
REQ2=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, reason, payload) SELECT '$PID', '2026-10-06', 'override_request', 'check@x.org', 'check: a moved proposal', jsonb_build_object('proposal', s::jsonb, 'proposal_sha256', encode(sha256(convert_to(s, 'UTF8')), 'hex')) FROM $SNAP x RETURNING id")
q "UPDATE trading.position_overrides SET status = 'running', started_at = now() WHERE id = $REQ2" >/dev/null
q "UPDATE trading.position_overrides SET status = 'done', finished_at = now(), token_hash = md5(random()::text), token_expires_at = now() + interval '48 hours' WHERE id = $REQ2" >/dev/null
q "UPDATE trading.positions SET quantity = quantity + 1 WHERE portfolio_id = '$PID' AND date = '2026-10-06' AND portfolio_type = 'qt_proposal' AND symbol = '$SYM'" >/dev/null
DEC2=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, payload, parent_id, approver_role) VALUES ('$PID', '2026-10-06', 'override_decision', 'pres@x.org', '{\"approved\":true}', $REQ2, 'president') RETURNING id")
check "$(run --override --portfolio-config $DIR --date 2026-10-06 --audit-id "$DEC2")" 2 "an approval of a moved proposal is refused"
check "$(q "SELECT status || ' | ' || message FROM trading.position_overrides WHERE id = $DEC2")" "refused | the proposal changed after the override was requested; request a new override" "its row is refused with the contract's message"
publish 2026-10-06
# C3: a published day is frozen
PUB2=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by) VALUES ('$PID', '2026-10-06', 'publish', 'check@x.org') RETURNING id")
check "$(run --publish --portfolio-config $DIR --date 2026-10-06 --audit-id "$PUB2")" 2 "a second publish of 10-06 is refused"
SAVE2=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, reason, payload) VALUES ('$PID', '2026-10-06', 'save', 'check@x.org', 'check: after publish', '{}') RETURNING id")
check "$(run --desk --portfolio-config $DIR --date 2026-10-06 --audit-id "$SAVE2")" 2 "a desk run on a published day is refused"
FP="SELECT md5(string_agg(t, ';' ORDER BY t)) FROM (SELECT to_jsonb(p)::text t FROM trading.positions p WHERE portfolio_id = '$PID' AND date = '2026-10-06' AND portfolio_type IN ('qt', 'qt_proposal') UNION ALL SELECT (to_jsonb(r) - 'id' - 'created_at')::text FROM trading.live_results r WHERE portfolio_id = '$PID' AND date = '2026-10-06' AND portfolio_type = 'qt') x"
BEFORE=$(q "$FP")
check "$(run --portfolio-config $DIR --date 2026-10-06)" 0 "a model re-run of the published 10-06"
check "$(q "$FP")" "$BEFORE" "the re-run left the published day's qt and qt_proposal alone"

echo "== $([ "$FAILS" -eq 0 ] && echo "ALL PASSED" || echo "$FAILS FAILED")"
exit "$FAILS"
