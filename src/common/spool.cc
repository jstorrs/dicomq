// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/spool.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace dicomq {

std::string spoolRoot()
{
  const char *env = std::getenv("DICOMQ_SPOOL");
  return (env && *env) ? env : "/var/spool/dicomq";
}

std::string generateId()
{
  static int counter = 0;

  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t secs = system_clock::to_time_t(now);
  const int ms = static_cast<int>(
      duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  struct tm tm;
  gmtime_r(&secs, &tm);

  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d%03d.%ld.%06d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, ms,
                static_cast<long>(getpid()), counter++);
  return buf;
}

std::string sanitizeAET(const std::string& aet)
{
  size_t first = 0;
  size_t last = aet.length();
  while (first < last && !isgraph(static_cast<unsigned char>(aet[first])))
    first++;
  while (last > first && !isgraph(static_cast<unsigned char>(aet[last - 1])))
    last--;

  std::string dest;
  for (size_t i = first; i < last; i++)
  {
    const unsigned char c = static_cast<unsigned char>(aet[i]);
    dest += (isalnum(c) || c == '-') ? static_cast<char>(c) : '_';
  }
  if (dest.empty())
    dest = "_";
  return dest;
}

bool isReservedName(const std::string& name)
{
  std::string lower;
  for (char c : name)
    lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return lower == "tmp" || lower == "new" || lower == "todo";
}

static std::string dirOf(const std::string& path)
{
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos)
    return ".";
  if (slash == 0)
    return "/";
  return path.substr(0, slash);
}

static bool flushToStableStorage(int fd)
{
#ifdef __APPLE__
  // fsync(2) on macOS flushes only to the drive's cache; F_FULLFSYNC is
  // the documented way to reach stable storage, and the deliver-before-
  // acknowledge contract depends on that. Some filesystems (e.g. SMB)
  // do not support it — fall back to fsync there.
  if (fcntl(fd, F_FULLFSYNC) == 0)
    return true;
#endif
  return fsync(fd) == 0;
}

bool fsyncPath(const std::string& path, std::string& err)
{
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
  {
    err = "cannot open '" + path + "' for sync: " + strerror(errno);
    return false;
  }
  if (!flushToStableStorage(fd))
  {
    err = "cannot sync '" + path + "': " + strerror(errno);
    close(fd);
    return false;
  }
  close(fd);
  return true;
}

bool commitFile(const std::string& tmpPath, const std::string& finalPath,
                std::string& err)
{
  if (!fsyncPath(tmpPath, err))
    return false;
  if (rename(tmpPath.c_str(), finalPath.c_str()) != 0)
  {
    err = "cannot rename '" + tmpPath + "' to '" + finalPath + "': "
          + strerror(errno);
    return false;
  }
  return fsyncPath(dirOf(finalPath), err);
}

bool linkIdempotent(const std::string& from, const std::string& to,
                    std::string& err)
{
  if (link(from.c_str(), to.c_str()) != 0 && errno != EEXIST)
  {
    err = "cannot link '" + from + "' to '" + to + "': " + strerror(errno);
    return false;
  }
  return fsyncPath(dirOf(to), err);
}

std::vector<std::string> listIds(const std::string& dir)
{
  std::vector<std::string> ids;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec))
  {
    const std::string name = entry.path().filename().string();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".env") == 0)
      ids.push_back(name.substr(0, name.size() - 4));
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<std::string> listSubdirs(const std::string& dir)
{
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec))
  {
    if (entry.is_directory(ec))
      names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool copyFile(const std::string& src, const std::string& dst, std::string& err)
{
  const int in = open(src.c_str(), O_RDONLY);
  if (in < 0)
  {
    err = "cannot open '" + src + "': " + strerror(errno);
    return false;
  }
  const int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0)
  {
    err = "cannot create '" + dst + "': " + strerror(errno);
    close(in);
    return false;
  }
  char buf[65536];
  ssize_t got;
  while ((got = read(in, buf, sizeof(buf))) > 0)
  {
    ssize_t done = 0;
    while (done < got)
    {
      const ssize_t put = write(out, buf + done, got - done);
      if (put < 0)
      {
        err = "write error on '" + dst + "': " + strerror(errno);
        close(in);
        close(out);
        return false;
      }
      done += put;
    }
  }
  const bool readOk = (got == 0);
  if (!readOk)
    err = "read error on '" + src + "': " + strerror(errno);
  close(in);
  close(out);
  return readOk;
}

time_t idTime(const std::string& id)
{
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  if (sscanf(id.c_str(), "%4d%2d%2d%2d%2d%2d", &tm.tm_year, &tm.tm_mon,
             &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
    return 0;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  return timegm(&tm);
}

std::string isoTime(time_t t)
{
  struct tm tm;
  gmtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string isoTimeMillis()
{
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t secs = system_clock::to_time_t(now);
  const int ms = static_cast<int>(
      duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  struct tm tm;
  gmtime_r(&secs, &tm);
  char buf[40];
  const size_t n = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  std::snprintf(buf + n, sizeof(buf) - n, ".%03dZ", ms);
  return buf;
}

time_t parseIsoTime(const std::string& s)
{
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  if (!strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm))
    return 0;
  return timegm(&tm);
}

long long freeBytes(const std::string& path)
{
  struct statvfs vfs;
  if (statvfs(path.c_str(), &vfs) != 0)
    return -1;
  return static_cast<long long>(vfs.f_bavail) * vfs.f_frsize;
}

bool pathExists(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

bool isDir(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static const long BACKOFF_BASE = 420;    // ~7 minutes
static const long BACKOFF_CAP = 21600;   // 6 hours

long backoffSeconds(long ageSeconds)
{
  if (ageSeconds <= 0)
    return 0;
  const double b = 2.0 * sqrt(static_cast<double>(ageSeconds) * BACKOFF_BASE);
  return b > BACKOFF_CAP ? BACKOFF_CAP : static_cast<long>(b);
}

bool isDue(time_t now, time_t lastAttempt, time_t received)
{
  return now - lastAttempt >= backoffSeconds(static_cast<long>(now - received));
}

} // namespace dicomq