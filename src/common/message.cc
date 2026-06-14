// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/message.h"

#include <cerrno>
#include <cstring>

#include <sys/stat.h>
#include <unistd.h>

namespace dicomq {

std::string dcmPath(const std::string& dir, const std::string& id)
{
  return dir + "/" + id + ".dcm";
}

bool moveMessage(const std::string& fromDir, const std::string& toDir,
                 const std::string& id, std::string& err)
{
  const std::string src = dcmPath(fromDir, id);
  const std::string dst = dcmPath(toDir, id);
  if (rename(src.c_str(), dst.c_str()) == 0)
    return fsyncPath(dirOf(dst), err) && fsyncPath(dirOf(src), err);
  if (errno == ENOENT && pathExists(dst))
    return true;  // a prior pass already moved it
  err = "cannot rename '" + src + "' to '" + dst + "': " + strerror(errno);
  return false;
}

bool linkMessage(const std::string& fromDir, const std::string& toDir,
                 const std::string& id, std::string& err)
{
  return linkIdempotent(dcmPath(fromDir, id), dcmPath(toDir, id), err);
}

bool removeMessage(const std::string& dir, const std::string& id,
                   std::string& err)
{
  if (unlink(dcmPath(dir, id).c_str()) != 0 && errno != ENOENT)
  {
    err = "cannot unlink '" + dcmPath(dir, id) + "': " + strerror(errno);
    return false;
  }
  return true;
}

bool messageDue(const std::string& dir, const std::string& id,
                int retryLevel, time_t now)
{
  if (retryLevel <= 0)
    return true;  // todo/: never attempted, always due
  struct stat st;
  return stat(dcmPath(dir, id).c_str(), &st) == 0
         && now - st.st_mtime >= retryBackoff(retryLevel);
}

} // namespace dicomq
