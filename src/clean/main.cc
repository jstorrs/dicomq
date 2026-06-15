// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// dicomq-clean — tmp reaper (qmail-clean analog). Run from a timer/cron.
//
//   dicomq-clean [-s <spool>] [-g <grace-hours>]
//
// Removes queue/tmp/ entries older than the grace period (default 36
// hours): interrupted receive/inject/transcode writes that never
// committed. A committed message is a single <id>.dcm placed by one
// atomic rename, so the queues never hold half-written objects and there
// are no sidecar orphans to reap. Touches nothing younger and nothing
// outside queue/tmp/. Reports every removal on stdout.
//
// Speaks no DICOM; links only dicomq-common.

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

int main(int argc, char **argv) {
  std::string spoolArg;
  long graceHours = 36;
  int opt;
  while ((opt = getopt(argc, argv, "s:g:")) != -1) {
    switch (opt) {
    case 's':
      spoolArg = optarg;
      break;
    case 'g':
      graceHours = atol(optarg);
      break;
    default:
      std::fprintf(stderr,
                   "usage: dicomq-clean [-s <spool>] [-g <grace-hours>]\n");
      return 100;
    }
  }
  if (graceHours < 0) {
    // a negative grace pushes the cutoff into the future, which would
    // reap freshly written tmp/ and not-yet-committed objects
    std::fprintf(stderr, "dicomq-clean: -g must not be negative\n");
    return 100;
  }

  const Spool sp(spoolArg);
  cutoff = time(nullptr) - graceHours * 3600;

  cleanTmp(sp.queueTmp());

  return problems ? 111 : 0;
}
