// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// dicomq-queue — queue inspection (postqueue/qshape analog).
//
//   dicomq-queue [-s <spool>] [<DEST>]
//
// Without arguments: one line per queue — counts, due counts, oldest
// message age, hold flags, destination backoff status, and retry-rung
// breakdown. With a destination name: one line per queued message (id,
// age, retry rung, AETs). Strictly read-only; safe for any user with
// read access.
//
// Reads each message's file-meta header (0002,0016/0018) to show its
// AETs in the per-destination listing — a routed message carries no
// sidecar that records them. The summary view opens no DICOM.

#include "dcmtk/config/osconfig.h"

#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/dcmdata/dcmetinf.h"
#include "dcmtk/dcmdata/dcxfer.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "common/kvfile.h"
#include "common/message.h"
#include "common/spool.h"

namespace fs = std::filesystem;
using namespace dicomq;

static Spool sp;

static std::string humanAge(long s)
{
  char buf[32];
  if (s < 0)
    s = 0;
  if (s < 120)
    snprintf(buf, sizeof(buf), "%lds", s);
  else if (s < 7200)
    snprintf(buf, sizeof(buf), "%ldm", s / 60);
  else if (s < 172800)
    snprintf(buf, sizeof(buf), "%ldh", s / 3600);
  else
    snprintf(buf, sizeof(buf), "%ldd", s / 86400);
  return buf;
}

struct QueueStats {
  size_t count = 0, due = 0;
  long oldest = -1;
};

// Fold the messages in dir (sitting at retry rung `level`) into q.
static void scanInto(QueueStats& q, const std::string& dir, int level,
                     time_t now)
{
  for (const auto& id : listIds(dir))
  {
    q.count++;
    const long age = static_cast<long>(now - idTime(id));
    if (age > q.oldest)
      q.oldest = age;
    if (messageDue(dir, id, level, now))
      q.due++;
  }
}

// Count .dcm files under dir, recursing (hold/ mirrors origin subpaths).
static size_t countDcm(const std::string& dir)
{
  size_t n = 0;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(dir, ec))
  {
    if (!e.is_regular_file(ec))
      continue;
    const std::string nm = e.path().filename().string();
    if (nm.size() > 4 && nm.compare(nm.size() - 4, 4, ".dcm") == 0)
      n++;
  }
  return n;
}

static void summaryLine(const std::string& label, const QueueStats& q,
                        const std::string& extra)
{
  std::printf("%-20s %4zu message%s", label.c_str(), q.count,
              q.count == 1 ? " " : "s");
  if (q.count)
    std::printf("  (%zu due)  oldest %s", q.due, humanAge(q.oldest).c_str());
  if (!extra.empty())
    std::printf("  %s", extra.c_str());
  std::printf("\n");
}

// Read Source/Receiving AET from a message's file-meta header.
static void readAETs(const std::string& path, std::string& from,
                     std::string& to)
{
  DcmFileFormat ff;
  if (ff.loadFile(path.c_str(), EXS_Unknown, EGL_noChange, DCM_MaxReadLength,
                  ERM_metaOnly).bad())
    return;
  DcmMetaInfo *m = ff.getMetaInfo();
  if (!m)
    return;
  OFString s;
  if (m->findAndGetOFString(DCM_SourceApplicationEntityTitle, s).good())
    from = s.c_str();
  if (m->findAndGetOFString(DCM_ReceivingApplicationEntityTitle, s).good())
    to = s.c_str();
}

// todo/ (rung 0) then each retry/<k> rung, oldest schedule first.
static std::vector<std::pair<std::string, int>> destDirs(const std::string& d)
{
  std::vector<std::pair<std::string, int>> dirs;
  dirs.emplace_back(sp.routeTodo(d), 0);
  for (const auto& lvl : listSubdirs(sp.routeRetryRoot(d)))
  {
    const int k = atoi(lvl.c_str());
    if (k >= 1)
      dirs.emplace_back(sp.routeRetry(d, k), k);
  }
  return dirs;
}

static void listMessages(const std::string& dest)
{
  const time_t now = time(nullptr);
  for (const auto& d : destDirs(dest))
    for (const auto& id : listIds(d.first))
    {
      std::string from = "?", to = "?";
      readAETs(dcmPath(d.first, id), from, to);
      std::printf("%s  age %-4s  retry %d  %s -> %s\n", id.c_str(),
                  humanAge(static_cast<long>(now - idTime(id))).c_str(),
                  d.second, from.c_str(), to.c_str());
    }
}

static void destSummary(const std::string& dest)
{
  const time_t now = time(nullptr);
  QueueStats q;
  std::string rungs;
  for (const auto& d : destDirs(dest))
  {
    QueueStats r;
    scanInto(r, d.first, d.second, now);
    if (d.second >= 1 && r.count)
      rungs += " L" + std::to_string(d.second) + ":" + std::to_string(r.count);
    q.count += r.count;
    q.due += r.due;
    if (r.oldest > q.oldest)
      q.oldest = r.oldest;
  }
  std::string extra;
  if (pathExists(sp.routeHoldFlag(dest)))
    extra += "[held] ";
  KeyValueFile status;
  std::string err;
  if (KeyValueFile::read(sp.routeStatus(dest), status, err))
    extra += "[down until " + status.get("next-attempt-after") + ": "
             + status.get("last-failure") + "] ";
  if (!rungs.empty())
    extra += "[retry" + rungs + "]";
  summaryLine("route/" + dest, q, extra);
}

int main(int argc, char **argv)
{
  std::string spoolArg, destArg;
  int opt;
  while ((opt = getopt(argc, argv, "s:")) != -1)
  {
    if (opt == 's')
      spoolArg = optarg;
    else
    {
      std::fprintf(stderr, "usage: dicomq-queue [-s <spool>] [<DEST>]\n");
      return 100;
    }
  }
  if (optind < argc)
    destArg = argv[optind];

  sp = Spool(spoolArg);

  if (!destArg.empty())
  {
    listMessages(destArg);
    return 0;
  }

  const time_t now = time(nullptr);
  for (const auto& aet : listSubdirs(sp.queueTodo()))
  {
    QueueStats q;
    scanInto(q, sp.queueTodoAET(aet), 0, now);
    summaryLine("queue/todo/" + aet, q, "");
  }
  for (const auto& dest : listSubdirs(sp.routeRoot()))
    destSummary(dest);
  std::printf("%-20s %4zu messages\n", "hold", countDcm(sp.holdDir()));
  std::printf("%-20s %4zu messages\n", "corrupt", countDcm(sp.corruptDir()));
  std::printf("%-20s %4zu messages\n", "failed", countDcm(sp.failedDir()));
  return 0;
}
