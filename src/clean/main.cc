// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// dicomq-clean — tmp reaper (qmail-clean analog). Run from a timer/cron.
//
//   dicomq-clean [-s <spool>] [-g <grace-hours>] [-G <complete-grace-hours>]
//
// Removes queue/tmp/ entries older than -g (default 36 hours): interrupted
// receive/inject/transcode writes that never committed. A committed message
// is a single <id>.dcm placed by one atomic rename, so the queues never hold
// half-written objects and there are no sidecar orphans to reap.
//
// Also reaps route/<DEST>/complete/ — messages a destination's queue runner
// finished forwarding and moved aside — once their message-id timestamp is
// older than -G (default 72 hours). complete/ is the recently-delivered
// audit/recovery window; -G 0 clears it every pass. (failed/ and corrupt/
// are operator-managed and never auto-reaped.)
//
// Finally empties trash/ — batch directories a discard renamed aside for
// deletion but a crash left behind (normally deleted inline). These are
// already dequeued, so they are reaped unconditionally.
//
// Reports every removal on stdout. Speaks no DICOM; links only
// dicomq-common.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "common/spool.h"

namespace fs = std::filesystem;
using namespace dicomq;

static time_t cutoff;
static int problems = 0;

static void reap(const std::string &path) {
  if (unlink(path.c_str()) == 0)
    std::printf("removed %s\n", path.c_str());
  else if (errno != ENOENT) {
    std::fprintf(stderr, "dicomq-clean: cannot remove '%s': %s\n", path.c_str(),
                 strerror(errno));
    problems++;
  }
}

static bool olderThanCutoff(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && st.st_mtime < cutoff;
}

static void cleanTmp(const std::string &dir) {
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    const std::string p = entry.path().string();
    if (entry.is_regular_file(ec) && olderThanCutoff(p))
      reap(p);
  }
}

// Remove a delivered message from a complete/ dir — a single <id>.dcm or a
// batch directory <id>/ — recursively. Age is taken from the message-id
// timestamp (idTime), as everywhere else in dicomq, not the file mtime,
// which a rename preserves and would not reflect time-in-complete/.
static void reapComplete(const Spool &sp, time_t completeCutoff) {
  for (const auto &dest : listSubdirs(sp.routeRoot())) {
    std::error_code ec;
    for (const auto &entry :
         fs::directory_iterator(sp.routeComplete(dest), ec)) {
      const std::string name = entry.path().filename().string();
      const std::string id =
          hasDcmSuffix(name) ? name.substr(0, name.size() - 4) : name;
      const time_t t = idTime(id);
      if (t == 0 || t >= completeCutoff)
        continue; // unparseable id, or not old enough — leave it
      const std::string p = entry.path().string();
      const auto n = fs::remove_all(p, ec);
      if (ec) {
        std::fprintf(stderr, "dicomq-clean: cannot remove '%s': %s\n",
                     p.c_str(), ec.message().c_str());
        problems++;
      } else if (n > 0)
        std::printf("removed %s\n", p.c_str());
    }
  }
}

// Reap trash/: a batch that a discard renamed aside but a crash left
// undeleted (the normal case deletes it inline). Each entry is already
// dequeued, so remove it unconditionally — no grace. Errors are ignored:
// the likeliest one is a concurrent discard removing the same entry, which
// is the outcome we wanted anyway.
static void reapTrash(const Spool &sp) {
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(sp.trashDir(), ec)) {
    const std::string p = entry.path().string();
    std::error_code rec;
    if (fs::remove_all(p, rec) > 0 && !rec)
      std::printf("removed %s\n", p.c_str());
  }
}

int main(int argc, char **argv) {
  std::string spoolArg;
  long graceHours = 36;
  long completeGraceHours = 72;
  int opt;
  while ((opt = getopt(argc, argv, "s:g:G:")) != -1) {
    switch (opt) {
    case 's':
      spoolArg = optarg;
      break;
    case 'g':
      if (!parseWholeInt(optarg, graceHours)) {
        std::fprintf(stderr, "dicomq-clean: -g must be a number of hours\n");
        return 100;
      }
      break;
    case 'G':
      if (!parseWholeInt(optarg, completeGraceHours)) {
        std::fprintf(stderr, "dicomq-clean: -G must be a number of hours\n");
        return 100;
      }
      break;
    default:
      std::fprintf(stderr,
                   "usage: dicomq-clean [-s <spool>] [-g <grace-hours>] "
                   "[-G <complete-grace-hours>]\n");
      return 100;
    }
  }
  if (graceHours < 0 || completeGraceHours < 0) {
    // a negative grace pushes the cutoff into the future, which would
    // reap freshly written tmp/ and not-yet-committed objects
    std::fprintf(stderr, "dicomq-clean: -g and -G must not be negative\n");
    return 100;
  }

  const Spool sp(spoolArg);
  const time_t now = time(nullptr);
  cutoff = now - graceHours * 3600;

  cleanTmp(sp.queueTmp());
  reapComplete(sp, now - completeGraceHours * 3600);
  reapTrash(sp);

  return problems ? 111 : 0;
}
