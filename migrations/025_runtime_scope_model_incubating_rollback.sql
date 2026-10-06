-- STAGED PROPOSAL ONLY (lane N5). Restores the 013 trading.lock_runtime_scope body
-- (system rows and the seven other fenced tables require a live, active scope again).
-- No row is changed or discarded: rows already written for an incubating scope remain,
-- and from then on the fence refuses further writes, updates and deletes of them, as
-- 013 does for any scope that is not live. The fence function and triggers are untouched.
-- Idempotent: a database that already has the 013 body is left as it is.
BEGIN;
DO $preflight$
DECLARE
    lock_oid oid := to_regprocedure('trading.lock_runtime_scope(text,text,boolean)');
    fence_oid oid := to_regprocedure('trading.fence_runtime_publication_row()');
    lock_013 constant text := 'bdda022b058f416faf198fe4e4fae95d';
    lock_025 constant text := '0bd1e0badb6bee13c329268ded8f0e03';
    fence_013 constant text := 'a469e2b5cfd77d447383ae1fb78d5e7a';
    fence_015 constant text := '419771cec97836560ae952c120aac4d1';
BEGIN
    IF lock_oid IS NULL OR fence_oid IS NULL THEN
        RAISE EXCEPTION 'runtime025 rollback prerequisites missing';
    END IF;
    IF (SELECT count(*) FROM pg_proc WHERE pronamespace='trading'::regnamespace
          AND proname='lock_runtime_scope') <> 1 THEN
        RAISE EXCEPTION 'runtime025 rollback lock_runtime_scope overload unsupported';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_proc f JOIN pg_language l ON l.oid=f.prolang
        WHERE f.oid=lock_oid AND l.lanname='plpgsql' AND f.prokind='f' AND f.proretset
          AND NOT f.prosecdef AND f.proconfig IS NULL
          AND pg_get_function_arguments(f.oid)='engine_id text, book text, allow_incubating boolean DEFAULT false'
          AND pg_get_function_result(f.oid)='TABLE(registry_id text, registry_revision bigint)'
          AND md5(f.prosrc) IN (lock_025,lock_013)) THEN
        RAISE EXCEPTION 'runtime025 rollback requires the 025 lock_runtime_scope';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_proc f JOIN pg_language l ON l.oid=f.prolang
        WHERE f.oid=fence_oid AND f.prorettype='pg_catalog.trigger'::regtype AND f.pronargs=0
          AND l.lanname='plpgsql' AND md5(f.prosrc) IN (fence_013,fence_015)) THEN
        RAISE EXCEPTION 'runtime025 rollback requires the 013 or 015 publication fence';
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
        IF (coalesce(allow_incubating,false) AND matched.lifecycle NOT IN ('live','incubating')) OR
           (NOT coalesce(allow_incubating,false) AND (matched.lifecycle <> 'live' OR NOT matched.is_active)) THEN
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
