-- STAGED PROPOSAL ONLY (lane N5, incubating model admission). Not applied to any
-- database outside the owned, network-none test fixture.
--
-- Model (system-stream) publication admits an incubating, ACTIVE scope exactly like a
-- live one. Only trading.lock_runtime_scope is replaced (013_runtime_control.sql:158-185);
-- the md5-pinned trading.fence_runtime_publication_row (013, re-created by 015) and every
-- trigger are untouched, so every capability check keeps its pinned hash.
--
-- The fence passes allow_incubating = (positions AND stream IN ('qt','qt_proposal')).
--   allow_incubating = false (system rows, and every row of the other seven tables):
--       013: lifecycle = 'live' AND is_active
--       025: lifecycle IN ('live','incubating') AND is_active
--   allow_incubating = true (positions qt/qt_proposal): unchanged,
--       lifecycle IN ('live','incubating'), is_active not required.
-- Retired (and any other lifecycle) stays refused on every path. An engine/book scope
-- with no, or more than one, registry match stays 'runtime_scope_unsupported', exactly
-- as in 013 (unregistered strategies keep that protection).
-- The function cannot see the stream, so a qt-stream row of equity_curve, live_results
-- or executions for an incubating, active scope now also passes this database layer;
-- the QT desk stays live-only in both applications (qt_desk_owner_scope.hpp,
-- qt_desk_current_facts.cpp, AlgoLens qt_workflow / qt_decision_read).
--
-- Desk finalizers (qt-stream positions/live_results/equity_curve of a processed desk day) have no
-- lifecycle check of their own; AlgoLens refuses any lifecycle change away from live while a desk
-- day on the strategy's books awaits its next-day finalization (N5 r2, repositories.py).
-- Re-applying 013 (it is repeatable) restores the 013 lock body AND the 013 fence body over 015's;
-- after any 013 re-apply, re-apply 015 and then 025.
-- Rollback: 025_runtime_scope_model_incubating_rollback.sql restores the 013 body.
BEGIN;
DO $preflight$
DECLARE
    lock_oid oid := to_regprocedure('trading.lock_runtime_scope(text,text,boolean)');
    fence_oid oid := to_regprocedure('trading.fence_runtime_publication_row()');
    lock_013 constant text := 'bdda022b058f416faf198fe4e4fae95d';
    lock_025 constant text := '0bd1e0badb6bee13c329268ded8f0e03';
    fence_013 constant text := 'a469e2b5cfd77d447383ae1fb78d5e7a';
    fence_015 constant text := '419771cec97836560ae952c120aac4d1';
    attached text[];
BEGIN
    IF lock_oid IS NULL OR fence_oid IS NULL
       OR to_regclass('trading.strategy_registry') IS NULL
       OR to_regclass('trading.strategy_book_memberships') IS NULL THEN
        RAISE EXCEPTION 'runtime025 requires migration 013';
    END IF;
    IF (SELECT count(*) FROM information_schema.columns WHERE table_schema='trading'
          AND table_name='strategy_registry'
          AND column_name IN ('id','strategy_type','portfolio_id','lifecycle','is_active','runtime_revision')) <> 6 THEN
        RAISE EXCEPTION 'runtime025 registry shape unsupported';
    END IF;
    -- The new predicate relies on both columns being NOT NULL (AlgoLens 001/002): with a NULL
    -- is_active, NOT allow_incubating AND NOT is_active is NULL and would not refuse.
    IF (SELECT count(*) FROM pg_attribute WHERE attrelid='trading.strategy_registry'::regclass
          AND attname IN ('lifecycle','is_active') AND attnum>0 AND NOT attisdropped AND attnotnull) <> 2 THEN
        RAISE EXCEPTION 'runtime025 requires NOT NULL strategy_registry.lifecycle and is_active';
    END IF;
    -- Exactly the 013 function (or this migration's own body: re-application is a no-op).
    IF (SELECT count(*) FROM pg_proc WHERE pronamespace='trading'::regnamespace
          AND proname='lock_runtime_scope') <> 1 THEN
        RAISE EXCEPTION 'runtime025 lock_runtime_scope overload unsupported';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_proc f JOIN pg_language l ON l.oid=f.prolang
        WHERE f.oid=lock_oid AND l.lanname='plpgsql' AND f.prokind='f' AND f.proretset
          AND NOT f.prosecdef AND f.proconfig IS NULL
          AND pg_get_function_arguments(f.oid)='engine_id text, book text, allow_incubating boolean DEFAULT false'
          AND pg_get_function_result(f.oid)='TABLE(registry_id text, registry_revision bigint)'
          AND md5(f.prosrc) IN (lock_013,lock_025)) THEN
        RAISE EXCEPTION 'runtime025 requires the unchanged 013 lock_runtime_scope';
    END IF;
    -- The only caller is the 013 or 015 fence (both pinned); it decides allow_incubating.
    IF NOT EXISTS (SELECT 1 FROM pg_proc f JOIN pg_language l ON l.oid=f.prolang
        WHERE f.oid=fence_oid AND f.prorettype='pg_catalog.trigger'::regtype AND f.pronargs=0
          AND l.lanname='plpgsql' AND md5(f.prosrc) IN (fence_013,fence_015)) THEN
        RAISE EXCEPTION 'runtime025 requires the 013 or 015 publication fence';
    END IF;
    SELECT array_agg(n.nspname||'.'||r.relname ORDER BY n.nspname,r.relname) INTO attached
      FROM pg_trigger t JOIN pg_class r ON r.oid=t.tgrelid
      JOIN pg_namespace n ON n.oid=r.relnamespace
      WHERE t.tgfoid=fence_oid AND NOT t.tgisinternal
        AND t.tgname='runtime_publication_fence' AND t.tgenabled='O' AND t.tgtype=31
        AND t.tgqual IS NULL AND t.tgattr::text='' AND t.tgnargs=0
        AND octet_length(t.tgargs)=0 AND t.tgconstraint=0
        AND NOT t.tgdeferrable AND NOT t.tginitdeferred AND t.tgparentid=0
        AND t.tgoldtable IS NULL AND t.tgnewtable IS NULL;
    IF attached IS DISTINCT FROM ARRAY[
      'trading.equity_curve','trading.executions','trading.live_results',
      'trading.live_run_metadata','trading.positions','trading.risk_limits',
      'trading.run_inputs','trading.signals']
      OR (SELECT count(*) FROM pg_trigger WHERE tgfoid=fence_oid AND NOT tgisinternal)<>8 THEN
        RAISE EXCEPTION 'runtime025 fence triggers unsupported';
    END IF;
END $preflight$;

CREATE OR REPLACE FUNCTION trading.lock_runtime_scope(engine_id text, book text,
                                                      allow_incubating boolean DEFAULT false)
RETURNS TABLE(registry_id text, registry_revision bigint)
LANGUAGE plpgsql AS $$
DECLARE matched trading.strategy_registry%ROWTYPE;
DECLARE count_matches integer := 0;
BEGIN
    FOR matched IN SELECT r.* FROM trading.strategy_registry r
        WHERE r.strategy_type = engine_id AND
          (upper(btrim(r.portfolio_id)) = upper(btrim(book)) OR EXISTS (
            SELECT 1 FROM trading.strategy_book_memberships m
            WHERE m.strategy_id = r.id AND upper(btrim(m.portfolio_id)) = upper(btrim(book))))
        ORDER BY r.id FOR UPDATE OF r
    LOOP
        count_matches := count_matches + 1;
        registry_id := matched.id;
        registry_revision := matched.runtime_revision;
        -- Model publication (025): live or incubating, and active. Positions qt/qt_proposal
        -- rows keep the 013 rule (active not required). Retired stays closed everywhere.
        IF matched.lifecycle NOT IN ('live','incubating') OR
           (NOT coalesce(allow_incubating,false) AND NOT matched.is_active) THEN
            RAISE EXCEPTION 'runtime_scope_ineligible';
        END IF;
    END LOOP;
    IF count_matches <> 1 OR book IS NULL OR btrim(book) = '' THEN
        RAISE EXCEPTION 'runtime_scope_unsupported';
    END IF;
    PERFORM pg_advisory_xact_lock(hashtextextended('algolens:qt-book:' || upper(btrim(book)),0));
    RETURN NEXT;
END $$;
COMMIT;
