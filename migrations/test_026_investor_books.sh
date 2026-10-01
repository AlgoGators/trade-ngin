#!/usr/bin/env bash
set -euo pipefail

psql_bin="${PSQL_BIN:-psql}"
if [[ -z "${INVESTOR_BOOK_TEST_DSN:-}" || -z "${MIGRATION_TEST_DB:-}" ]]; then
    echo "Set INVESTOR_BOOK_TEST_DSN and MIGRATION_TEST_DB to an empty owned test database." >&2
    exit 2
fi
actual_db="$("${psql_bin}" "${INVESTOR_BOOK_TEST_DSN}" -XAtc 'select current_database()')"
if [[ "${actual_db}" != "${MIGRATION_TEST_DB}" || "${actual_db}" != *test* ]]; then
    echo "Refusing non-test or mismatched database: ${actual_db}" >&2
    exit 2
fi
existing="$("${psql_bin}" "${INVESTOR_BOOK_TEST_DSN}" -XAtc \
    "select count(*) from information_schema.tables where table_schema='trading'")"
if [[ "${existing}" != "0" ]]; then
    echo "Refusing database that already contains trading tables." >&2
    exit 2
fi

root="$(cd "$(dirname "$0")" && pwd)"
"${psql_bin}" "${INVESTOR_BOOK_TEST_DSN}" -Xv ON_ERROR_STOP=1 <<'SQL'
CREATE SCHEMA trading;
CREATE TABLE trading.strategy_trading_days_metadata (
    strategy_id varchar(100) PRIMARY KEY,
    live_start_date date NOT NULL
);
INSERT INTO trading.strategy_trading_days_metadata
VALUES ('LIVE_TREND_FOLLOWING', '2025-01-01');
SQL

"${psql_bin}" "${INVESTOR_BOOK_TEST_DSN}" -Xv ON_ERROR_STOP=1 \
    -f "${root}/026_investor_books_and_publications.sql" >/dev/null

"${psql_bin}" "${INVESTOR_BOOK_TEST_DSN}" -Xv ON_ERROR_STOP=1 <<'SQL'
DO $$
DECLARE
    first_id uuid;
    replay_id uuid;
    before_count bigint;
BEGIN
    first_id := trading.onboard_investor_book(
        'investor_alpha', 'INVESTOR_ALPHA', 1000000, '2026-10-01',
        '["LIVE_EQUITY_ALPHA_BETA","LIVE_TREND_FOLLOWING"]'::jsonb,
        'migration-test');
    replay_id := trading.onboard_investor_book(
        'investor_alpha', 'INVESTOR_ALPHA', 1000000, '2026-10-01',
        '["LIVE_TREND_FOLLOWING","LIVE_EQUITY_ALPHA_BETA"]'::jsonb,
        'migration-test');
    IF first_id IS DISTINCT FROM replay_id THEN
        RAISE EXCEPTION 'identical onboarding replay changed identity';
    END IF;

    SELECT count(*) INTO before_count FROM trading.investor_books;
    BEGIN
        PERFORM trading.onboard_investor_book(
            'investor_alpha', 'INVESTOR_ALPHA', 2000000, '2026-10-01',
            '["LIVE_EQUITY_ALPHA_BETA","LIVE_TREND_FOLLOWING"]'::jsonb,
            'migration-test');
        RAISE EXCEPTION 'conflicting replay was accepted';
    EXCEPTION WHEN OTHERS THEN
        IF SQLERRM = 'conflicting replay was accepted' THEN RAISE; END IF;
    END;
    IF (SELECT count(*) FROM trading.investor_books) <> before_count THEN
        RAISE EXCEPTION 'conflicting replay partially wrote';
    END IF;

    BEGIN
        UPDATE trading.investor_books SET initial_capital = 2
         WHERE portfolio_id = 'INVESTOR_ALPHA';
        RAISE EXCEPTION 'capital mutation was accepted';
    EXCEPTION WHEN OTHERS THEN
        IF SQLERRM = 'capital mutation was accepted' THEN RAISE; END IF;
    END;
    UPDATE trading.investor_books SET is_active = false
     WHERE portfolio_id = 'INVESTOR_ALPHA';
    IF (SELECT is_active FROM trading.investor_books
         WHERE portfolio_id = 'INVESTOR_ALPHA') THEN
        RAISE EXCEPTION 'active status did not update';
    END IF;

    BEGIN
        DELETE FROM trading.investor_book_strategies
         WHERE portfolio_id = 'INVESTOR_ALPHA';
        RAISE EXCEPTION 'membership deletion was accepted';
    EXCEPTION WHEN OTHERS THEN
        IF SQLERRM = 'membership deletion was accepted' THEN RAISE; END IF;
    END;
    BEGIN
        UPDATE trading.strategy_trading_days_metadata SET live_start_date = '2026-10-02'
         WHERE portfolio_id = 'INVESTOR_ALPHA';
        RAISE EXCEPTION 'anchor mutation was accepted';
    EXCEPTION WHEN OTHERS THEN
        IF SQLERRM = 'anchor mutation was accepted' THEN RAISE; END IF;
    END;

    PERFORM trading.onboard_investor_book(
        'investor_beta', 'INVESTOR_BETA', 500000, '2026-10-15',
        '["LIVE_TREND_FOLLOWING"]'::jsonb, 'migration-test');

    IF (SELECT count(*) FROM trading.investor_books) <> 2
       OR (SELECT count(*) FROM trading.investor_book_strategies) <> 3
       OR (SELECT count(*) FROM trading.strategy_trading_days_metadata
            WHERE portfolio_id IS NOT NULL) <> 3
       OR (SELECT count(*) FROM trading.strategy_trading_days_metadata
            WHERE strategy_id = 'LIVE_TREND_FOLLOWING') <> 3 THEN
        RAISE EXCEPTION 'onboarding rows or portfolio-scoped anchors are incomplete';
    END IF;
    IF (SELECT count(*) FROM trading.investor_books WHERE model_stream <> 'system') <> 0 THEN
        RAISE EXCEPTION 'non-system investor stream exists';
    END IF;
END $$;
SQL

if "${psql_bin}" "${INVESTOR_BOOK_TEST_DSN}" -Xv ON_ERROR_STOP=1 \
    -f "${root}/026_investor_books_and_publications_rollback.sql" >/dev/null 2>&1; then
    echo "Rollback should refuse durable investor books." >&2
    exit 1
fi

echo "investor-book onboarding migration: PASS"
