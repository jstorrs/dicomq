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
  mkdir -p "$DICOMQ_SPOOL"/{queue/{tmp,todo},route,aet,dest,failed,hold}
}

# the meta header carries every routing field now; assert one of them
meta_has() { # meta_has <file> <pattern>
  dcmdump "$1" 2>/dev/null | grep -q "$2"
}

listening() { # listening <port> — passive check, no connection made
  # Try each available probe and fall through when the tool itself fails
  # rather than reports "not listening" — e.g. `ss` exits non-zero with
  # "Cannot open netlink socket" in a restricted sandbox, where lsof or
  # netstat still work. Only conclude not-listening once all have been tried.
  local out
  if command -v ss >/dev/null 2>&1 && out=$(ss -tln 2>/dev/null); then
    grep -q ":$1 " <<<"$out" && return 0
  fi
  if command -v lsof >/dev/null 2>&1; then          # macOS/BSD
    lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1 && return 0
  fi
  if command -v netstat >/dev/null 2>&1; then
    netstat -an 2>/dev/null | grep LISTEN | grep -q "[.:]$1 " && return 0
  fi
  return 1
}

wait_listen() { # wait_listen <port>
  for _ in $(seq 1 100); do
    listening "$1" && return 0
    sleep 0.1
  done
  return 1
}

# A listener we just started must come up, or every dependent network check
# would fail in turn. Stop at the first one with a single clear diagnostic
# rather than cascading dozens of confusing storescu failures — the usual
# cause is loopback being unavailable (e.g. a sandbox) or a stale recv still
# bound to the port.
require_listen() { # require_listen <port>
  wait_listen "$1" && return 0
  echo "FAIL - listener never came up on port $1 (loopback unavailable, or a"
  echo "       prior dicomq-recv still bound?); aborting network tests."
  echo
  echo "$PASS passed, $((FAIL + 1)) failed"
  exit 1
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

# a second object in the SAME study (0020,000d) but a different series and
# SOP instance — for study-mode, where both must land in one batch
sed -e 's#3.1.4.42.1#3.1.4.42.2#' -e 's#3.1.3.42.1#3.1.3.42.2#' \
    "$WORK/test.dump" > "$WORK/test2.dump"
dump2dcm +te "$WORK/test2.dump" "$WORK/test2.dcm" 2>/dev/null \
  || { echo "cannot create second test object"; exit 1; }

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
check_not "inject refuses a reserved called AET" "$BIN/dicomq-inject" -c tmp "$WORK/test.dcm"

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

# clean reaps route/<dest>/complete/ by message-id age (idTime, not mtime),
# leaving recent deliveries; failed/ and corrupt/ are never auto-reaped
new_spool
CDIR="$DICOMQ_SPOOL/route/PACS1/complete"
mkdir -p "$CDIR" "$CDIR/20000102000000000.0.000000"
OLD=20000101000000000.0.000000                       # year 2000: aged out
NEW=$(date -u +%Y%m%d%H%M%S)000.0.000000             # ~now: within grace
touch "$CDIR/$OLD.dcm" "$CDIR/$NEW.dcm" "$CDIR/20000102000000000.0.000000/a.dcm"
mkdir -p "$DICOMQ_SPOOL/route/PACS1/failed"; touch "$DICOMQ_SPOOL/route/PACS1/failed/$OLD.dcm"
# trash/ holds a crash-leftover discard; clean empties it unconditionally
mkdir -p "$DICOMQ_SPOOL/trash/leftover.123.0"; touch "$DICOMQ_SPOOL/trash/leftover.123.0/a.dcm"
"$BIN/dicomq-clean" >/dev/null
check "clean reaps an aged complete/ object"   test ! -e "$CDIR/$OLD.dcm"
check "clean reaps an aged complete/ batch"    test ! -e "$CDIR/20000102000000000.0.000000"
check "clean keeps a recent complete/ object"  test -e "$CDIR/$NEW.dcm"
check "clean never reaps per-dest failed/"     test -e "$DICOMQ_SPOOL/route/PACS1/failed/$OLD.dcm"
check "clean empties trash/"                   test ! -e "$DICOMQ_SPOOL/trash/leftover.123.0"

# CLI numeric bounds: reject values that would overflow the hours*3600 /
# interval*1000 arithmetic (an overflowed poll timeout blocks forever)
check_not "clean rejects an out-of-range -g"   "$BIN/dicomq-clean" -g 99999999
check_not "send rejects an out-of-range -i"    "$BIN/dicomq-send" -i 999999999
new_spool
check "send accepts -i at the cap (one pass)"  "$BIN/dicomq-send" -i 86400 --once

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
# a different inode already at new/<id>.dcm is a collision, not a replayed
# delivery: refuse so the source is not dequeued against the wrong object, and
# report it as PERMANENT (exit 100) — retrying can never resolve it
cp "$WORK/test.dcm" "$SRC/COLLIDE.dcm"
echo "a different object" > "$MD/new/COLLIDE.dcm"
"$BIN/dicomq-local" COLLIDE "$MD" "$SRC" >/dev/null 2>&1; rc=$?
check "local reports a wrong-object collision as permanent (exit 100)" \
      test "$rc" = 100
rm -f "$MD/new/COLLIDE.dcm" "$SRC/COLLIDE.dcm"
# ...but a byte-identical slot under a different inode is a crash-replayed
# cross-filesystem copy delivery, not a collision: delivered, exit 0
cp "$WORK/test.dcm" "$SRC/REPLAY.dcm"
cp "$SRC/REPLAY.dcm" "$MD/new/REPLAY.dcm"
check "local treats an identical-copy slot as delivered" \
      "$BIN/dicomq-local" REPLAY "$MD" "$SRC"
rm -f "$MD/new/REPLAY.dcm" "$SRC/REPLAY.dcm"
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

# a PERMANENT local-delivery failure (a different object already occupies the
# maildir slot) escalates to failed/, not an endless re-attempt every scan
new_spool
mkdir -p "$DICOMQ_SPOOL/aet/COLL"/{tmp,new}
ID=$("$BIN/dicomq-inject" -c COLL "$WORK/test.dcm")
echo "a different object" > "$DICOMQ_SPOOL/aet/COLL/new/$ID.dcm"   # collide
"$BIN/dicomq-send" --once 2>/dev/null
check "permanent local failure escalates to failed/" \
      test -f "$DICOMQ_SPOOL/failed/$ID.dcm"
check "permanent local failure dequeues from todo" \
      test ! -e "$DICOMQ_SPOOL/queue/todo/COLL/$ID.dcm"

# a byte-identical copy in the slot (a crash-replayed cross-fs delivery)
# is a completed delivery: send dequeues it, nothing escalates
new_spool
mkdir -p "$DICOMQ_SPOOL/aet/COLL"/{tmp,new}
ID=$("$BIN/dicomq-inject" -c COLL "$WORK/test.dcm")
cp "$DICOMQ_SPOOL/queue/todo/COLL/$ID.dcm" "$DICOMQ_SPOOL/aet/COLL/new/$ID.dcm"
"$BIN/dicomq-send" --once 2>/dev/null
check "replayed copy delivery dequeues without escalating" \
      test ! -e "$DICOMQ_SPOOL/queue/todo/COLL/$ID.dcm" -a \
           ! -e "$DICOMQ_SPOOL/failed/$ID.dcm"

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

# the global failed/ is dicomq's to create on first use (DESIGN.md "Spool
# layout"), like every per-destination sink — a spool skeleton lacking it must
# not strand send (re-failing every scan) or error out ctl.
new_spool
rm -rf "$DICOMQ_SPOOL/failed"
ID=$("$BIN/dicomq-inject" -c UNKNOWNAET "$WORK/test.dcm")
"$BIN/dicomq-send" --once 2>/dev/null
check "send creates a missing failed/ to fail an unknown AET" \
      test -f "$DICOMQ_SPOOL/failed/$ID.dcm"

new_spool
mkdir -p "$DICOMQ_SPOOL/aet/KAET"/{tmp,new}
ID=$("$BIN/dicomq-inject" -c KAET "$WORK/test.dcm")
rm -rf "$DICOMQ_SPOOL/failed"
check "ctl fail creates a missing failed/" "$BIN/dicomq-ctl" fail "$ID"
check "ctl fail landed the message in the recreated failed/" \
      test -f "$DICOMQ_SPOOL/failed/$ID.dcm"

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

# queue must not keep reporting a destination as "down" once its dead-site
# backoff has elapsed — dicomq-send would already be retrying it (the status
# file lingers until a successful association). Past next-attempt-after =>
# "last failure"; future => "down until".
printf 'last-failure: 2000-01-01T00:00:00Z connection refused\nfailures: 3\nnext-attempt-after: 2000-01-01T00:05:00Z\n' \
  > "$DICOMQ_SPOOL/route/PACS1/status"
PAST=$("$BIN/dicomq-queue")
check_not "queue does not call an elapsed-backoff dest down" \
      grep -q 'down until' <<<"$PAST"
check "queue shows the last failure for a retry-eligible dest" \
      grep -q 'last failure' <<<"$PAST"
printf 'last-failure: 2000-01-01T00:00:00Z connection refused\nfailures: 3\nnext-attempt-after: 2999-01-01T00:00:00Z\n' \
  > "$DICOMQ_SPOOL/route/PACS1/status"
FUT=$("$BIN/dicomq-queue")
check "queue reports a still-backed-off dest as down" \
      grep -q 'down until' <<<"$FUT"
rm -f "$DICOMQ_SPOOL/route/PACS1/status"
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

# an unreadable queue directory must not pass silently as "no work": the
# listing helpers report a real error (EACCES) but stay quiet for a merely
# absent directory ("missing dir = empty"). chmod 000 has no effect as root.
new_spool
mkdir -p "$DICOMQ_SPOOL/dest/PACS1" "$DICOMQ_SPOOL/route/PACS1/todo"
CLEANOUT=$("$BIN/dicomq-queue" 2>&1 >/dev/null)
check "absent dirs produce no spurious diagnostics" \
      test -z "$CLEANOUT"
if [ "$(id -u)" != 0 ]; then
  chmod 000 "$DICOMQ_SPOOL/route/PACS1/todo"
  DERR=$("$BIN/dicomq-queue" PACS1 2>&1 >/dev/null)
  chmod 755 "$DICOMQ_SPOOL/route/PACS1/todo"   # restore so cleanup can rm it
  check "an unreadable queue dir is reported, not silently empty" \
        grep -qi 'cannot read directory' <<<"$DERR"
else
  echo "skip - unreadable-dir diagnostic (running as root)"
fi

# --- recv (needs storescu/echoscu on PATH) --------------------------------
if command -v storescu >/dev/null; then
  PORT=11177
  new_spool
  mkdir -p "$DICOMQ_SPOOL/aet/ARCHIVE"/{tmp,new}

  "$BIN/dicomq-recv" --listen $PORT --once 2>/dev/null &
  RECV=$!; require_listen $PORT
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
  RECV=$!; require_listen $PORT
  check_not "recv rejects an unknown called AET" \
        storescu -aet MOD1 -aec NOSUCH localhost $PORT "$WORK/test.dcm"
  wait $RECV

  "$BIN/dicomq-recv" --listen $PORT --once -w 10000000 2>/dev/null &
  RECV=$!; require_listen $PORT
  check_not "recv refuses below the free-space watermark" \
        storescu -aet MOD1 -aec ARCHIVE localhost $PORT "$WORK/test.dcm"
  wait $RECV

  if command -v echoscu >/dev/null; then
    "$BIN/dicomq-recv" --listen $PORT --once 2>/dev/null &
    RECV=$!; require_listen $PORT
    check "recv answers C-ECHO"              echoscu -aec ARCHIVE localhost $PORT
    wait $RECV
  fi

  # accept profile: ARCHIVE refuses implicit-only proposals
  printf 'ExplicitVRLittleEndian\n' > "$DICOMQ_SPOOL/aet/ARCHIVE/accept"
  "$BIN/dicomq-recv" --listen $PORT --once 2>/dev/null &
  RECV=$!; require_listen $PORT
  check_not "accept profile refuses excluded syntaxes" \
        storescu -xi -aet MOD1 -aec ARCHIVE localhost $PORT "$WORK/test.dcm"
  wait $RECV

  # no accept file: the compiled-in default accepts every standard syntax
  # (uncompressed preferred), so a compressed-only proposer can still store
  if command -v dcmcjpls >/dev/null; then
    rm "$DICOMQ_SPOOL/aet/ARCHIVE/accept"
    dcmcjpls "$WORK/test.dcm" "$WORK/test-jls.dcm" 2>/dev/null
    cat > "$WORK/jls-only.cfg" <<'EOF'
[[TransferSyntaxes]]
[JLSOnly]
TransferSyntax1 = 1.2.840.10008.1.2.4.80

[[PresentationContexts]]
[JLSContext]
PresentationContext1 = 1.2.840.10008.5.1.4.1.1.7\JLSOnly

[[Profiles]]
[JLS]
PresentationContexts = JLSContext
EOF
    "$BIN/dicomq-recv" --listen $PORT --once 2>/dev/null &
    RECV=$!; require_listen $PORT
    check "default accept profile takes a compressed-only proposal" \
          storescu -xf "$WORK/jls-only.cfg" JLS -aet MOD1 -aec ARCHIVE \
                   localhost $PORT "$WORK/test-jls.dcm"
    wait $RECV
    JLSDCM="$DICOMQ_SPOOL/queue/todo/ARCHIVE/$(ls "$DICOMQ_SPOOL/queue/todo/ARCHIVE" | sort | tail -1)"
    check "compressed object is queued as received" \
          meta_has "$JLSDCM" '0002,0010.*JPEGLSLossless'
  else
    echo "skip - compressed-only default-profile leg (no dcmcjpls on PATH)"
  fi
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
  require_listen $PORT
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  check "remote delivers to the destination"  test -n "$(ls -A "$WORK/pacs1")"
  check "remote dequeues after delivery"      test -z "$(ls -A "$DICOMQ_SPOOL/route/PACS1/todo")"
  check "remote moves the delivered object to complete/" \
        test -f "$DICOMQ_SPOOL/route/PACS1/complete/$ID.dcm"
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
  require_listen $PORT
  RERR=$("$BIN/dicomq-remote" PACS1 2>&1)
  check "transcode never demotes to retry/1"  test -f "$DICOMQ_SPOOL/route/PACS1/retry/1/$ID.dcm"
  check "demotion left todo empty"            test -z "$(ls -A "$DICOMQ_SPOOL/route/PACS1/todo")"
  check "demotion names the syntax problem"   grep -q "transcode is 'never'" <<<"$RERR"
  # the just-demoted object's mtime is ~now, so retry/1's backoff (~7 min) has
  # NOT elapsed: a second remote run must skip it. Every other retry test
  # age_out's first, so this is the only check the backoff gate actually works
  # — a re-attempt at the top rung would wrongly fail the message.
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  check "a not-yet-due retry object is skipped" \
        test -f "$DICOMQ_SPOOL/route/PACS1/retry/1/$ID.dcm"
  check "a not-yet-due retry object is not re-attempted (no early fail)" \
        test ! -e "$DICOMQ_SPOOL/route/PACS1/failed/$ID.dcm"
  # rejection at the top rung (no retry/2 dir) fails the message rather
  # than creating retry/2 — the ladder depth is the directories present
  age_out "$DICOMQ_SPOOL/route/PACS1/retry/1/$ID.dcm"
  RERR2=$("$BIN/dicomq-remote" PACS1 2>&1)
  check "rejection at the top rung fails it"   test -f "$DICOMQ_SPOOL/route/PACS1/failed/$ID.dcm"
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
  require_listen $PORT
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

  # an unreadable queued object is quarantined to corrupt/, not retried as if
  # it were deliverable (gatherDueWork reads each object's file meta first)
  new_spool
  mkdir -p "$DICOMQ_SPOOL/dest/PACS1" "$DICOMQ_SPOOL/route/PACS1/todo"
  printf "host: localhost\nport: $PORT\naet: PACS1\n" > "$DICOMQ_SPOOL/dest/PACS1/remote"
  GARBAGE=20200101000000000.9.000000
  echo "not a dicom file" > "$DICOMQ_SPOOL/route/PACS1/todo/$GARBAGE.dcm"
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  check "an unreadable queued object is quarantined to corrupt/" \
        test -f "$DICOMQ_SPOOL/route/PACS1/corrupt/$GARBAGE.dcm"
  check "quarantine empties todo"             test -z "$(ls -A "$DICOMQ_SPOOL/route/PACS1/todo")"

  # demotion COPIES to a private inode (its mtime is the backoff clock) so a
  # co-queued hardlink at another destination keeps an independent clock. Fan
  # out one object to two destinations, reject at PACS1 (hold PACS2), and check
  # the demoted copy does NOT share PACS2's still-queued inode (nlinks stays 1).
  new_spool
  mkdir -p "$DICOMQ_SPOOL/aet/FAN2"/{tmp,new} \
           "$DICOMQ_SPOOL/dest/PACS1" "$DICOMQ_SPOOL/route/PACS1/todo" \
           "$DICOMQ_SPOOL/route/PACS1/retry/1" \
           "$DICOMQ_SPOOL/dest/PACS2" "$DICOMQ_SPOOL/route/PACS2/todo"
  printf "host: localhost\nport: $PORT\naet: PACS1\n" > "$DICOMQ_SPOOL/dest/PACS1/remote"
  printf "host: localhost\nport: $PORT\naet: PACS2\n" > "$DICOMQ_SPOOL/dest/PACS2/remote"
  printf 'ImplicitVRLittleEndian\ntranscode: never\n' > "$DICOMQ_SPOOL/dest/PACS1/propose"
  printf 'forward PACS1\nforward PACS2\n' > "$DICOMQ_SPOOL/aet/FAN2/deliver"
  touch "$DICOMQ_SPOOL/route/PACS1/hold" "$DICOMQ_SPOOL/route/PACS2/hold"
  ID=$("$BIN/dicomq-inject" -c FAN2 "$WORK/test.dcm")
  "$BIN/dicomq-send" --once 2>/dev/null
  rm "$DICOMQ_SPOOL/route/PACS1/hold"   # leave PACS2 held so its copy stays put
  check "fan-out shares one inode across destinations" \
        test "$(nlinks "$DICOMQ_SPOOL/route/PACS2/todo/$ID.dcm")" = 2
  storescp -od "$WORK/pacs1" $PORT 2>/dev/null & SCP=$!
  require_listen $PORT
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null
  check "rejected fan-out object demotes to PACS1 retry/1" \
        test -f "$DICOMQ_SPOOL/route/PACS1/retry/1/$ID.dcm"
  check "demotion copies to a private inode (not sharing PACS2's)" \
        test "$(nlinks "$DICOMQ_SPOOL/route/PACS1/retry/1/$ID.dcm")" = 1
  check "the co-queued PACS2 copy is undisturbed" \
        test -f "$DICOMQ_SPOOL/route/PACS2/todo/$ID.dcm"

  # dicomq-send honours the dead-site backoff: a status with a future
  # next-attempt-after suppresses triggering dicomq-remote even with due work;
  # once it elapses, delivery resumes.
  new_spool
  mkdir -p "$DICOMQ_SPOOL/aet/BO"/{tmp,new} "$DICOMQ_SPOOL/dest/PACS1" \
           "$DICOMQ_SPOOL/route/PACS1/todo" "$WORK/bopacs"
  printf "host: localhost\nport: $PORT\naet: PACS1\n" > "$DICOMQ_SPOOL/dest/PACS1/remote"
  printf 'forward PACS1\n' > "$DICOMQ_SPOOL/aet/BO/deliver"
  ID=$("$BIN/dicomq-inject" -c BO "$WORK/test.dcm")
  printf 'failures: 1\nnext-attempt-after: 2999-01-01T00:00:00Z\n' > "$DICOMQ_SPOOL/route/PACS1/status"
  storescp -od "$WORK/bopacs" $PORT 2>/dev/null & SCP=$!
  require_listen $PORT
  "$BIN/dicomq-send" --once 2>/dev/null
  check "send routes despite the backoff"     test -f "$DICOMQ_SPOOL/route/PACS1/todo/$ID.dcm"
  check "send does not deliver while backed off" test -z "$(ls -A "$WORK/bopacs")"
  printf 'failures: 1\nnext-attempt-after: 2000-01-01T00:00:00Z\n' > "$DICOMQ_SPOOL/route/PACS1/status"
  "$BIN/dicomq-send" --once 2>/dev/null
  check "send delivers once the backoff has elapsed" test -n "$(ls -A "$WORK/bopacs")"
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
  require_listen $TPORT
  check "recv accepts a TLS store" \
        storescu +tla +cf "$CERTS/ca.pem" -aet MOD1 -aec ARCHIVE localhost $TPORT "$WORK/test.dcm"
  wait $RECV
  check "TLS store is queued" test -n "$(find "$DICOMQ_SPOOL/queue/todo" -name '*.dcm')"

  "$BIN/dicomq-recv" --listen $TPORT --once --tls 2>/dev/null & RECV=$!
  require_listen $TPORT
  check_not "plaintext client cannot store to a TLS listener" \
        storescu -aet MOD1 -aec ARCHIVE localhost $TPORT "$WORK/test.dcm"
  kill $RECV 2>/dev/null; wait $RECV 2>/dev/null

  # ca.pem in <spool>/tls switches on client-certificate verification:
  # an anonymous client must be refused, a CA-signed one accepted
  openssl req -newkey rsa:2048 -keyout "$CERTS/cli.key" -out "$CERTS/cli.csr" \
      -nodes -subj "/CN=dicomq-test-client" 2>/dev/null
  openssl x509 -req -in "$CERTS/cli.csr" -CA "$CERTS/ca.pem" -CAkey "$CERTS/ca.key" \
      -days 1 -out "$CERTS/cli.pem" 2>/dev/null
  cp "$CERTS/ca.pem" "$DICOMQ_SPOOL/tls/ca.pem"
  "$BIN/dicomq-recv" --listen $TPORT --once --tls 2>/dev/null & RECV=$!
  require_listen $TPORT
  check_not "recv with ca.pem refuses an anonymous client" \
        storescu +tla +cf "$CERTS/ca.pem" -aet MOD1 -aec ARCHIVE localhost $TPORT "$WORK/test.dcm"
  kill $RECV 2>/dev/null; wait $RECV 2>/dev/null
  "$BIN/dicomq-recv" --listen $TPORT --once --tls 2>/dev/null & RECV=$!
  require_listen $TPORT
  check "recv with ca.pem accepts a CA-signed client certificate" \
        storescu +tls "$CERTS/cli.key" "$CERTS/cli.pem" +cf "$CERTS/ca.pem" \
                 -aet MOD1 -aec ARCHIVE localhost $TPORT "$WORK/test.dcm"
  wait $RECV

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
  require_listen $TPORT
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  check "remote delivers over TLS"            test -n "$(ls -A "$WORK/tlspacs")"
  check "TLS route queue drained"             test -z "$(ls -A "$DICOMQ_SPOOL/route/PACS1/todo")"
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null

  # remote must reject a server whose certificate does not chain to the
  # destination's ca.pem — a wrong-CA peer is a connection failure
  # (dead-site backoff), never a delivery
  openssl req -x509 -newkey rsa:2048 -keyout "$CERTS/rogue.key" -out "$CERTS/rogue.pem" \
      -days 1 -nodes -subj "/CN=localhost" 2>/dev/null
  ID=$("$BIN/dicomq-inject" -c FWD "$WORK/test.dcm")
  touch "$DICOMQ_SPOOL/route/PACS1/hold"
  "$BIN/dicomq-send" --once 2>/dev/null
  rm "$DICOMQ_SPOOL/route/PACS1/hold"
  storescp +tls "$CERTS/rogue.key" "$CERTS/rogue.pem" -ic -od "$WORK/tlspacs" $TPORT 2>/dev/null & SCP=$!
  require_listen $TPORT
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null
  check "remote rejects a server cert from the wrong CA" \
        test -f "$DICOMQ_SPOOL/route/PACS1/todo/$ID.dcm"
  check "a wrong-CA peer counts as a connection failure (backoff)" \
        test -f "$DICOMQ_SPOOL/route/PACS1/status"

  # a half-configured outbound identity (cert.pem, no key.pem) is a
  # config error — never a silent anonymous connection
  rm -f "$DICOMQ_SPOOL/route/PACS1/status"
  cp "$CERTS/cli.pem" "$DICOMQ_SPOOL/dest/PACS1/tls/cert.pem"
  storescp +tls "$CERTS/srv.key" "$CERTS/srv.pem" -ic -od "$WORK/tlspacs" $TPORT 2>/dev/null & SCP=$!
  require_listen $TPORT
  RERR=$("$BIN/dicomq-remote" PACS1 2>&1 >/dev/null)
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null
  check "a lone cert.pem refuses to connect (no silent anonymous TLS)" \
        test -f "$DICOMQ_SPOOL/route/PACS1/todo/$ID.dcm"
  check "the half-configured identity is named in the error" \
        grep -q "key.pem" <<<"$RERR"

  # with the pair complete, remote authenticates to a server that
  # requires a client certificate and delivers
  cp "$CERTS/cli.key" "$DICOMQ_SPOOL/dest/PACS1/tls/key.pem"
  rm -f "$DICOMQ_SPOOL/route/PACS1/status"
  storescp +tls "$CERTS/srv.key" "$CERTS/srv.pem" +cf "$CERTS/ca.pem" -rc \
           -od "$WORK/tlspacs" $TPORT 2>/dev/null & SCP=$!
  require_listen $TPORT
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null
  check "remote authenticates with a client certificate" \
        test ! -e "$DICOMQ_SPOOL/route/PACS1/todo/$ID.dcm"
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
  require_listen $RPORT; require_listen $PPORT
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

# --- study-mode: recv accumulates -> send seals -> remote one batch --------
# A study/series AET (aet/<AET>/group) collects objects in accum/<AET>/<UID>/
# until quiescent, then dicomq-send seals the directory as one batch message
# that dicomq-remote delivers over a single association. See docs/study-mode.md.
if command -v storescu >/dev/null && command -v storescp >/dev/null; then
  SRPORT=11187 SPPORT=11188
  SUID=1_2_276_0_7230010_3_1_2_42_1     # sanitized StudyInstanceUID (.->_)
  ndcm() { ls "$1"/*.dcm 2>/dev/null | wc -l | tr -d '[:space:]'; }
  new_spool
  mkdir -p "$DICOMQ_SPOOL/aet/STUDYR"/{tmp,new} "$DICOMQ_SPOOL/dest/PACS1" \
           "$DICOMQ_SPOOL/route/PACS1/todo" "$WORK/studypacs"
  printf "host: localhost\nport: $SPPORT\naet: PACS1\n" > "$DICOMQ_SPOOL/dest/PACS1/remote"
  printf 'maildir ./\nforward PACS1\n' > "$DICOMQ_SPOOL/aet/STUDYR/deliver"
  # large T: natural quiescence never fires mid-test; we force it with age_out
  printf 'study 3600\n' > "$DICOMQ_SPOOL/aet/STUDYR/group"

  # both objects of one study arrive over a single association
  "$BIN/dicomq-recv" --listen $SRPORT --once 2>/dev/null & RECV=$!
  require_listen $SRPORT
  check "study-mode: recv accepts the study" \
        storescu -aet MOD1 -aec STUDYR localhost $SRPORT "$WORK/test.dcm" "$WORK/test2.dcm"
  wait $RECV
  ACC="$DICOMQ_SPOOL/accum/STUDYR/$SUID"
  check "study-mode: objects accumulate under accum/<AET>/<UID>/" \
        test "$(ndcm "$ACC")" = 2
  check "study-mode: nothing reaches queue/todo before sealing" \
        test -z "$(find "$DICOMQ_SPOOL/queue/todo" -name '*.dcm')"

  # not yet quiescent (T=3600): a send pass must not seal
  "$BIN/dicomq-send" --once 2>/dev/null
  check "study-mode: send does not seal before quiescence" test -d "$ACC"

  # force quiescence; hold the destination so the sealed batch can be inspected
  age_out "$ACC"
  touch "$DICOMQ_SPOOL/route/PACS1/hold"
  "$BIN/dicomq-send" --once 2>/dev/null
  BATCH=$(ls "$DICOMQ_SPOOL/route/PACS1/todo" 2>/dev/null | head -1)
  check "study-mode: sealing consumes the accumulation dir" test ! -e "$ACC"
  check "study-mode: the sealed batch is a directory message" \
        test -n "$BATCH" -a -d "$DICOMQ_SPOOL/route/PACS1/todo/$BATCH"
  check "study-mode: the batch holds the whole study" \
        test "$(ndcm "$DICOMQ_SPOOL/route/PACS1/todo/$BATCH")" = 2
  check "study-mode: maildir delivery is an atomic new/<id>/ subdir" \
        test -d "$DICOMQ_SPOOL/aet/STUDYR/new/$BATCH"
  check "study-mode: the maildir batch holds the whole study" \
        test "$(ndcm "$DICOMQ_SPOOL/aet/STUDYR/new/$BATCH")" = 2
  check "study-mode: no staging left in the maildir tmp/" \
        test -z "$(ls -A "$DICOMQ_SPOOL/aet/STUDYR/tmp")"
  check "study-mode: queue/todo drained after routing" \
        test -z "$(find "$DICOMQ_SPOOL/queue/todo" -name '*.dcm')"
  check "study-mode: batch fan-out leaves no trash residue" \
        test -z "$(ls -A "$DICOMQ_SPOOL/trash" 2>/dev/null)"

  # deliver the held batch: one association carries the whole study
  storescp -od "$WORK/studypacs" $SPPORT 2>/dev/null & SCP=$!
  require_listen $SPPORT
  rm "$DICOMQ_SPOOL/route/PACS1/hold"
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  check "study-mode: remote delivers the whole study" \
        test "$(ls -A "$WORK/studypacs" | wc -l | tr -d '[:space:]')" = 2
  check "study-mode: remote drains the batch" \
        test -z "$(ls -A "$DICOMQ_SPOOL/route/PACS1/todo")"
  check "study-mode: delivered batch lands in complete/" \
        test -d "$DICOMQ_SPOOL/route/PACS1/complete/$BATCH"
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null

  # a straggler for the same study starts a fresh batch — no name collision
  # with the already-shipped one (sealed batches are timestamped, not UIDs)
  "$BIN/dicomq-recv" --listen $SRPORT --once 2>/dev/null & RECV=$!
  require_listen $SRPORT
  storescu -aet MOD1 -aec STUDYR localhost $SRPORT "$WORK/test.dcm" >/dev/null 2>&1
  wait $RECV
  check "study-mode: a straggler starts a new accumulation" \
        test "$(ndcm "$ACC")" = 1

  # all-or-nothing: a rejected batch demotes as a whole. Fresh spool, a
  # two-object study, and a destination policy (implicit-only, transcode
  # never) that rejects the stored explicit objects.
  new_spool
  mkdir -p "$DICOMQ_SPOOL/aet/STUDYR"/{tmp,new} "$DICOMQ_SPOOL/dest/PACS1" \
           "$DICOMQ_SPOOL/route/PACS1/todo" "$DICOMQ_SPOOL/route/PACS1/retry/1" \
           "$WORK/studypacs2"
  printf "host: localhost\nport: $SPPORT\naet: PACS1\n" > "$DICOMQ_SPOOL/dest/PACS1/remote"
  printf 'forward PACS1\n' > "$DICOMQ_SPOOL/aet/STUDYR/deliver"
  printf 'study 3600\n' > "$DICOMQ_SPOOL/aet/STUDYR/group"
  printf 'ImplicitVRLittleEndian\ntranscode: never\n' > "$DICOMQ_SPOOL/dest/PACS1/propose"
  "$BIN/dicomq-recv" --listen $SRPORT --once 2>/dev/null & RECV=$!
  require_listen $SRPORT
  storescu -aet MOD1 -aec STUDYR localhost $SRPORT "$WORK/test.dcm" "$WORK/test2.dcm" >/dev/null 2>&1
  wait $RECV
  age_out "$DICOMQ_SPOOL/accum/STUDYR/$SUID"
  touch "$DICOMQ_SPOOL/route/PACS1/hold"
  "$BIN/dicomq-send" --once 2>/dev/null
  rm "$DICOMQ_SPOOL/route/PACS1/hold"
  storescp -od "$WORK/studypacs2" $SPPORT 2>/dev/null & SCP=$!
  require_listen $SPPORT
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null
  DBATCH=$(ls "$DICOMQ_SPOOL/route/PACS1/retry/1" 2>/dev/null | head -1)
  check "study-mode: a rejected batch demotes as a whole to retry/1" \
        test -n "$DBATCH" -a "$(ndcm "$DICOMQ_SPOOL/route/PACS1/retry/1/$DBATCH")" = 2
  check "study-mode: demotion empties todo" \
        test -z "$(ls -A "$DICOMQ_SPOOL/route/PACS1/todo")"
  check "study-mode: batch demote leaves no trash residue" \
        test -z "$(ls -A "$DICOMQ_SPOOL/trash" 2>/dev/null)"

  # queue + ctl treat the batch (now in retry/1) as one message
  check "study-mode: queue lists a batch with its object count" \
        grep -q "$DBATCH.*batch: 2 objects" <<<"$("$BIN/dicomq-queue" PACS1)"
  "$BIN/dicomq-ctl" hold "$DBATCH" >/dev/null
  check "study-mode: ctl holds a batch directory" \
        test -d "$DICOMQ_SPOOL/hold/route/PACS1/retry/1/$DBATCH"
  check "study-mode: hold removes the batch from its origin" \
        test ! -e "$DICOMQ_SPOOL/route/PACS1/retry/1/$DBATCH"
  "$BIN/dicomq-ctl" release "$DBATCH" >/dev/null
  check "study-mode: ctl releases a batch to its origin" \
        test -d "$DICOMQ_SPOOL/route/PACS1/retry/1/$DBATCH"
  "$BIN/dicomq-ctl" requeue "$DBATCH" >/dev/null
  check "study-mode: ctl requeues a batch to its AET queue" \
        test -d "$DICOMQ_SPOOL/queue/todo/STUDYR/$DBATCH"
  "$BIN/dicomq-ctl" fail "$DBATCH" "operator says no" >/dev/null
  check "study-mode: ctl fails a batch" test -d "$DICOMQ_SPOOL/failed/$DBATCH"

  # sink collision: a crash between a demotion's link-tree and its discard
  # leaves a batch on two rungs, and the second copy's arrival at a sink
  # finds the first already there (a directory rename cannot clobber). The
  # collision must count as sinked — discarding the source — not strand the
  # batch to be re-forwarded forever.
  mkdir -p "$DICOMQ_SPOOL/route/PACS1/todo/$DBATCH" \
           "$DICOMQ_SPOOL/route/PACS1/complete/$DBATCH"
  ln "$DICOMQ_SPOOL/failed/$DBATCH/"*.dcm "$DICOMQ_SPOOL/route/PACS1/todo/$DBATCH/"
  ln "$DICOMQ_SPOOL/failed/$DBATCH/"*.dcm "$DICOMQ_SPOOL/route/PACS1/complete/$DBATCH/"
  printf 'ExplicitVRLittleEndian\ntranscode: never\n' > "$DICOMQ_SPOOL/dest/PACS1/propose"
  storescp -od "$WORK/studypacs2" $SPPORT 2>/dev/null & SCP=$!
  require_listen $SPPORT
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null
  check "study-mode: a sink collision dequeues the delivered batch" \
        test ! -e "$DICOMQ_SPOOL/route/PACS1/todo/$DBATCH"
  check "study-mode: the resident sink copy survives the collision" \
        test "$(ndcm "$DICOMQ_SPOOL/route/PACS1/complete/$DBATCH")" = 2
  check "study-mode: the collision leaves no trash residue" \
        test -z "$(ls -A "$DICOMQ_SPOOL/trash" 2>/dev/null)"

  # a batch link-tree is staged under a dot-name and published atomically:
  # a stage (a fan-out or demotion a crash interrupted) must be invisible
  # to the queue walkers — a half-linked tree delivered as a message would
  # be a shrunken study — and clean reaps it once aged
  STAGE="$DICOMQ_SPOOL/route/PACS1/todo/.20990101000000000.1.000000.stage"
  mkdir -p "$STAGE"
  ln "$DICOMQ_SPOOL/failed/$DBATCH/"*.dcm "$STAGE/"
  check "study-mode: a staged tree is invisible to dicomq-queue" \
        sh -c "! '$BIN/dicomq-queue' PACS1 | grep -q stage"
  storescp -od "$WORK/studypacs2" $SPPORT 2>/dev/null & SCP=$!
  require_listen $SPPORT
  "$BIN/dicomq-remote" PACS1 2>/dev/null
  kill $SCP 2>/dev/null; wait $SCP 2>/dev/null
  check "study-mode: a staged tree is not forwarded" \
        test -d "$STAGE" -a ! -e "$DICOMQ_SPOOL/route/PACS1/complete/.20990101000000000.1.000000.stage"
  "$BIN/dicomq-clean" >/dev/null
  check "study-mode: clean spares a fresh stage" test -d "$STAGE"
  age_out "$STAGE"
  "$BIN/dicomq-clean" >/dev/null
  check "study-mode: clean reaps an aged stage" test ! -e "$STAGE"

  # a grouping AET refuses an object with no StudyInstanceUID
  new_spool
  mkdir -p "$DICOMQ_SPOOL/aet/STUDYR"/{tmp,new}
  printf 'study 3600\n' > "$DICOMQ_SPOOL/aet/STUDYR/group"
  printf 'forward PACS1\n' > "$DICOMQ_SPOOL/aet/STUDYR/deliver"
  DENO="$WORK/nostudy.dump"
  grep -v '0020,000d' "$WORK/test.dump" > "$DENO"
  dump2dcm +te "$DENO" "$WORK/nostudy.dcm" 2>/dev/null
  "$BIN/dicomq-recv" --listen $SRPORT --once 2>/dev/null & RECV=$!
  require_listen $SRPORT
  check_not "study-mode: object with no grouping UID is refused" \
        storescu -aet MOD1 -aec STUDYR localhost $SRPORT "$WORK/nostudy.dcm"
  kill $RECV 2>/dev/null; wait $RECV 2>/dev/null
else
  echo "skip - study-mode test (needs storescu and storescp)"
fi

echo
echo "$PASS passed, $FAIL failed"
test "$FAIL" -eq 0
