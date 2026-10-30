#!/bin/zsh
# 00_prerequisites.sh: checks this machine before the first step: the commands, the Python modules the reports import, the
# two config files. Run it from the repository root, once, before anything else.
#
#   zsh 00_prerequisites.sh
#
# CHANGES: nothing. READS: no database; config/defaults.json and config/databento.env are only looked for, never opened.
# TAKES: seconds. Exit 0 when everything is there, 2 otherwise, with each missing item named.
set -u
. "${0:A:h}/lib.zsh"
bad=0
miss() { print -r -- "MISSING: $1"; bad=1 }
[ "$(uname)" = Darwin ] && print -r -- "ok: macOS" || miss "macOS (the scripts use sed -i '' and md5)"
for c in zsh python3 zstd cmake git curl shasum md5 gzip; do
  if command -v "$c" > /dev/null; then print -r -- "ok: $c ($(command -v "$c"))"; else miss "the command $c"; fi
done
for c in psql pg_dump pg_restore; do
  if [ -x "$PGBIN/$c" ]; then print -r -- "ok: $c ($("$PGBIN/$c" --version))"; else miss "$c in PGBIN ($PGBIN)"; fi
done
print -r -- "pg_dump must be the server's major version or newer: step 14 stops, with nothing deleted, if it is older"
for m in numpy pandas; do
  v=$(python3 -c "import $m; print($m.__version__)" 2> /dev/null) && print -r -- "ok: Python module $m $v" || miss "the Python module $m ($(command -v python3))"
done
for f in config/defaults.json config/databento.env; do
  if [ -s "$REPO/$f" ]; then print -r -- "ok: $f is there"; else miss "$f"; fi
done
[ $bad = 0 ] || { print -r -- "STOPPED: fix the lines marked MISSING"; exit 2 }
print -r -- "PREREQUISITES OK"
