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
| `dicomq-clean` | `qmail-clean` | reap orphaned temporary files |
| `dicomq-inject` | `qmail-inject` | enqueue a local DICOM file as if it had been received |
| `dicomq-queue` | `postqueue`/`qshape` | show what is queued where: counts, ages, destination status (read-only) |
| `dicomq-super` | `postsuper` | queue surgery: hold, release, requeue, fail |

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
`-s` flag on every program). All paths below are relative to the root.

```
queue/
  tmp/                 # in-progress writes; never read by consumers
  todo/                # committed inbound messages awaiting routing
route/
  <DEST>/
    todo/              # objects queued for destination <DEST>
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
failed/                # terminal failures: object + annotated envelope
hold/                  # operator-frozen messages (via dicomq-super)
corrupt/               # quarantined malformed messages
```

dicomq never creates `aet/`, `dest/`, or `route/<DEST>/` entries —
creating them **is** configuration, done by the operator. `queue/`,
`failed/`, `hold/`, `corrupt/`, route `status` files, and the contents
of maildirs are dicomq's to write.

Directory names under `aet/` are *sanitized* AE titles: trimmed,
alphanumerics and `-` kept, everything else replaced by `_`, empty
becomes `_`. `.` is always replaced because it separates filename
fields. Sanitized names `tmp`, `new`, `todo` are reserved and refused.
The unsanitized AET always travels in the envelope; sanitization affects
paths only.

## Messages

A message is a pair of files sharing an id:

```
<id>.dcm   the DICOM object, exactly as it will be delivered
<id>.env   the envelope: routing metadata, one "key: value" per line
```

`<id>` is `YYYYMMDDHHMMSSMMM.<pid>.<counter>` — unique per host by
construction, lexically ordered by receive time within a process.

Envelope keys are lowercase `[a-z0-9-]`; values are single-line UTF-8.
Keys may repeat (used for `attempt`). Initial fields, written by the
receiver:

```
id: 20260612174231313.376213.000000
received: 2026-06-12T17:42:31.313Z
peer: 192.0.2.10
calling-aet: MOD1
called-aet: ARCHIVE
sop-class-uid: 1.2.840.10008.5.1.4.1.1.7
sop-instance-uid: 1.2.276.0.7230010.3.1.4.123456.1
transfer-syntax-uid: 1.2.840.10008.1.2.1
```

Immutability rules, which make hardlink fan-out safe:

- **`.dcm` files are immutable** from the moment they are committed.
  They are *hardlinked* between queues (fan-out costs no copies; the
  link count is the garbage collector — when the last queue unlinks,
  the data is gone).
- **`.env` files in `queue/todo/` are immutable.** Each routing target
  gets its own *copy* of the envelope, which the delivery agent may
  annotate (`attempt:`, `failed:` lines) and whose mtime carries retry
  state. Envelopes are small; copying them keeps mutation private.

Because ids are unique, every queue transition is idempotent:
`link(2)` returning `EEXIST` means "already done", so a routing pass
interrupted by a crash can simply run again.

### Why a sidecar, not the DICOM preamble (decided 2026-06)

The 128-byte preamble of a Part 10 file is application-defined and never
transmitted (C-STORE carries the dataset; the receiver constructs the
preamble and file meta itself), so embedding the envelope there is
tempting — one self-describing file, one rename to commit. Rejected:

- **It doesn't fit.** The three UIDs alone (SOP class, SOP instance,
  transfer syntax) can total 192 bytes before id, timestamp, peer, and
  AETs.
- **The envelope is mutable and grows; the preamble is fixed and
  shared.** Retry state appends `attempt:` lines and lives in the env
  copy's mtime. Inside the `.dcm`, an in-place write in one route queue
  would corrupt every hardlink of the object (and mtime is an inode
  attribute, also shared). The only crash-safe update would be
  copy-rewrite-rename of the whole object per attempt.
- **Inject can't claim the preamble.** Objects also enter via
  `dicomq-inject`, and some real files carry load-bearing preambles
  (dual TIFF/DICOM whole-slide images embed a TIFF header there).
  Preambles of injected files are preserved, never overwritten.
- **Delivered files would carry queue state into archives.** A sidecar
  can be kept or dropped by the consumer; bytes inside the object
  cannot.
- **Plain text is the debugging story.** `cat`/`grep` on `.env` files
  inspects queue state with no tools; a packed preamble needs a reader.

What `dicomq-recv` *does* write into the file is the standard-blessed
in-file metadata: file meta group tags `SourceApplicationEntityTitle
(0002,0016)`, `SendingApplicationEntityTitle (0002,0017)`, and
`ReceivingApplicationEntityTitle (0002,0018)`, stamped at receive time.
Delivered objects stay self-describing even if a consumer discards the
`.env`; the sidecar remains the routing/retry vehicle because that
state is per-queue and mutable by design. The preamble is written as
all zeros (the "unused" form) for received objects.

Postfix made the opposite choice from the sidecar, too — one binary
queue file per message holding envelope and content records, with
per-recipient delivery state updated *in place* — and it works there
because a queue file is never shared between queues, and recipients
share one small file. Our fan-out shares the multi-megabyte object
instead, and in-place record updates need write-ordering discipline
that `rename(2)` gives us for free. Same verdict as the preamble, for
the same reasons.

## Commit protocols

Every transition follows the same shape: write into `tmp/` (or link/copy
directly when the source is already durable), then `rename(2)` into the
visible directory, then fsync the directory. The `.env` file is always
committed **last**; its presence is the commit point. A `.dcm` without
its `.env` is an orphan, invisible to consumers, reaped by
`dicomq-clean` after a grace period (default 36 hours).

**Receive** (`dicomq-recv`, per object, before the C-STORE response):

0. precondition, checked at association time: free space on the spool
   filesystem is above a watermark (default 1 GiB; Postfix's
   `queue_minfree`). Below it the association is refused — fail toward
   the sender's retry queue, never toward `ENOSPC` mid-object, which is
   the one failure the commit protocol cannot make graceful
1. write `queue/tmp/<id>.dcm` — zeroed preamble, file meta stamped with
   (0002,0016/0017/0018) — and fsync
2. write `queue/tmp/<id>.env`, fsync
3. rename both into `queue/todo/` (`.dcm` first, `.env` last), fsync `todo/`
4. send C-STORE success

Any failure: remove both tmp files, answer with a refused status; the
sender keeps the object.

**Route** (`dicomq-send`, per message in `queue/todo/`):

1. read the envelope; look up `aet/<called-aet>/deliver`
2. for each `forward <DEST>` instruction: link `<id>.dcm` into
   `route/<DEST>/todo/`, copy `<id>.env` beside it (env committed last)
3. for each `maildir <dir>` instruction: invoke `dicomq-local`
4. when every instruction has been committed: unlink the message from
   `queue/todo/` (`.env` first, then `.dcm`)

A crash mid-routing re-routes the whole message on restart; step 2 is
idempotent and step 3 must be (see below).

A message whose envelope cannot be parsed is moved to `corrupt/`
(object first, envelope last) and logged, and routing continues with
the next message — a malformed message never blocks the queue and is
never deleted by software (Postfix's `corrupt/` queue). After
inspection the operator re-injects or removes it.

**Local delivery** (`dicomq-local`): link `<id>.dcm` into
`<dir>/new/` (`EEXIST` = already delivered = success). With the `env`
option, the envelope is copied to `<dir>/new/<id>.env` first, so the
object's appearance remains the commit point. A maildir on a different
filesystem (link gives `EXDEV`) is delivered by copy through the
maildir's own `tmp/` and committed by rename — which is what maildirs
have `tmp/` for. Consumers must treat delivered files as read-only —
they may share an inode with the spool — but may move or delete them
freely.

**Remote delivery** (`dicomq-remote <DEST>`): open one association to
the destination, proposing presentation contexts per its `propose`
profile, then C-STORE every due message in `route/<DEST>/todo/` over
that single association (objects for one destination batch naturally —
an improvement over SMTP's one-message channels). Per message: on
success, unlink (`.env` first, `.dcm` last); on failure, append an
`attempt:` line, rewrite-and-commit the envelope copy (its mtime is now
the last-attempt time), and leave it.

A *connection-level* failure (unreachable, refused, association
rejected) is destination state, not message state: `dicomq-remote`
records it in `route/<DEST>/status` (envelope format: `last-failure:`,
`next-attempt-after:`) and exits, and `dicomq-send` skips the whole
destination until that time without reading a single per-message
envelope — Postfix's dead-site backoff. Per-message `attempt:` records
are for objects a *reachable* destination rejected. A successful
association removes the status file.

**Failure** (`dicomq-remote`, when a message exhausts its queue
lifetime, or the destination rejects it permanently): link the `.dcm`
into `failed/`, write an annotated envelope copy (with a final
`failed:` reason line) beside it, then unlink from the route queue.
`failed/` is the bounce pile; alerting watches it.

**Hold and quarantine** (Postfix's `hold/` and `corrupt/` queues).
`dicomq-super hold <id>` moves a message from its queue into `hold/`
(same discipline: object first and envelope last on the way in,
envelope first on the way out; a `held-from:` line records where it
came from so `release` can return it). Touching `route/<DEST>/hold`
freezes a whole destination: `dicomq-send` stops triggering
`dicomq-remote <DEST>` until the flag is removed — a PACS migration is
`touch`, migrate, `rm`. Nothing in `hold/`, `corrupt/`, or `failed/`
is ever deleted by dicomq; leaving those directories is the operator's
decision.

## Routing instructions: `aet/<AET>/deliver`

One instruction per line, executed in order, all must succeed:

```
# deliver into this AET's own maildir (the default when no file exists)
maildir ./

# deliver into an arbitrary maildir-style directory, with envelope
maildir /export/research/incoming env

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
aet/INBOUND/deliver:      maildir /var/filter/work env

(the filter watches work/new/, processes each object, then:)
dicomq-inject -c FILTERED result.dcm
```

The queue core never executes user code; a crashed filter loses nothing
because both sides are ordinary queue transitions; and the filter's
output is a first-class message with its own envelope, retry schedule,
and failure handling.

## Transfer syntax profiles

Both halves hang off the directory they describe.

**Inbound — `aet/<AET>/accept`.** Read by `dicomq-recv` when the
A-ASSOCIATE-RQ arrives (the called AET is known before presentation
contexts are accepted). Lines are transfer syntax UIDs or DCMTK names in
preference order; first line of `*` means accept-all with proposer's
preference. No file ⇒ a compiled-in default (uncompressed preferred,
all standard syntaxes accepted). Example:

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
  count as delivery failures (and eventually land in `failed/`)
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

## Retry and the queue lifetime

qmail's scheme, kept for the same reasons (no counters needed, state is
the file itself): a message in `route/<DEST>/todo/` is *due* when

```
now − mtime(<id>.env)  ≥  backoff(age)
```

where `age` is `now − receive time` (from the id) and `backoff` grows
quadratically from ~7 minutes toward a cap (~6 hours). After the queue
lifetime (default 7 days) the message is failed as above. `dicomq-send`
triggers `dicomq-remote <DEST>` when any of its messages are due, or
when new messages arrive; one `dicomq-remote` runs per destination at a
time (per-channel serialization, like qmail's).

Three Postfix lessons bound the cost of this scheme:

- **Destination-level backoff.** Per-message mtimes alone mean a down
  destination is reconnected once per due cohort per scan cycle. The
  `route/<DEST>/status` file (see Remote delivery) makes "PACS1 is down
  until 14:32" one file read instead of N envelope stats.
- **Scan cost is O(backlog).** Deciding due-ness stats every envelope
  in every route queue each cycle. That is fine to roughly 10⁴ queued
  messages; beyond it, the known fix is Postfix's active/deferred
  split — a `deferred/` sibling per destination holding not-yet-due
  messages, so the scheduler scans only a bounded active set. Recorded
  here so the v1 simplification is a decision, not an accident.
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
- `dicomq-send` is the one long-running process. It watches
  `queue/todo/` (inotify, plus a periodic scan as backstop) and owns all
  routing decisions.
- `dicomq-local` and `dicomq-remote` are short-lived children of
  `dicomq-send`, also runnable by hand against the spool — which is the
  debugging story: every stage can be re-run from the shell.
- `dicomq-clean` runs from a timer/cron.
- `dicomq-queue` is read-only and safe for any user with read access to
  the spool; `dicomq-super` performs queue surgery and runs as the
  send user.
- Two users suffice: one for recv (may write only `queue/`), one for
  send/local/remote/clean (everything else). The receiver compromise
  blast radius is "can enqueue objects", nothing more.

## What is pruned from storescp, and what replaces it

| storescp feature | replacement |
|---|---|
| `--exec-on-reception`, `--exec-on-eostudy`, `--exec-sync` | consumers watch maildir `new/`; transforms use filter re-injection (see "Filters"); study grouping is the consumer's concern (the queue is per-object by design) |
| `--sort-conc-studies`, `--sort-on-*`, `--timenames`, `--unique-filenames` | spool naming + per-AET maildirs |
| `--rename-on-eostudy`, `--eostudy-timeout` | gone; "end of study" is not an event a per-object receiver can know |
| `--fork` / `--single-process` / `--inetd` | socket supervisor, one process per association |
| `--prefer-*` (20 options) | `aet/<AET>/accept` profiles |
| `--config-file` association profiles | same |
| `--bit-preserving`, `--ignore`, `--abort-*`, `--sleep-*`, `--refuse` | test-harness features; out of scope |
| output post-processing (`--write-xfer-*`, padding, group length…) | objects are stored as received; conversion happens at forwarding time per destination profile |
| AE title in filename | envelope file |

## Transition

`storescp+` (the patched DCMTK storescp in this repository) remains the
production receiver while dicomq grows. Its `--imagedir` mode is the
prototype of `dicomq-recv` + default-maildir delivery, and its delivery
invariants (fsync→rename→fsync-dir before ack, refuse on failure) carry
over verbatim. The expected order of work:

1. `common/` spool primitives + `dicomq-inject` + `dicomq-clean`
   (testable without any DICOM networking)
2. `dicomq-send` + `dicomq-local` (the queue core, still no networking)
3. `dicomq-queue` + `dicomq-super` (operability before the network: the
   queue core is testable and inspectable from the shell)
4. `dicomq-recv` (DCMTK association handling; replaces storescp+)
5. `dicomq-remote` + transcoding profiles (new capability)

## Open questions

- Should `accept` profiles also constrain SOP classes (a promiscuous
  flag per AET)?
- Envelope `attempt:` history: cap the number of recorded attempts?
- Multi-host spools are out of scope; is single-writer-per-queue worth
  asserting with `O_EXCL` lockfiles anyway, to catch operator error
  (two dicomq-sends)?
