// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// dicomq-ctl — queue surgery (postsuper analog). Runs as the send user.
//
//   dicomq-ctl [-s <spool>] hold <id>            freeze a message in hold/
//   dicomq-ctl [-s <spool>] release <id>         return it whence it came
//   dicomq-ctl [-s <spool>] requeue <id>         back into queue/todo/<AET>/
//                                                  for fresh routing
//   dicomq-ctl [-s <spool>] fail <id> [reason]   move into failed/
//
// The message — a single <id>.dcm, or a sealed study/series batch
// directory <id>/ — is found by searching queue/todo/*/, every
// route/<DEST>/{todo,retry/*,complete,failed,corrupt}/, hold/, and the
// global failed/ (pre-routing failures). Every move is one atomic rename
// (of a file or a directory) and is idempotent via unique ids. hold
// remembers a message's origin by MIRRORING its source path under hold/
// (hold/route/PACS1/retry/2/<id>.dcm or .../<id>/), so release recovers
// the origin from the path — no sidecar. requeue reads the called AET
// from the file-meta header (0002,0018) — of the message, or of a batch's
// first member — to choose queue/todo/<AET>/. Reasons are logged, not
// stored. Nothing is ever deleted; removal from hold/, the per-<DEST>
// corrupt/, or failed/ is the operator's own rm.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "common/dcm.h"
#include "common/message.h"
#include "common/spool.h"

namespace fs = std::filesystem;
using namespace dicomq;

static Spool sp;

static int usage() {
  std::fprintf(stderr,
               "usage: dicomq-ctl [-s <spool>] hold|release|requeue <id>\n"
               "       dicomq-ctl [-s <spool>] fail <id> [reason]\n");
  return 100;
}

// Flat queues a message can live in, as root-relative names. hold/ is
// searched separately because it mirrors arbitrary origin subpaths.
static std::vector<std::string> allQueues() {
  std::vector<std::string> dirs;
  for (const auto &aet : listSubdirs(sp.queueTodo()))
    dirs.push_back("queue/todo/" + aet);
  for (const auto &dest : listSubdirs(sp.routeRoot())) {
    dirs.push_back("route/" + dest + "/todo");
    for (const auto &lvl : listSubdirs(sp.routeRetryRoot(dest)))
      dirs.push_back("route/" + dest + "/retry/" + lvl);
    // per-destination terminal sinks: complete/ (still reapable, so requeue
    // can resend a just-delivered message), failed/, and corrupt/
    dirs.push_back("route/" + dest + "/complete");
    dirs.push_back("route/" + dest + "/failed");
    dirs.push_back("route/" + dest + "/corrupt");
  }
  dirs.push_back("failed");
  return dirs;
}

// Root-relative directory holding the message, or "" if not found, with
// isBatch set to whether it is a directory <id>/ (vs a file <id>.dcm). A
// match under hold/ comes back as e.g. "hold/route/PACS1/todo" — the
// mirrored origin is part of the path.
static std::string findMessage(const std::string &id, bool &isBatch) {
  for (const auto &rel : allQueues()) {
    const std::string base = sp.root + "/" + rel;
    if (pathExists(dcmPath(base, id))) {
      isBatch = false;
      return rel;
    }
    if (isDir(base + "/" + id)) {
      isBatch = true;
      return rel;
    }
  }
  std::error_code ec;
  for (const auto &e : fs::recursive_directory_iterator(sp.holdDir(), ec)) {
    const std::string rel =
        e.path().parent_path().string().substr(sp.root.size() + 1);
    if (e.is_regular_file(ec) && e.path().filename() == id + ".dcm") {
      isBatch = false;
      return rel;
    }
    if (e.is_directory(ec) && e.path().filename() == id) {
      isBatch = true;
      return rel;
    }
  }
  return "";
}

// Meta-bearing path for a message: the file itself, or a batch's first
// member object (every member carries the same routing AETs). "" if a
// batch has no readable member.
static std::string metaPath(const std::string &dir, const std::string &id,
                            bool isBatch) {
  if (!isBatch)
    return dcmPath(dir, id);
  const auto objs = listIds(dir + "/" + id);
  return objs.empty() ? "" : dcmPath(dir + "/" + id, objs.front());
}

// Called AET from the file-meta header (0002,0018), sanitized for a path.
// "" if the header cannot be read or carries no receiving AET.
static std::string readCalledAET(const std::string &path) {
  FileMeta m;
  if (!readFileMeta(path, m) || m.receivingAET.empty())
    return "";
  return sanitizeAET(m.receivingAET);
}

int main(int argc, char **argv) {
  std::string spoolArg;
  int opt;
  while ((opt = getopt(argc, argv, "s:")) != -1) {
    if (opt == 's')
      spoolArg = optarg;
    else
      return usage();
  }
  if (argc - optind < 2)
    return usage();
  const std::string verb = argv[optind];
  const std::string id = argv[optind + 1];

  sp = Spool(spoolArg);
  std::string err;

  bool isBatch = false;
  const std::string from = findMessage(id, isBatch);
  if (from.empty()) {
    std::fprintf(stderr, "dicomq-ctl: no message '%s' in any queue\n",
                 id.c_str());
    return 111;
  }
  const std::string fromDir = sp.root + "/" + from;

  if (verb == "hold") {
    if (from.rfind("hold", 0) == 0)
      return 0; // already held: idempotent
    // mirror the origin path under hold/ so release can recover it
    const std::string toDir = sp.holdDir() + "/" + from;
    if (!mkdirsUnder(sp.holdDir(), from, err)) {
      std::fprintf(stderr, "dicomq-ctl: %s\n", err.c_str());
      return 111;
    }
    if (!moveMessage(fromDir, toDir, id, err, isBatch)) {
      std::fprintf(stderr, "dicomq-ctl: %s\n", err.c_str());
      return 111;
    }
    std::printf("held %s (from %s)\n", id.c_str(), from.c_str());
    return 0;
  }

  if (verb == "release") {
    if (from.rfind("hold/", 0) != 0) {
      std::fprintf(stderr, "dicomq-ctl: '%s' is not in hold/\n", id.c_str());
      return 111;
    }
    const std::string origin = from.substr(5); // strip "hold/"
    const std::string toDir = sp.root + "/" + origin;
    if (!mkdirsUnder(sp.root, origin, err)) {
      std::fprintf(stderr, "dicomq-ctl: %s\n", err.c_str());
      return 111;
    }
    if (!moveMessage(fromDir, toDir, id, err, isBatch)) {
      std::fprintf(stderr, "dicomq-ctl: %s\n", err.c_str());
      return 111;
    }
    std::printf("released %s (to %s)\n", id.c_str(), origin.c_str());
    return 0;
  }

  if (verb == "requeue") {
    const std::string mp = metaPath(fromDir, id, isBatch);
    const std::string aet = mp.empty() ? "" : readCalledAET(mp);
    if (aet.empty()) {
      std::fprintf(stderr, "dicomq-ctl: cannot read a called AET from '%s'\n",
                   id.c_str());
      return 111;
    }
    const std::string toDir = sp.queueTodoAET(aet);
    if (from == "queue/todo/" + aet)
      return 0; // idempotent
    if (!mkdirIfMissing(toDir, err) ||
        !moveMessage(fromDir, toDir, id, err, isBatch)) {
      std::fprintf(stderr, "dicomq-ctl: %s\n", err.c_str());
      return 111;
    }
    std::printf("requeued %s (from %s, to queue/todo/%s)\n", id.c_str(),
                from.c_str(), aet.c_str());
    return 0;
  }

  if (verb == "fail") {
    if (from == "failed")
      return 0; // idempotent
    std::string reason = "failed by operator";
    if (argc - optind > 2)
      reason = argv[optind + 2];
    if (!moveMessage(fromDir, sp.failedDir(), id, err, isBatch)) {
      std::fprintf(stderr, "dicomq-ctl: %s\n", err.c_str());
      return 111;
    }
    std::fprintf(stderr, "dicomq-ctl: failed %s: %s\n", id.c_str(),
                 reason.c_str());
    std::printf("failed %s (from %s)\n", id.c_str(), from.c_str());
    return 0;
  }

  return usage();
}
