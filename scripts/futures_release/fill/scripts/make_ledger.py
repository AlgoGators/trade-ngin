"""The fill ledger: one line per filled row per table (symbol, date, table, instrument_id of the contract whose bar was
written, the id written to the raw row (blank = NULL, rule V), contract, rule branch, class,
junk verdict by the classifier replica, the bar). usage: make_ledger.py <fill_rows.csv> <junk.csv> <out.csv>"""
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
junk = {(r['symbol'], r['date']): r for r in csv.DictReader(open(sys.argv[2]))}
with open(sys.argv[3], 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['symbol', 'date', 'table', 'instrument_id', 'raw_instrument_id_written', 'raw_symbol', 'rule_branch', 'class', 'junk_verdict', 'junk_reason', 'open', 'high', 'low', 'close', 'volume', 'rtype', 'publisher_id'])
    for r in rows:
        j = junk[(r['symbol'], r['d'])]
        for t in ('futures_data.ohlcv_1d', 'futures_data.ohlcv_1d_raw'):
            w.writerow([r['symbol'], r['d'], t, r['instrument_id'], ('' if r['branch'].startswith('V_') else r['instrument_id']) if t.endswith('raw') else '', r['raw_symbol'], r['branch'], r['klass'], j['junk_verdict'], j['reason'], r['open'], r['high'], r['low'], r['close'], r['volume'], r['rtype'] if t.endswith('raw') else '', r['publisher_id'] if t.endswith('raw') else ''])
print(sys.argv[3], 2 * len(rows), 'lines')
