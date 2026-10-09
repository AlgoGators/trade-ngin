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
#   ruling 29  a non-trading day (T-1 a weekend) is published by the model run itself
#              (published_by system:non-trading-day), and the next trading day runs without a
#              desk publish of it; an unpublished trading day refuses the next run;
#   ruling 16  a desk flatten of a held symbol stores a 0-quantity qt row priced at the latest
#              known price, with moved_by.
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
    local id
    id=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by) VALUES ('$PID', '$1', 'publish', 'check@x.org') RETURNING id")
    check "$(run --publish --portfolio-config $DIR --date "$1" --audit-id "$id")" 0 "publish $1"
}
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
SYS_BEFORE=$(row 2026-10-01 system)
check "$(run --portfolio-config $DIR --date 2026-10-02)" 0 "model 10-02 (from qt)"
SYS_AFTER=$(row 2026-10-01 system)
QT_AFTER=$(row 2026-10-01 qt)
echo "   system 10-01 before: $SYS_BEFORE; after: $SYS_AFTER; qt 10-01: $QT_AFTER"
check "$SYS_AFTER" "$QT_AFTER" "system's Day T-1 finalised like qt's (identical books)"
check "$(q "SELECT round(e.equity::numeric, 4) = round(g.equity::numeric, 4) FROM trading.equity_curve e JOIN trading.equity_curve g USING (portfolio_id, strategy_id, \"timestamp\") WHERE e.portfolio_id = '$PID' AND DATE(e.\"timestamp\") = '2026-10-01' AND e.portfolio_type = 'system' AND g.portfolio_type = 'qt'")" t "system equity 10-01 == qt equity 10-01"

echo "== ruling 29"
check "$(run --portfolio-config $DIR --date 2026-10-03)" 1 "model 10-03 refuses while 10-02 is unpublished"
publish 2026-10-02
check "$(run --portfolio-config $DIR --date 2026-10-03)" 0 "model 10-03 after the catch-up"
publish 2026-10-03
check "$(run --portfolio-config $DIR --date 2026-10-04)" 0 "model 10-04 (T-1 Saturday)"
check "$(q "SELECT published_by FROM trading.live_run_metadata WHERE portfolio_id = '$PID' AND date = '2026-10-04'")" system:non-trading-day "10-04 published by the model run"
check "$(q "SELECT book_source FROM trading.live_results WHERE portfolio_id = '$PID' AND date = '2026-10-04' AND portfolio_type = 'qt'")" model "10-04 qt book_source model"
check "$(run --portfolio-config $DIR --date 2026-10-05)" 0 "model 10-05"
P5=$(q "SELECT COALESCE(published_by, '') FROM trading.live_run_metadata WHERE portfolio_id = '$PID' AND date = '2026-10-05'")
echo "   10-05 published_by: '${P5}'"
[ -z "$P5" ] && publish 2026-10-05
check "$(run --portfolio-config $DIR --date 2026-10-06)" 0 "model 10-06 runs with no desk publish of the weekend"

echo "== ruling 16"
read -r SLEEVE SYM HELD <<<"$(q "SELECT strategy_name, symbol, quantity FROM trading.positions WHERE portfolio_id = '$PID' AND date = '2026-10-05' AND portfolio_type = 'qt' AND quantity <> 0 ORDER BY abs(quantity) DESC, symbol LIMIT 1" | tr '|' ' ')"
echo "   flatten $SYM ($SLEEVE, held $HELD)"
q "UPDATE trading.positions SET quantity = 0 WHERE portfolio_id = '$PID' AND date = '2026-10-06' AND portfolio_type = 'qt_proposal' AND symbol = '$SYM'"
SAVE=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, reason, payload) VALUES ('$PID', '2026-10-06', 'save', 'check@x.org', 'check: flatten', '{}') RETURNING id")
check "$(run --desk --portfolio-config $DIR --date 2026-10-06 --audit-id "$SAVE")" 0 "desk run 10-06"
echo "   desk run gave $SYM: $(q "SELECT s->>'given' || ' moved_by ' || (s->>'moved_by') FROM trading.position_overrides, jsonb_array_elements(result->'symbols') s WHERE id = $SAVE AND s->>'symbol' = '$SYM'")"
# the loop may keep it (the no-trade buffer); an approved override trades the flatten exactly
REQ=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, reason, status, token_hash, token_expires_at) VALUES ('$PID', '2026-10-06', 'override_request', 'check@x.org', 'check: flatten exactly', 'done', 'x', now() + interval '48 hours') RETURNING id")
DEC=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, payload, parent_id, approver_role) VALUES ('$PID', '2026-10-06', 'override_decision', 'vp@x.org', '{\"approved\":true}', $REQ, 'vp') RETURNING id")
check "$(run --override --portfolio-config $DIR --date 2026-10-06 --audit-id "$DEC")" 0 "override run 10-06"
ZERO=$(q "SELECT count(*) || ' ' || bool_and(quantity = 0)::text || ' ' || bool_and(average_price > 0)::text || ' ' || bool_and(moved_by IS NOT NULL)::text FROM trading.positions WHERE portfolio_id = '$PID' AND date = '2026-10-06' AND portfolio_type = 'qt' AND symbol = '$SYM'")
echo "   qt rows of $SYM: $ZERO (count, all zero, priced, moved_by)"
check "$(echo "$ZERO" | cut -d' ' -f2-)" "true true true" "the close is a zero-quantity qt row, priced, with moved_by"
check "$(q "SELECT count(*) > 0 FROM trading.executions WHERE portfolio_id = '$PID' AND date = '2026-10-06' AND portfolio_type = 'qt' AND symbol = '$SYM' AND order_id LIKE 'qt-%'")" t "the close traded in qt with a qt- order id"

echo "== $([ "$FAILS" -eq 0 ] && echo "ALL PASSED" || echo "$FAILS FAILED")"
exit "$FAILS"
