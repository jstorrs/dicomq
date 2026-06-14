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

# the meta header carries every routing field now; assert one of them
meta_has() { # meta_has <file> <pattern>
  dcmdump "$1" 2>/dev/null | grep -q "$2"
}

listening() { # listening <port> — passive check, no connection made
  if command -v ss >/dev/null 2>&1; then
    ss -tln 2>/dev/null | grep -q ":$1 "
  elif command -v lsof >/dev/null 2>&1; then        # macOS/BSD
    lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1
  else
    netstat -an 2>/dev/null | grep LISTEN | grep -q "[.:]$1 "
  fi
}

wait_listen() { # wait_listen <port>
  for _ in $(seq 1 100); do
    listening "$1" && return 0
    sleep 0.1
  done
  return 1
}

nlinks() { ls -ld "$1" | awk '{print $2}'; }  # portable stat -c %h

age_out() { touch -t 200001010000 "$@"; }     # older than any grace period

# --- a minimal valid Part 10 test object (with pixels, so transcoding
# --- to image-compression syntaxes is exercised for real) -----------------
cat > "$WORK/test.dump" <<'EOF'
(0008,0016) UI [1.2.840.10008.5.1.4.1.1.7]
(0008,0018) UI [1.2.276.0.7230010.3.1.4.42.1]
(0008,0060) CS [OT]
(0010,0010) PN [TEST^PATIENT]
(0020,000d) UI [1.2.276.0.7230010.3.1.2.42.1]
(0020,000e) UI [1.2.276.0.7230010.3.1.3.42.1]
(0028,0002) US 1
(0028,0004) CS [MONOCHROME2]
(0028,0010) US 8
(0028,0011) US 8
(0028,0100) US 8
(0028,0101) US 8
(0028,0102) US 7
(0028,0103) US 0
(7fe0,0010) OB 00\10\20\30\40\50\60\70\01\11\21\31\41\51\61\71\02\12\22\32\42\52\62\72\03\13\23\33\43\53\63\73\04\14\24\34\44\54\64\74\05\15\25\35\45\55\65\75\06\16\26\36\46\56\66\76\07\17\27\37\47\57\67\77
EOF
dump2dcm +te "$WORK/test.dump" "$WORK/test.dcm" 2>/dev/null \
  || { echo "cannot create test object (dump2dcm missing?)"; exit 1; }

# --- inject ---------------------------------------------------------------
new_spool
ID=$("$BIN/dicomq-inject" -c ARCHIVE -a MOD1 "$WORK/test.dcm")
DCM="$DICOMQ_SPOOL/queue/todo/ARCHIVE/$ID.dcm"
check "inject returns an id"               test -n "$ID"
check "inject commits the object by AET"   test -f "$DCM"
check "inject keys the queue by called AET" test ! -e "$DICOMQ_SPOOL/queue/todo/$ID.dcm"
check "object writes no sidecar"           test ! -e "$DICOMQ_SPOOL/queue/todo/ARCHIVE/$ID.env"
check "meta stamps the called AET"         meta_has "$DCM" '0002,0018.*ARCHIVE'
check "meta stamps the calling AET"        meta_has "$DCM" '0002,0016.*MOD1'
check "meta has the SOP instance UID"      meta_has "$DCM" '0002,0003.*1.2.276.0.7230010.3.1.4.42.1'
check "meta has the transfer syntax"       meta_has "$DCM" '0002,0010.*LittleEndianExplicit'
check "queue/tmp left empty"               test -z "$(ls -A "$DICOMQ_SPOOL/queue/tmp")"
check_not "inject refuses a non-DICOM file" "$BIN/dicomq-inject" -c X "$WORK/test.dump"

# --- unit: pure common helpers --------------------------------------------
if [ -x "$BIN/dicomq-unit-profile" ]; then
  check "unit: profile helpers" "$BIN/dicomq-unit-profile"
else
  echo "skip - unit-profile (binary not built)"
fi

# --- clean ----------------------------------------------------------------
# A committed message is one atomic-renamed .dcm, so the queues never hold
# half-written objects: clean only reaps interrupted queue/tmp/ writes.
new_spool
mkdir -p "$DICOMQ_SPOOL/queue/todo/ARCHIVE"
touch "$DICOMQ_SPOOL/queue/tmp/stale.dcm" "$DICOMQ_SPOOL/queue/todo/ARCHIVE/kept.dcm"
age_out "$DICOMQ_SPOOL/queue/tmp/stale.dcm" "$DICOMQ_SPOOL/queue/todo/ARCHIVE/kept.dcm"
"$BIN/dicomq-clean" >/dev/null
check "clean reaps stale tmp files"        test ! -e "$DICOMQ_SPOOL/queue/tmp/stale.dcm"
check "clean never touches committed objects" test -e "$DICOMQ_SPOOL/queue/todo/ARCHIVE/kept.dcm"

# --- local ----------------------------------------------------------------
new_spool
mkdir -p "$DICOMQ_SPOOL/aet/ARCHIVE"/{tmp,new}
ID=$("$BIN/dicomq-inject" -c ARCHIVE "$WORK/test.dcm")
MD="$DICOMQ_SPOOL/aet/ARCHIVE"
SRC="$DICOMQ_SPOOL/queue/todo/ARCHIVE"
check "local delivers the object"          "$BIN/dicomq-local" "$ID" "$MD" "$SRC"
check "delivered object exists"            test -f "$MD/new/$ID.dcm"
check "delivery is a hardlink"             test "$(nlinks "$MD/new/$ID.dcm")" = 2
check "local is idempotent"                "$BIN/dicomq-local" "$ID" "$MD" "$SRC"
check_not "local refuses a missing maildir" "$BIN/dicomq-local" "$ID" "$DICOMQ_SPOOL/aet/NOWHERE" "$SRC"
# cross-filesystem fallback: /dev/shm is a different fs from /tmp
if [ -d /dev/shm ] && [ "$(stat -fc %i /dev/shm 2>/dev/null)" != "$(stat -fc %i "$WORK" 2>/dev/null)" ]; then
  XMD=$(mktemp -d /dev/shm/dicomq-md.XXXXXX); mkdir -p "$XMD"/{tmp,new}
  check "local copies across filesystems"  "$BIN/dicomq-local" "$ID" "$XMD" "$SRC"
  check "cross-fs object delivered intact" cmp -s "$SRC/$ID.dcm" "$XMD/new/$ID.dcm"
  rm -rf "$XMD"
fi

# --- send: routing --------------------------------------------------------
new_spool
mkdir -p "$DICOMQ_SPOOL/aet/ARCHIVE"/{tmp,new}
ID=$("$BIN/dicomq-inject" -c ARCHIVE "$WORK/test.dcm")
"$BIN/dicomq-send" --once 2>/dev/null
check "send delivers to the default maildir" test -f "$DICOMQ_SPOOL/aet/ARCHIVE/new/$ID.dcm"
check "send dequeues after delivery"        test ! -e "$DICOMQ_SPOOL/queue/todo/ARCHIVE/$ID.dcm"

# fan-out: maildir + two forwards
new_spool
mkdir -p "$DICOMQ_SPOOL/aet/FAN"/{tmp,new} "$DICOMQ_SPOOL/dest"/{PACS1,PACS2} \
         "$DICOMQ_SPOOL/route"/{PACS1,PACS2}/todo
printf 'host: localhost\nport: 11178\naet: PACS1\n' > "$DICOMQ_SPOOL/dest/PACS1/remote"
printf 'host: localhost\nport: 11179\naet: PACS2\n' > "$DICOMQ_SPOOL/dest/PACS2/remote"
printf 'maildir ./\nforward PACS1\nforward PACS2\n' > "$DICOMQ_SPOOL/aet/FAN/deliver"
touch "$DICOMQ_SPOOL/route/PACS1/hold" "$DICOMQ_SPOOL/route/PACS2/hold"
ID=$("$BIN/dicomq-inject" -c FAN "$WORK/test.dcm")
"$BIN/dicomq-send" --once 2>/dev/null
check "fan-out delivers to the maildir"     test -f "$DICOMQ_SPOOL/aet/FAN/new/$ID.dcm"
check "fan-out routes to PACS1"             test -f "$DICOMQ_SPOOL/route/PACS1/todo/$ID.dcm"
check "fan-out routes to PACS2"             test -f "$DICOMQ_SPOOL/route/PACS2/todo/$ID.dcm"
check "fan-out object is hardlinked 3 ways" test "$(nlinks "$DICOMQ_SPOOL/route/PACS1/todo/$ID.dcm")" = 3
check "fan-out dequeues from todo"          test ! -e "$DICOMQ_SPOOL/queue/todo/FAN/$ID.dcm"
check "send respects hold flags (no agent spawned for held dest)" \
      test ! -e "$DICOMQ_SPOOL/route/PACS1/status"

# unknown AET, deferral (send opens no DICOM, so there is no corrupt path here)
new_spool
ID=$("$BIN/dicomq-inject" -c NOSUCHAET "$WORK/test.dcm")
mkdir -p "$DICOMQ_SPOOL/aet/DEFER"/{tmp,new}
printf 'forward MISSINGDEST\n' > "$DICOMQ_SPOOL/aet/DEFER/deliver"
ID2=$("$BIN/dicomq-inject" -c DEFER "$WORK/test.dcm")
"$BIN/dicomq-send" --once 2>/dev/null
check "unknown called AET is failed"        test -f "$DICOMQ_SPOOL/failed/$ID.dcm"
check "unsatisfiable instruction defers in place" \
      test -f "$DICOMQ_SPOOL/queue/todo/DEFER/$ID2.dcm"

# --- send: daemon mode reacts to new work via inotify ----------------------
# inotify is Linux-only; elsewhere (e.g. macOS) dicomq-send falls back to the
# periodic scan, so sub-scan-interval delivery is not expected. Skip there.
if [ "$(uname)" = Linux ]; then
  new_spool
  mkdir -p "$DICOMQ_SPOOL/aet/ARCHIVE"/{tmp,new}
  "$BIN/dicomq-send" -i 60 2>/dev/null & SEND=$!
  sleep 0.5
  ID=$("$BIN/dicomq-inject" -c ARCHIVE "$WORK/test.dcm")
  DELIVERED=no
  for _ in $(seq 1 30); do
    [ -f "$DICOMQ_SPOOL/aet/ARCHIVE/new/$ID.dcm" ] && { DELIVERED=yes; break; }
    sleep 0.1
  done
  kill $SEND 2>/dev/null; wait $SEND 2>/dev/null
  check "daemon send delivers promptly (inotify, not the 60s scan)" test "$DELIVERED" = yes
else
  echo "skip - daemon send prompt delivery (no inotify on $(uname))"
fi

# --- queue + ctl --------------------------------------------------------
new_spool
mkdir -p "$DICOMQ_SPOOL/dest/PACS1" "$DICOMQ_SPOOL/route/PACS1/todo"
mkdir -p "$DICOMQ_SPOOL/aet/FWD"/{tmp,new}
printf 'host: localhost\nport: 11178\naet: PACS1\n' > "$DICOMQ_SPOOL/dest/PACS1/remote"
printf 'forward PACS1\n' > "$DICOMQ_SPOOL/aet/FWD/deliver"
touch "$DICOMQ_SPOOL/route/PACS1/hold"
ID=$("$BIN/dicomq-inject" -c FWD -a MOD1 "$WORK/test.dcm")
"$BIN/dicomq-send" --once 2>/dev/null
OUT=$("$BIN/dicomq-queue")
check "queue shows the route backlog"      grep -q 'route/PACS1.*1 message' <<<"$OUT"
check "queue shows the hold flag"          grep -q 'held' <<<"$OUT"
check "queue lists messages per dest"      grep -q "^$ID " <<<"$("$BIN/dicomq-queue" PACS1)"
check "queue listing shows the AETs"       grep -q 'MOD1 -> FWD' <<<"$("$BIN/dicomq-queue" PACS1)"
"$BIN/dicomq-ctl" hold "$ID" >/dev/null
check "ctl hold moves the message"       test -f "$DICOMQ_SPOOL/hold/route/PACS1/todo/$ID.dcm"
check "hold mirrors the source path"     test ! -e "$DICOMQ_SPOOL/route/PACS1/todo/$ID.dcm"
check "ctl hold is idempotent"           "$BIN/dicomq-ctl" hold "$ID"
"$BIN/dicomq-ctl" release "$ID" >/dev/null
check "ctl release returns it"           test -f "$DICOMQ_SPOOL/route/PACS1/todo/$ID.dcm"
FOUT=$("$BIN/dicomq-ctl" fail "$ID" "operator says no" 2>&1)
check "ctl fail moves to failed/"        test -f "$DICOMQ_SPOOL/failed/$ID.dcm"
check "ctl fail logs the reason"         grep -q 'operator says no' <<<"$FOUT"
"$BIN/dicomq-ctl" requeue "$ID" >/dev/null
check "ctl requeue returns it to its AET queue" test -f "$DICOMQ_SPOOL/queue/todo/FWD/$ID.dcm"
check_not "ctl refuses an unknown id"    "$BIN/dicomq-ctl" hold 19990101000000000.0.000000

# --- recv (needs storescu/echoscu on PATH) --------------------------------
if command -v storescu >/dev/null; then
  PORT=11177
  new_spool
  mkdir -p "$DICOMQ_SPOOL/aet/ARCHIVE"/{tmp,new}

  "$BIN/dicomq-recv" --listen $PORT --once 2>/dev/null &
  RECV=$!; wait_listen $PORT
  check "recv accepts a store for a known AET" \
        storescu -aet MOD1 -aec ARCHIVE localhost $PORT "$WORK/test.dcm"
  wait $RECV
  ID=$(ls "$DICOMQ_SPOOL/queue/todo/ARCHIVE" 2>/dev/null | sed -n 's/\.dcm$//p' | head -1)
  DCM="$DICOMQ_SPOOL/queue/todo/ARCHIVE/$ID.dcm"
  check "recv commits the message by AET"    test -n "$ID" -a -f "$DCM"
  check "meta stamped with source AET"       meta_has "$DCM" '0002,0016.*MOD1'
  check "meta stamped with receiving AET"    meta_has "$DCM" '0002,0018.*ARCHIVE'
  check "meta has the SOP instance UID"      meta_has "$DCM" '0002,0003.*1.2.276.0.7230010.3.1.4.42.1'

  "$BIN/dicomq-recv" --listen $PORT --once 2>/dev/null &
  RECV=$!; wait_listen $PORT
  check_not "recv rejects an unknown called AET" \
        storescu -aet MOD1 -aec NOSUCH localhost $PORT "$WORK/test.dcm"
  wait $RECV

  "$BIN/dicomq-recv" --listen $PORT --once -w 10000000 2>/dev/null &
  RECV=$!; wait_listen $PORT
  check_not "recv refuses below the free-space watermark" \
        storescu -aet MOD1 -aec ARCHIVE localhost $PORT "$WORK/test.dcm"
  wait $RECV

  if command -v echoscu >/dev/null; then
    "$BIN/dicomq-recv" --listen $PORT --once 2>/dev/null &
    RECV=$!; wait_listen $PORT
    check "recv answers C-ECHO"              echoscu -aec ARCHIVE localhost $PORT
    wait $RECV
  fi

  # accept profile: ARCHIVE refuses implicit-only proposals
  printf 'ExplicitVRLittleEndian\n' > "$DICOMQ_SPOOL/aet/ARCHIVE/accept"
  "$BIN/dicomq-recv" --listen $PORT --once 2>/dev/null &
  RECV=$!; wait_listen $PORT
  check_not "accept profile refuses excluded syntaxes" \
        storescu -xi -aet MOD1 -aec ARCHIVE localhost $PORT "$WORK/test.dcm"
  wait $RECV
else
  echo "skip - recv tests (no storescu on PATH)"
fi

# --- remote (needs storescp on PATH) ---------------------------------------
if command -v storescp >/dev/null; then
  PORT=11178
  new_spool
  # the operator sizes the retry ladder by creating retry/<k> dirs; here
  # only retry/1 exists, so a rejection at retry/1 is terminal
  mkdir -p "$DICOMQ_SPOOL/aet/FWD"/{tmp,new} "$DICOMQ_SPOOL/dest/PACS1" \
           "$DICOMQ_SPOOL/route/PACS1/todo" "$DICOMQ_SPOOL/route/PACS1/retry/1" \
           "$WORK/pacs1"
  printf "host: localhost\nport: $PORT\naet: PACS1\n" > "$DICOMQ_SPOOL/dest/PACS1/remote"
  printf 'forward PACS1\n' > "$DICOMQ_SPOOL/aet/FWD/deliver"

  # happy path: route then forward to a stock storescp
  ID=$("$BIN/dicomq-inject" -c FWD "$WORK/test.dcm")
  touch "$DICOMQ_SPOOL/route/PACS1/hold"      # route without triggering
  "$BIN/dicomq-send" --once 2>/dev/null
  rm "$DICOMQ_SPOOL/route/PACS1/hold"
  storescp -od "$WORK/pacs1" $PORT 2>/dev/null & SCP=$!
  wait_listen $PORT
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  check "remote delivers to the destination"  test -n "$(ls -A "$WORK/pacs1")"
  check "remote dequeues after delivery"      test -z "$(ls -A "$DICOMQ_SPOOL/route/PACS1/todo")"
  check "no status file after success"        test ! -e "$DICOMQ_SPOOL/route/PACS1/status"
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null

  # dead site: nothing listening
  ID=$("$BIN/dicomq-inject" -c FWD "$WORK/test.dcm")
  touch "$DICOMQ_SPOOL/route/PACS1/hold"
  "$BIN/dicomq-send" --once 2>/dev/null
  rm "$DICOMQ_SPOOL/route/PACS1/hold"
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  check "down destination writes a status file" test -f "$DICOMQ_SPOOL/route/PACS1/status"
  check "status has a next-attempt time"      grep -q '^next-attempt-after: ' "$DICOMQ_SPOOL/route/PACS1/status"
  check "message survives a down destination" test -f "$DICOMQ_SPOOL/route/PACS1/todo/$ID.dcm"
  check "a connection failure climbs no rung" test ! -e "$DICOMQ_SPOOL/route/PACS1/retry/1/$ID.dcm"

  # transcode policy: destination accepts implicit only, object is explicit.
  # A per-message rejection climbs the retry ladder (todo -> retry/1).
  printf 'ImplicitVRLittleEndian\ntranscode: never\n' > "$DICOMQ_SPOOL/dest/PACS1/propose"
  rm -f "$DICOMQ_SPOOL/route/PACS1/status"
  storescp -od "$WORK/pacs1" $PORT 2>/dev/null & SCP=$!
  wait_listen $PORT
  RERR=$("$BIN/dicomq-remote" PACS1 2>&1)
  check "transcode never demotes to retry/1"  test -f "$DICOMQ_SPOOL/route/PACS1/retry/1/$ID.dcm"
  check "demotion left todo empty"            test -z "$(ls -A "$DICOMQ_SPOOL/route/PACS1/todo")"
  check "demotion names the syntax problem"   grep -q "transcode is 'never'" <<<"$RERR"
  # rejection at the top rung (no retry/2 dir) fails the message rather
  # than creating retry/2 — the ladder depth is the directories present
  age_out "$DICOMQ_SPOOL/route/PACS1/retry/1/$ID.dcm"
  RERR2=$("$BIN/dicomq-remote" PACS1 2>&1)
  check "rejection at the top rung fails it"   test -f "$DICOMQ_SPOOL/failed/$ID.dcm"
  check "no next rung is created"             test ! -e "$DICOMQ_SPOOL/route/PACS1/retry/2"
  check "failure names the missing rung"      grep -q 'no retry/2 rung' <<<"$RERR2"

  printf 'ImplicitVRLittleEndian\ntranscode: lossless\n' > "$DICOMQ_SPOOL/dest/PACS1/propose"
  "$BIN/dicomq-ctl" requeue "$ID" >/dev/null
  touch "$DICOMQ_SPOOL/route/PACS1/hold"
  "$BIN/dicomq-send" --once 2>/dev/null
  rm "$DICOMQ_SPOOL/route/PACS1/hold"
  rm -f "$WORK/pacs1"/*
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  check "transcode lossless converts and delivers" test -n "$(ls -A "$WORK/pacs1")"
  RECEIVED=$(ls "$WORK/pacs1" | head -1)
  check "delivered object is implicit VR"     sh -c "dcmdump +f '$WORK/pacs1/$RECEIVED' 2>/dev/null | grep -qi 'implicit'"
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null

  # compressing transcode: destination wants JPEG-LS lossless
  printf 'JPEGLSLossless\ntranscode: lossless\n' > "$DICOMQ_SPOOL/dest/PACS1/propose"
  ID=$("$BIN/dicomq-inject" -c FWD "$WORK/test.dcm")
  touch "$DICOMQ_SPOOL/route/PACS1/hold"
  "$BIN/dicomq-send" --once 2>/dev/null
  rm "$DICOMQ_SPOOL/route/PACS1/hold"
  rm -f "$WORK/pacs1"/*
  storescp +xa -od "$WORK/pacs1" $PORT 2>/dev/null & SCP=$!
  wait_listen $PORT
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  check "lossless compressing transcode delivers" test -n "$(ls -A "$WORK/pacs1")"
  RECEIVED=$(ls "$WORK/pacs1" | head -1)
  check "delivered object is JPEG-LS"         sh -c "dcmdump +f '$WORK/pacs1/$RECEIVED' 2>/dev/null | grep -q 'JPEGLSLossless'"

  # lossy target: refused under lossless, allowed under as-needed
  printf 'JPEGBaseline\ntranscode: lossless\n' > "$DICOMQ_SPOOL/dest/PACS1/propose"
  ID=$("$BIN/dicomq-inject" -c FWD "$WORK/test.dcm")
  touch "$DICOMQ_SPOOL/route/PACS1/hold"
  "$BIN/dicomq-send" --once 2>/dev/null
  rm "$DICOMQ_SPOOL/route/PACS1/hold"
  LERR=$("$BIN/dicomq-remote" PACS1 2>&1)
  check "lossy target refused under 'lossless'" test -f "$DICOMQ_SPOOL/route/PACS1/retry/1/$ID.dcm"
  check "refusal names the lossy syntax"      grep -q "is lossy and transcode is 'lossless'" <<<"$LERR"

  printf 'JPEGBaseline\ntranscode: as-needed\n' > "$DICOMQ_SPOOL/dest/PACS1/propose"
  "$BIN/dicomq-ctl" requeue "$ID" >/dev/null
  touch "$DICOMQ_SPOOL/route/PACS1/hold"
  "$BIN/dicomq-send" --once 2>/dev/null
  rm "$DICOMQ_SPOOL/route/PACS1/hold"
  rm -f "$WORK/pacs1"/*
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  check "lossy transcode delivers under 'as-needed'" test -n "$(ls -A "$WORK/pacs1")"
  RECEIVED=$(ls "$WORK/pacs1" | head -1)
  check "delivered object is JPEG baseline"   sh -c "dcmdump +f '$WORK/pacs1/$RECEIVED' 2>/dev/null | grep -q 'JPEGBaseline'"
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null
else
  echo "skip - remote tests (no storescp on PATH)"
fi

# --- TLS (needs openssl and TLS-enabled DCMTK tools) -----------------------
if command -v openssl >/dev/null && storescu --help 2>&1 | grep -q anonymous-tls; then
  TPORT=11186
  CERTS="$WORK/certs"; mkdir -p "$CERTS"
  openssl req -x509 -newkey rsa:2048 -keyout "$CERTS/ca.key" -out "$CERTS/ca.pem" \
      -days 1 -nodes -subj "/CN=dicomq-test-ca" 2>/dev/null
  openssl req -newkey rsa:2048 -keyout "$CERTS/srv.key" -out "$CERTS/srv.csr" \
      -nodes -subj "/CN=localhost" 2>/dev/null
  openssl x509 -req -in "$CERTS/srv.csr" -CA "$CERTS/ca.pem" -CAkey "$CERTS/ca.key" \
      -days 1 -out "$CERTS/srv.pem" 2>/dev/null

  # recv serves TLS from <spool>/tls/
  new_spool
  mkdir -p "$DICOMQ_SPOOL/aet/ARCHIVE"/{tmp,new} "$DICOMQ_SPOOL/tls"
  cp "$CERTS/srv.key" "$DICOMQ_SPOOL/tls/key.pem"
  cp "$CERTS/srv.pem" "$DICOMQ_SPOOL/tls/cert.pem"
  "$BIN/dicomq-recv" --listen $TPORT --once --tls 2>/dev/null & RECV=$!
  wait_listen $TPORT
  check "recv accepts a TLS store" \
        storescu +tla +cf "$CERTS/ca.pem" -aet MOD1 -aec ARCHIVE localhost $TPORT "$WORK/test.dcm"
  wait $RECV
  check "TLS store is queued" test -n "$(find "$DICOMQ_SPOOL/queue/todo" -name '*.dcm')"

  "$BIN/dicomq-recv" --listen $TPORT --once --tls 2>/dev/null & RECV=$!
  wait_listen $TPORT
  check_not "plaintext client cannot store to a TLS listener" \
        storescu -aet MOD1 -aec ARCHIVE localhost $TPORT "$WORK/test.dcm"
  kill $RECV 2>/dev/null; wait $RECV 2>/dev/null

  # remote speaks TLS when dest/<DEST>/tls/ exists
  new_spool
  mkdir -p "$DICOMQ_SPOOL/aet/FWD"/{tmp,new} "$DICOMQ_SPOOL/dest/PACS1/tls" \
           "$DICOMQ_SPOOL/route/PACS1/todo" "$WORK/tlspacs"
  printf "host: localhost\nport: $TPORT\naet: PACS1\n" > "$DICOMQ_SPOOL/dest/PACS1/remote"
  printf 'forward PACS1\n' > "$DICOMQ_SPOOL/aet/FWD/deliver"
  cp "$CERTS/ca.pem" "$DICOMQ_SPOOL/dest/PACS1/tls/ca.pem"
  ID=$("$BIN/dicomq-inject" -c FWD "$WORK/test.dcm")
  touch "$DICOMQ_SPOOL/route/PACS1/hold"
  "$BIN/dicomq-send" --once 2>/dev/null
  rm "$DICOMQ_SPOOL/route/PACS1/hold"
  storescp +tls "$CERTS/srv.key" "$CERTS/srv.pem" -ic -od "$WORK/tlspacs" $TPORT 2>/dev/null & SCP=$!
  wait_listen $TPORT
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  check "remote delivers over TLS"            test -n "$(ls -A "$WORK/tlspacs")"
  check "TLS route queue drained"             test -z "$(ls -A "$DICOMQ_SPOOL/route/PACS1/todo")"
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null
else
  echo "skip - TLS tests (no openssl or no TLS-enabled DCMTK)"
fi

# --- end to end: modality -> recv -> send -> maildir + remote -> PACS ------
if command -v storescu >/dev/null && command -v storescp >/dev/null; then
  RPORT=11184 PPORT=11185
  new_spool
  mkdir -p "$DICOMQ_SPOOL/aet/ROUTER"/{tmp,new} "$DICOMQ_SPOOL/dest/PACS1" \
           "$DICOMQ_SPOOL/route/PACS1/todo" "$WORK/endpacs"
  printf "host: localhost\nport: $PPORT\naet: PACS1\n" > "$DICOMQ_SPOOL/dest/PACS1/remote"
  printf 'maildir ./\nforward PACS1\n' > "$DICOMQ_SPOOL/aet/ROUTER/deliver"

  storescp -od "$WORK/endpacs" $PPORT 2>/dev/null & SCP=$!
  "$BIN/dicomq-recv" --listen $RPORT --once 2>/dev/null & RECV=$!
  wait_listen $RPORT; wait_listen $PPORT
  check "e2e: modality stores to dicomq-recv" \
        storescu -aet CT99 -aec ROUTER localhost $RPORT "$WORK/test.dcm"
  wait $RECV
  "$BIN/dicomq-send" --once 2>/dev/null
  check "e2e: maildir copy delivered"        test -n "$(ls "$DICOMQ_SPOOL/aet/ROUTER/new/" | grep '\.dcm$')"
  check "e2e: forwarded to the PACS"         test -n "$(ls -A "$WORK/endpacs")"
  check "e2e: every queue drained"           test -z "$(find "$DICOMQ_SPOOL/queue/todo" "$DICOMQ_SPOOL/route/PACS1/todo" -name '*.dcm')"
  EDCM=$(ls "$DICOMQ_SPOOL/aet/ROUTER/new/"*.dcm | head -1)
  check "e2e: delivered object records the modality" meta_has "$EDCM" '0002,0016.*CT99'
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null
else
  echo "skip - end-to-end test (needs storescu and storescp)"
fi

echo
echo "$PASS passed, $FAIL failed"
test "$FAIL" -eq 0
