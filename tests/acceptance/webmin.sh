#!/usr/bin/env bash
#
# webmin.sh -- Acceptance checks A1-A10 for the Perl/CGI work on a Webmin
# checkout (https://github.com/webmin/webmin, read-only: never modified,
# never executed).
#
# Usage: tests/acceptance/webmin.sh [CORPUS_DIR] [TOKTOKEN_BINARY]
#   CORPUS_DIR       default: $WEBMIN_CORPUS or ~/temp/webmin
#   TOKTOKEN_BINARY  default: $TOKTOKEN_BIN, build-rel/toktoken, build/toktoken
#
# The index lives in a throwaway HOME (mktemp), so the real
# ~/.cache/toktoken is never touched. Skips (exit 0) when the corpus is
# absent. Exit 1 when any check fails. Requires python3 for JSON checks.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CORPUS="${1-${WEBMIN_CORPUS-$HOME/temp/webmin}}"
BIN="${2-${TOKTOKEN_BIN-}}"

if [ -z "$BIN" ]; then
    for c in "$ROOT/build-rel/toktoken" "$ROOT/build/toktoken"; do
        if [ -x "$c" ]; then
            BIN="$c"
            break
        fi
    done
fi

if [ ! -d "$CORPUS" ] || [ ! -f "$CORPUS/web-lib-funcs.pl" ]; then
    echo "SKIP: Webmin corpus not found at $CORPUS"
    exit 0
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    echo "FAIL: toktoken binary not found (build first or set TOKTOKEN_BIN)" >&2
    exit 1
fi
command -v python3 >/dev/null || { echo "FAIL: python3 required" >&2; exit 1; }

TT_HOME="$(mktemp -d)"
trap 'rm -r -- "$TT_HOME"' EXIT
OUT="$TT_HOME/out"
mkdir -p "$OUT"

FAILS=0
report() {
    local id="${1-}" ok="${2-}" detail="${3-}"
    if [ "$ok" = "1" ]; then
        printf '%-4s PASS  %s\n' "$id" "$detail"
    else
        printf '%-4s FAIL  %s\n' "$id" "$detail"
        FAILS=$((FAILS + 1))
    fi
}

# tt NAME ARGS... -> JSON in $OUT/NAME.json, wall ms in $OUT/NAME.ms
tt() {
    local name="${1-}"
    shift
    local t0 t1
    t0=$(date +%s%N)
    (cd "$CORPUS" && HOME="$TT_HOME" "$BIN" "$@" >"$OUT/$name.json" 2>"$OUT/$name.err") || true
    t1=$(date +%s%N)
    echo $(((t1 - t0) / 1000000)) >"$OUT/$name.ms"
}

ms() { cat "$OUT/${1-}.ms"; }

# check NAME 'python expression over d (parsed JSON)' -> prints "ok<TAB>detail"
check() {
    local name="${1-}" expr="${2-}"
    python3 -I - "$OUT/$name.json" "$expr" <<'PY'
import json, sys
path, expr = sys.argv[1], sys.argv[2]
try:
    d = json.load(open(path))
except Exception as e:
    print("0\tinvalid JSON: %s" % e)
    sys.exit(0)
ok, detail = eval(expr, {"d": d})
print("%d\t%s" % (1 if ok else 0, detail))
PY
}

run_check() {
    local id="${1-}" name="${2-}" expr="${3-}" res
    res="$(check "$name" "$expr")"
    report "$id" "${res%%$'\t'*}" "${res#*$'\t'}"
}

echo "corpus: $CORPUS ($(cat "$CORPUS/version" 2>/dev/null || echo '?'))"
echo "binary: $BIN ($("$BIN" --version 2>/dev/null | head -1))"
echo

# ---- Index (A9, A10) ----
tt index index:create
INDEX_MS=$(ms index)

# A1: outline of cron/cron-lib.pl
tt a1 inspect:outline cron/cron-lib.pl --kind subroutine
run_check A1 a1 '(lambda n: (len(d["symbols"]) >= 30 and {"can_edit_user","create_cron_job","list_cron_jobs","can_use_cron"} <= n, "%d subroutines with line ranges" % len(d["symbols"])))({s["name"] for s in d["symbols"]})'

# A2: can_edit_user body, kind, signature
tt a2 inspect:symbol 'cron/cron-lib.pl::can_edit_user#subroutine'
run_check A2 a2 '(d["kind"] == "subroutine" and d["line"] == 757 and d["end"] == 779 and d["sig"] == "can_edit_user(&access, user)" and d["source"].startswith("sub can_edit_user") and d["source"].rstrip().endswith("}"), "kind=%s %d-%d sig=%s" % (d.get("kind"), d.get("line", 0), d.get("end", 0), d.get("sig")))'

# A3: importers of cron/cron-lib.pl
tt a3 find:importers cron/cron-lib.pl
run_check A3 a3 '(lambda f: ({"cron/save_cron.cgi","cron/index.cgi","cron/edit_cron.cgi"} <= f, "%d importers" % len(f)))({i["from_file"] for i in d["importers"]})'

# A4: dependents of usermin/usermin-lib.pl
tt a4 inspect:dependencies usermin/usermin-lib.pl
run_check A4 a4 '(lambda f: ({"usermin/start.cgi","usermin/stop.cgi","usermin/edit_configs.cgi","usermin/save_configs.cgi"} <= f, "%d dependents" % len(f)))({x["file"] for x in d["dependents"]})'

# A5: regex search == grep -E (symlinks followed, like the index)
tt a5 search:text 'system_logged\("' --regex --case-sensitive --max 20000 --limit 20000
(cd "$CORPUS" && grep -REn --exclude-dir=.git 'system_logged\("' . | sed -E 's#^\./##' | cut -d: -f1,2 | sort -u) >"$OUT/a5.grep"
run_check A5 a5 '(lambda tt, gr: (tt == gr and "usermin/usermin-lib.pl:901" in tt, "%d results, grep -E %d, %s" % (len(tt), len(gr), "identical" if tt == gr else "DIFFERENT")))({"%s:%d" % (r.get("f", r.get("file")), r.get("l", r.get("line"))) for r in d["results"]}, set(open("'"$OUT"'/a5.grep").read().split()))'
SEARCH_MS=$(ms a5)

# A6: codebase detection
tt a6 codebase:detect
run_check A6 a6 '(d["primary"] == "perl" and {"javascript","bash"} <= set(d["secondary"]), "primary=%s secondary=%s" % (d["primary"], ",".join(d["secondary"])))'

# A7: stats vs suggest units
tt a7s stats
tt a7g suggest
run_check A7 a7s '(lambda g: (d["languages"] == g["language_distribution"] and d["units"]["languages"] == "files" and g["units"]["language_distribution"] == "files", "languages=%s (files)" % d["languages"]))(__import__("json").load(open("'"$OUT"'/a7g.json")))'

# A8: definitions vs references
tt a8 search:symbols system_logged --count
run_check A8 a8 '(d["definitions"] >= 1 and d["references"] > d["definitions"], "count=%d definitions=%d references=%d in %d files" % (d["count"], d["definitions"], d["references"], d["reference_files"]))'

# A9: default index excludes vendor_perl/ and .py/.c/verilog
DB="$(ls "$TT_HOME"/.cache/toktoken/projects/*/db.sqlite)"
python3 -I - "$DB" >"$OUT/a9.json" <<'PY'
import json, sqlite3, sys
c = sqlite3.connect("file:%s?mode=ro" % sys.argv[1], uri=True)
q = lambda s: c.execute(s).fetchone()[0]
print(json.dumps({
    "vendor_perl": q("SELECT COUNT(*) FROM files WHERE path LIKE 'vendor_perl/%' OR path LIKE '%/vendor_perl/%'"),
    "py_c_verilog": q("SELECT COUNT(*) FROM files WHERE language IN ('python','c','verilog')"),
    "files": q("SELECT COUNT(*) FROM files"),
}))
PY
run_check A9 a9 '(d["vendor_perl"] == 0 and d["py_c_verilog"] == 0, "files=%d vendor_perl=%d py/c/verilog=%d" % (d["files"], d["vendor_perl"], d["py_c_verilog"]))'

# A10: budgets (wall clock, process start included)
tt a10 inspect:symbol 'cron/cron-lib.pl::can_edit_user#subroutine'
SYMBOL_MS=$(ms a10)
ok=0
if [ "$INDEX_MS" -le 5000 ] && [ "$SEARCH_MS" -le 300 ] && [ "$SYMBOL_MS" -le 50 ]; then ok=1; fi
report A10 "$ok" "index:create ${INDEX_MS} ms (<=5000), search:text ${SEARCH_MS} ms (<=300), inspect:symbol ${SYMBOL_MS} ms (<=50)"

# Informational
tt cyc inspect:cycles
tt flow security:flow --taint in --count
echo
echo "info: $(check cyc '(True, "%d import cycles" % d["summary"]["total_cycles"])' | cut -f2-)"
echo "info: $(check flow '(True, "security:flow --taint in: %d flows, %d unknown" % (d["count"], d["unknown_count"]))' | cut -f2-) in $(ms flow) ms"

echo
if [ "$FAILS" -gt 0 ]; then
    echo "$FAILS check(s) failed"
    exit 1
fi
echo "all checks passed"
