#!/bin/sh
# Install dicomq as per-user launchd agents (regular user, no root).
# Creates the spool under ~/Library/Application Support/dicomq, fills in
# the plists for this user, and loads the agents. Idempotent.
set -eu

SPOOL="$HOME/Library/Application Support/dicomq"
AGENTS="$HOME/Library/LaunchAgents"
HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$SPOOL/queue/tmp" "$SPOOL/queue/todo" \
         "$SPOOL/route" "$SPOOL/aet" "$SPOOL/dest" \
         "$SPOOL/failed" "$SPOOL/hold" "$SPOOL/corrupt"
mkdir -p "$AGENTS" "$HOME/Library/Logs"

for plist in org.dicomq.recv org.dicomq.send org.dicomq.clean; do
  sed "s|/Users/YOURNAME|$HOME|g" "$HERE/$plist.plist" > "$AGENTS/$plist.plist"
  launchctl unload "$AGENTS/$plist.plist" 2>/dev/null || true
  launchctl load "$AGENTS/$plist.plist"
done

echo "spool:  $SPOOL"
echo "agents: loaded from $AGENTS"
echo
echo "Next: accept a Called AE Title, e.g."
echo "  mkdir -p \"$SPOOL/aet/ARCHIVE/tmp\" \"$SPOOL/aet/ARCHIVE/new\""
echo "then store to localhost:11112 with called AET ARCHIVE."
