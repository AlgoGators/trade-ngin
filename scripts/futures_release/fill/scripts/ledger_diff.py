"""The dry run's ledger against the tested ledger (the fill tested on a copy of production, data through the tested end). Within the tested range every line must be equal on the fields that define the fill:
symbol, date, table, instrument_id, raw id written, raw_symbol, rule branch, the bar, rtype, publisher_id (class and junk
verdict are descriptive and left out). The tested ledger holds no bar: it holds bar_sha256, the sha256 of open, high, low,
close and volume as make_ledger.py writes them, joined by commas; the dry run's bars are hashed the same way. The tested ledger holds no bar: it holds bar_sha256, the sha256 of open, high, low,
close and volume as make_ledger.py writes them, joined by commas; the dry run's bars are hashed the same way. Lines dated after the tested end are listed separately with their rule picks.
Exit 0 when the tested range is identical, 6 when it differs.
usage: ledger_diff.py <dry-run ledger> <tested ledger> <tested end YYYY-MM-DD> <out.txt>"""
import csv, hashlib, sys
dry_p, tested_p, end, out_p = sys.argv[1:5]
F = ['symbol', 'date', 'table', 'instrument_id', 'raw_instrument_id_written', 'raw_symbol', 'rule_branch',
     'bar_sha256', 'rtype', 'publisher_id']
BAR = ('open', 'high', 'low', 'close', 'volume')
bar_hash = lambda r: hashlib.sha256(','.join(r[k] for k in BAR).encode()).hexdigest()
key = lambda r: tuple(r[f] for f in F)
dry = [dict(r, bar_sha256=bar_hash(r)) for r in csv.DictReader(open(dry_p))]
tested = [r for r in csv.DictReader(open(tested_p))]
d_in = {key(r) for r in dry if r['date'] <= end}
t_in = {key(r) for r in tested if r['date'] <= end}
new = [r for r in dry if r['date'] > end]
L = [f'tested ledger {tested_p}: {len(tested)} lines; dry-run ledger: {len(dry)} lines; tested range ends {end}',
     f'within the tested range: dry run {len(d_in)}, tested {len(t_in)}, only in the dry run {len(d_in - t_in)}, only in the tested ledger {len(t_in - d_in)}']
for k in sorted(d_in - t_in)[:40]: L.append('  only in the dry run:     ' + ','.join(k))
for k in sorted(t_in - d_in)[:40]: L.append('  only in the tested ledger: ' + ','.join(k))
L.append(f'after the tested range (inserted only with FILL_INCLUDE_NEW=1): {len([r for r in new if r["table"].endswith("ohlcv_1d")])} rows')
for r in new:
    if r['table'] == 'futures_data.ohlcv_1d':
        L.append(f"  new: {r['symbol']} {r['date']} {r['raw_symbol']} ({r['instrument_id']}) {r['rule_branch']} {r['class']} volume {r['volume']} junk {r['junk_verdict']}")
open(out_p, 'w').write('\n'.join(L) + '\n'); print('\n'.join(L[:60]))
sys.exit(0 if d_in == t_in else 6)
