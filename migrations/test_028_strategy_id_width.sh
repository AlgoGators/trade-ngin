#!/usr/bin/env bash
set -euo pipefail

psql_bin="${PSQL_BIN:-psql}"

if [[ -z "${STRATEGY_WIDTH_TEST_DSN:-}" || -z "${MIGRATION_TEST_DB:-}" ]]; then
    echo "Set STRATEGY_WIDTH_TEST_DSN and MIGRATION_TEST_DB to an owned disposable database." >&2
    exit 2
fi

actual_db="$("${psql_bin}" "${STRATEGY_WIDTH_TEST_DSN}" -XAtc 'select current_database()')"
if [[ "${actual_db}" != "${MIGRATION_TEST_DB}" || "${actual_db}" != *test* ]]; then
    echo "Refusing non-test or mismatched database: ${actual_db}" >&2
    exit 2
fi

existing="$("${psql_bin}" "${STRATEGY_WIDTH_TEST_DSN}" -XAtc \
    "select count(*) from information_schema.tables where table_schema='trading' and table_name in ('positions','live_results','signals')")"
if [[ "${existing}" != "0" ]]; then
    echo "Refusing database that already contains a strategy-width target table." >&2
    exit 2
fi

root="$(cd "$(dirname "$0")" && pwd)"
joined='LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST_TREND_FOLLOWING_SLOW'

"${psql_bin}" "${STRATEGY_WIDTH_TEST_DSN}" -Xv ON_ERROR_STOP=1 <<'SQL'
CREATE SCHEMA IF NOT EXISTS trading;
CREATE TABLE trading.positions (strategy_id varchar(50) NOT NULL);
CREATE TABLE trading.live_results (strategy_id varchar(50) NOT NULL);
CREATE TABLE trading.signals (strategy_id varchar(50) NOT NULL);
SQL

before="$("${psql_bin}" "${STRATEGY_WIDTH_TEST_DSN}" -XAtc \
    "select string_agg(table_name||':'||character_maximum_length,',' order by table_name) from information_schema.columns where table_schema='trading' and table_name in ('positions','live_results','signals') and column_name='strategy_id'")"
[[ "${before}" == 'live_results:50,positions:50,signals:50' ]]

if "${psql_bin}" "${STRATEGY_WIDTH_TEST_DSN}" -Xv ON_ERROR_STOP=1 -c \
    "insert into trading.positions values ('${joined}')" >/dev/null 2>&1; then
    echo "Expected the 62-byte identity to fail before widening." >&2
    exit 1
fi

"${psql_bin}" "${STRATEGY_WIDTH_TEST_DSN}" -Xv ON_ERROR_STOP=1 -f "${root}/028_strategy_id_width.sql" >/dev/null
"${psql_bin}" "${STRATEGY_WIDTH_TEST_DSN}" -Xv ON_ERROR_STOP=1 -f "${root}/028_strategy_id_width.sql" >/dev/null

after="$("${psql_bin}" "${STRATEGY_WIDTH_TEST_DSN}" -XAtc \
    "select string_agg(table_name||':'||character_maximum_length,',' order by table_name) from information_schema.columns where table_schema='trading' and table_name in ('positions','live_results','signals') and column_name='strategy_id'")"
[[ "${after}" == 'live_results:100,positions:100,signals:100' ]]

for table_name in positions live_results signals; do
    "${psql_bin}" "${STRATEGY_WIDTH_TEST_DSN}" -Xv ON_ERROR_STOP=1 -c \
        "insert into trading.${table_name} values ('${joined}')" >/dev/null
done

if "${psql_bin}" "${STRATEGY_WIDTH_TEST_DSN}" -Xv ON_ERROR_STOP=1 \
    -f "${root}/028_strategy_id_width_rollback.sql" >/dev/null 2>&1; then
    echo "Rollback should refuse rows whose identity exceeds 50 bytes." >&2
    exit 1
fi

echo "strategy-id-width migration: PASS"
