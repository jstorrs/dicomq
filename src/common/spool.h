// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DICOMQ_SPOOL_H
#define DICOMQ_SPOOL_H

#include <ctime>
#include <string>
#include <utility>
#include <vector>

// Spool primitives shared by every dicomq program. See dicomq/DESIGN.md
// "Spool layout" and "Commit protocols". Nothing here knows about DICOM.

namespace dicomq {

struct KeyValueFile;

// Spool root from $DICOMQ_SPOOL, else the compiled-in default
// /var/spool/dicomq.
std::string spoolRoot();

// Well-known paths under one spool root.
struct Spool {
  std::string root;

  Spool() : root(spoolRoot()) {}
  explicit Spool(const std::string &r) : root(r.empty() ? spoolRoot() : r) {}

  std::string queueTmp() const { return root + "/queue/tmp"; }
  std::string queueTodo() const { return root + "/queue/todo"; }
  std::string queueTodoAET(const std::string &a) const {
    return root + "/queue/todo/" + a;
  }
  // Study/series accumulation stage: objects of one study/series gather
  // in accum/<AET>/<UID>/ (UID is the rendezvous key) until dicomq-send
  // seals the directory into queue/todo/<AET>/<id>/. dicomq's to create.
  std::string accumRoot() const { return root + "/accum"; }
  std::string accumAET(const std::string &a) const {
    return root + "/accum/" + a;
  }
  std::string accumGroup(const std::string &a, const std::string &uid) const {
    return accumAET(a) + "/" + uid;
  }
  std::string routeRoot() const { return root + "/route"; }
  std::string routeDir(const std::string &d) const {
    return root + "/route/" + d;
  }
  std::string routeTodo(const std::string &d) const {
    return routeDir(d) + "/todo";
  }
  std::string routeRetryRoot(const std::string &d) const {
    return routeDir(d) + "/retry";
  }
  std::string routeRetry(const std::string &d, int k) const {
    return routeDir(d) + "/retry/" + std::to_string(k);
  }
  std::string routeStatus(const std::string &d) const {
    return routeDir(d) + "/status";
  }
  // Per-destination terminal sinks for a forwarding message. They are
  // per-destination (not a single shared sink) so a fan-out object that ends
  // differently at two destinations never aliases in one shared sink —
  // same-inode hardlinks rename to a no-op, leaving the source stuck.
  // complete/ is auto-reaped by dicomq-clean; failed/ and corrupt/ are
  // operator-managed. dicomq creates each on first use.
  std::string routeComplete(const std::string &d) const {
    return routeDir(d) + "/complete";
  }
  std::string routeFailed(const std::string &d) const {
    return routeDir(d) + "/failed";
  }
  std::string routeCorrupt(const std::string &d) const {
    return routeDir(d) + "/corrupt";
  }
  std::string routeHoldFlag(const std::string &d) const {
    return routeDir(d) + "/hold";
  }
  std::string aetRoot() const { return root + "/aet"; }
  std::string aetDir(const std::string &a) const { return root + "/aet/" + a; }
  std::string destDir(const std::string &d) const {
    return root + "/dest/" + d;
  }
  // Global failed/ holds only pre-routing failures (dicomq-send: an unknown
  // called AET or no satisfiable instruction — no destination to attribute
  // them to) and dicomq-ctl's operator-initiated fail. Per-destination
  // forwarding failures live in routeFailed()/routeCorrupt() instead.
  std::string failedDir() const { return root + "/failed"; }
  std::string holdDir() const { return root + "/hold"; }
  // Staging area for discarding a superseded batch: it is renamed here
  // whole (one atomic step that dequeues it) and then deleted. dicomq's to
  // create and reap (dicomq-clean); no queue walker scans it.
  std::string trashDir() const { return root + "/trash"; }
};

// True if name carries the ".dcm" message suffix.
bool hasDcmSuffix(const std::string &name);

// Ids of committed messages in dir (entries ending ".dcm", suffix
// stripped), sorted — which is receive order. Missing dir = empty.
std::vector<std::string> listIds(const std::string &dir);

// Sorted subdirectory names of dir. Missing dir = empty.
std::vector<std::string> listSubdirs(const std::string &dir);

// The (directory, retry-level) pairs a destination's queue runner walks:
// route/<dest>/todo at level 0, then each existing route/<dest>/retry/<k>
// rung in ascending k. The rung-enumeration order lives here so that
// dicomq-send, dicomq-remote, and dicomq-queue all agree on it.
std::vector<std::pair<std::string, int>>
routeQueueDirs(const Spool &sp, const std::string &dest);

// Plain byte copy, no durability (callers publish with commitFile).
bool copyFile(const std::string &src, const std::string &dst, std::string &err);

// Receive time encoded in a message id, or 0 if unparseable.
time_t idTime(const std::string &id);

// "2026-06-12T17:42:31Z" <-> time_t (UTC). parseIsoTime returns 0 on
// failure (and tolerates a fractional-second field, which it ignores).
std::string isoTime(time_t t);
time_t parseIsoTime(const std::string &s);

// Free bytes on the filesystem holding path, or -1.
long long freeBytes(const std::string &path);

// Parse an integer that must consume the WHOLE token. Use for operator-facing
// numeric input (config values, CLI flags) rather than atoi/atol, which
// silently accept trailing garbage ("30x" -> 30, "36h" -> 36) and so mask
// typos. Returns false on empty, malformed, partial, or out-of-range input;
// the caller still range-checks the value (e.g. non-negative, 1..65535).
bool parseWholeInt(const std::string &s, long &out);

// Write "<prog>: <msg>\n" to stderr. The single place the diagnostic-line
// format lives; each program wraps it with its own name (dicomq-recv etc.).
void logmsg(const std::string &prog, const std::string &msg);

// Filesystem predicates: pathExists is any stat-able entry; isDir is a
// directory. Both report false (never an error) for a missing path.
bool pathExists(const std::string &path);
bool isDir(const std::string &path);

// qmail's quadratic retry schedule. retryBackoff(level) is the wait a
// message must sit at retry rung `level` before it is due again: the wait
// grows quadratically with the rung number (~BASE*level^2 seconds), capped
// at 6 hours. A message in todo/ (level 0) is always due.
long retryBackoff(int level);

// Message id: "<YYYYMMDDHHMMSSMMM>.<pid>.<counter>" (UTC). Unique per
// host by construction; lexically ordered by creation time within a
// process.
std::string generateId();

// AE title -> path component: trimmed, alphanumerics and '-' kept,
// everything else replaced by '_', empty becomes "_". '.' is never kept
// because it separates filename fields.
std::string sanitizeAET(const std::string &aet);

// Sanitized names that may never name an aet/ directory or delivery
// destination ("tmp", "new", "todo"), compared case-insensitively.
bool isReservedName(const std::string &name);

// Directory portion of path ("." if none, "/" for a root entry).
std::string dirOf(const std::string &path);

// mkdir(path) treating an existing *directory* as success, then fsync
// the parent so the new directory survives a crash. Fails if the path
// exists as a non-directory (a misconfigured spool). Intermediate
// parents must already exist (the spool skeleton is operator-created;
// this only fills in the per-AET and per-retry-rung leaves dicomq owns).
bool mkdirIfMissing(const std::string &path, std::string &err);

// Durably create `base` (if absent) and then every component of `rel` (a
// '/'-separated relative path) beneath it — each level via mkdirIfMissing,
// so parents are fsynced. base's own parent must already exist. Used to
// mirror a message's origin path under hold/ (and back). Reports a clear
// error instead of leaving a half-made tree for a later rename to trip over.
bool mkdirsUnder(const std::string &base, const std::string &rel,
                 std::string &err);

// fsync the file or directory at path. On failure returns false and
// sets err.
bool fsyncPath(const std::string &path, std::string &err);

// Durably publish tmpPath at finalPath: fsync(tmpPath), rename(2),
// fsync the containing directory. The commit point of every queue
// transition.
bool commitFile(const std::string &tmpPath, const std::string &finalPath,
                std::string &err);

// Outcome of linkOrSame: the link now exists (freshly made, or `to` was
// already the same inode as `from`); it would cross filesystems (the
// caller may copy instead); or it failed. err is set on both CrossDevice
// and Failed, so a caller that cannot copy can return false straight away.
enum class LinkOutcome { Ok, CrossDevice, Failed };

// link(2) from -> to with idempotent same-inode semantics, WITHOUT any
// fsync — callers that link many members fsync the directory once at the
// end. EEXIST is success only when `to` is already the same inode as
// `from`: ids are unique, so "already linked" means a previous (possibly
// crashed) pass did this work, while a different inode is an id collision
// or stale file and yields Failed. EXDEV yields CrossDevice so a caller
// spanning filesystems can fall back to a copy.
LinkOutcome linkOrSame(const std::string &from, const std::string &to,
                       std::string &err);

// linkOrSame plus an fsync of the destination directory on success.
// Cross-device links are an error here (route/ fan-out stays on one
// filesystem), reported with the message linkOrSame already recorded.
bool linkIdempotent(const std::string &from, const std::string &to,
                    std::string &err);

// Serialize kv into queue/tmp/ and commit it atomically at finalPath
// (replacing any existing file). Used for the dest/<DEST>/status
// dead-site cache. The tmp name carries the pid so concurrent writers
// cannot collide.
bool writeKeyValueCommitted(const Spool &sp, const KeyValueFile &kv,
                            const std::string &finalPath, std::string &err);

} // namespace dicomq

#endif // DICOMQ_SPOOL_H
