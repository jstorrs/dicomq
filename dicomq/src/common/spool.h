#ifndef DICOMQ_SPOOL_H
#define DICOMQ_SPOOL_H

#include <string>

// Spool primitives shared by every dicomq program. See dicomq/DESIGN.md
// "Spool layout" and "Commit protocols". Nothing here knows about DICOM.

namespace dicomq {

// Spool root from $DICOMQ_SPOOL, else the compiled-in default
// /var/spool/dicomq.
std::string spoolRoot();

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
