// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/message.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace dicomq {

std::string dcmPath(const std::string &dir, const std::string &id) {
  return dir + "/" + id + ".dcm";
}

std::string messagePath(const std::string &dir, const std::string &id,
                        bool isBatch) {
  return isBatch ? dir + "/" + id : dcmPath(dir, id);
}

std::vector<Message> listMessages(const std::string &dir) {
  std::vector<Message> out;
  for (const auto &id : listIds(dir))
    out.push_back({id, false});
  for (const auto &id : listSubdirs(dir))
    out.push_back({id, true});
  std::sort(out.begin(), out.end(),
            [](const Message &a, const Message &b) { return a.id < b.id; });
  return out;
}

bool linkBatchTree(const std::string &srcParent, const std::string &dstParent,
                   const std::string &id, std::string &err) {
  const std::string src = srcParent + "/" + id;
  const std::string dst = dstParent + "/" + id;
  // a fresh directory carries a fresh mtime — the retry backoff clock —
  // while the member objects below share inodes with the source
  if (!mkdirIfMissing(dst, err))
    return false;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(src, ec)) {
    const std::string name = entry.path().filename().string();
    // route/ fan-out is same-filesystem, so a member can only end up
    // Ok or Failed; an existing member with a different inode (a stale or
    // wrong tree) fails here rather than being accepted as delivered.
    if (linkOrSame(src + "/" + name, dst + "/" + name, err) != LinkOutcome::Ok)
      return false;
  }
  if (ec) {
    err = "cannot read batch '" + src + "': " + ec.message();
    return false;
  }
  return fsyncPath(dst, err);
}

bool moveMessage(const Spool &sp, const std::string &fromDir,
                 const std::string &toDir, const std::string &id,
                 std::string &err, bool isBatch) {
  const std::string src = messagePath(fromDir, id, isBatch);
  const std::string dst = messagePath(toDir, id, isBatch);
  if (rename(src.c_str(), dst.c_str()) == 0)
    return fsyncPath(dirOf(dst), err) && fsyncPath(dirOf(src), err);
  if (errno == ENOENT && pathExists(dst))
    return true; // a prior pass already moved it
  // A directory rename cannot clobber a non-empty target the way a file
  // rename does. A crash between a demotion's link-tree and its discard
  // leaves a batch on two rungs; both eventually head for the same sink,
  // and the second arrival collides with the first. Ids are unique, so
  // the resident batch is this same message: treat it as moved and
  // discard the stranded source, or it would be re-processed forever.
  if (isBatch && (errno == ENOTEMPTY || errno == EEXIST) && isDir(dst))
    return discardMessage(sp, fromDir, id, true, err);
  err = "cannot rename '" + src + "' to '" + dst + "': " + strerror(errno);
  return false;
}

bool linkMessage(const std::string &fromDir, const std::string &toDir,
                 const std::string &id, std::string &err, bool isBatch) {
  if (isBatch)
    return linkBatchTree(fromDir, toDir, id, err);
  return linkIdempotent(dcmPath(fromDir, id), dcmPath(toDir, id), err);
}

bool removeMessage(const std::string &dir, const std::string &id,
                   std::string &err, bool isBatch) {
  if (isBatch) {
    std::error_code ec;
    fs::remove_all(dir + "/" + id, ec); // tolerates a missing directory
    if (ec) {
      err = "cannot remove batch '" + dir + "/" + id + "': " + ec.message();
      return false;
    }
    return true;
  }
  if (unlink(dcmPath(dir, id).c_str()) != 0 && errno != ENOENT) {
    err = "cannot unlink '" + dcmPath(dir, id) + "': " + strerror(errno);
    return false;
  }
  return true;
}

bool discardMessage(const Spool &sp, const std::string &dir,
                    const std::string &id, bool isBatch, std::string &err) {
  const std::string src = messagePath(dir, id, isBatch);
  if (!isBatch) {
    // a file unlink is already atomic; just make the dequeue durable
    if (unlink(src.c_str()) != 0 && errno != ENOENT) {
      err = "cannot unlink '" + src + "': " + strerror(errno);
      return false;
    }
    return fsyncPath(dirOf(src), err);
  }
  // a batch is a directory, and an in-place recursive delete is not atomic:
  // a crash partway could leave a shrunken batch to be re-delivered. Rename
  // it whole into trash/ under a process-unique name (so concurrent or
  // fan-out discards of the same id never collide) — that single rename is
  // the dequeue — then delete it there. The trash delete is best-effort:
  // dicomq-clean reaps anything a crash leaves behind.
  static int counter = 0;
  if (!mkdirIfMissing(sp.trashDir(), err))
    return false;
  const std::string dst = sp.trashDir() + "/" + id + "." +
                          std::to_string(getpid()) + "." +
                          std::to_string(counter++);
  if (rename(src.c_str(), dst.c_str()) != 0) {
    if (errno == ENOENT && !pathExists(src))
      return true; // a prior pass already discarded it
    err = "cannot discard '" + src + "': " + strerror(errno);
    return false;
  }
  if (!fsyncPath(dirOf(src), err)) // the dequeue is now durable
    return false;
  std::error_code ec;
  fs::remove_all(dst, ec); // best-effort; clean reaps a leftover
  return true;
}

bool messageDue(const std::string &dir, const std::string &id, int retryLevel,
                time_t now, bool isBatch) {
  if (retryLevel <= 0)
    return true; // todo/: never attempted, always due
  struct stat st;
  return stat(messagePath(dir, id, isBatch).c_str(), &st) == 0 &&
         now - st.st_mtime >= retryBackoff(retryLevel);
}

} // namespace dicomq
