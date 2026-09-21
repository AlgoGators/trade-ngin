#!/usr/bin/env bash
# Verifies 012_position_overrides_portfolio_scope.sql against disposable PostgreSQL.
# It seeds one uniquely attributable and one ambiguous legacy audit row, then
# checks immutable originals, NOT VALID enforcement, append-only mappings, and
# a second forward application.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONTAINER_NAME="${CONTAINER_NAME:-trade-ngin-test-012-$RANDOM}"
PGDATABASE="${PGDATABASE:-trade_ngin_test}"
PGUSER="${PGUSER:-postgres}"
PGPASSWORD="${PGPASSWORD:-postgres}"
IMAGE="${POSTGRES_IMAGE:-postgres:16-alpine}"
CONTAINER_ID=""
CONTAINER_CANDIDATE=""
PSQL=()
pass=0
fail=0

ok()  { echo "  PASS  $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $1"; fail=$((fail + 1)); }
cleanup() {
    if [ -n "$CONTAINER_ID" ]; then
        docker rm -f "$CONTAINER_ID" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

if ! command -v docker >/dev/null 2>&1; then
    echo "BLOCKED: Docker is required to run this disposable PostgreSQL harness."
    exit 2
fi
if ! docker info >/dev/null 2>&1; then
    echo "BLOCKED: Docker daemon is unavailable; no database was started."
    exit 2
fi
if ! CONTAINER_CANDIDATE="$(docker run --rm -d --name "$CONTAINER_NAME" \
    -e "POSTGRES_DB=$PGDATABASE" -e "POSTGRES_USER=$PGUSER" \
    -e "POSTGRES_PASSWORD=$PGPASSWORD" "$IMAGE")"; then
    echo "BLOCKED: could not start disposable PostgreSQL container from $IMAGE."
    exit 2
fi
if [ -z "$CONTAINER_CANDIDATE" ]; then
    echo "BLOCKED: Docker started no identifiable disposable PostgreSQL container."
    exit 2
fi
CONTAINER_ID="$CONTAINER_CANDIDATE"
PSQL=(docker exec -i "$CONTAINER_ID" psql -v ON_ERROR_STOP=1 -q -X -U "$PGUSER" -d "$PGDATABASE")
for _ in $(seq 1 30); do
    "${PSQL[@]}" -c 'SELECT 1' >/dev/null 2>&1 && break
    sleep 1
done
if ! "${PSQL[@]}" -c 'SELECT 1' >/dev/null 2>&1; then
    echo "BLOCKED: disposable PostgreSQL container did not become ready."
    exit 2
fi

apply() { "${PSQL[@]}" < "$HERE/$1" >/dev/null 2>&1; }
scalar() { "${PSQL[@]}" -At -c "$1" 2>/dev/null; }

echo "########## BASELINE: pre-012 append-only override audit ##########"
"${PSQL[@]}" <<'SQL'
CREATE SCHEMA trading;
CREATE TABLE trading.position_overrides (
    id BIGSERIAL PRIMARY KEY, user_id INTEGER NOT NULL, source_app TEXT NOT NULL,
    strategy_id TEXT NOT NULL, symbol TEXT NOT NULL, before_state JSONB NOT NULL,
    after_state JSONB NOT NULL, reason TEXT NOT NULL,
    risk_check_result JSONB NOT NULL, overrode_risk BOOLEAN NOT NULL DEFAULT FALSE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_position_overrides_strategy_created
    ON trading.position_overrides (strategy_id, created_at DESC);
CREATE OR REPLACE RULE position_overrides_no_update AS
    ON UPDATE TO trading.position_overrides DO INSTEAD NOTHING;
CREATE OR REPLACE RULE position_overrides_no_delete AS
    ON DELETE TO trading.position_overrides DO INSTEAD NOTHING;
CREATE TABLE trading.positions (
    id BIGSERIAL PRIMARY KEY, portfolio_id TEXT NOT NULL, strategy_id TEXT NOT NULL,
    symbol TEXT NOT NULL, date DATE NOT NULL
);
INSERT INTO trading.position_overrides
    (user_id, source_app, strategy_id, symbol, before_state, after_state,
     reason, risk_check_result, created_at)
VALUES
    (1, 'algolens', 'unique-strategy', 'UNIQUE', '{}'::jsonb, '{}'::jsonb,
     'legacy unique', '{"passed": true}'::jsonb, '2026-09-01T10:00:00Z'),
    (1, 'algolens', 'ambiguous-strategy', 'AMBIG', '{}'::jsonb, '{}'::jsonb,
     'legacy ambiguous', '{"passed": true}'::jsonb, '2026-09-01T11:00:00Z');
INSERT INTO trading.positions (portfolio_id, strategy_id, symbol, date)
VALUES
    ('BOOK_UNIQUE', 'unique-strategy', 'UNIQUE', '2026-09-01'),
    ('BOOK_AMBIG_A', 'ambiguous-strategy', 'AMBIG', '2026-09-01'),
    ('BOOK_AMBIG_B', 'ambiguous-strategy', 'AMBIG', '2026-09-01');
SQL
[ $? -eq 0 ] && ok "pre-012 fixture built" || bad "pre-012 fixture failed"

before_rows="$(scalar 'SELECT count(*) FROM trading.position_overrides')"
[ "$before_rows" = "2" ] && ok "two legacy audit rows seeded" || bad "expected two legacy audit rows, got $before_rows"

echo ""
echo "########## APPLYING 012 ##########"
if apply 012_position_overrides_portfolio_scope.sql; then
    ok "012 applied"
else
    bad "012 failed to apply"
fi
legacy_nulls="$(scalar 'SELECT count(*) FROM trading.position_overrides WHERE portfolio_id IS NULL')"
[ "$legacy_nulls" = "2" ] && ok "original legacy audit rows remain unscoped" || bad "legacy audit rows were rewritten (NULL count $legacy_nulls)"
unique_scope="$(scalar "SELECT portfolio_id FROM trading.position_override_legacy_scopes WHERE override_id = 1")"
[ "$unique_scope" = "BOOK_UNIQUE" ] && ok "unique legacy override mapped once" || bad "unique legacy override mapping is $unique_scope"
ambiguous_scopes="$(scalar "SELECT count(*) FROM trading.position_override_legacy_scopes WHERE override_id = 2")"
[ "$ambiguous_scopes" = "0" ] && ok "ambiguous legacy override remains unscoped" || bad "ambiguous legacy override received $ambiguous_scopes mapping(s)"

if apply 012_position_overrides_portfolio_scope_rollback.sql; then
    bad "rollback discarded inferred legacy attribution"
else
    ok "rollback refuses inferred legacy attribution"
fi

if "${PSQL[@]}" -c "INSERT INTO trading.position_overrides
    (user_id, source_app, strategy_id, symbol, before_state, after_state, reason, risk_check_result)
    VALUES (2, 'algolens', 'new-strategy', 'UNSCOPED', '{}'::jsonb, '{}'::jsonb,
            'missing portfolio', '{\"passed\": true}'::jsonb)" >/dev/null 2>&1; then
    bad "new unscoped override INSERT succeeded"
else
    ok "new unscoped override INSERT rejected"
fi
if "${PSQL[@]}" -c "INSERT INTO trading.position_overrides
    (user_id, source_app, strategy_id, symbol, before_state, after_state, reason,
     risk_check_result, portfolio_id)
    VALUES (2, 'algolens', 'new-strategy', 'SCOPED', '{}'::jsonb, '{}'::jsonb,
            'with portfolio', '{\"passed\": true}'::jsonb, 'BOOK_NEW')" >/dev/null 2>&1; then
    ok "new scoped override INSERT accepted"
else
    bad "new scoped override INSERT rejected"
fi

"${PSQL[@]}" -c "UPDATE trading.position_overrides SET reason = 'tampered' WHERE id = 1" >/dev/null 2>&1
reason="$(scalar 'SELECT reason FROM trading.position_overrides WHERE id = 1')"
[ "$reason" = "legacy unique" ] && ok "original audit UPDATE remains refused" || bad "original audit UPDATE changed reason to $reason"
"${PSQL[@]}" -c 'DELETE FROM trading.position_overrides WHERE id = 1' >/dev/null 2>&1
original_row="$(scalar 'SELECT count(*) FROM trading.position_overrides WHERE id = 1')"
[ "$original_row" = "1" ] && ok "original audit DELETE remains refused" || bad "original audit DELETE removed the row"
"${PSQL[@]}" -c "UPDATE trading.position_override_legacy_scopes SET portfolio_id = 'TAMPERED' WHERE override_id = 1" >/dev/null 2>&1
unique_scope="$(scalar 'SELECT portfolio_id FROM trading.position_override_legacy_scopes WHERE override_id = 1')"
[ "$unique_scope" = "BOOK_UNIQUE" ] && ok "legacy scope UPDATE is refused" || bad "legacy scope UPDATE changed portfolio to $unique_scope"
"${PSQL[@]}" -c 'DELETE FROM trading.position_override_legacy_scopes WHERE override_id = 1' >/dev/null 2>&1
mapping_row="$(scalar 'SELECT count(*) FROM trading.position_override_legacy_scopes WHERE override_id = 1')"
[ "$mapping_row" = "1" ] && ok "legacy scope DELETE is refused" || bad "legacy scope DELETE removed the mapping"

echo ""
echo "########## IDEMPOTENCE: applying 012 twice ##########"
if apply 012_position_overrides_portfolio_scope.sql; then
    ok "second 012 succeeded"
else
    bad "second 012 failed"
fi
mapping_count="$(scalar 'SELECT count(*) FROM trading.position_override_legacy_scopes')"
[ "$mapping_count" = "1" ] && ok "second 012 created no duplicate mappings" || bad "second 012 left $mapping_count mappings"
scoped_rows="$(scalar "SELECT count(*) FROM trading.position_overrides WHERE portfolio_id = 'BOOK_NEW'")"
[ "$scoped_rows" = "1" ] && ok "second 012 preserved scoped audit row" || bad "second 012 disturbed scoped audit rows"

echo ""
echo "########## ROLLBACK: attribution safety and transaction locks ##########"
if apply 012_position_overrides_portfolio_scope_rollback.sql; then
    bad "rollback discarded portfolio attribution"
else
    ok "rollback refuses portfolio attribution"
fi
[ "$(scalar 'SELECT count(*) FROM trading.position_override_legacy_scopes')" = "1" ] \
    && ok "refused rollback preserves companion" || bad "refused rollback lost companion"

# Fixture reset is confined to this disposable container. Probe the actual
# rollback transaction immediately before its first safety read. ACCESS
# EXCLUSIVE conflicts with writers' ROW EXCLUSIVE locks and survives to COMMIT.
"${PSQL[@]}" -c 'TRUNCATE trading.position_override_legacy_scopes, trading.position_overrides' >/dev/null
if sed '/    IF EXISTS (/i\
    IF (SELECT count(*) FROM pg_locks WHERE pid = pg_backend_pid()\
        AND granted AND mode = '\''AccessExclusiveLock'\''\
        AND relation IN ('\''trading.position_overrides'\''::regclass,\
                         '\''trading.position_override_legacy_scopes'\''::regclass)) <> 2 THEN\
        RAISE EXCEPTION '\''attribution locks missing before safety read'\'';\
    END IF;\
' "$HERE/012_position_overrides_portfolio_scope_rollback.sql" | "${PSQL[@]}"; then
    ok "safe rollback holds both exclusive locks before attribution checks"
else
    bad "safe rollback or transaction lock assertion failed"
fi
if apply 012_position_overrides_portfolio_scope_rollback.sql; then
    ok "safe rollback is idempotent"
else
    bad "repeated safe rollback failed"
fi
column_count="$(scalar "SELECT count(*) FROM information_schema.columns WHERE table_schema='trading' AND table_name='position_overrides' AND column_name='portfolio_id'")"
[ "$column_count" = "0" ] \
    && ok "safe rollback removes portfolio column" || bad "safe rollback left portfolio column"

echo ""
echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
