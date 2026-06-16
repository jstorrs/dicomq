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
// Exit: 0 delivered; 100 bad usage; 111 temporary failure (missing
// target, unreadable message) — the caller leaves the message queued.

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

// link into the maildir, falling back to copy-through-tmp across
// filesystems; idempotent either way
static bool deliverFile(const std::string &src, const std::string &dir,
                        const std::string &name, std::string &err) {
  const std::string dst = dir + "/new/" + name;
  switch (linkOrSame(src, dst, err)) {
  case LinkOutcome::Ok:
    return fsyncPath(dir + "/new", err);
  case LinkOutcome::Failed:
    return false;
  case LinkOutcome::CrossDevice:
    break; // a cross-filesystem maildir: copy through its own tmp/
  }
  const std::string tmp = dir + "/tmp/" + name;
  if (!copyFile(src, tmp, err))
    return false;
  if (!commitFile(tmp, dst, err)) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

// deliver a sealed batch as new/<id>/: stage its objects in tmp/<id>/
// (hardlink on the spool fs, copy across filesystems), then publish with a
// single atomic rename so the study appears all at once. Idempotent.
static bool deliverBatch(const std::string &srcBatch, const std::string &dir,
                         const std::string &id, std::string &err) {
  const std::string dst = dir + "/new/" + id;
  if (isDir(dst))
    return true; // already delivered
  if (!isDir(dir + "/tmp")) {
    err = "'" + dir + "/tmp' is not a directory (maildir needs tmp/)";
    return false;
  }
  const std::string stage = dir + "/tmp/" + id;
  std::error_code ec;
  fs::remove_all(stage, ec); // clear any crashed partial staging
  if (!mkdirIfMissing(stage, err))
    return false;
  for (const auto &objid : listIds(srcBatch)) {
    const std::string src = dcmPath(srcBatch, objid);
    const std::string tgt = dcmPath(stage, objid);
    switch (linkOrSame(src, tgt, err)) {
    case LinkOutcome::Ok:
      continue;
    case LinkOutcome::Failed:
      return false;
    case LinkOutcome::CrossDevice:
      if (!copyFile(src, tgt, err)) // cross-filesystem maildir
        return false;
    }
  }
  if (!fsyncPath(stage, err))
    return false;
  if (rename(stage.c_str(), dst.c_str()) != 0) {
    if (errno == ENOTEMPTY || errno == EEXIST) {
      fs::remove_all(stage, ec); // raced: another pass delivered it
      return true;
    }
    err = "cannot rename '" + stage + "' to '" + dst + "': " + strerror(errno);
    return false;
  }
  return fsyncPath(dir + "/new", err);
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
    std::fprintf(stderr,
                 "dicomq-local: '%s/new' is not a directory "
                 "(dicomq never creates maildirs)\n",
                 dir.c_str());
    return 111;
  }

  // a batch is a directory <srcdir>/<id>/; a single object is <id>.dcm
  const std::string srcBatch = srcDir + "/" + id;
  const bool ok = isDir(srcBatch)
                      ? deliverBatch(srcBatch, dir, id, err)
                      : deliverFile(dcmPath(srcDir, id), dir, id + ".dcm", err);
  if (!ok) {
    std::fprintf(stderr, "dicomq-local: %s\n", err.c_str());
    return 111;
  }
  return 0;
}
