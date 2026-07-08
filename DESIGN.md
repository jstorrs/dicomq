# dicomq — a qmail-inspired DICOM store-and-forward suite

`dicomq` receives DICOM objects over C-STORE, queues them durably on the
filesystem, and delivers them — into local directories or onward to other
DICOM nodes — the way qmail moves mail. This document is the design
contract; the programs implement it.

## Principles

These are qmail's, translated:

1. **Deliver first, acknowledge second.** A C-STORE success response is
   sent only after the object is durable (written, fsynced, renamed into
   place, directory entry fsynced). An object is never acknowledged and
   lost, and never delivered when the response reports failure.
2. **The filesystem is the database.** Queue state is files and
   directories; transitions are `rename(2)` and `link(2)`; recovery after
   a crash is "start the programs again". There are no locks, no
   daemons that must be running for the on-disk state to be valid, and
   no state files that can disagree with reality.
3. **Small programs, one job each.** The programs that speak DICOM
   (receiver, forwarder) are separate from the program that routes
   (queue runner), which is separate from delivery. Each can be run,
   tested, and replaced by hand.
4. **Configuration is directories.** An AE title is accepted because a
   directory for it exists. A destination exists because its directory
   does. Routing instructions are lines in a file, like `.qmail`.
5. **At-least-once, never at-most-once.** A crash at the wrong moment
   duplicates an object; it never loses one. Consumers that need
   exactly-once deduplicate on SOP Instance UID.

Non-goals: indexing, query/retrieve, worklists, storage commitment, a
database of any kind. dicomq moves objects; it does not manage them.

The suite is POSIX-only. It depends on `rename(2)` and `link(2)`
semantics within one filesystem; the spool must not span filesystems and
must not be on NFS.

## Components

| program | analog | job |
|---|---|---|
| `dicomq-recv` | `qmail-smtpd` | accept one association, write objects into the queue, ack |
| `dicomq-send` | `qmail-send` | watch the queue, route each object per its called AET |
| `dicomq-local` | `qmail-local` | deliver one object into a maildir-style directory |
| `dicomq-remote` | `qmail-remote` | forward queued objects to one destination over C-STORE |
| `dicomq-clean` | `qmail-clean` | reap interrupted `queue/tmp/` writes, aged `route/<DEST>/complete/` deliveries, and `trash/` |
| `dicomq-inject` | `qmail-inject` | enqueue a local DICOM file as if it had been received |
| `dicomq-queue` | `postqueue`/`qshape` | show what is queued where: counts, ages, destination status (read-only) |
| `dicomq-ctl` | `postsuper` | queue surgery: hold, release, requeue, fail |

The last two are Postfix's contribution: qmail's queue was famously
opaque, and Postfix won operators with inspection and surgery tools.
"What is queued for PACS1, how old, and why" must be one command, not a
shell pipeline invented mid-incident.

Dataflow:

```
            ┌────────────┐   queue/todo    ┌─────────────┐
 C-STORE →  │ dicomq-recv│ ──────────────▶ │ dicomq-send │
            └────────────┘                 └──────┬──────┘
                                  ┌───────────────┼───────────────┐
                                  ▼               ▼               ▼
                           ┌────────────┐  route/<DEST>/   ┌────────────┐
                           │dicomq-local│  ┌────────────┐  │   failed/  │
                           └─────┬──────┘  │dicomq-remote│ └────────────┘
                                 ▼         └─────┬──────┘
                          aet/<AET>/...          ▼
                          (maildir new/)     remote SCP (C-STORE)
```

## Spool layout

Default root `/var/spool/dicomq`, overridable with `$DICOMQ_SPOOL` (and a
`-s` flag on every program — a no-op on the internal `dicomq-local`, which
its caller invokes with absolute paths). All paths below are relative to
the root.

```
queue/
  tmp/                 # in-progress writes; never read by consumers
  todo/
    <CALLEDAET>/       # committed inbound messages, keyed by called AET —
                       # each an <id>.dcm object or a sealed <id>/ batch
accum/                 # study/series-mode only (docs/study-mode.md)
  <CALLEDAET>/
    <UID>/             # objects of one study/series accumulating, keyed by
                       # Study/SeriesInstanceUID, until dicomq-send seals
                       # the directory into queue/todo/<AET>/<id>/
route/
  <DEST>/
    todo/              # objects queued for <DEST>, never attempted
    retry/<k>/         # objects rejected k times, awaiting their k-th backoff
    complete/          # forwarded OK; auto-reaped by dicomq-clean (-G window)
    failed/            # terminal forwarding failure (reason is in the log)
    corrupt/           # quarantined: unreadable when this <DEST> went to send
    status             # optional: destination-level backoff (dicomq-remote)
    hold               # optional flag: operator froze this destination
aet/
  <CALLEDAET>/         # existence ⇒ this called AE title is accepted
    accept             # optional: inbound transfer syntax profile
    deliver            # optional: routing instructions (default: maildir ./)
    tmp/  new/         # default local maildir (when delivering to ./)
dest/
  <DEST>/              # existence ⇒ this forwarding destination exists
    remote             # host, port, AET of the remote SCP
    propose            # optional: outbound transfer syntax profile
failed/                # pre-routing failures only: dicomq-send could not
                       # route (unknown called AET / no instruction), and
                       # dicomq-ctl fail. Forwarding failures live per-DEST.
hold/                  # operator-frozen messages, origin mirrored as a subpath
trash/                 # batches renamed here to be deleted; reaped by clean
```

The per-destination `complete/`, `failed/`, and `corrupt/` are **per
destination on purpose**: under a multi-`forward` fan-out the same object is
hardlinked into each destination's queue, so a single shared sink would
alias — two same-inode hardlinks rename to a POSIX no-op, leaving the source
behind for that destination to re-process forever (a directory batch fails
the second move with `ENOTEMPTY` instead). Scoping each sink under its
`route/<DEST>/` removes the aliasing and records *which* destination produced
each outcome. `complete/` is the only auto-expiring one; `failed/` and
`corrupt/` are operator-managed, like the global `failed/`.

dicomq never creates `aet/`, `dest/`, `route/<DEST>/`, or `retry/<k>/`
entries — creating them **is** configuration, done by the operator (the
retry-ladder depth is exactly which `retry/<k>/` dirs exist). The per-AET
`queue/todo/<AET>/` leaves, the per-destination `complete/`, `failed/`, and
`corrupt/` sinks (created on first use), the `accum/<AET>/<UID>/`
accumulation directories, the global `failed/`, `hold/`, `trash/`, route
`status` files, and the contents of maildirs are dicomq's to write.

Directory names under `aet/` (and `queue/todo/`) are *sanitized* AE
titles: trimmed, alphanumerics and `-` kept, everything else replaced by
`_`, empty becomes `_`. `.` is always replaced because it separates
filename fields. Sanitized names `tmp`, `new`, `todo` are reserved and
refused. The unsanitized AET travels in the object's file-meta header
(0002,0016/0018); sanitization affects paths only.

## Messages

A message is a single file:

```
<id>.dcm   the DICOM object, exactly as it will be delivered
```

`<id>` is `YYYYMMDDHHMMSSMMM.<pid>.<counter>` — unique per host by
construction, lexically ordered by receive time within a process. The
receive time is recoverable from the id; everything else routing needs
is in the object's file-meta header (see below). There is no sidecar.

In study/series mode a message is instead a **batch** — a directory
`<id>/` of `<objid>.dcm` objects that `dicomq-recv` accumulated and
`dicomq-send` sealed (docs/study-mode.md). It flows through the same
queues and transitions as a single object — the only difference is that
the leaf is a directory, so a move is a directory `rename(2)` (still
atomic) and fan-out / copy-on-demote hardlink-tree the directory: the new
directory's own mtime is the private retry-backoff clock while its members
share inodes. `dicomq-remote` delivers a batch over one association,
all-or-nothing.

Two things make a message self-describing without one:

- **The directory is the routing/retry state.** The called AET is the
  `queue/todo/<AET>/` subdirectory the object sits in; the retry depth is
  the `route/<DEST>/retry/<k>/` rung. State that used to be mutable
  envelope lines is now *where the file is*, changed by `rename(2)`.
- **The file-meta header is the denormalized metadata.** A Part 10 file
  already carries `MediaStorageSOPClassUID (0002,0002)`,
  `MediaStorageSOPInstanceUID (0002,0003)`, and `TransferSyntaxUID
  (0002,0010)`; `dicomq-recv`/`dicomq-inject` additionally stamp
  `SourceApplicationEntityTitle (0002,0016)`,
  `SendingApplicationEntityTitle (0002,0017)`, and
  `ReceivingApplicationEntityTitle (0002,0018)`. `dicomq-remote` reads
  the three UIDs to plan presentation contexts; the AET tags keep a
  delivered object self-describing in the archive.

Immutability and fan-out:

- **`.dcm` files are immutable** from the moment they are committed.
  They are *hardlinked* between queues (fan-out costs no copies; the
  link count is the garbage collector — when the last queue unlinks, the
  data is gone).
- **Retry demotion copies to a private inode.** When `dicomq-remote`
  moves a rejected object down a retry rung, it *copies* rather than
  hardlink-renames, because the rung's mtime is the backoff clock and the
  same object may be hardlinked into other destinations' queues —
  independent inodes keep each destination's backoff independent. This is
  the one place a queued object is duplicated, and only for a message
  that is actively failing.

Because ids are unique, every queue transition is idempotent: a single
`rename(2)` is atomic (the object is in exactly one directory at every
instant), and `link(2)` returning `EEXIST` means "already done", so a
routing pass interrupted by a crash can simply run again.

### Where message metadata lives (decided 2026-06)

An earlier design carried queue state in a per-message `<id>.env`
sidecar. It was removed: the sidecar was redundant with the DICOM
file-meta header for the immutable fields, and its mutable fields
(`attempt:` lines, an `mtime` retry clock) forced a fragile two-file
commit ("object first, envelope last; its presence is the commit point")
through every transition. The single-file design collapses that to one
atomic rename.

Two homes were considered for the data the sidecar held and rejected in
favour of "the file meta plus the directory":

- **The DICOM preamble.** The 128-byte preamble is application-defined
  and never transmitted, so a packed binary struct there is tempting —
  but it is fixed-size and *shared across hardlinks*, so it cannot hold
  per-destination mutable state, and `dicomq-inject` must preserve
  load-bearing preambles (dual TIFF/DICOM whole-slide images embed a TIFF
  header there). Queue state stays out of the object entirely.
- **A private DICOM group.** Conformant, but a private group in the
  *dataset* would be transmitted to the destination PACS, and mutating it
  per attempt would rewrite the shared object. The directory carries the
  one thing that actually changes (retry depth) for free.

The fields the sidecar used to denormalize are not lost, only relocated:
SOP/transfer-syntax UIDs and AETs are read from the file-meta header; the
receive time is the id; the retry count is the rung. The sending peer's
IP — the only field with no DICOM home — is logged at receive time
rather than stored (source-IP validation, if ever wanted, belongs in a
recv-side calling-AET config table, not in per-message state). Every
failure *reason* is logged, not stored: the folder number says how many
times, the log says why.

Postfix keeps one binary queue file per message with per-recipient
delivery state updated *in place*, which works because its queue file is
never shared between queues and recipients share one small file. Our
fan-out shares the multi-megabyte object instead, so we keep mutable
state in the cheap, unshared thing — the directory entry — and let
`rename(2)` give us the write ordering for free.

## Commit protocols

A message is one file, so a transition is one atomic `rename(2)` (or a
`link(2)` for fan-out) followed by an fsync of the directory. The rename
**is** the commit point: the object is in exactly one queue at every
instant, never half-committed. There are no sidecar orphans; the only
crash residue is an un-renamed `queue/tmp/` write, reaped by
`dicomq-clean` after a grace period (default 36 hours).

**Receive** (`dicomq-recv`, per object, before the C-STORE response):

0. precondition, checked at association time: free space on the spool
   filesystem is above a watermark (default 1 GiB; Postfix's
   `queue_minfree`). Below it the association is refused — fail toward
   the sender's retry queue, never toward `ENOSPC` mid-object, which is
   the one failure the commit protocol cannot make graceful
1. write `queue/tmp/<id>.dcm` — zeroed preamble, file meta stamped with
   (0002,0016/0017/0018) — and fsync
2. rename it into `queue/todo/<called-aet>/` (creating that directory if
   needed), then fsync the directory
3. send C-STORE success

Any failure: remove the tmp file, answer with a refused status; the
sender keeps the object. In study/series mode (`aet/<AET>/group`) step 2
instead renames into `accum/<called-aet>/<UID>/`, recreating that
directory and retrying if `dicomq-send` sealed it between the `mkdir` and
the rename (docs/study-mode.md); an object lacking the grouping UID is
refused, like a SOP-class mismatch.

**Route** (`dicomq-send`, per object in `queue/todo/<AET>/`, opening no
DICOM — the called AET is the subdirectory name):

1. look up `aet/<AET>/deliver`
2. for each `forward <DEST>` instruction: `link` the object into
   `route/<DEST>/todo/` (`EEXIST` = already routed = success)
3. for each `maildir <dir>` instruction: invoke `dicomq-local`. A
   *temporary* failure (exit 111 — missing maildir, transient I/O) defers
   the whole message in place; a *permanent* failure (exit 100 — a
   different object already occupies the `new/<id>` slot, which a unique
   id makes a real collision rather than a replay) escalates it to
   `failed/`, mirroring the unknown-AET path, rather than re-attempting
   it every scan
4. when every instruction has committed: discard the object from
   `queue/todo/<AET>/` — `unlink` for a single object, but for a batch a
   rename of the whole directory into `trash/` followed by a delete, so the
   dequeue is one atomic step (an in-place recursive delete a crash could
   leave half-done would re-deliver a shrunken study). `dicomq-remote` uses
   the same discard when it copies a rejected batch up to the next retry
   rung.

A crash mid-routing re-routes the whole message on restart; steps 2–3
are idempotent. An object whose AET has no `aet/` directory (only
possible for a hand-placed file — recv validates the AET before
queueing) is moved to `failed/` and logged. `dicomq-send` never opens
the object, so it has no `corrupt/` path: quarantine of an unreadable
object belongs to the programs that do parse it (`dicomq-remote`,
`dicomq-inject`).

**Local delivery** (`dicomq-local <id> <dir> <srcdir>`): link the object
from `<srcdir>` into `<dir>/new/` (`EEXIST` = already delivered =
success). A maildir on a different filesystem (link gives `EXDEV`) is
delivered by copy through the maildir's own `tmp/` and committed by
rename — which is what maildirs have `tmp/` for. Consumers must treat
delivered files as read-only — they may share an inode with the spool —
but may move or delete them freely. Exit codes follow qmail-local: `0`
delivered, `111` temporary (the caller leaves the message queued), `100`
permanent (the caller escalates it to `failed/`).

**Remote delivery** (`dicomq-remote <DEST>`): open one association to the
destination, proposing presentation contexts per its `propose` profile
(the SOP class and transfer syntax of each due object are read from its
file-meta header), then C-STORE every due message in `route/<DEST>/todo/`
and the `retry/<k>/` rungs over that single association (objects for one
destination batch naturally — an improvement over SMTP's one-message
channels). Per message: on success, unlink it; on a rejection by the
reachable destination, demote it one retry rung (see "Retry" below). A
permanent impossibility (no accepted context, transcode forbidden or
unavailable) demotes too — a config fix between attempts can still rescue
it, and rung N is the backstop. The one exception is a single message that
on its own needs more presentation contexts than an association can hold
(many SOP classes × proposed syntaxes): no later batch is smaller than one
message, so deferring would livelock — that structural case fails straight
to `route/<DEST>/failed/` rather than demoting.

A *connection-level* failure (unreachable, refused, association rejected)
is destination state, not message state: `dicomq-remote` records it in
`route/<DEST>/status` (a small key/value file: `last-failure:`,
`failures:`, `next-attempt-after:`) and exits without moving any object,
and `dicomq-send` skips the whole destination until that time — Postfix's
dead-site backoff. A successful association removes the status file.

**Success and failure** (`dicomq-remote`). A delivered object is renamed
into `route/<DEST>/complete/` — not deleted in place — so the dequeue is one
atomic rename (a whole-directory rename for a batch, which a non-atomic
recursive delete could leave half-undone and re-send). `complete/` is a
recently-delivered audit/recovery window that `dicomq-clean` reaps on age.
When a reachable destination rejects an object already at the last retry
rung, it is renamed into `route/<DEST>/failed/` instead; the reason is
logged, not stored. These sinks (and `corrupt/`) are **per destination**
because under fan-out the same object is hardlinked into several
destinations' queues — a shared sink would alias and strand the source (see
"Spool layout"). The per-`<DEST>` `failed/` is the bounce pile; alerting
watches it.

**Hold and quarantine** (Postfix's `hold/` and `corrupt/` queues).
`dicomq-ctl hold <id>` renames a message into `hold/`, mirroring its
origin as a subpath (`hold/route/PACS1/retry/2/<id>.dcm`) so `release`
recovers the origin from the path — no sidecar records it. `requeue`
reads the called AET from the object's file meta to return it to
`queue/todo/<AET>/`. Touching `route/<DEST>/hold` freezes a whole
destination: `dicomq-send` stops triggering `dicomq-remote <DEST>` until
the flag is removed — a PACS migration is `touch`, migrate, `rm`. Nothing
in `hold/`, the per-`<DEST>` `failed/`/`corrupt/`, or the global `failed/`
is ever deleted by dicomq; leaving those directories is the operator's
decision. (`route/<DEST>/complete/` is the exception — delivered messages
there auto-expire on age, see `dicomq-clean`.)

## Routing instructions: `aet/<AET>/deliver`

One instruction per line, executed in order, all must succeed:

```
# deliver into this AET's own maildir (the default when no file exists)
maildir ./

# deliver into an arbitrary maildir-style directory
maildir /export/research/incoming

# forward to configured destinations
forward PACS1
forward OFFSITE
```

`maildir` paths are relative to the `aet/<AET>/` directory unless
absolute; the target must contain `new/` (and dicomq never creates it).
This file is the `.qmail` analog: fan-out is adding a line, a new
receiving AET is `mkdir aet/NEWAET/{tmp,new}`.

An association addressed to a called AET with no `aet/` directory is
**rejected at association time** — the unknown-recipient error happens
at "RCPT TO", not as a later bounce.

### Filters re-inject; nothing runs inline

The replacement for storescp's `--exec-on-*` — and the answer to any
future "transform objects in flight" request — is Postfix's
content-filter pattern: deliver to the filter, let it process, and have
it hand the result back to the queue as a new submission.

```
aet/INBOUND/deliver:      maildir /var/filter/work

(the filter watches work/new/, processes each object, then:)
dicomq-inject -c FILTERED result.dcm
```

The queue core never executes user code; a crashed filter loses nothing
because both sides are ordinary queue transitions; and the filter's
output is a first-class message with its own retry schedule and failure
handling.

### Study/series accumulation: `aet/<AET>/group`

By default a message is one object. An optional per-AET `group` file opts
that called AET into delivering a whole study or series atomically:

```
study 120          # accumulate by StudyInstanceUID, seal after 120s quiet
# or: series 90    # accumulate by SeriesInstanceUID, seal after 90s quiet
```

`dicomq-recv` then writes each object into `accum/<AET>/<UID>/` keyed by
the grouping UID instead of committing it per-object; `dicomq-send` seals
a directory once it has been quiet for the timeout (its mtime — the last
arrival — is the clock) with one atomic rename into
`queue/todo/<AET>/<id>/`, after which it routes like any message. This is
quiescence batching, not end-of-study detection: a late object simply
starts the next batch, and consumers reconcile on Study/Series and SOP
Instance UID. The full rationale, the race-free seal, and the deferred
knobs (max-age cap, per-AET overrides) are in docs/study-mode.md.

## Transfer syntax profiles

Both halves hang off the directory they describe.

**Inbound — `aet/<AET>/accept`.** Read by `dicomq-recv` when the
A-ASSOCIATE-RQ arrives (the called AET is known before presentation
contexts are accepted). Lines are transfer syntax UIDs or DCMTK names in
preference order; first line of `*` means accept-all (the receiver still
chooses its preferred syntax among those proposed, compressed first). No
file ⇒ a compiled-in default (uncompressed preferred, all standard
syntaxes accepted). Example:

```
# archive prefers lossless compression, refuses lossy
JPEGLSLossless
JPEG2000LosslessOnly
ExplicitVRLittleEndian
ImplicitVRLittleEndian
```

**Outbound — `dest/<DEST>/propose`.** Read by `dicomq-remote`. The
syntaxes to propose, in preference order, plus a transcode policy for
when the stored object's syntax is not among what the destination
accepted:

```
ImplicitVRLittleEndian
transcode: lossless        # never | lossless | as-needed
```

- `never` — only send objects already in an accepted syntax; others
  count as delivery failures (and eventually land in `route/<DEST>/failed/`)
- `lossless` — transcode if a lossless path exists, else fail
- `as-needed` — transcode even lossily if that is the only option

Transcoding happens at delivery time and never modifies the queued
object: `dicomq-remote` transcodes into memory (or a tmp file) per
attempt. The queue always holds what was received.

**`dest/<DEST>/remote`** holds the connection parameters:

```
host: pacs1.example.org
port: 11112
aet: PACS1
calling-aet: DICOMQ        # optional; default compiled-in
```

## Retry: the numbered-folder ladder

Retry state is the directory, not a counter or a sidecar line. A
forwarded object starts in `route/<DEST>/todo/`; each rejection by a
reachable destination demotes it one rung — `todo → retry/1 → retry/2 →
…` — and `ls route/PACS1/retry/3/` is "every object that has failed PACS1
three times". This replaces qmail's queue lifetime (a count of attempts,
not a wall-clock age, bounds retries) and its `attempt:` envelope history
(the rung is the count; the log is the why).

The ladder depth is **configuration, not a flag**: dicomq never creates a
rung, so the operator sizes it by which `route/<DEST>/retry/<k>`
directories exist. With `retry/1`…`retry/5` present, a rejection at
`retry/5` moves the object to `route/<DEST>/failed/` rather than creating
`retry/6`;
with no `retry/` directory at all, the first rejection fails immediately
(retries are opt-in, per principle 4, "Configuration is directories").
`mkdir -p route/PACS1/retry/{1..8}` sets that destination's tolerance to
eight tries.

An object is *due* when

```
now − mtime(<id>.dcm)  ≥  retryBackoff(rung)
```

— an object in `todo/` (rung 0) is always due; an object in `retry/k` is
due `retryBackoff(k)` after it landed there. `retryBackoff` grows
quadratically with the rung from ~7 minutes toward a cap (~6 hours). The
demotion *copies* the object to a fresh inode (and its mtime is the
landing time), so the backoff clock is private even though the same
object may be hardlinked into other destinations' queues — without that,
one destination's retry would reset another's. `dicomq-send` triggers
`dicomq-remote <DEST>` when any object in its `todo/` or `retry/<k>/`
rungs is due, or when new objects arrive; one `dicomq-remote` runs per
destination at a time (per-channel serialization, like qmail's).

Three Postfix lessons bound the cost of this scheme:

- **Destination-level backoff.** Per-message mtimes alone mean a down
  destination is reconnected once per due cohort per scan cycle. The
  `route/<DEST>/status` file (see Remote delivery) makes "PACS1 is down
  until 14:32" one file read instead of N stats.
- **Scan cost is O(backlog).** Deciding due-ness stats every object in
  every route queue and rung each cycle. That is fine to roughly 10⁴
  queued messages; beyond it, the known fix is Postfix's active/deferred
  split — and the rung directories already are that shape, so a scheduler
  that scans only `todo/` plus the rungs whose backoff has elapsed is a
  natural extension. Recorded here so the v1 simplification is a
  decision, not an accident.
- **Outbound concurrency is uncapped.** A *single* destination is
  already treated gently: one `dicomq-remote` per destination at a time
  (per-channel serialization), and within its one association objects
  are C-STOREd serially — any one node sees a single, ordered stream,
  never a parallel hammering. What is unbounded is concurrency *across*
  destinations: each scan, `dicomq-send` forks one `dicomq-remote` for
  every destination with due work, so the number of simultaneous agents
  (and outbound associations) is bounded only by how many destinations
  are configured. With a handful of destinations this is fine, and the
  load it risks is on the *sending* host and its network, not on any
  remote node. Beyond that, the known fixes are Postfix's
  `default_destination_concurrency_limit` (a global cap on simultaneous
  `dicomq-remote` agents) and, for pacing a large recovered backlog into
  one fragile node, `default_destination_rate_delay` / a per-association
  batch cap. Deliberately omitted in v1 (YAGNI until the destination
  count grows); recorded here so adding it later is a planned step, not a
  scramble.

## Process and privilege model

- `dicomq-recv` runs **one process per association** under a socket
  supervisor — systemd socket activation (`Accept=yes`) or
  s6/ucspi-tcp. It inherits the connected socket, never listens, never
  forks. TLS terminates in recv (decided 2026-06: DICOM TLS profiles
  are negotiated in-protocol, so a TLS-stripping proxy would break
  conformance): `--tls` serves BCP 195 from `<spool>/tls/{key,cert}.pem`,
  and a `tls/ca.pem` switches on peer-certificate verification. The
  supervisor decides per listening socket whether to pass `--tls`.
  Outbound, `dest/<DEST>/tls/` existing makes `dicomq-remote` speak TLS
  to that destination (`ca.pem` verifies the server; optional
  `key.pem`/`cert.pem` authenticate us).
- `dicomq-send` is the one long-running process. It watches the
  `queue/todo/<AET>/` subdirectories (inotify on the parent for new AET
  dirs plus each subdir for arrivals, with a periodic scan as backstop)
  and owns all routing decisions. It opens no DICOM — routing is purely
  by directory.
- `dicomq-local` and `dicomq-remote` are short-lived children of
  `dicomq-send`, also runnable by hand against the spool — which is the
  debugging story: every stage can be re-run from the shell.
- `dicomq-clean` runs from a timer/cron. It reaps interrupted `queue/tmp/`
  writes (`-g`, default 36h), aged `route/<DEST>/complete/` deliveries
  (`-G`, default 72h; `-G 0` clears them each pass), and `trash/` (batches a
  discard renamed aside but a crash left undeleted; reaped unconditionally).
  Complete-age is the message-id timestamp, as everywhere else, not file
  mtime (a rename preserves mtime).
- `dicomq-queue` is read-only and safe for any user with read access to
  the spool; `dicomq-ctl` performs queue surgery and runs as the send
  user. Both do a cheap *meta-only* read of an object's file header to
  recover its AETs (queue listing) or called AET (ctl `requeue`) — the
  only DICOM the queue-side tools touch; the rest is pure filesystem.
- Two users suffice: one for recv (may write only `queue/`, plus `accum/`
  when any AET runs in study/series mode), one for send/local/remote/clean
  (everything else). The receiver compromise blast radius is "can enqueue
  objects", nothing more.

## What is pruned from storescp, and what replaces it

| storescp feature | replacement |
|---|---|
| `--exec-on-reception`, `--exec-on-eostudy`, `--exec-sync` | consumers watch maildir `new/`; transforms use filter re-injection (see "Filters"); study grouping, when wanted, is the opt-in study/series mode (`aet/<AET>/group`) rather than per-consumer logic |
| `--sort-conc-studies`, `--sort-on-*`, `--timenames`, `--unique-filenames` | spool naming + per-AET maildirs |
| `--rename-on-eostudy`, `--eostudy-timeout` | study/series mode (`aet/<AET>/group`, docs/study-mode.md): not end-of-study detection (unknowable to a receiver), but quiescence batching that delivers a whole study/series atomically |
| `--fork` / `--single-process` / `--inetd` | socket supervisor, one process per association |
| `--prefer-*` (20 options) | `aet/<AET>/accept` profiles |
| `--config-file` association profiles | same |
| `--bit-preserving`, `--ignore`, `--abort-*`, `--sleep-*`, `--refuse` | test-harness features; out of scope |
| output post-processing (`--write-xfer-*`, padding, group length…) | objects are stored as received; conversion happens at forwarding time per destination profile |
| AE title in filename | file-meta tags (0002,0016/0018) + the `queue/todo/<AET>/` path |

## Lineage

dicomq grew out of `storescp+`, a patched DCMTK `storescp` whose
`--imagedir` mode was the prototype of `dicomq-recv` + default-maildir
delivery. Its delivery invariants (fsync→rename→fsync-dir before ack,
refuse on failure) carry over verbatim into `dicomq-recv`. storescp+ has
since been removed; `dicomq-recv` is the receiver and all eight programs
are implemented and covered by the integration suite.

## Open questions

- Should `accept` profiles also constrain SOP classes (a promiscuous
  flag per AET)?
- Should `dicomq-recv` gain a calling-AET table (with optional source-IP
  validation), now that the peer IP is logged rather than stored?
- Multi-host spools are out of scope; is single-writer-per-queue worth
  asserting with `O_EXCL` lockfiles anyway, to catch operator error
  (two dicomq-sends)?
