-- Micro Gold (MGC) and Micro Silver (SIL), the bought data, in production's shapes.
--
-- Run with psql, ON_ERROR_STOP on, from any folder, naming the rows folder (build_rows.py's output) and the part:
--   psql -X -v ON_ERROR_STOP=1 -v rows="<folder>" -v part=A -f load_micro_metals.sql
--   psql -X -v ON_ERROR_STOP=1 -v rows="<backfill folder>" -v part=A2 -f load_micro_metals.sql
--   psql -X -v ON_ERROR_STOP=1 -v rows="<folder>" [-v backfill="<backfill folder>"] -v part=B -f load_micro_metals.sql
-- The wrapper apply_micro_metals_on_production.sh runs it (README.md: the order, which cannot be changed).
--
-- Part A (inert: no runner reads a root that is not in futures_data.ohlcv_1d): the per-contract bars of both roots
--   into futures_data.contract_ohlcv_1d and one metadata.contract_metadata row each.
-- Part A2: the days after the bought rows' last day, contract rows only
--   (build_rows.py with START), any number of times after part A and before part B. Insert only: it refuses before
--   part A, after part B, and when any of its (instrument_id, date) is already stored; an id already stored must keep
--   its symbol and expiration. Its continuous rows go in with part B (-v backfill), never on their own.
-- Part B (adds MGC.v.0 and SIL.v.0 to every futures runner's universe, which is the table's symbol list): the
--   vendor's continuous rows into futures_data.ohlcv_1d and futures_data.ohlcv_1d_raw. Apply B only together with
--   portfolio.json's listing_dates entries for MGC (before GC, 2010-10-04, ratio 10) and SIL (before SI,
--   2013-06-17, ratio 5): with them, GC.v.0 and SI.v.0 stay stored (the micros' grid before their listing) and
--   are not instruments of the run; without them the book would hold GC, SI, MGC and SIL side by side.
--
-- Each part is one transaction: it refuses when any row of its roots is already stored, checks its counts and
-- what it loaded, and commits only if every check passes.

\set ON_ERROR_STOP on
\if :{?rows}
\else
  \echo 'name the rows folder: -v rows=<folder>'
  \quit
\endif
\set contract_file :rows '/contract_ohlcv_1d.csv'
\set v0_file :rows '/ohlcv_1d.csv'
\set raw_file :rows '/ohlcv_1d_raw.csv'

SELECT current_database() AS database, inet_server_addr() AS host, inet_server_port() AS port, now() AS at;

\if :{?part}
\else
  \echo 'name the part: -v part=A or -v part=B'
  \quit
\endif

SELECT :'part' = 'A' AS is_a \gset
\if :is_a
BEGIN;
DO $$ DECLARE n int; BEGIN
  SELECT count(*) INTO n FROM futures_data.contract_ohlcv_1d WHERE root IN ('MGC', 'SIL');
  IF n <> 0 THEN RAISE EXCEPTION 'MGC or SIL contract rows already stored: %', n; END IF;
  SELECT count(*) INTO n FROM metadata.contract_metadata WHERE "Databento Symbol" IN ('MGC', 'SIL');
  IF n <> 0 THEN RAISE EXCEPTION 'MGC or SIL metadata rows already stored: %', n; END IF;
  SELECT count(*) INTO n FROM metadata.contract_metadata WHERE "Databento Symbol" IN ('GC', 'SI');
  IF n <> 2 THEN RAISE EXCEPTION 'expected one GC and one SI metadata row, found %', n; END IF;
END $$;

CREATE TEMP TABLE micro_contract (instrument_id text, date date, root text, raw_symbol text, expiration date,
  open double precision, high double precision, low double precision, close double precision, volume bigint)
  ON COMMIT DROP;
\set copy_contract '\\copy micro_contract FROM ' :'contract_file' ' WITH (FORMAT csv, HEADER true)'
:copy_contract

DO $$ DECLARE n int; BEGIN
  SELECT count(*) INTO n FROM micro_contract WHERE root NOT IN ('MGC', 'SIL') OR expiration IS NULL
    OR raw_symbol !~ ('^' || root || '[FGHJKMNQUVXZ][0-9]{1,2}$') OR least(open, high, low, close) <= 0 OR high < low;
  IF n <> 0 THEN RAISE EXCEPTION 'rows outside the shape: %', n; END IF;
  SELECT count(*) INTO n FROM (SELECT 1 FROM micro_contract GROUP BY instrument_id, date HAVING count(*) > 1) d;
  IF n <> 0 THEN RAISE EXCEPTION 'duplicate (instrument_id, date): %', n; END IF;
  SELECT count(*) INTO n FROM (SELECT instrument_id FROM micro_contract GROUP BY 1
                               HAVING count(DISTINCT raw_symbol) > 1 OR count(DISTINCT expiration) > 1) d;
  IF n <> 0 THEN RAISE EXCEPTION 'an id with two symbols or expirations: %', n; END IF;
  SELECT count(*) INTO n FROM micro_contract c JOIN futures_data.contract_ohlcv_1d p USING (instrument_id, date);
  IF n <> 0 THEN RAISE EXCEPTION 'an (instrument_id, date) already stored for another root: %', n; END IF;
END $$;

INSERT INTO futures_data.contract_ohlcv_1d (instrument_id, date, root, raw_symbol, expiration, open, high, low, close, volume)
SELECT instrument_id, date, root, raw_symbol, expiration, open, high, low, close, volume FROM micro_contract;

-- Metadata: the contract's own size, tick, tick value and fee; everything else as its full-size twin's row, the
-- margins the twin's divided by the ratio (10, 5). Fees: IBKR's pages of 2026-10-26 as read on that date:
-- 0.25 commission + exchange + 0.01 regulatory; SIL's exchange fee 0.60 is CME SER-8425 (IBKR lists no SIL row).
INSERT INTO metadata.contract_metadata
  ("Databento Symbol", "IB Symbol", "Name", "Exchange", "Intraday Initial Margin", "Intraday Maintenance Margin",
   "Overnight Initial Margin", "Overnight Maintenance Margin", "Asset Type", "Sector", "Contract Size", "Units",
   "Minimum Price Fluctuation", "Tick Size", "Settlement Type", "Trading Hours (EST)", "Data Provider", "Dataset",
   "Newest Month Additions", "Contract Months", "Time of Expiry", "Additional Notes", "Fee Per Contract")
SELECT m.sym, m.sym, m.name, t."Exchange",
       round(trim(t."Intraday Initial Margin")::numeric / m.ratio, 2)::text,
       round(trim(t."Intraday Maintenance Margin")::numeric / m.ratio, 2)::text,
       round(trim(t."Overnight Initial Margin")::numeric / m.ratio, 2)::text,
       round(trim(t."Overnight Maintenance Margin")::numeric / m.ratio, 2)::text,
       t."Asset Type", t."Sector", m.size, t."Units", m.mpf, m.tick, t."Settlement Type", t."Trading Hours (EST)",
       t."Data Provider", t."Dataset", t."Newest Month Additions", m.months, t."Time of Expiry", m.notes, m.fee
FROM (VALUES
  ('MGC', 'GC', 10, 'Micro Gold', '10', '1.00', '0.10', 'FEB, APR, JUN, AUG, OCT, DEC', '0.960',
   E'[QUOTE=USD_PER_OZ]\nPhysical contract: 10 troy ounces (COMEX MGC, Micro Gold; 1/10 of GC), listed 2010-10-04 (CME SER-5391).\n'
   'OHLCV close stored in $/oz directly, as GC.\n"Contract Size" 10; "Tick Size" 0.10 $/oz; "Minimum Price Fluctuation" 0.10 x 10 = 1.00.\n'
   'INVARIANT 1 - Notional: notional_USD = quantity x ohlcv_close x "Contract Size".\n'
   'Margins: GC''s divided by 10 at the load (not confirmed against CME''s or IBKR''s page, which refused a scripted read).\n'
   'Fee: IBKR 0.25 commission + 0.70 exchange + 0.01 regulatory (IBKR pages of 2026-10-26).\n'
   'Rolls: GC''s cycle G J M Q Z (V lists but is never the busiest); before 2010-10-04 the book trades GC.\n'
   'CONSUMERS: position sizing, daily P&L, notional, transaction cost.'),
  ('SIL', 'SI', 5, 'Micro Silver', '1000', '5.00', '0.005', 'JAN, FEB, MAR, APR, MAY, JUN, JUL, AUG, SEP, OCT, NOV, DEC', '0.860',
   E'[QUOTE=USD_PER_OZ]\nPhysical contract: 1,000 troy ounces (COMEX SIL, Micro Silver; 1/5 of SI), listed 2013-06-17 (CME SER-6658).\n'
   'OHLCV close stored in $/oz directly, as SI.\n"Contract Size" 1000; "Tick Size" 0.005 $/oz; "Minimum Price Fluctuation" 0.005 x 1000 = 5.00.\n'
   'INVARIANT 1 - Notional: notional_USD = quantity x ohlcv_close x "Contract Size".\n'
   'Margins: SI''s divided by 5 at the load (not confirmed against CME''s or IBKR''s page, which refused a scripted read).\n'
   'Fee: IBKR 0.25 commission + 0.60 exchange (CME SER-8425; IBKR''s COMEX page lists no SIL row) + 0.01 regulatory.\n'
   'Rolls: SI''s cycle H K N U Z (every month lists); before 2013-06-17 the book trades SI.\n'
   'CONSUMERS: position sizing, daily P&L, notional, transaction cost.')
) AS m(sym, twin, ratio, name, size, mpf, tick, months, fee, notes)
JOIN metadata.contract_metadata t ON t."Databento Symbol" = m.twin;

DO $$ DECLARE n int; BEGIN
  SELECT count(*) INTO n FROM metadata.contract_metadata WHERE "Databento Symbol" IN ('MGC', 'SIL');
  IF n <> 2 THEN RAISE EXCEPTION 'metadata rows written: % (expected 2)', n; END IF;
END $$;
SELECT root, count(*) AS rows, count(DISTINCT instrument_id) AS contracts, min(date) AS first, max(date) AS last,
       count(*) FILTER (WHERE expiration IS NULL) AS no_expiration
FROM futures_data.contract_ohlcv_1d WHERE root IN ('MGC', 'SIL') GROUP BY 1 ORDER BY 1;
SELECT "Databento Symbol", "Sector", "Exchange", "Contract Size", "Tick Size", "Minimum Price Fluctuation",
       "Fee Per Contract", "Overnight Initial Margin", "Overnight Maintenance Margin"
FROM metadata.contract_metadata WHERE "Databento Symbol" IN ('GC', 'MGC', 'SI', 'SIL') ORDER BY 1;
COMMIT;
\echo 'PART A LOADED'
\else
SELECT :'part' = 'A2' AS is_a2 \gset
\if :is_a2
BEGIN;
DO $$ DECLARE n int; BEGIN
  SELECT count(DISTINCT root) INTO n FROM futures_data.contract_ohlcv_1d WHERE root IN ('MGC', 'SIL');
  IF n <> 2 THEN RAISE EXCEPTION 'part A first: the contract rows of MGC and SIL are not both stored'; END IF;
  SELECT count(*) INTO n FROM futures_data.ohlcv_1d WHERE symbol IN ('MGC.v.0', 'SIL.v.0');
  IF n <> 0 THEN RAISE EXCEPTION 'part B is loaded (MGC.v.0 or SIL.v.0 rows: %): the backfill comes before it; after it the daily pulls (#84, #85) write these days', n; END IF;
END $$;
CREATE TEMP TABLE micro_contract (instrument_id text, date date, root text, raw_symbol text, expiration date,
  open double precision, high double precision, low double precision, close double precision, volume bigint)
  ON COMMIT DROP;
\set copy_contract '\\copy micro_contract FROM ' :'contract_file' ' WITH (FORMAT csv, HEADER true)'
:copy_contract
DO $$ DECLARE n int; BEGIN
  SELECT count(*) INTO n FROM micro_contract;
  IF n = 0 THEN RAISE EXCEPTION 'the backfill folder holds no contract row'; END IF;
  SELECT count(*) INTO n FROM micro_contract WHERE root NOT IN ('MGC', 'SIL') OR expiration IS NULL
    OR raw_symbol !~ ('^' || root || '[FGHJKMNQUVXZ][0-9]{1,2}$') OR least(open, high, low, close) <= 0 OR high < low;
  IF n <> 0 THEN RAISE EXCEPTION 'rows outside the shape: %', n; END IF;
  SELECT count(*) INTO n FROM (SELECT 1 FROM micro_contract GROUP BY instrument_id, date HAVING count(*) > 1) d;
  IF n <> 0 THEN RAISE EXCEPTION 'duplicate (instrument_id, date): %', n; END IF;
  SELECT count(*) INTO n FROM micro_contract c JOIN futures_data.contract_ohlcv_1d p USING (instrument_id, date);
  IF n <> 0 THEN RAISE EXCEPTION 'an (instrument_id, date) already stored: % (insert only)', n; END IF;
  SELECT count(*) INTO n FROM micro_contract c JOIN (SELECT DISTINCT instrument_id, root, raw_symbol, expiration
      FROM futures_data.contract_ohlcv_1d WHERE root IN ('MGC', 'SIL')) p USING (instrument_id)
    WHERE p.root <> c.root OR p.raw_symbol <> c.raw_symbol OR p.expiration <> c.expiration;
  IF n <> 0 THEN RAISE EXCEPTION 'a stored id with another symbol or expiration in the backfill: %', n; END IF;
  SELECT count(*) INTO n FROM micro_contract c
    WHERE c.date <= (SELECT max(date) FROM futures_data.contract_ohlcv_1d p WHERE p.root = c.root);
  IF n <> 0 THEN RAISE EXCEPTION 'backfill rows dated on or before the stored rows'' last day: % (the backfill extends the history forward only)', n; END IF;
END $$;
INSERT INTO futures_data.contract_ohlcv_1d (instrument_id, date, root, raw_symbol, expiration, open, high, low, close, volume)
SELECT instrument_id, date, root, raw_symbol, expiration, open, high, low, close, volume FROM micro_contract;
SELECT root, count(*) AS rows, count(DISTINCT instrument_id) AS contracts, min(date) AS first, max(date) AS last
FROM futures_data.contract_ohlcv_1d WHERE root IN ('MGC', 'SIL') GROUP BY 1 ORDER BY 1;
COMMIT;
\echo 'PART A2 LOADED'
\else
SELECT :'part' = 'B' AS is_b \gset
\if :is_b
BEGIN;
DO $$ DECLARE n int; BEGIN
  SELECT count(*) INTO n FROM futures_data.ohlcv_1d WHERE symbol IN ('MGC.v.0', 'SIL.v.0');
  IF n <> 0 THEN RAISE EXCEPTION 'MGC.v.0 or SIL.v.0 rows already stored: %', n; END IF;
  SELECT count(*) INTO n FROM futures_data.ohlcv_1d_raw WHERE symbol IN ('MGC.v.0', 'SIL.v.0');
  IF n <> 0 THEN RAISE EXCEPTION 'MGC.v.0 or SIL.v.0 raw rows already stored: %', n; END IF;
  SELECT count(DISTINCT root) INTO n FROM futures_data.contract_ohlcv_1d WHERE root IN ('MGC', 'SIL');
  IF n <> 2 THEN RAISE EXCEPTION 'part A first: the contract rows of MGC and SIL are not both stored'; END IF;
END $$;
CREATE TEMP TABLE micro_v0 (time timestamptz, symbol text, volume integer, open double precision,
  high double precision, low double precision, close double precision) ON COMMIT DROP;
CREATE TEMP TABLE micro_raw (ts_event timestamptz, symbol text, open double precision, high double precision,
  low double precision, close double precision, volume bigint, rtype text, publisher_id text, instrument_id text)
  ON COMMIT DROP;
\set copy_v0 '\\copy micro_v0 FROM ' :'v0_file' ' WITH (FORMAT csv, HEADER true)'
:copy_v0
\set copy_raw '\\copy micro_raw FROM ' :'raw_file' ' WITH (FORMAT csv, HEADER true)'
:copy_raw
-- the backfill's continuous rows (part A2's days) go in here, in part B's transaction
\if :{?backfill}
\set bf_v0_file :backfill '/ohlcv_1d.csv'
\set bf_raw_file :backfill '/ohlcv_1d_raw.csv'
\set copy_bf_v0 '\\copy micro_v0 FROM ' :'bf_v0_file' ' WITH (FORMAT csv, HEADER true)'
:copy_bf_v0
\set copy_bf_raw '\\copy micro_raw FROM ' :'bf_raw_file' ' WITH (FORMAT csv, HEADER true)'
:copy_bf_raw
\endif
DO $$ DECLARE n int; BEGIN
  SELECT count(*) INTO n FROM micro_v0 WHERE symbol NOT IN ('MGC.v.0', 'SIL.v.0')
    OR time <> date_trunc('day', time AT TIME ZONE 'UTC') AT TIME ZONE 'UTC' OR least(open, high, low, close) <= 0;
  IF n <> 0 THEN RAISE EXCEPTION 'rows outside the shape: %', n; END IF;
  SELECT count(*) INTO n FROM (SELECT 1 FROM micro_v0 GROUP BY time, symbol HAVING count(*) > 1) d;
  IF n <> 0 THEN RAISE EXCEPTION 'duplicate (time, symbol): %', n; END IF;
  SELECT count(*) INTO n FROM micro_v0 v FULL JOIN micro_raw r ON r.ts_event = v.time AND r.symbol = v.symbol
    WHERE v.symbol IS NULL OR r.symbol IS NULL OR r.close <> v.close OR r.volume <> v.volume;
  IF n <> 0 THEN RAISE EXCEPTION 'continuous and raw rows differ: %', n; END IF;
  -- every vendor bar names a stored contract bar of its root, same date, same close (R6's check)
  SELECT count(*) INTO n FROM micro_raw r LEFT JOIN futures_data.contract_ohlcv_1d c
    ON c.instrument_id = r.instrument_id AND c.date = (r.ts_event AT TIME ZONE 'UTC')::date
   AND c.root = replace(r.symbol, '.v.0', '') AND c.close = r.close
   WHERE c.instrument_id IS NULL;
  IF n <> 0 THEN RAISE EXCEPTION 'vendor bars naming no stored contract bar of the same date and close: %', n; END IF;
  -- the continuous rows reach the stored contract rows' last day (pass the backfill folder when
  -- part A2 added days), so no day of the micro has contract bars and no continuous bar
  SELECT count(*) INTO n FROM (SELECT root, max(date) AS last FROM futures_data.contract_ohlcv_1d
      WHERE root IN ('MGC', 'SIL') GROUP BY 1) c
    LEFT JOIN (SELECT replace(symbol, '.v.0', '') AS root, max((time AT TIME ZONE 'UTC')::date) AS last
      FROM micro_v0 GROUP BY 1) v USING (root)
    WHERE v.last IS NULL OR v.last < c.last;
  IF n <> 0 THEN RAISE EXCEPTION 'the continuous rows end before the stored contract rows on % root(s): pass the backfill folder (-v backfill) built to the same day', n; END IF;
END $$;
INSERT INTO futures_data.ohlcv_1d (time, symbol, volume, open, high, low, close)
SELECT time, symbol, volume, open, high, low, close FROM micro_v0;
INSERT INTO futures_data.ohlcv_1d_raw (ts_event, symbol, open, high, low, close, volume, rtype, publisher_id, instrument_id)
SELECT ts_event, symbol, open, high, low, close, volume, rtype, publisher_id, instrument_id FROM micro_raw;
SELECT symbol, count(*) AS rows, min(time)::date AS first, max(time)::date AS last
FROM futures_data.ohlcv_1d WHERE symbol IN ('MGC.v.0', 'SIL.v.0') GROUP BY 1 ORDER BY 1;
COMMIT;
\echo 'PART B LOADED'
\else
\echo 'part must be A, A2 or B'
\endif
\endif
\endif
