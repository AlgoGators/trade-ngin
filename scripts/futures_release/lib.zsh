# lib.zsh: sourced by the numbered release scripts. Not run on its own.
#
# The target, from the environment:
#   TARGET=production                 the database named in config/defaults.json (it must be new_algo_data). Its address,
#                                     user and password are read from that file at run time, put in PG* variables and
#                                     never printed or written anywhere.
#   TARGET=scratch SCRATCH_DB=<db>    a test copy whose name contains "scratch". PGHOST, PGPORT, PGUSER and PGPASSWORD come
#                                     from the environment.
# Every script refuses any other target. Nothing here sends e-mail: no script passes --send-email, and the live and
# backtest logs are checked for an e-mail line after every run.
#
# Everything a run writes goes under RUN_DIR (default <repository>/futures_release_run, not tracked).
setopt pipefail
PROD="${0:A:h}"                          # this folder
REPO="${PROD:h:h}"
RUN_DIR="${RUN_DIR:-$REPO/futures_release_run}"
REFCFG="$PROD/config_reference"          # the frozen run config (portfolio.json, risk.json, defaults.json)
LOCKS="$RUN_DIR/locks"
mkdir -p "$RUN_DIR" "$LOCKS"
PGBIN="${PGBIN:-${$(command -v psql):h}}"
AWAKE=(); command -v caffeinate > /dev/null && AWAKE=(caffeinate -i)
PID=CONSERVATIVE_PORTFOLIO
export PGTZ=UTC PGCONNECT_TIMEOUT=20

say() { print -r -- "$*" | tee -a "${LOG:-/dev/null}" }
die() { local code=${2:-1}; say "STOPPED (exit $code): $1"; exit $code }

resolve_target() {
  TARGET="${TARGET:-}"
  case "$TARGET" in
    production)
      local cfg="$REPO/config/defaults.json"
      [ -s "$cfg" ] || die "no config/defaults.json" 2
      eval "$(python3 - "$cfg" <<'PY'
import json, shlex, sys
d = json.load(open(sys.argv[1]))["database"]
user = d.get("username", d.get("user"))
for k, v in (("PGHOST", d["host"]), ("PGPORT", str(d.get("port", "5432"))), ("PGUSER", user),
             ("PGDATABASE", d["name"]), ("PGPASSWORD", d["password"])):
    print("export %s=%s" % (k, shlex.quote(v)))
PY
)" || die "config/defaults.json has no usable database block" 2
      [ "$PGDATABASE" = new_algo_data ] || die "config/defaults.json names database '$PGDATABASE', not new_algo_data" 2
      DBNAME=new_algo_data; TAG=production ;;
    scratch)
      DBNAME="${SCRATCH_DB:-}"
      [[ "$DBNAME" == *scratch* ]] || die "TARGET=scratch needs SCRATCH_DB=<a database whose name contains scratch>" 2
      [ -n "${PGHOST:-}" ] && [ -n "${PGPORT:-}" ] && [ -n "${PGUSER:-}" ] || die "TARGET=scratch needs PGHOST, PGPORT, PGUSER in the environment" 2
      export PGDATABASE="$DBNAME"
      TAG="scratch_${DBNAME##*_}" ;;
    *) die "set TARGET=production, or TARGET=scratch SCRATCH_DB=<scratch database>" 2 ;;
  esac
}

q()  { $PGBIN/psql -X -At -v ON_ERROR_STOP=1 -c "$1" }                                            # read or write
qr() { PGOPTIONS='-c default_transaction_read_only=on' $PGBIN/psql -X -At -v ON_ERROR_STOP=1 -c "$1" }   # read only
copyout() { PGOPTIONS='-c default_transaction_read_only=on' $PGBIN/psql -X -q -v ON_ERROR_STOP=1 -c "\\copy ($1) to '$2' with (format csv, header)" }
csvrows() { python3 -c "import csv,sys;print(sum(1 for _ in csv.reader(open(sys.argv[1], newline='')))-1)" "$1" }

check_db() {   # the session must reach the target database in UTC
  local line; line=$(qr "select current_database()||' tz='||current_setting('TimeZone')") || die "cannot reach $DBNAME"
  [ "$line" = "$DBNAME tz=UTC" ] || [ "$line" = "$DBNAME tz=Etc/UTC" ] || die "the database reads '$line'"
  say "database: $DBNAME ($TARGET; $line)"
}

lock_run() {   # <what>: the run lock, waited for, never broken
  RUNLOCK="${RUNLOCK:-$LOCKS/RUN_LOCK}"
  until ( setopt noclobber; print -r -- "RELEASE $1 $(date -u +%FT%TZ) pid=$$" > "$RUNLOCK" ) 2>/dev/null; do sleep 15; done
}
unlock_run() { [[ "$(cat "$RUNLOCK" 2>/dev/null)" == "RELEASE "*" pid=$$" ]] && /bin/rm -f "$RUNLOCK" }

# The worktree that runs the seed: a linked worktree at the release commit, never the main checkout.
resolve_worktree() {
  WT="${WT:-$REPO/.worktrees/futures_release}"
  REF="${REF:-}"
  [ -n "$REF" ] || die "set REF=<the merged release commit>" 2
  REF=$(git -C "$REPO" rev-parse -q --verify "$REF^{commit}") || die "no commit $REF" 2
}

prepare_worktree() {   # <targets...>: the worktree at REF, clean, built
  if [ ! -d "$WT" ]; then git -C "$REPO" worktree add -q --detach "$WT" "$REF" || die "git worktree add $WT failed"
  else
    [ -z "$(git -C "$WT" status --porcelain --untracked-files=no)" ] || die "$WT has local changes"
    git -C "$WT" checkout -q --detach "$REF" || die "checkout $REF in $WT failed"
  fi
  [ "$(git -C "$WT" rev-parse HEAD)" = "$REF" ] || die "$WT is not at $REF"
  ( cmake -S "$WT" -B "$WT/build" -DCMAKE_BUILD_TYPE=Release > "$OUT/build_configure.log" 2>&1 \
    && cmake --build "$WT/build" -j 6 --target "$@" > "$OUT/build.log" 2>&1 ) || die "build failed, see $OUT/build.log"
}

engine_line() {
  print -r -- "engine: $(git -C "$WT" log --oneline -1 HEAD)"
  print -r -- "tree: $(git -C "$WT" rev-parse 'HEAD^{tree}')"
  ( cd "$WT/build/bin/Release" && shasum -a 256 libtrade_ngin.dylib "$@" ) | sed 's/^/sha256: /'
}

# The run config: the frozen reference, with the target's database block, written into the worktree's config/. Only
# ever called while the script holds the run lock. <lookback years> [pinned end date]; for the live runner pass "-" as
# the lookback (the backtest block is left as in the reference, which the live runner does not read).
write_config() {
  local ly="$1" pin="${2:-}"
  for f in portfolio.json risk.json defaults.json email.json; do [ -s "$REFCFG/$f" ] || die "reference config $REFCFG/$f missing"; done
  ( cd "$REFCFG" && shasum -a 256 -c --quiet SHA256SUMS ) || die "the reference config does not match its SHA256SUMS"
  /bin/rm -rf "$WT/config"; mkdir -p "$WT/config/portfolios/conservative"
  cp "$REFCFG/portfolio.json" "$REFCFG/risk.json" "$REFCFG/email.json" "$WT/config/portfolios/conservative/"
  python3 - "$REFCFG/defaults.json" "$WT/config/defaults.json" "$ly" "$pin" <<'PY' || die "writing the run config failed"
import json, os, sys
src, out, ly, pin = sys.argv[1:5]
d = json.load(open(src))
d["database"] = {"host": os.environ["PGHOST"], "port": str(os.environ["PGPORT"]), "username": os.environ["PGUSER"],
                 "password": os.environ["PGPASSWORD"], "name": os.environ["PGDATABASE"]}
b = d.setdefault("backtest", {})
for k in ("frozen_end_date", "first_traded_date"):
    b.pop(k, None)
if ly != "-":
    b["lookback_years"] = int(ly)
if pin:
    b["frozen_end_date"] = pin
json.dump(d, open(out, "w"), indent=2)
PY
}

save_config() {   # <folder>: the run config as used, without the login
  mkdir -p "$1/config_used"
  cp "$WT/config/portfolios/conservative/portfolio.json" "$WT/config/portfolios/conservative/risk.json" "$1/config_used/"
  python3 -c "import json,sys;d=json.load(open(sys.argv[1]));d['database']={'name':d['database']['name'],'_note':'host, user and password left out'};json.dump(d,open(sys.argv[2],'w'),indent=2)" \
    "$WT/config/defaults.json" "$1/config_used/defaults.json"
}

template_diff() {   # information only: the reference config against the release's own conservative template (should be none)
  python3 - "$REFCFG" "$WT/config_template" <<'PY'
import json, os, sys
ref, tpl = sys.argv[1:3]
def walk(a, b, p, out):
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            if k.startswith("_"): continue
            if k not in a: out.append("only in the template: " + p + k)
            elif k not in b: out.append("only in the reference: " + p + k)
            else: walk(a[k], b[k], p + k + ".", out)
    elif a != b:
        out.append("differs: %s reference %s, template %s" % (p.rstrip("."), json.dumps(a)[:120], json.dumps(b)[:120]))
for f, sub in (("portfolio.json", "portfolios/conservative"), ("risk.json", "portfolios/conservative"), ("defaults.json", "")):
    a = json.load(open(os.path.join(ref, f))); b = json.load(open(os.path.join(tpl, sub, f)))
    if f == "defaults.json":
        for x in (a, b):
            x.pop("database", None); x.get("backtest", {}).pop("lookback_years", None)
            x.get("backtest", {}).pop("frozen_end_date", None); x.get("backtest", {}).pop("first_traded_date", None)
    out = []; walk(a, b, "", out)
    print("template diff %s: %s" % (f, "none" if not out else "%d keys" % len(out)))
    for line in out: print("  " + line)
PY
}

no_email_in_log() {   # <log (plain or .gz)>: the run must not have sent or built a send
  local n; n=$( { [[ "$1" == *.gz ]] && gzip -dc "$1" || cat "$1" } | grep -aicE "sending email|email sent|Email sending enabled|smtp" )
  [ "$n" = 0 ] || die "the run log $1 has $n e-mail lines"
}

scrub() {   # <files...>: replace any IPv4 address with <host> in place (a preflight prints the server's address)
  local f; for f in "$@"; do [ -f "$f" ] && sed -i '' -E 's/[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}/<host>/g' "$f"; done; return 0
}
