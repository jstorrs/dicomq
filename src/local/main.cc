// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// dicomq-local — maildir-style local delivery (qmail-local analog).
//
//   dicomq-local [-s <spool>] <id> <dir> <srcdir>
//
// Delivers a message from <srcdir> into <dir>/new/. A single object
// (<srcdir>/<id>.dcm) is hardlinked when <dir> is on the spool filesystem
// (EEXIST = already delivered = success), or copied through the maildir's
// own tmp/ across filesystems (which is what maildirs have tmp/ for). A
// sealed study/series batch (<srcdir>/<id>/) is delivered as one
// subdirectory <dir>/new/<id>/: its objects are staged in <dir>/tmp/<id>/
// and published with a single atomic rename, so the whole study appears
// in new/ at once. Never creates <dir> or its subdirectories. Delivered
// files may share an inode with the spool: consumers may move or delete
// them, never modify in place.
//
// Exit (qmail-local's delivery convention): 0 delivered; 111 temporary
// failure (missing target, transient I/O) — the caller leaves the message
// queued and retries; 100 permanent failure — re-running cannot help, so the
// caller (dicomq-send) escalates the message to failed/ rather than re-attempt
// it every scan. A permanent failure is a slot collision (a *different* object
// already occupies new/<id>; ids are unique, so this is never a delivery
// replay) or a bad invocation.

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "common/message.h"
#include "common/spool.h"

namespace fs = std::filesystem;
using namespace dicomq;

static void logmsg(const std::string &m) { dicomq::logmsg("dicomq-local", m); }

// Outcome of a delivery attempt. Permanent = re-running cannot help, so the
// caller escalates to failed/; Temporary = transient, leave the message queued.
enum class Delivery { Ok, Temporary, Permanent };

// link into the maildir, falling back to copy-through-tmp across
// filesystems; idempotent either way
static Delivery deliverFile(const std::string &src, const std::string &dir,
                            const std::string &name, std::string &err) {
  const std::string dst = dir + "/new/" + name;
  switch (linkOrSame(src, dst, err)) {
  case LinkOutcome::Ok:
    return fsyncPath(dir + "/new", err) ? Delivery::Ok : Delivery::Temporary;
  case LinkOutcome::Failed:
    // A *different* object already occupying new/<id> is a permanent collision
    // (ids are unique, so this is not a delivery replay and retrying can never
    // resolve it). Any other link error (ENOSPC, EACCES) is transient — tell
    // the two apart by whether the destination actually exists.
    return pathExists(dst) ? Delivery::Permanent : Delivery::Temporary;
  case LinkOutcome::CrossDevice:
    break; // a cross-filesystem maildir: copy through its own tmp/
  }
  const std::string tmp = dir + "/tmp/" + name;
  if (!copyFile(src, tmp, err))
    return Delivery::Temporary;
  if (!commitFile(tmp, dst, err)) {
    unlink(tmp.c_str());
    return Delivery::Temporary;
  }
  return Delivery::Ok;
}

// deliver a sealed batch as new/<id>/: stage its objects in tmp/<id>/
// (hardlink on the spool fs, copy across filesystems), then publish with a
// single atomic rename so the study appears all at once. Idempotent.
static Delivery deliverBatch(const std::string &srcBatch,
                             const std::string &dir, const std::string &id,
                             std::string &err) {
  const std::string dst = dir + "/new/" + id;
  if (isDir(dst))
    return Delivery::Ok; // already delivered
  if (!isDir(dir + "/tmp")) {
    err = "'" + dir + "/tmp' is not a directory (maildir needs tmp/)";
    return Delivery::Temporary;
  }
  const std::string stage = dir + "/tmp/" + id;
  std::error_code ec;
  fs::remove_all(stage, ec); // clear any crashed partial staging
  if (!mkdirIfMissing(stage, err))
    return Delivery::Temporary;
  for (const auto &objid : listIds(srcBatch)) {
    const std::string src = dcmPath(srcBatch, objid);
    const std::string tgt = dcmPath(stage, objid);
    switch (linkOrSame(src, tgt, err)) {
    case LinkOutcome::Ok:
      continue;
    case LinkOutcome::Failed:
      return Delivery::Temporary;
    case LinkOutcome::CrossDevice:
      // cross-filesystem maildir: copyFile is non-durable, so flush the
      // copied contents before the staging dir is published by rename —
      // otherwise a crash can leave a complete-looking batch of truncated
      // members (the single-object path gets this via commitFile).
      if (!copyFile(src, tgt, err) || !fsyncPath(tgt, err))
        return Delivery::Temporary;
    }
  }
  if (!fsyncPath(stage, err))
    return Delivery::Temporary;
  if (rename(stage.c_str(), dst.c_str()) != 0) {
    if (errno == ENOTEMPTY || errno == EEXIST) {
      fs::remove_all(stage, ec); // raced: another pass delivered it
      return Delivery::Ok;
    }
    err = "cannot rename '" + stage + "' to '" + dst + "': " + strerror(errno);
    return Delivery::Temporary;
  }
  return fsyncPath(dir + "/new", err) ? Delivery::Ok : Delivery::Temporary;
}

int main(int argc, char **argv) {
  int opt;
  while ((opt = getopt(argc, argv, "s:")) != -1) {
    if (opt != 's') // -s accepted for suite consistency; unused here
    {
      std::fprintf(stderr,
                   "usage: dicomq-local [-s <spool>] <id> <dir> <srcdir>\n");
      return 100;
    }
  }
  if (argc - optind != 3) {
    std::fprintf(stderr,
                 "usage: dicomq-local [-s <spool>] <id> <dir> <srcdir>\n");
    return 100;
  }
  const std::string id = argv[optind];
  const std::string dir = argv[optind + 1];
  const std::string srcDir = argv[optind + 2];

  std::string err;

  if (!isDir(dir + "/new")) {
    logmsg("'" + dir +
           "/new' is not a directory (dicomq never creates maildirs)");
    return 111;
  }

  // a batch is a directory <srcdir>/<id>/; a single object is <id>.dcm
  const std::string srcBatch = srcDir + "/" + id;
  const Delivery d =
      isDir(srcBatch) ? deliverBatch(srcBatch, dir, id, err)
                      : deliverFile(dcmPath(srcDir, id), dir, id + ".dcm", err);
  switch (d) {
  case Delivery::Ok:
    return 0;
  case Delivery::Permanent:
    logmsg(err);
    return 100; // re-running cannot help; the caller escalates to failed/
  case Delivery::Temporary:
    logmsg(err);
    return 111;
  }
  return 111; // unreachable; keep the compiler happy
}
