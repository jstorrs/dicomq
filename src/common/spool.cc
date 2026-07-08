// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/spool.h"

#include "common/kvfile.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace dicomq {

std::string spoolRoot() {
  const char *env = std::getenv("DICOMQ_SPOOL");
  return (env && *env) ? env : "/var/spool/dicomq";
}

std::string generateId() {
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
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, ms, static_cast<long>(getpid()),
                counter++);
  return buf;
}

std::string sanitizeAET(const std::string &aet) {
  size_t first = 0;
  size_t last = aet.length();
  while (first < last && !isgraph(static_cast<unsigned char>(aet[first])))
    first++;
  while (last > first && !isgraph(static_cast<unsigned char>(aet[last - 1])))
    last--;

  std::string dest;
  for (size_t i = first; i < last; i++) {
    const unsigned char c = static_cast<unsigned char>(aet[i]);
    dest += (isalnum(c) || c == '-') ? static_cast<char>(c) : '_';
  }
  if (dest.empty())
    dest = "_";
  return dest;
}

bool isReservedName(const std::string &name) {
  std::string lower;
  for (char c : name)
    lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return lower == "tmp" || lower == "new" || lower == "todo";
}

std::string dirOf(const std::string &path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos)
    return ".";
  if (slash == 0)
    return "/";
  return path.substr(0, slash);
}

bool mkdirIfMissing(const std::string &path, std::string &err) {
  if (mkdir(path.c_str(), 0755) != 0) {
    if (errno != EEXIST) {
      err = "cannot create '" + path + "': " + strerror(errno);
      return false;
    }
    // EEXIST only excuses an existing *directory*. A regular file (or
    // anything else) at this path is a misconfigured spool: report it
    // here rather than letting a later rename into it fail obscurely.
    if (!isDir(path)) {
      err = "'" + path + "' exists but is not a directory";
      return false;
    }
  }
  return fsyncPath(dirOf(path), err);
}

bool mkdirsUnder(const std::string &base, const std::string &rel,
                 std::string &err) {
  if (!mkdirIfMissing(base, err)) // base itself (e.g. hold/) may be absent
    return false;
  std::string path = base;
  size_t start = 0;
  while (start < rel.size()) {
    const size_t slash = rel.find('/', start);
    const std::string comp =
        rel.substr(start, slash == std::string::npos ? slash : slash - start);
    if (!comp.empty()) {
      path += "/" + comp;
      if (!mkdirIfMissing(path, err))
        return false;
    }
    if (slash == std::string::npos)
      break;
    start = slash + 1;
  }
  return true;
}

static bool flushToStableStorage(int fd) {
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

bool fsyncPath(const std::string &path, std::string &err) {
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    err = "cannot open '" + path + "' for sync: " + strerror(errno);
    return false;
  }
  if (!flushToStableStorage(fd)) {
    err = "cannot sync '" + path + "': " + strerror(errno);
    close(fd);
    return false;
  }
  close(fd);
  return true;
}

bool commitFile(const std::string &tmpPath, const std::string &finalPath,
                std::string &err) {
  if (!fsyncPath(tmpPath, err))
    return false;
  if (rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
    err = "cannot rename '" + tmpPath + "' to '" + finalPath +
          "': " + strerror(errno);
    return false;
  }
  return fsyncPath(dirOf(finalPath), err);
}

LinkOutcome linkOrSame(const std::string &from, const std::string &to,
                       std::string &err) {
  if (link(from.c_str(), to.c_str()) == 0)
    return LinkOutcome::Ok;
  const int e = errno;
  if (e != EEXIST) {
    err = "cannot link '" + from + "' to '" + to + "': " + strerror(e);
    return e == EXDEV ? LinkOutcome::CrossDevice : LinkOutcome::Failed;
  }
  // EEXIST is the crash-replay case only when `to` is already the same
  // inode as `from`. A different inode means an id collision or a stale
  // wrong file; treating it as delivered would let the source be
  // dequeued, so fail loudly instead.
  struct stat sf, st;
  if (stat(from.c_str(), &sf) != 0 || stat(to.c_str(), &st) != 0 ||
      sf.st_dev != st.st_dev || sf.st_ino != st.st_ino) {
    err = "'" + to + "' already exists and differs from '" + from + "'";
    return LinkOutcome::Failed;
  }
  return LinkOutcome::Ok;
}

bool linkIdempotent(const std::string &from, const std::string &to,
                    std::string &err) {
  return linkOrSame(from, to, err) == LinkOutcome::Ok &&
         fsyncPath(dirOf(to), err);
}

bool hasDcmSuffix(const std::string &name) {
  return name.size() > 4 && name.compare(name.size() - 4, 4, ".dcm") == 0;
}

// A missing directory is legitimately empty (every queue walker relies on
// this), so it stays silent. Any OTHER iteration failure — EACCES, EIO,
// ENOTDIR — would otherwise read as "no work" and hide a sick spool, so report
// it on stderr. Deduplicated per (path, error) because the queue runner rescans
// every interval and must not spam a persistent permission problem.
static void reportDirError(const std::string &dir, const std::error_code &ec) {
  if (!ec || ec == std::errc::no_such_file_or_directory)
    return;
  static std::set<std::string> reported;
  if (reported.insert(dir + '\0' + ec.message()).second)
    logmsg("dicomq", "cannot read directory '" + dir + "': " + ec.message());
}

std::vector<std::string> listIds(const std::string &dir) {
  std::vector<std::string> ids;
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  reportDirError(dir, ec); // capture the construction error before the loop
  for (const auto &entry : it) {
    const std::string name = entry.path().filename().string();
    if (hasDcmSuffix(name))
      ids.push_back(name.substr(0, name.size() - 4));
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<std::string> listSubdirs(const std::string &dir) {
  std::vector<std::string> names;
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  reportDirError(dir, ec);
  for (const auto &entry : it) {
    std::error_code dec; // per-entry; keep it off the construction error above
    const std::string name = entry.path().filename().string();
    // dot-names are private staging (a batch link-tree being built, see
    // linkBatchTree), never messages or configuration — skip them so a
    // half-built tree is not walkable as a queue entry
    if (entry.is_directory(dec) && name.rfind('.', 0) != 0)
      names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::pair<std::string, int>>
routeQueueDirs(const Spool &sp, const std::string &dest) {
  std::vector<std::pair<std::string, int>> dirs;
  dirs.emplace_back(sp.routeTodo(dest), 0); // todo/ is rung 0: always due
  // listSubdirs sorts lexically, which would walk retry/10 before retry/2;
  // the contract is ascending rung order, so collect the rung numbers and
  // sort them numerically. Accept only canonical positive integers — the
  // rung path is rebuilt from the number, so a non-canonical name ("02") or
  // a malformed one ("2x", "tmp") is ignored rather than mis-targeted.
  std::vector<int> rungs;
  for (const auto &lvl : listSubdirs(sp.routeRetryRoot(dest))) {
    const int k = atoi(lvl.c_str());
    if (k >= 1 && std::to_string(k) == lvl)
      rungs.push_back(k);
  }
  std::sort(rungs.begin(), rungs.end());
  for (const int k : rungs)
    dirs.emplace_back(sp.routeRetry(dest, k), k);
  return dirs;
}

bool copyFile(const std::string &src, const std::string &dst,
              std::string &err) {
  const int in = open(src.c_str(), O_RDONLY);
  if (in < 0) {
    err = "cannot open '" + src + "': " + strerror(errno);
    return false;
  }
  const int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) {
    err = "cannot create '" + dst + "': " + strerror(errno);
    close(in);
    return false;
  }
  // copyFile is all-or-nothing: on any failure after dst is created, remove it
  // so a truncated file is never left behind. Callers publish the result with
  // commitFile/rename and trust a present file to be complete (a half-written
  // maildir or retry-rung copy a crash or ENOSPC leaves would corrupt
  // delivery).
  auto fail = [&](const std::string &message) {
    err = message;
    close(in);
    close(out);
    unlink(dst.c_str());
    return false;
  };
  char buf[65536];
  ssize_t got;
  while ((got = read(in, buf, sizeof(buf))) > 0) {
    ssize_t done = 0;
    while (done < got) {
      const ssize_t put = write(out, buf + done, got - done);
      if (put < 0)
        return fail("write error on '" + dst + "': " + strerror(errno));
      done += put;
    }
  }
  if (got < 0)
    return fail("read error on '" + src + "': " + strerror(errno));
  close(in);
  close(out);
  return true;
}

time_t idTime(const std::string &id) {
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  if (sscanf(id.c_str(), "%4d%2d%2d%2d%2d%2d", &tm.tm_year, &tm.tm_mon,
             &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
    return 0;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  return timegm(&tm);
}

std::string isoTime(time_t t) {
  struct tm tm;
  gmtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

time_t parseIsoTime(const std::string &s) {
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  if (!strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm))
    return 0;
  return timegm(&tm);
}

long long freeBytes(const std::string &path) {
  struct statvfs vfs;
  if (statvfs(path.c_str(), &vfs) != 0)
    return -1;
  return static_cast<long long>(vfs.f_bavail) * vfs.f_frsize;
}

bool parseWholeInt(const std::string &s, long &out) {
  const char *begin = s.data();
  const char *end = begin + s.size();
  long value = 0;
  const auto res = std::from_chars(begin, end, value);
  if (res.ec != std::errc() || res.ptr != end)
    return false;
  out = value;
  return true;
}

void logmsg(const std::string &prog, const std::string &msg) {
  std::fprintf(stderr, "%s: %s\n", prog.c_str(), msg.c_str());
}

bool pathExists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

bool isDir(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static const long BACKOFF_BASE = 420;  // ~7 minutes
static const long BACKOFF_CAP = 21600; // 6 hours

long retryBackoff(int level) {
  if (level <= 0)
    return 0; // todo/: always due
  // wait grows quadratically with the rung number (~7 min at rung 1,
  // reaching the 6-hour cap by rung 8), so early retries are quick and
  // late ones are spaced out
  const long w = BACKOFF_BASE * static_cast<long>(level) * level;
  return w > BACKOFF_CAP ? BACKOFF_CAP : w;
}

bool writeKeyValueCommitted(const Spool &sp, const KeyValueFile &kv,
                            const std::string &finalPath, std::string &err) {
  const std::string tmp = sp.queueTmp() + "/" +
                          finalPath.substr(finalPath.rfind('/') + 1) + ".w" +
                          std::to_string(getpid());
  if (!kv.write(tmp, err))
    return false;
  if (!commitFile(tmp, finalPath, err)) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

DestStatus readDestStatus(const Spool &sp, const std::string &dest) {
  DestStatus st;
  KeyValueFile kv;
  std::string err;
  if (!KeyValueFile::read(sp.routeStatus(dest), kv, err))
    return st; // no status file => not present, not backed off
  st.present = true;
  st.nextAttempt = parseIsoTime(kv.get("next-attempt-after"));
  st.lastFailure = kv.get("last-failure");
  return st;
}

} // namespace dicomq
