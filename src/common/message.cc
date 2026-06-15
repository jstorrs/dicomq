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
    if (link((src + "/" + name).c_str(), (dst + "/" + name).c_str()) != 0 &&
        errno != EEXIST) {
      err = "cannot link '" + src + "/" + name + "' to '" + dst + "/" + name +
            "': " + strerror(errno);
      return false;
    }
  }
  if (ec) {
    err = "cannot read batch '" + src + "': " + ec.message();
    return false;
  }
  return fsyncPath(dst, err);
}

bool moveMessage(const std::string &fromDir, const std::string &toDir,
                 const std::string &id, std::string &err, bool isBatch) {
  const std::string src = messagePath(fromDir, id, isBatch);
  const std::string dst = messagePath(toDir, id, isBatch);
  if (rename(src.c_str(), dst.c_str()) == 0)
    return fsyncPath(dirOf(dst), err) && fsyncPath(dirOf(src), err);
  if (errno == ENOENT && pathExists(dst))
    return true; // a prior pass already moved it
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

bool messageDue(const std::string &dir, const std::string &id, int retryLevel,
                time_t now, bool isBatch) {
  if (retryLevel <= 0)
    return true; // todo/: never attempted, always due
  struct stat st;
  return stat(messagePath(dir, id, isBatch).c_str(), &st) == 0 &&
         now - st.st_mtime >= retryBackoff(retryLevel);
}

} // namespace dicomq
