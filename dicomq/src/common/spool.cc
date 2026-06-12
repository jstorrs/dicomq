#include "common/spool.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

bool fsyncPath(const std::string& path, std::string& err)
{
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
  {
    err = "cannot open '" + path + "' for sync: " + strerror(errno);
    return false;
  }
  if (fsync(fd) != 0)
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

} // namespace dicomq
