#ifndef DICOMQ_SPOOL_H
#define DICOMQ_SPOOL_H

#include <ctime>
#include <string>
#include <vector>

// Spool primitives shared by every dicomq program. See dicomq/DESIGN.md
// "Spool layout" and "Commit protocols". Nothing here knows about DICOM.

namespace dicomq {

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
  std::string routeRoot() const { return root + "/route"; }
  std::string routeDir(const std::string& d) const { return root + "/route/" + d; }
  std::string routeTodo(const std::string& d) const { return routeDir(d) + "/todo"; }
  std::string routeStatus(const std::string& d) const { return routeDir(d) + "/status"; }
  std::string routeHoldFlag(const std::string& d) const { return routeDir(d) + "/hold"; }
  std::string aetRoot() const { return root + "/aet"; }
  std::string aetDir(const std::string& a) const { return root + "/aet/" + a; }
  std::string destDir(const std::string& d) const { return root + "/dest/" + d; }
  std::string failedDir() const { return root + "/failed"; }
  std::string holdDir() const { return root + "/hold"; }
  std::string corruptDir() const { return root + "/corrupt"; }
};

// Ids of committed messages in dir (entries ending ".env", suffix
// stripped), sorted — which is receive order. Missing dir = empty.
std::vector<std::string> listIds(const std::string& dir);

// Sorted subdirectory names of dir. Missing dir = empty.
std::vector<std::string> listSubdirs(const std::string& dir);

// Plain byte copy, no durability (callers publish with commitFile).
bool copyFile(const std::string& src, const std::string& dst, std::string& err);

// Receive time encoded in a message id, or 0 if unparseable.
time_t idTime(const std::string& id);

// "2026-06-12T17:42:31Z" <-> time_t (UTC). parseIsoTime returns 0 on
// failure.
std::string isoTime(time_t t);
time_t parseIsoTime(const std::string& s);

// Free bytes on the filesystem holding path, or -1.
long long freeBytes(const std::string& path);

// qmail's quadratic retry schedule: a message is due for another
// delivery attempt when (now - last attempt) >= backoffSeconds(age).
// Grows as 2*sqrt(age*BASE), so successive attempts happen at ages
// ~BASE*n^2; capped at 6 hours.
long backoffSeconds(long ageSeconds);
bool isDue(time_t now, time_t lastAttempt, time_t received);

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

} // namespace dicomq

#endif // DICOMQ_SPOOL_H
