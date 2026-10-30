"""The session classifier's bar verdict (src/data/session_classifier.cpp, judge_bar + session_norm +
session_flags), on one symbol's series of the .v.0 rows after the fill. Replicates the locked limb, the 1 % / 1,000-lot
limb, the 50-lot / 25 % floor, the no-norm floor, and the norm (median of the 20 trailing weekday SESSION bars, strictly
before the date; bootstrap on all trailing weekday bars until one is SESSION; a median of 0 is no norm).
Not replicated: the instrument-id limbs (in per-contract mode the ids are the held contract's, which change only at
rolls).
usage: junk_replica.py <series.csv: symbol,date,volume,high,low> <fills.csv> <out.csv>"""
import csv, sys, datetime, collections
series_csv, fills_csv, out_csv = sys.argv[1:4]
fills = list(csv.DictReader(open(fills_csv)))
want = collections.defaultdict(set)
for f in fills: want[f['symbol']].add(f['d'])
ser = collections.defaultdict(dict)
for r in csv.DictReader(open(series_csv)):
    if r['symbol'] in want: ser[r['symbol']][r['date']] = (float(r['volume']), float(r['high']), float(r['low']))
for f in fills:  # the series after the fill
    ser[f['symbol']][f['d']] = (float(f['volume']), float(f['high']), float(f['low']))
wd = lambda d: datetime.date.fromisoformat(d).isoweekday() < 6
def judge(b, nm):
    v, hi, lo = b
    if hi == lo: return 'JUNK', 'locked'
    if nm is None: return ('JUNK', 'floor, no norm') if v < 50 else ('SESSION', 'no norm, above floor')
    if v < 0.01 * nm and v < 1000: return 'JUNK', f'1% limb (norm {nm:g})'
    if v < 50 and v < 0.25 * nm: return 'JUNK', f'floor (norm {nm:g})'
    return 'SESSION', f'norm {nm:g}'
out = []
for sym, s in ser.items():
    dates = sorted(s); flags = {}; hist = []  # hist: (date, volume, session) of weekday bars so far
    for d in dates:
        win = [v for (_, v, ok) in reversed(hist) if ok][:20]
        if not win: win = [v for (_, v, _) in reversed(hist)][:20]
        nm = None
        if win:
            w = sorted(win); n = len(w); m = w[n // 2] if n % 2 else 0.5 * (w[n // 2 - 1] + w[n // 2])
            nm = m if m > 0 else None
        verdict, why = judge(s[d], nm)
        if d in want[sym]: out.append((sym, d, verdict, why, s[d][0], nm))
        if wd(d): hist.append((d, s[d][0], verdict == 'SESSION'))
with open(out_csv, 'w', newline='') as f:
    w = csv.writer(f); w.writerow(['symbol', 'date', 'junk_verdict', 'reason', 'volume', 'norm']); w.writerows(sorted(out))
c = collections.Counter(v for _, _, v, *_ in out); print(out_csv, dict(c))
