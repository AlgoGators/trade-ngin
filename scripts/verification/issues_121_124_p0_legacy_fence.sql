\set ON_ERROR_STOP on

-- OLD-side adapter for the legacy house artifact. Migration 015 was the last
-- row-fence definition before investor-book migration 026 extended it. The
-- OLD binary validates this exact function body by digest.
CREATE OR REPLACE FUNCTION trading.fence_runtime_publication_row()
RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE row_value jsonb;
DECLARE stream text;
BEGIN
    row_value := CASE WHEN TG_OP = 'DELETE' THEN to_jsonb(OLD) ELSE to_jsonb(NEW) END;
    stream := row_value->>'portfolio_type';
    IF TG_OP = 'UPDATE' AND
       (to_jsonb(OLD)->'strategy_id',to_jsonb(OLD)->'portfolio_id',to_jsonb(OLD)->'portfolio_type')
       IS DISTINCT FROM
       (to_jsonb(NEW)->'strategy_id',to_jsonb(NEW)->'portfolio_id',to_jsonb(NEW)->'portfolio_type') THEN
        RAISE EXCEPTION 'runtime_scope_move_unsupported';
    END IF;
    IF stream IS NOT NULL AND stream NOT IN
       ('system','qt','benchmark','benchmark_rebench','benchmark_frozen_shadow')
       AND NOT (TG_TABLE_SCHEMA = 'trading' AND TG_TABLE_NAME = 'positions' AND stream = 'qt_proposal') THEN
        RAISE EXCEPTION 'runtime_stream_unsupported';
    END IF;
    IF TG_TABLE_NAME IN ('positions','equity_curve') AND
       stream IN ('benchmark','benchmark_rebench','benchmark_frozen_shadow') THEN
        -- Replay uses its historical snapshot, but still serializes the book.
        PERFORM pg_advisory_xact_lock(hashtextextended(
            'algolens:qt-book:' || upper(btrim(row_value->>'portfolio_id')),0));
    ELSE
        PERFORM trading.lock_runtime_scope(row_value->>'strategy_id',row_value->>'portfolio_id',
                                          TG_TABLE_NAME = 'positions' AND stream IN ('qt','qt_proposal'));
    END IF;
    IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
    RETURN NEW;
END $$;

DO $$
BEGIN
    IF md5((SELECT prosrc FROM pg_proc
            WHERE oid=to_regprocedure('trading.fence_runtime_publication_row()')))
       <> '419771cec97836560ae952c120aac4d1' THEN
        RAISE EXCEPTION 'legacy P0 fence digest mismatch';
    END IF;
END $$;
