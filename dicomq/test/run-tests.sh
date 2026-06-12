#!/bin/bash
# dicomq integration tests. Usage: run-tests.sh [<bindir>]
# Needs DCMTK's dump2dcm/dcmdump (and storescu/storescp for the network
# tests) on PATH. Exercises the real binaries against a throwaway spool.
set -u

BIN=$(cd "${1:-build}" && pwd)
WORK=$(mktemp -d /tmp/dicomq-test.XXXXXX)
export DICOMQ_SPOOL="$WORK/spool"
PASS=0 FAIL=0
cleanup() { kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

ok()   { PASS=$((PASS+1)); echo "ok   - $1"; }
bad()  { FAIL=$((FAIL+1)); echo "FAIL - $1"; }
check() { # check <description> <command...>
  local desc=$1; shift
  if "$@" >/dev/null 2>&1; then ok "$desc"; else bad "$desc"; fi
}
check_not() {
  local desc=$1; shift
  if "$@" >/dev/null 2>&1; then bad "$desc"; else ok "$desc"; fi
}

new_spool() {
  rm -rf "$DICOMQ_SPOOL"
  mkdir -p "$DICOMQ_SPOOL"/{queue/{tmp,todo},route,aet,dest,failed,hold,corrupt}
}

# --- a minimal valid Part 10 test object ---------------------------------
cat > "$WORK/test.dump" <<'EOF'
(0008,0016) UI [1.2.840.10008.5.1.4.1.1.7]
(0008,0018) UI [1.2.276.0.7230010.3.1.4.42.1]
(0008,0060) CS [OT]
(0010,0010) PN [TEST^PATIENT]
(0020,000d) UI [1.2.276.0.7230010.3.1.2.42.1]
(0020,000e) UI [1.2.276.0.7230010.3.1.3.42.1]
EOF
dump2dcm +te "$WORK/test.dump" "$WORK/test.dcm" 2>/dev/null \
  || { echo "cannot create test object (dump2dcm missing?)"; exit 1; }

# --- inject ---------------------------------------------------------------
new_spool
ID=$("$BIN/dicomq-inject" -c ARCHIVE -a MOD1 "$WORK/test.dcm")
check "inject returns an id"               test -n "$ID"
check "inject commits the object"          test -f "$DICOMQ_SPOOL/queue/todo/$ID.dcm"
check "inject commits the envelope"        test -f "$DICOMQ_SPOOL/queue/todo/$ID.env"
check "envelope has the called AET"        grep -q '^called-aet: ARCHIVE$' "$DICOMQ_SPOOL/queue/todo/$ID.env"
check "envelope has the SOP instance UID"  grep -q '^sop-instance-uid: 1.2.276.0.7230010.3.1.4.42.1$' "$DICOMQ_SPOOL/queue/todo/$ID.env"
check "envelope has the transfer syntax"   grep -q '^transfer-syntax-uid: 1.2.840.10008.1.2.1$' "$DICOMQ_SPOOL/queue/todo/$ID.env"
check "injected object is byte-identical"  cmp -s "$WORK/test.dcm" "$DICOMQ_SPOOL/queue/todo/$ID.dcm"
check "queue/tmp left empty"               test -z "$(ls -A "$DICOMQ_SPOOL/queue/tmp")"
check_not "inject refuses a non-DICOM file" "$BIN/dicomq-inject" -c X "$WORK/test.dump"

# --- clean ----------------------------------------------------------------
new_spool
touch "$DICOMQ_SPOOL/queue/tmp/stale.dcm" "$DICOMQ_SPOOL/queue/todo/orphan.dcm"
touch "$DICOMQ_SPOOL/queue/todo/kept.dcm"; touch "$DICOMQ_SPOOL/queue/todo/kept.env"
touch -d '2 days ago' "$DICOMQ_SPOOL/queue/tmp/stale.dcm" "$DICOMQ_SPOOL/queue/todo/orphan.dcm"
"$BIN/dicomq-clean" >/dev/null
check "clean reaps stale tmp files"        test ! -e "$DICOMQ_SPOOL/queue/tmp/stale.dcm"
check "clean reaps old orphan objects"     test ! -e "$DICOMQ_SPOOL/queue/todo/orphan.dcm"
check "clean keeps committed messages"     test -e "$DICOMQ_SPOOL/queue/todo/kept.dcm"
touch -d '2 days ago' "$DICOMQ_SPOOL/queue/todo/kept.dcm"
"$BIN/dicomq-clean" >/dev/null
check "clean keeps old committed messages" test -e "$DICOMQ_SPOOL/queue/todo/kept.dcm"

echo
echo "$PASS passed, $FAIL failed"
test "$FAIL" -eq 0
