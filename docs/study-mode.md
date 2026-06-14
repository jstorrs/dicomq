# dicomq — study-mode / series-mode (design note)

**Status: implemented (June 2026).** The full path is built and covered
by the integration suite: `dicomq-recv` accumulates, `dicomq-send` seals
on quiescence and routes, `dicomq-remote` delivers each batch over one
association (all-or-nothing), `dicomq-local` delivers a batch atomically
as `new/<id>/`, and `dicomq-queue` / `dicomq-ctl` show and operate on
batches. The config knob is `aet/<AET>/group` (see "Configuration").
Deferred by design (not yet needed): a max-age cap and per-AET overrides
of a global default — see "Deliberately deferred".

## What it is

An optional receive mode in which `dicomq-recv` does not commit each
object as its own message. Instead it accumulates the objects of a study
(or a series) into a directory, and when that directory has been quiet
for a configured interval it seals the whole directory as a single
message. The unit of commit, routing, retry, fan-out, and delivery
becomes a *directory of objects* instead of one `.dcm` file. A study (or
series) then appears downstream atomically — the whole batch arrives, or
none of it does.

Two granularities, identical machinery, different grouping key:

- **study-mode** groups on `StudyInstanceUID (0020,000D)`.
- **series-mode** groups on `SeriesInstanceUID (0020,000E)`.

## Configuration

A per-AET file `aet/<AET>/group` turns the mode on, matching the
`accept`/`deliver` convention (existence/contents are the configuration).
One line, `<mode> <seconds>`:

```
study 120          # accumulate by StudyInstanceUID, seal after 120s quiet
# or: series 90    # accumulate by SeriesInstanceUID, seal after 90s quiet
```

Absent ⇒ per-object delivery, the historical behaviour. Both `dicomq-recv`
(to bucket by the grouping UID) and `dicomq-send` (to read the timeout T)
read this file. A grouping AET's `deliver` file works as usual: `forward`
sends the whole batch over one association, and `maildir` delivers it as
an atomic `new/<id>/` subdirectory. An object that lacks the grouping tag
is refused at receive time (it cannot be routed), like a SOP-class
mismatch.

## Why this reverses an earlier "no"

`DESIGN.md` ("What is pruned from storescp") lists `--rename-on-eostudy`
/ `--eostudy-timeout` as **gone**, on the grounds that "end of study is
not an event a per-object receiver can know", and assigns study grouping
to the consumer. Both statements stay true. What changed is the goal.

This mode does **not** claim to detect end-of-study. It detects
*quiescence* — "no new object for this key in T seconds" — which is a
heuristic, not an event. And it does not move grouping into the routing
core: `dicomq-send` still opens no DICOM and routes purely by directory.
The grouping happens at the edge, in `dicomq-recv`, which already parses
each object. The feature is justified not by "knowing" a study is
complete but by a concrete delivery property (atomic batches, one
association per study) that some consumers want and that is awkward to
reconstruct downstream.

## The honest semantic: atomic *batch*, not atomic *study*

DICOM gives no reliable "study complete" signal — a study may arrive over
several associations, from several modalities, minutes or hours apart; a
PACS may re-push priors late. A quiescence timeout therefore seals "the
study so far", not "the whole study". Objects that arrive after a seal
form a **second batch** for the same UID.

This is principle 5 ("at-least-once, never at-most-once") one level up:
the guarantee is **at-least-one-batch (possibly more)**, and each batch
is delivered atomically. As with single-object at-least-once, the
consumer reconciles — it files by Study/Series UID and deduplicates on
SOP Instance UID, so a straggler batch merges into the same study on its
side. dicomq does not try to be exactly-one-batch-per-study; that is not
knowable.

## Naming: UID only while accumulating (decided 2026-06)

The Study/Series UID names the directory **only while it accumulates** —
it is the rendezvous point that lets concurrent objects of the same study
coalesce. At seal, the directory is renamed to an ordinary timestamped
message id, exactly like a single-object message
(`YYYYMMDDHHMMSSMMM.<pid>.<counter>`, here the seal time). From that
moment the batch is an opaque message; the UID is no longer in its name.

This is what dissolves the straggler problem. Because committed batches
are never named by UID, two batches of the same study are simply two
messages — there is no name collision between a shipped batch and a fresh
straggler batch, and the queue never has to merge anything. The UID-keyed
directory exists at exactly one place (the accumulation stage) for
exactly one reason (rendezvous).

## Spool layout

A single new staging stage in front of `queue/todo/`; everything
downstream is unchanged.

```
accum/
  <CALLEDAET>/
    <UID>/             # objects of one study/series, accumulating
                       # the directory's mtime is the quiescence clock
queue/
  todo/
    <CALLEDAET>/
      <id>/            # a sealed batch: a directory message (was <id>.dcm)
route/
  <DEST>/
    todo/
      <id>/            # hardlink-tree of a batch, awaiting first send
    retry/<k>/
      <id>/            # a demoted batch, backing off
failed/
  <id>/                # a batch that exhausted its retry ladder
```

`accum/` is dicomq's to create and write (like `queue/`), not operator
configuration. A directory message is just a message whose leaf is a
directory rather than a file; `route/`, `retry/<k>/`, `failed/`, `hold/`,
`dicomq-ctl`, and `dicomq-queue` treat it as one opaque unit.

## Quiescence: reset-on-arrival (decided 2026-06)

The clock is the accumulation directory's mtime. On POSIX, renaming an
object into `accum/<AET>/<UID>/` bumps that directory's mtime, so each
arrival **resets** the timer. The batch is due to seal when

```
now − mtime(accum/<AET>/<UID>)  ≥  T
```

This is deliberately the same shape as the retry ladder's due-ness test
(`now − mtime(<id>) ≥ retryBackoff(rung)`), so the sealing sweep is the
retry sweep with a different threshold — no new mechanism, just another
directory swept by mtime.

`T` is a single global interval in the first cut (e.g. 120 s).

## Seal and commit: one atomic rename

Sealing and committing are the **same event** — a single rename does both
jobs:

```
rename( accum/<AET>/<UID>  →  queue/todo/<AET>/<id> )
```

The instant it returns, the UID name is free (the next object for that
study recreates `accum/<AET>/<UID>/` and starts the next batch) and the
batch is durably committed in `queue/todo/`. No intermediate "sealing"
directory is needed: its only purpose would have been to free the UID
name before committing, and the rename already frees it atomically.

### Why the batch boundary is race-free

The sweep reads mtime at *t*, decides quiet, and renames at *t+ε*. An
object arriving in that window lands in exactly one batch:

- it commits into `accum/<AET>/<UID>/` **before** the rename → it rides in
  this batch; or
- its commit lands **after** the rename → the directory is gone, the
  rename fails `ENOENT`, recv recreates `accum/<AET>/<UID>/` and retries →
  it starts the next batch.

No object is lost or misfiled at the boundary; a borderline object is
just in batch N or batch N+1, which is precisely the straggler semantics
already accepted. The one piece of logic this requires is in
`dicomq-recv`: the per-object commit (write `accum/.../tmp`, rename into
`accum/<AET>/<UID>/`) retries on `ENOENT` by re-creating the directory.
This is a bounded loop in the same spirit as the existing `tmp → todo`
commit.

## How a directory message rides the existing machinery

Nothing downstream of `queue/todo/` is new; it inherits the rules in
`DESIGN.md` with "directory" substituted for "file".

- **Fan-out (`dicomq-send`).** A file is hardlinked into each
  `route/<DEST>/todo/`; a batch is hardlink-tree'd (`cp -al`) instead. The
  new directory gets a fresh, private mtime (its own backoff clock) while
  the objects share inodes — pixels are stored once even when a study
  fans out to several destinations.
- **Retry demotion (`dicomq-remote`).** Same `cp -al` to a fresh rung
  directory: a private directory mtime for the backoff clock, shared
  object inodes for the data. The existing "copy-on-demote keeps backoff
  independent" rationale extends unchanged; only the leaf is bigger.
- **Delivery (`dicomq-remote`).** A study maps naturally to **one
  association**: open one association to the destination and C-STORE every
  object in the batch directory over it, amortizing association setup and
  presentation-context negotiation across the whole study. Commit is
  **all-or-nothing**: if the association drops partway, the whole batch
  demotes one rung and re-sends; the destination deduplicates the objects
  it already received on SOP Instance UID. Per-object progress *within* a
  batch is deliberately not tracked — it would reintroduce the per-object
  state the directory model removes, and dedup makes the re-send cheap.
- **Surgery (`dicomq-ctl`) and inspection (`dicomq-queue`).** Hold,
  release, requeue, and fail operate on the batch directory as the unit.

## A consequence: UID is not in the queued name

Once sealed, the message is named by timestamp, so the Study/Series UID is
no longer visible structurally. This is intentional and consistent with
"DICOM at the edge, structural core": `dicomq-send`/`dicomq-remote` never
need it for routing. Inspection tools that want to show "which study is
this batch" do a cheap meta-only read of one object in the directory —
the same exception `dicomq-queue`/`dicomq-ctl` already make to recover
AETs. Encoding a UID hash into the id was considered and rejected: it
muddies the message id for an ops convenience obtainable by reading a
file.

## What this subsumes

This mode generalises — and makes unnecessary — the hand-rolled
study-buffering a content filter would otherwise need. A filter that must
make one decision per study (for example: forward most studies to a
viewer, but divert specific ones identified only after a particular series
is seen) would, against a per-object queue, have to buffer a study's
objects, keep sticky per-UID state, and run its own quiescence timeout —
which *is* study-mode aggregation, re-implemented per filter.

With study-mode, that filter becomes stateless: it receives the whole
study as a directory, inspects every object at once, decides once, and
renames the directory to the chosen pathway. The "wait for a series or
time out" logic lives in dicomq, written once, instead of in every filter
that ever needs it.

## Cost

Every study incurs at least `T` of added latency before it ships, since
the batch cannot seal until it has been quiet for `T`. This is fine for
forward/archive workflows and wrong for STAT / real-time reads. It is the
price of atomic-batch semantics and should be a per-deployment choice
(which is also why the mode is opt-in, not the default).

## Deliberately deferred

Recorded so adding them later is a planned step. Neither changes the data
model — each is a read-time refinement.

- **Max-age cap.** Reset-on-arrival alone never seals a study that drips a
  new object every `< T` forever. In practice modalities burst and go
  quiet, so this has not been needed. Adding it is a second clause in the
  sweep's due-ness test (`quiet for T` **or** `older than M since first
  arrival`) — no new directories, ids, or consumer impact. Whether the cap
  is measured from first arrival, and whether it is global or per-AET, is
  left open until a real case appears.
- **Per-AET settings.** A single global `T` (and grouping mode) to start;
  per-AET overrides are a config read-time refinement when needed.

## Relationship to `DESIGN.md`

`DESIGN.md` documents this mode: `accum/` is in the spool layout, the
"Messages" section covers the batch (directory) form, the Receive commit
protocol notes the `accum/` path, "Routing instructions" has a
"Study/series accumulation: `aet/<AET>/group`" subsection, and the
storescp pruning table rows for `--rename-on-eostudy` /
`--eostudy-timeout` (and `--exec-on-eostudy`) now point at this mode
rather than calling it "gone". End-of-study is still not detected;
quiescence batching is an opt-in receive mode for consumers that want
atomic studies/series.
