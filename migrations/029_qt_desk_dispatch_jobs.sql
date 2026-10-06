-- Durable orchestration for the existing one-shot QT desk tools. This migration
-- never creates processor receipts and never enables a book.
BEGIN;

DO $$ BEGIN
  IF to_regclass('trading.qt_decisions') IS NULL
     OR to_regclass('trading.qt_desk_receipts') IS NULL
     OR to_regclass('trading.qt_model_seed_publications') IS NULL
     OR to_regclass('trading.strategy_registry') IS NULL
     OR to_regclass('trading.strategy_book_memberships') IS NULL THEN
    RAISE EXCEPTION 'qt029 prerequisites unavailable';
  END IF;
  IF to_regclass('trading.qt_desk_dispatch_jobs') IS NOT NULL
     OR to_regclass('trading.qt_desk_dispatch_attempts') IS NOT NULL THEN
    RAISE EXCEPTION 'qt029 reserved tables already exist';
  END IF;
END $$;

CREATE TABLE trading.qt_desk_dispatch_jobs (
  decision_id uuid PRIMARY KEY REFERENCES trading.qt_decisions(decision_id) ON DELETE RESTRICT,
  book_id text NOT NULL CHECK (book_id='CONSERVATIVE_PORTFOLIO'),
  strategy_id text NOT NULL CHECK (strategy_id='LIVE_TREND_FOLLOWING'),
  source_day date NOT NULL,
  attempt_id uuid NOT NULL UNIQUE,
  input_id uuid NOT NULL UNIQUE,
  market_source_id uuid NOT NULL UNIQUE,
  finalization_id uuid NOT NULL UNIQUE,
  state text NOT NULL CHECK (state IN ('ready','running','retry_wait','succeeded','dead_letter')),
  owner_token uuid,
  lease_expires_at timestamptz,
  next_attempt_at timestamptz NOT NULL DEFAULT clock_timestamp(),
  attempt_count smallint NOT NULL DEFAULT 0 CHECK (attempt_count BETWEEN 0 AND 8),
  first_seen_at timestamptz NOT NULL DEFAULT clock_timestamp(),
  updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
  completed_at timestamptz,
  error_code text CHECK (error_code IS NULL OR error_code ~ '^[a-z0-9_]{1,64}$'),
  CHECK ((state='running') = (owner_token IS NOT NULL AND lease_expires_at IS NOT NULL)),
  CHECK ((state IN ('succeeded','dead_letter')) = (completed_at IS NOT NULL)),
  CHECK (state <> 'succeeded' OR error_code IS NULL),
  CHECK (state <> 'dead_letter' OR error_code IS NOT NULL)
);

CREATE INDEX qt_desk_dispatch_ready_idx
  ON trading.qt_desk_dispatch_jobs(next_attempt_at,first_seen_at,decision_id)
  WHERE state IN ('ready','retry_wait');
CREATE INDEX qt_desk_dispatch_expired_idx
  ON trading.qt_desk_dispatch_jobs(lease_expires_at,first_seen_at,decision_id)
  WHERE state='running';

CREATE TABLE trading.qt_desk_dispatch_attempts (
  event_id bigserial PRIMARY KEY,
  decision_id uuid NOT NULL REFERENCES trading.qt_desk_dispatch_jobs(decision_id) ON DELETE RESTRICT,
  attempt_number smallint NOT NULL CHECK (attempt_number BETWEEN 1 AND 8),
  owner_token uuid NOT NULL,
  phase text NOT NULL CHECK (phase IN ('dispatcher','bootstrap','prepare','run')),
  outcome text NOT NULL CHECK (outcome IN ('claimed','retry','succeeded','dead_letter')),
  error_code text CHECK (error_code IS NULL OR error_code ~ '^[a-z0-9_]{1,64}$'),
  occurred_at timestamptz NOT NULL DEFAULT clock_timestamp(),
  CHECK ((outcome IN ('claimed','succeeded')) = (error_code IS NULL))
);

CREATE FUNCTION trading.qt_dispatch_attempts_immutable() RETURNS trigger
LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'QT dispatch attempts are append only'; END $$;
CREATE TRIGGER qt_dispatch_attempts_immutable BEFORE UPDATE OR DELETE
  ON trading.qt_desk_dispatch_attempts FOR EACH ROW
  EXECUTE FUNCTION trading.qt_dispatch_attempts_immutable();
CREATE TRIGGER qt_dispatch_attempts_no_truncate BEFORE TRUNCATE
  ON trading.qt_desk_dispatch_attempts FOR EACH STATEMENT
  EXECUTE FUNCTION trading.qt_dispatch_attempts_immutable();

CREATE FUNCTION trading.qt_dispatch_job_guard() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF (NEW.decision_id,NEW.book_id,NEW.strategy_id,NEW.source_day,NEW.attempt_id,
      NEW.input_id,NEW.market_source_id,NEW.finalization_id,NEW.first_seen_at)
     IS DISTINCT FROM
     (OLD.decision_id,OLD.book_id,OLD.strategy_id,OLD.source_day,OLD.attempt_id,
      OLD.input_id,OLD.market_source_id,OLD.finalization_id,OLD.first_seen_at) THEN
    RAISE EXCEPTION 'QT dispatch identity is immutable';
  END IF;
  IF OLD.state IN ('succeeded','dead_letter') OR NOT (
       (OLD.state='ready' AND NEW.state='running') OR
       (OLD.state='running' AND NEW.state IN ('running','retry_wait','succeeded','dead_letter')) OR
       (OLD.state='retry_wait' AND NEW.state='running')) THEN
    RAISE EXCEPTION 'invalid QT dispatch transition';
  END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER qt_dispatch_job_guard BEFORE UPDATE ON trading.qt_desk_dispatch_jobs
  FOR EACH ROW EXECUTE FUNCTION trading.qt_dispatch_job_guard();
CREATE TRIGGER qt_dispatch_jobs_no_delete BEFORE DELETE ON trading.qt_desk_dispatch_jobs
  FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_dispatch_attempts_immutable();
CREATE TRIGGER qt_dispatch_jobs_no_truncate BEFORE TRUNCATE ON trading.qt_desk_dispatch_jobs
  FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_dispatch_attempts_immutable();

CREATE FUNCTION trading.enqueue_qt_desk_dispatch(
  p_decision_id uuid,p_attempt_id uuid,p_input_id uuid,
  p_market_source_id uuid,p_finalization_id uuid) RETURNS void
LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, trading AS $$
DECLARE admitted record; existing trading.qt_desk_dispatch_jobs%ROWTYPE;
BEGIN
  SELECT d.book_id,d.source_day,p.strategy_id INTO admitted
    FROM trading.qt_decisions d
    JOIN trading.qt_model_seed_publications p ON p.publication_id=d.model_publication_id
    JOIN trading.strategy_registry r ON r.id='trendfollowing'
      AND r.strategy_type=p.strategy_id AND r.lifecycle='live' AND r.is_active
      AND (r.portfolio_id=d.book_id OR EXISTS (
        SELECT 1 FROM trading.strategy_book_memberships m
         WHERE m.strategy_id=r.id AND m.portfolio_id=d.book_id))
   WHERE d.decision_id=p_decision_id AND d.status='confirmed_decision'
     AND d.book_id='CONSERVATIVE_PORTFOLIO'
     AND p.portfolio_id=d.book_id AND p.source_day=d.source_day
     AND p.strategy_id='LIVE_TREND_FOLLOWING';
  IF NOT FOUND THEN RAISE EXCEPTION 'qt_dispatch_scope_unsupported'; END IF;
  IF EXISTS (SELECT 1 FROM trading.qt_desk_receipts
              WHERE decision_id=p_decision_id AND status='processed') THEN
    RAISE EXCEPTION 'qt_dispatch_already_processed';
  END IF;
  INSERT INTO trading.qt_desk_dispatch_jobs(
    decision_id,book_id,strategy_id,source_day,attempt_id,input_id,
    market_source_id,finalization_id,state)
  VALUES(p_decision_id,admitted.book_id,admitted.strategy_id,admitted.source_day,
    p_attempt_id,p_input_id,p_market_source_id,p_finalization_id,'ready')
  ON CONFLICT(decision_id) DO NOTHING;
  SELECT * INTO existing FROM trading.qt_desk_dispatch_jobs WHERE decision_id=p_decision_id;
  IF (existing.attempt_id,existing.input_id,existing.market_source_id,existing.finalization_id)
     IS DISTINCT FROM (p_attempt_id,p_input_id,p_market_source_id,p_finalization_id) THEN
    RAISE EXCEPTION 'qt_dispatch_identity_conflict';
  END IF;
END $$;

CREATE FUNCTION trading.claim_qt_desk_dispatch(p_owner_token uuid,p_lease interval)
RETURNS TABLE(decision_id uuid,book_id text,strategy_id text,source_day date,
 attempt_id uuid,input_id uuid,market_source_id uuid,finalization_id uuid,
 attempt_number smallint,first_seen_at timestamptz)
LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, trading AS $$
DECLARE selected uuid;
BEGIN
  IF p_owner_token IS NULL OR p_lease < interval '1 millisecond'
     OR p_lease > interval '15 minutes' THEN
    RAISE EXCEPTION 'qt_dispatch_claim_invalid';
  END IF;
  WITH exhausted AS (
    SELECT j.decision_id,j.owner_token,j.attempt_count
      FROM trading.qt_desk_dispatch_jobs j
     WHERE j.state='running' AND j.lease_expires_at<clock_timestamp()
       AND j.attempt_count>=8
     ORDER BY j.first_seen_at,j.decision_id FOR UPDATE SKIP LOCKED LIMIT 1
  ), closed AS (
    UPDATE trading.qt_desk_dispatch_jobs j SET state='dead_letter',owner_token=NULL,
      lease_expires_at=NULL,completed_at=clock_timestamp(),updated_at=clock_timestamp(),
      error_code='retry_exhausted'
      FROM exhausted e WHERE j.decision_id=e.decision_id
      RETURNING j.decision_id,e.owner_token,e.attempt_count
  )
  INSERT INTO trading.qt_desk_dispatch_attempts(
    decision_id,attempt_number,owner_token,phase,outcome,error_code)
  SELECT decision_id,attempt_count,owner_token,'dispatcher','dead_letter','retry_exhausted'
    FROM closed;
  SELECT j.decision_id INTO selected FROM trading.qt_desk_dispatch_jobs j
   WHERE (j.state IN ('ready','retry_wait') AND j.next_attempt_at<=clock_timestamp())
      OR (j.state='running' AND j.lease_expires_at<clock_timestamp()
          AND j.attempt_count<8)
   ORDER BY j.first_seen_at,j.decision_id FOR UPDATE SKIP LOCKED LIMIT 1;
  IF selected IS NULL THEN RETURN; END IF;
  UPDATE trading.qt_desk_dispatch_jobs j SET
    state='running',owner_token=p_owner_token,
    lease_expires_at=clock_timestamp()+p_lease,
    attempt_count=j.attempt_count+1,updated_at=clock_timestamp(),error_code=NULL
   WHERE j.decision_id=selected
   RETURNING j.decision_id,j.book_id,j.strategy_id,j.source_day,j.attempt_id,
     j.input_id,j.market_source_id,j.finalization_id,j.attempt_count,j.first_seen_at
   INTO decision_id,book_id,strategy_id,source_day,attempt_id,input_id,
     market_source_id,finalization_id,attempt_number,first_seen_at;
  INSERT INTO trading.qt_desk_dispatch_attempts(
    decision_id,attempt_number,owner_token,phase,outcome)
  VALUES(decision_id,attempt_number,p_owner_token,'dispatcher','claimed');
  RETURN NEXT;
END $$;

CREATE FUNCTION trading.finish_qt_desk_dispatch(
 p_decision_id uuid,p_owner_token uuid,p_phase text,p_outcome text,
 p_error_code text,p_retry_after interval) RETURNS void
LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, trading AS $$
DECLARE job trading.qt_desk_dispatch_jobs%ROWTYPE;
BEGIN
  SELECT * INTO job FROM trading.qt_desk_dispatch_jobs
   WHERE decision_id=p_decision_id FOR UPDATE;
  IF NOT FOUND OR job.state<>'running' OR job.owner_token<>p_owner_token THEN
    RAISE EXCEPTION 'qt_dispatch_owner_mismatch';
  END IF;
  IF p_phase NOT IN ('bootstrap','prepare','run') OR
     p_outcome NOT IN ('retry','succeeded','dead_letter') THEN
    RAISE EXCEPTION 'qt_dispatch_finish_invalid';
  END IF;
  IF p_outcome='succeeded' THEN
    IF p_error_code IS NOT NULL OR NOT EXISTS (
      SELECT 1 FROM trading.qt_desk_receipts r WHERE r.decision_id=p_decision_id
       AND r.attempt_id=job.attempt_id AND r.status='processed') THEN
      RAISE EXCEPTION 'qt_dispatch_receipt_unavailable';
    END IF;
    UPDATE trading.qt_desk_dispatch_jobs SET state='succeeded',owner_token=NULL,
      lease_expires_at=NULL,completed_at=clock_timestamp(),updated_at=clock_timestamp(),
      error_code=NULL WHERE decision_id=p_decision_id;
  ELSIF p_outcome='retry' THEN
    IF p_error_code IS NULL OR p_retry_after IS NULL OR p_retry_after<interval '0'
       OR p_retry_after>interval '5 minutes' OR job.attempt_count>=8 THEN
      RAISE EXCEPTION 'qt_dispatch_retry_invalid';
    END IF;
    UPDATE trading.qt_desk_dispatch_jobs SET state='retry_wait',owner_token=NULL,
      lease_expires_at=NULL,next_attempt_at=clock_timestamp()+p_retry_after,
      updated_at=clock_timestamp(),error_code=p_error_code
      WHERE decision_id=p_decision_id;
  ELSE
    IF p_error_code IS NULL OR p_retry_after IS NOT NULL THEN
      RAISE EXCEPTION 'qt_dispatch_dead_letter_invalid';
    END IF;
    UPDATE trading.qt_desk_dispatch_jobs SET state='dead_letter',owner_token=NULL,
      lease_expires_at=NULL,completed_at=clock_timestamp(),updated_at=clock_timestamp(),
      error_code=p_error_code WHERE decision_id=p_decision_id;
  END IF;
  INSERT INTO trading.qt_desk_dispatch_attempts(
    decision_id,attempt_number,owner_token,phase,outcome,error_code)
  VALUES(p_decision_id,job.attempt_count,p_owner_token,p_phase,p_outcome,p_error_code);
END $$;

CREATE FUNCTION trading.renew_qt_desk_dispatch(
 p_decision_id uuid,p_owner_token uuid,p_lease interval) RETURNS void
LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, trading AS $$
BEGIN
  IF p_lease < interval '1 millisecond' OR p_lease > interval '15 minutes' THEN
    RAISE EXCEPTION 'qt_dispatch_renew_invalid';
  END IF;
  UPDATE trading.qt_desk_dispatch_jobs SET
    lease_expires_at=clock_timestamp()+p_lease,updated_at=clock_timestamp()
   WHERE decision_id=p_decision_id AND state='running' AND owner_token=p_owner_token;
  IF NOT FOUND THEN RAISE EXCEPTION 'qt_dispatch_owner_mismatch'; END IF;
END $$;

-- T3 grants only these four entry points to the dedicated worker role. Keeping
-- PUBLIC out prevents the definer privileges from becoming a general API.
REVOKE ALL ON FUNCTION trading.enqueue_qt_desk_dispatch(uuid,uuid,uuid,uuid,uuid) FROM PUBLIC;
REVOKE ALL ON FUNCTION trading.claim_qt_desk_dispatch(uuid,interval) FROM PUBLIC;
REVOKE ALL ON FUNCTION trading.finish_qt_desk_dispatch(uuid,uuid,text,text,text,interval) FROM PUBLIC;
REVOKE ALL ON FUNCTION trading.renew_qt_desk_dispatch(uuid,uuid,interval) FROM PUBLIC;

COMMIT;
