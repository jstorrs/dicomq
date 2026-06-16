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

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "common/dcm.h"
#include "common/kvfile.h"
#include "common/message.h"
#include "common/spool.h"

namespace fs = std::filesystem;
using namespace dicomq;

static Spool sp;

static std::string humanAge(long s) {
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

// Fold the messages in dir (sitting at retry rung `level`) into q. A batch
// counts as one message (its objects are one study/series unit).
static void scanInto(QueueStats &q, const std::string &dir, int level,
                     time_t now) {
  for (const auto &m : listMessages(dir)) {
    q.count++;
    const long age = static_cast<long>(now - idTime(m.id));
    if (age > q.oldest)
      q.oldest = age;
    if (messageDue(dir, m.id, level, now, m.isBatch))
      q.due++;
  }
}

// Count .dcm files under dir, recursing (hold/ mirrors origin subpaths).
static size_t countDcm(const std::string &dir) {
  size_t n = 0;
  std::error_code ec;
  for (const auto &e : fs::recursive_directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec) && hasDcmSuffix(e.path().filename().string()))
      n++;
  }
  return n;
}

static void summaryLine(const std::string &label, const QueueStats &q,
                        const std::string &extra) {
  std::printf("%-20s %4zu message%s", label.c_str(), q.count,
              q.count == 1 ? " " : "s");
  if (q.count)
    std::printf("  (%zu due)  oldest %s", q.due, humanAge(q.oldest).c_str());
  if (!extra.empty())
    std::printf("  %s", extra.c_str());
  std::printf("\n");
}

// Read Source/Receiving AET from a message's file-meta header, leaving a
// field untouched (the caller's "?" placeholder) when the tag is absent.
static void readAETs(const std::string &path, std::string &from,
                     std::string &to) {
  FileMeta m;
  if (!readFileMeta(path, m))
    return;
  if (!m.sourceAET.empty())
    from = m.sourceAET;
  if (!m.receivingAET.empty())
    to = m.receivingAET;
}

static void listDestMessages(const std::string &dest) {
  const time_t now = time(nullptr);
  for (const auto &d : routeQueueDirs(sp, dest))
    for (const auto &m : listMessages(d.first)) {
      std::string from = "?", to = "?", suffix;
      if (m.isBatch) {
        // a batch carries its AETs (and routing) on every member; read the
        // first, and note the object count
        const std::string bdir = messagePath(d.first, m.id, true);
        const auto objs = listIds(bdir);
        if (!objs.empty())
          readAETs(dcmPath(bdir, objs.front()), from, to);
        suffix = "  [batch: " + std::to_string(objs.size()) + " objects]";
      } else
        readAETs(dcmPath(d.first, m.id), from, to);
      std::printf("%s  age %-4s  retry %d  %s -> %s%s\n", m.id.c_str(),
                  humanAge(static_cast<long>(now - idTime(m.id))).c_str(),
                  d.second, from.c_str(), to.c_str(), suffix.c_str());
    }
}

static void destSummary(const std::string &dest) {
  const time_t now = time(nullptr);
  QueueStats q;
  std::string rungs;
  for (const auto &d : routeQueueDirs(sp, dest)) {
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
    extra += "[down until " + status.get("next-attempt-after") + ": " +
             status.get("last-failure") + "] ";
  if (!rungs.empty())
    extra += "[retry" + rungs + "] ";
  // per-destination terminal sinks (object counts, like the global hold/failed
  // lines below); shown only when non-empty to keep idle queues quiet
  if (const size_t n = countDcm(sp.routeComplete(dest)))
    extra += "[complete " + std::to_string(n) + "] ";
  if (const size_t n = countDcm(sp.routeFailed(dest)))
    extra += "[failed " + std::to_string(n) + "] ";
  if (const size_t n = countDcm(sp.routeCorrupt(dest)))
    extra += "[corrupt " + std::to_string(n) + "] ";
  summaryLine("route/" + dest, q, extra);
}

int main(int argc, char **argv) {
  std::string spoolArg, destArg;
  int opt;
  while ((opt = getopt(argc, argv, "s:")) != -1) {
    if (opt == 's')
      spoolArg = optarg;
    else {
      std::fprintf(stderr, "usage: dicomq-queue [-s <spool>] [<DEST>]\n");
      return 100;
    }
  }
  if (optind < argc)
    destArg = argv[optind];

  sp = Spool(spoolArg);

  if (!destArg.empty()) {
    listDestMessages(destArg);
    return 0;
  }

  const time_t now = time(nullptr);
  for (const auto &aet : listSubdirs(sp.queueTodo())) {
    QueueStats q;
    scanInto(q, sp.queueTodoAET(aet), 0, now);
    summaryLine("queue/todo/" + aet, q, "");
  }
  for (const auto &dest : listSubdirs(sp.routeRoot()))
    destSummary(dest);
  std::printf("%-20s %4zu messages\n", "hold", countDcm(sp.holdDir()));
  std::printf("%-20s %4zu messages\n", "failed", countDcm(sp.failedDir()));
  return 0;
}
