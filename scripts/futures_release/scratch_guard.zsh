# scratch_guard.zsh: sourced by every script that can take a scratch target. A scratch command must never reach production:
# scratch_guard stops the script when PGHOST and PGPORT are the server named in config/defaults.json. Needs REPO.
scratch_guard() {
  local cfg="$REPO/config/defaults.json"
  [ -s "$cfg" ] || return 0
  python3 - "$cfg" <<'PY' && return 0
import json, os, sys
d = json.load(open(sys.argv[1])).get("database", {})
same = str(d.get("host")) == os.environ.get("PGHOST", "") and str(d.get("port", "5432")) == os.environ.get("PGPORT", "5432")
sys.exit(1 if same else 0)
PY
  print -r -- "REFUSED: a scratch target, but PGHOST and PGPORT are the server of config/defaults.json"
  exit 2
}
