// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DICOMQ_MESSAGE_H
#define DICOMQ_MESSAGE_H

#include <ctime>
#include <string>
#include <vector>

#include "common/spool.h"

// Operations on a message. A message is normally a single <id>.dcm file;
// in study/series mode it is a *batch* — a directory <id>/ of <objid>.dcm
// files that dicomq-recv accumulated and dicomq-send sealed (DESIGN.md
// "Commit protocols", docs/study-mode.md). The retry state a sidecar
// envelope used to carry lives in the directory a message sits in
// (route/<DEST>/todo vs retry/<k>), so moving a message between queues is
// one atomic rename — of a file or of a directory, both atomic: the
// rename IS the commit. Nothing here knows about DICOM.

namespace dicomq {

// A queued message, and which on-disk form it takes: a single object
// (<id>.dcm, a file) or a sealed batch (<id>/, a directory).
struct Message {
  std::string id;
  bool isBatch = false;
};

std::string dcmPath(const std::string &dir, const std::string &id);

// Path of message <id> in dir: dir/<id>.dcm for a file, dir/<id> for a
// batch directory.
std::string messagePath(const std::string &dir, const std::string &id,
                        bool isBatch);

// Every committed message in dir — files ending ".dcm" (isBatch=false)
// and subdirectories (isBatch=true) — sorted by id, which is receive/seal
// order. Missing dir = empty.
std::vector<Message> listMessages(const std::string &dir);

// Recursively hardlink batch <id>/ from srcParent into a fresh directory
// <id>/ under dstParent: the new directory gets its own mtime (a private
// retry-backoff clock) while the member objects share inodes (fan-out and
// copy-on-demote cost no data copy). The tree is staged under a dot-name
// the queue walkers skip and published by one atomic rename, so a partial
// tree is never visible as a message. Idempotent (an existing <id>/ under
// dstParent means a prior pass published it).
bool linkBatchTree(const std::string &srcParent, const std::string &dstParent,
                   const std::string &id, std::string &err);

// Move message <id> from fromDir to toDir with a single rename(2) — of a
// file (isBatch=false) or a directory (isBatch=true), both atomic, so the
// message exists in exactly one directory at every instant. Idempotent:
// if the source is already gone but the target exists, a prior (possibly
// crashed) pass did this — success. A batch rename cannot clobber an
// existing non-empty <id>/ at the target (ENOTEMPTY, where a file rename
// silently replaces); ids are unique, so the resident batch is this same
// message and the stranded source is discarded (via sp's trash/) instead
// of being re-processed forever.
bool moveMessage(const Spool &sp, const std::string &fromDir,
                 const std::string &toDir, const std::string &id,
                 std::string &err, bool isBatch = false);

// Fan-out <id> from fromDir into toDir, leaving the source in place: a
// hardlink for a file, a recursive hardlink-tree for a batch. EEXIST
// (file) or already-present members (batch) mean a prior pass did it.
bool linkMessage(const std::string &fromDir, const std::string &toDir,
                 const std::string &id, std::string &err, bool isBatch = false);

// Remove message <id> from dir — unlink the file, or recursively remove
// the batch directory. A missing message is tolerated (idempotent). Use
// this only for an undelivered message (an interrupted write, an empty
// husk): the recursive batch delete is not atomic, so a crash partway can
// leave a partial directory.
bool removeMessage(const std::string &dir, const std::string &id,
                   std::string &err, bool isBatch = false);

// Discard a *superseded* message from dir — one whose content already lives
// safely elsewhere (fanned out, or copied to the next retry rung). Unlike
// removeMessage this is crash-atomic for a batch: the directory is renamed
// whole into trash/ (the dequeue) and then deleted there, so a crash can
// never leave a partial batch behind to be re-delivered. The source parent
// is fsynced, making the dequeue durable. A missing message is tolerated.
bool discardMessage(const Spool &sp, const std::string &dir,
                    const std::string &id, bool isBatch, std::string &err);

// Whether message <id> in dir, sitting at retry rung retryLevel, is due
// for a delivery attempt at time now. Rung 0 (todo/) is always due; rung
// k>=1 is due when now - mtime(message) >= retryBackoff(k) — the mtime of
// the <id>.dcm file or, for a batch, of the <id>/ directory. A vanished
// message reads as not-due.
bool messageDue(const std::string &dir, const std::string &id, int retryLevel,
                time_t now, bool isBatch = false);

} // namespace dicomq

#endif // DICOMQ_MESSAGE_H
