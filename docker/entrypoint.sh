#!/bin/sh
# Seed the spool, then exec the requested dicomq program.
#
# dicomq never creates configuration directories — creating them IS the
# configuration (DESIGN.md "Configuration is directories") — so this
# entrypoint plays operator: it builds the spool skeleton, accepts one
# called AE title, and (optionally) points its deliveries at a maildir.
# Everything is idempotent and existing files are left alone, so hand
# edits inside the spool volume survive restarts.
#
#   DICOMQ_SPOOL        spool root        (default /var/spool/dicomq)
#   DICOMQ_AET          called AE title accepted for C-STORE
#                                         (default ARCHIVE)
#   DICOMQ_DELIVER_DIR  absolute maildir to deliver into; created with
#                       tmp/ and new/ if missing. Empty = deliver into
#                       the AET's own maildir inside the spool.
set -eu

: "${DICOMQ_SPOOL:=/var/spool/dicomq}"
: "${DICOMQ_AET:=ARCHIVE}"
: "${DICOMQ_DELIVER_DIR:=}"

mkdir -p "$DICOMQ_SPOOL/queue/tmp" "$DICOMQ_SPOOL/queue/todo" \
         "$DICOMQ_SPOOL/accum" "$DICOMQ_SPOOL/route" "$DICOMQ_SPOOL/aet" \
         "$DICOMQ_SPOOL/dest" "$DICOMQ_SPOOL/failed" "$DICOMQ_SPOOL/hold"

# accept the called AET; its tmp/ and new/ make it a deliverable maildir
mkdir -p "$DICOMQ_SPOOL/aet/$DICOMQ_AET/tmp" \
         "$DICOMQ_SPOOL/aet/$DICOMQ_AET/new"

# route the AET's deliveries at an external maildir (a bind mount, say);
# an existing deliver file — an operator's routing — always wins
if [ -n "$DICOMQ_DELIVER_DIR" ]; then
  mkdir -p "$DICOMQ_DELIVER_DIR/tmp" "$DICOMQ_DELIVER_DIR/new"
  deliver="$DICOMQ_SPOOL/aet/$DICOMQ_AET/deliver"
  [ -e "$deliver" ] || printf 'maildir %s\n' "$DICOMQ_DELIVER_DIR" > "$deliver"
fi

exec "$@"
