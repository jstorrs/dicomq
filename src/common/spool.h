// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DICOMQ_SPOOL_H
#define DICOMQ_SPOOL_H

#include <ctime>
#include <string>
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
  explicit Spool(const std::string& r) : root(r.empty() ? spoolRoot() : r) {}

  std::string queueTmp() const { return root + "/queue/tmp"; }
  std::string queueTodo() const { return root + "/queue/todo"; }
  std::string queueTodoAET(const std::string& a) const { return root + "/queue/todo/" + a; }
  // Study/series accumulation stage: objects of one study/series gather
  // in accum/<AET>/<UID>/ (UID is the rendezvous key) until dicomq-send
  // seals the directory into queue/todo/<AET>/<id>/. dicomq's to create.
  std::string accumRoot() const { return root + "/accum"; }
  std::string accumAET(const std::string& a) const { return root + "/accum/" + a; }
  std::string accumGroup(const std::string& a, const std::string& uid) const
      { return accumAET(a) + "/" + uid; }
  std::string routeRoot() const { return root + "/route"; }
  std::string routeDir(const std::string& d) const { return root + "/route/" + d; }
  std::string routeTodo(const std::string& d) const { return routeDir(d) + "/todo"; }
  std::string routeRetryRoot(const std::string& d) const { return routeDir(d) + "/retry"; }
  std::string routeRetry(const std::string& d, int k) const
      { return routeDir(d) + "/retry/" + std::to_string(k); }
  std::string routeStatus(const std::string& d) const { return routeDir(d) + "/status"; }
  std::string routeHoldFlag(const std::string& d) const { return routeDir(d) + "/hold"; }
  std::string aetRoot() const { return root + "/aet"; }
  std::string aetDir(const std::string& a) const { return root + "/aet/" + a; }
  std::string destDir(const std::string& d) const { return root + "/dest/" + d; }
  std::string failedDir() const { return root + "/failed"; }
  std::string holdDir() const { return root + "/hold"; }
  std::string corruptDir() const { return root + "/corrupt"; }
};

// Ids of committed messages in dir (entries ending ".dcm", suffix
// stripped), sorted — which is receive order. Missing dir = empty.
std::vector<std::string> listIds(const std::string& dir);

// Sorted subdirectory names of dir. Missing dir = empty.
std::vector<std::string> listSubdirs(const std::string& dir);

// Plain byte copy, no durability (callers publish with commitFile).
bool copyFile(const std::string& src, const std::string& dst, std::string& err);

// Receive time encoded in a message id, or 0 if unparseable.
time_t idTime(const std::string& id);

// "2026-06-12T17:42:31Z" <-> time_t (UTC). parseIsoTime returns 0 on
// failure (and tolerates a fractional-second field, which it ignores).
std::string isoTime(time_t t);
time_t parseIsoTime(const std::string& s);

// Current UTC time to millisecond precision,
// "2026-06-12T17:42:31.313Z" — for the informational "received" stamp,
// matching the millisecond resolution of a message id. Operational
// timestamps that are parsed back (status, attempts) use isoTime.
std::string isoTimeMillis();

// Free bytes on the filesystem holding path, or -1.
long long freeBytes(const std::string& path);

// Filesystem predicates: pathExists is any stat-able entry; isDir is a
// directory. Both report false (never an error) for a missing path.
bool pathExists(const std::string& path);
bool isDir(const std::string& path);

// qmail's quadratic retry schedule: backoffSeconds grows as
// 2*sqrt(seconds*BASE), capped at 6 hours. retryBackoff(level) is the
// wait a message must sit at retry rung `level` before it is due again
// — the ladder keys the same quadratic cadence on the rung number, so
// successive rungs come due ~BASE*level^2 seconds apart. A message in
// todo/ (level 0) is always due.
long backoffSeconds(long seconds);
long retryBackoff(int level);

// Message id: "<YYYYMMDDHHMMSSMMM>.<pid>.<counter>" (UTC). Unique per
// host by construction; lexically ordered by creation time within a
// process.
std::string generateId();

// AE title -> path component: trimmed, alphanumerics and '-' kept,
// everything else replaced by '_', empty becomes "_". '.' is never kept
// because it separates filename fields.
std::string sanitizeAET(const std::string& aet);

// Sanitized names that may never name an aet/ directory or delivery
// destination ("tmp", "new", "todo"), compared case-insensitively.
bool isReservedName(const std::string& name);

// Directory portion of path ("." if none, "/" for a root entry).
std::string dirOf(const std::string& path);

// mkdir(path) treating EEXIST as success, then fsync the parent so the
// new directory survives a crash. Intermediate parents must already
// exist (the spool skeleton is operator-created; this only fills in the
// per-AET and per-retry-rung leaves dicomq owns).
bool mkdirIfMissing(const std::string& path, std::string& err);

// fsync the file or directory at path. On failure returns false and
// sets err.
bool fsyncPath(const std::string& path, std::string& err);

// Durably publish tmpPath at finalPath: fsync(tmpPath), rename(2),
// fsync the containing directory. The commit point of every queue
// transition.
bool commitFile(const std::string& tmpPath, const std::string& finalPath,
                std::string& err);

// link(2) from -> to, treating EEXIST as success: ids are unique, so
// "already linked" means a previous (possibly crashed) pass already did
// this work. Fsyncs the containing directory on success.
bool linkIdempotent(const std::string& from, const std::string& to,
                    std::string& err);

// Serialize kv into queue/tmp/ and commit it atomically at finalPath
// (replacing any existing file). Used for the dest/<DEST>/status
// dead-site cache. The tmp name carries the pid so concurrent writers
// cannot collide.
bool writeKeyValueCommitted(const Spool& sp, const KeyValueFile& kv,
                            const std::string& finalPath, std::string& err);

} // namespace dicomq

#endif // DICOMQ_SPOOL_H