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

# --- local ----------------------------------------------------------------
new_spool
mkdir -p "$DICOMQ_SPOOL/aet/ARCHIVE"/{tmp,new}
ID=$("$BIN/dicomq-inject" -c ARCHIVE "$WORK/test.dcm")
MD="$DICOMQ_SPOOL/aet/ARCHIVE"
check "local delivers the object"          "$BIN/dicomq-local" "$ID" "$MD"
check "delivered object exists"            test -f "$MD/new/$ID.dcm"
check "delivery is a hardlink"             test "$(stat -c %h "$MD/new/$ID.dcm")" = 2
check "local is idempotent"                "$BIN/dicomq-local" "$ID" "$MD"
check "local delivers the envelope on request" "$BIN/dicomq-local" "$ID" "$MD" env
check "delivered envelope exists"          test -f "$MD/new/$ID.env"
check_not "local refuses a missing maildir" "$BIN/dicomq-local" "$ID" "$DICOMQ_SPOOL/aet/NOWHERE"
# cross-filesystem fallback: /dev/shm is a different fs from /tmp
if [ -d /dev/shm ] && [ "$(stat -fc %i /dev/shm 2>/dev/null)" != "$(stat -fc %i "$WORK" 2>/dev/null)" ]; then
  XMD=$(mktemp -d /dev/shm/dicomq-md.XXXXXX); mkdir -p "$XMD"/{tmp,new}
  check "local copies across filesystems"  "$BIN/dicomq-local" "$ID" "$XMD"
  check "cross-fs object delivered intact" cmp -s "$WORK/test.dcm" "$XMD/new/$ID.dcm"
  rm -rf "$XMD"
fi

# --- send: routing --------------------------------------------------------
new_spool
mkdir -p "$DICOMQ_SPOOL/aet/ARCHIVE"/{tmp,new}
ID=$("$BIN/dicomq-inject" -c ARCHIVE "$WORK/test.dcm")
"$BIN/dicomq-send" --once 2>/dev/null
check "send delivers to the default maildir" test -f "$DICOMQ_SPOOL/aet/ARCHIVE/new/$ID.dcm"
check "send dequeues after delivery"        test ! -e "$DICOMQ_SPOOL/queue/todo/$ID.env"

# fan-out: maildir + two forwards
new_spool
mkdir -p "$DICOMQ_SPOOL/aet/FAN"/{tmp,new} "$DICOMQ_SPOOL/dest"/{PACS1,PACS2} \
         "$DICOMQ_SPOOL/route"/{PACS1,PACS2}/todo
printf 'host: localhost\nport: 11178\naet: PACS1\n' > "$DICOMQ_SPOOL/dest/PACS1/remote"
printf 'host: localhost\nport: 11179\naet: PACS2\n' > "$DICOMQ_SPOOL/dest/PACS2/remote"
printf 'maildir ./ env\nforward PACS1\nforward PACS2\n' > "$DICOMQ_SPOOL/aet/FAN/deliver"
touch "$DICOMQ_SPOOL/route/PACS1/hold" "$DICOMQ_SPOOL/route/PACS2/hold"
ID=$("$BIN/dicomq-inject" -c FAN "$WORK/test.dcm")
"$BIN/dicomq-send" --once 2>/dev/null
check "fan-out delivers to the maildir"     test -f "$DICOMQ_SPOOL/aet/FAN/new/$ID.dcm"
check "fan-out delivers the envelope"       test -f "$DICOMQ_SPOOL/aet/FAN/new/$ID.env"
check "fan-out routes to PACS1"             test -f "$DICOMQ_SPOOL/route/PACS1/todo/$ID.env"
check "fan-out routes to PACS2"             test -f "$DICOMQ_SPOOL/route/PACS2/todo/$ID.env"
check "fan-out object is hardlinked 3 ways" test "$(stat -c %h "$DICOMQ_SPOOL/route/PACS1/todo/$ID.dcm")" = 3
check "fan-out dequeues from todo"          test ! -e "$DICOMQ_SPOOL/queue/todo/$ID.env"
check "send respects hold flags (no agent spawned for held dest)" \
      test ! -e "$DICOMQ_SPOOL/route/PACS1/status"

# corrupt quarantine, unknown AET, deferral
new_spool
printf 'not an envelope\n' > "$DICOMQ_SPOOL/queue/todo/19990101000000000.1.000000.env"
touch "$DICOMQ_SPOOL/queue/todo/19990101000000000.1.000000.dcm"
ID=$("$BIN/dicomq-inject" -c NOSUCHAET "$WORK/test.dcm")
mkdir -p "$DICOMQ_SPOOL/aet/DEFER"/{tmp,new}
printf 'forward MISSINGDEST\n' > "$DICOMQ_SPOOL/aet/DEFER/deliver"
ID2=$("$BIN/dicomq-inject" -c DEFER "$WORK/test.dcm")
"$BIN/dicomq-send" --once 2>/dev/null
check "unparseable envelope is quarantined" test -f "$DICOMQ_SPOOL/corrupt/19990101000000000.1.000000.env"
check "quarantine keeps the object"         test -f "$DICOMQ_SPOOL/corrupt/19990101000000000.1.000000.dcm"
check "unknown called AET is failed"        test -f "$DICOMQ_SPOOL/failed/$ID.env"
check "failed envelope says why"            grep -q '^failed: .*unknown called AET' "$DICOMQ_SPOOL/failed/$ID.env"
check "unsatisfiable instruction defers in place" test -f "$DICOMQ_SPOOL/queue/todo/$ID2.env"

echo
echo "$PASS passed, $FAIL failed"
test "$FAIL" -eq 0
