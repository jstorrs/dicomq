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
// The message — a single <id>.dcm — is found by searching queue/todo/*/,
// every route/<DEST>/{todo,retry/*}/, hold/, corrupt/, and failed/. Every
// move is one atomic rename and is idempotent via unique ids. hold
// remembers a message's origin by MIRRORING its source path under hold/
// (hold/route/PACS1/retry/2/<id>.dcm), so release recovers the origin
// from the path — no sidecar. requeue reads the called AET from the
// file-meta header (0002,0018) to choose queue/todo/<AET>/. Reasons are
// logged, not stored. Nothing is ever deleted; removal from hold/,
// corrupt/, or failed/ is the operator's own rm.

#include "dcmtk/config/osconfig.h"

#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/dcmdata/dcmetinf.h"
#include "dcmtk/dcmdata/dcxfer.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "common/message.h"
#include "common/spool.h"

namespace fs = std::filesystem;
using namespace dicomq;

static Spool sp;

static int usage()
{
  std::fprintf(stderr,
      "usage: dicomq-ctl [-s <spool>] hold|release|requeue <id>\n"
      "       dicomq-ctl [-s <spool>] fail <id> [reason]\n");
  return 100;
}

// Flat queues a message can live in, as root-relative names. hold/ is
// searched separately because it mirrors arbitrary origin subpaths.
static std::vector<std::string> allQueues()
{
  std::vector<std::string> dirs;
  for (const auto& aet : listSubdirs(sp.queueTodo()))
    dirs.push_back("queue/todo/" + aet);
  for (const auto& dest : listSubdirs(sp.routeRoot()))
  {
    dirs.push_back("route/" + dest + "/todo");
    for (const auto& lvl : listSubdirs(sp.routeRetryRoot(dest)))
      dirs.push_back("route/" + dest + "/retry/" + lvl);
  }
  dirs.push_back("corrupt");
  dirs.push_back("failed");
  return dirs;
}

// Root-relative directory holding <id>.dcm, or "" if not found. A match
// under hold/ comes back as e.g. "hold/route/PACS1/todo" — the mirrored
// origin is part of the path.
static std::string findMessage(const std::string& id)
{
  for (const auto& rel : allQueues())
    if (pathExists(dcmPath(sp.root + "/" + rel, id)))
      return rel;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(sp.holdDir(), ec))
    if (e.is_regular_file(ec) && e.path().filename() == id + ".dcm")
      return e.path().parent_path().string().substr(sp.root.size() + 1);
  return "";
}

// Called AET from the file-meta header (0002,0018), sanitized for a path.
static std::string readCalledAET(const std::string& path)
{
  DcmFileFormat ff;
  if (ff.loadFile(path.c_str(), EXS_Unknown, EGL_noChange, DCM_MaxReadLength,
                  ERM_metaOnly).bad())
    return "";
  DcmMetaInfo *m = ff.getMetaInfo();
  OFString s;
  if (m && m->findAndGetOFString(DCM_ReceivingApplicationEntityTitle, s).good())
    return sanitizeAET(s.c_str());
  return "";
}

int main(int argc, char **argv)
{
  std::string spoolArg;
  int opt;
  while ((opt = getopt(argc, argv, "s:")) != -1)
  {
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

  const std::string from = findMessage(id);
  if (from.empty())
  {
    std::fprintf(stderr, "dicomq-ctl: no message '%s' in any queue\n",
                 id.c_str());
    return 111;
  }
  const std::string fromDir = sp.root + "/" + from;

  if (verb == "hold")
  {
    if (from.rfind("hold", 0) == 0)
      return 0;  // already held: idempotent
    // mirror the origin path under hold/ so release can recover it
    const std::string toDir = sp.holdDir() + "/" + from;
    std::error_code ec;
    fs::create_directories(toDir, ec);
    if (!moveMessage(fromDir, toDir, id, err))
    {
      std::fprintf(stderr, "dicomq-ctl: %s\n", err.c_str());
      return 111;
    }
    std::printf("held %s (from %s)\n", id.c_str(), from.c_str());
    return 0;
  }

  if (verb == "release")
  {
    if (from.rfind("hold/", 0) != 0)
    {
      std::fprintf(stderr, "dicomq-ctl: '%s' is not in hold/\n", id.c_str());
      return 111;
    }
    const std::string origin = from.substr(5);  // strip "hold/"
    const std::string toDir = sp.root + "/" + origin;
    std::error_code ec;
    fs::create_directories(toDir, ec);
    if (!moveMessage(fromDir, toDir, id, err))
    {
      std::fprintf(stderr, "dicomq-ctl: %s\n", err.c_str());
      return 111;
    }
    std::printf("released %s (to %s)\n", id.c_str(), origin.c_str());
    return 0;
  }

  if (verb == "requeue")
  {
    const std::string aet = readCalledAET(dcmPath(fromDir, id));
    if (aet.empty())
    {
      std::fprintf(stderr,
          "dicomq-ctl: cannot read a called AET from '%s'\n", id.c_str());
      return 111;
    }
    const std::string toDir = sp.queueTodoAET(aet);
    if (from == "queue/todo/" + aet)
      return 0;  // idempotent
    if (!mkdirIfMissing(toDir, err) || !moveMessage(fromDir, toDir, id, err))
    {
      std::fprintf(stderr, "dicomq-ctl: %s\n", err.c_str());
      return 111;
    }
    std::printf("requeued %s (from %s, to queue/todo/%s)\n", id.c_str(),
                from.c_str(), aet.c_str());
    return 0;
  }

  if (verb == "fail")
  {
    if (from == "failed")
      return 0;  // idempotent
    std::string reason = "failed by operator";
    if (argc - optind > 2)
      reason = argv[optind + 2];
    if (!moveMessage(fromDir, sp.failedDir(), id, err))
    {
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
