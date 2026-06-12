// dicomq-super — queue surgery (postsuper analog). Runs as the send
// user.
//
//   dicomq-super [-s <spool>] hold <id>            freeze a message in hold/
//   dicomq-super [-s <spool>] release <id>         return it whence it came
//   dicomq-super [-s <spool>] requeue <id>         back into queue/todo/ for
//                                                  fresh routing
//   dicomq-super [-s <spool>] fail <id> [reason]   move into failed/
//
// The message is found by searching queue/todo/, every route/<DEST>/todo/,
// hold/, corrupt/, and failed/. Every move uses the standard discipline
// (object first, envelope last in; envelope first out) and is idempotent
// via unique ids. hold records the source queue in a "held-from:"
// envelope line; release reads the most recent one. Nothing is ever
// deleted; removal from hold/, corrupt/, or failed/ is the operator's
// own rm.
//
// Speaks no DICOM; links only dicomq-common.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "common/envelope.h"
#include "common/message.h"
#include "common/spool.h"

using namespace dicomq;

static int usage()
{
  std::fprintf(stderr,
      "usage: dicomq-super [-s <spool>] hold|release|requeue <id>\n"
      "       dicomq-super [-s <spool>] fail <id> [reason]\n");
  return 100;
}

// queues a message can live in, as root-relative names
static std::vector<std::string> allQueues(const Spool& sp)
{
  std::vector<std::string> dirs{"queue/todo", "hold", "corrupt", "failed"};
  for (const auto& dest : listSubdirs(sp.routeRoot()))
    dirs.push_back("route/" + dest + "/todo");
  return dirs;
}

static std::string findMessage(const Spool& sp, const std::string& id)
{
  for (const auto& rel : allQueues(sp))
  {
    struct stat st;
    if (stat(envPath(sp.root + "/" + rel, id).c_str(), &st) == 0)
      return rel;
  }
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

  const Spool sp(spoolArg);
  std::string err;

  const std::string from = findMessage(sp, id);
  if (from.empty())
  {
    std::fprintf(stderr, "dicomq-super: no message '%s' in any queue\n",
                 id.c_str());
    return 111;
  }
  const std::string fromDir = sp.root + "/" + from;

  Envelope env;
  const bool parsed = Envelope::read(envPath(fromDir, id), env, err);

  if (verb == "hold")
  {
    if (from == "hold")
      return 0;  // already held: idempotent
    if (!parsed)
    {
      std::fprintf(stderr, "dicomq-super: cannot hold '%s': %s\n", id.c_str(),
                   err.c_str());
      return 111;
    }
    env.add("held-from", from);
    if (!movePairAnnotated(sp, fromDir, sp.holdDir(), id, env, err))
    {
      std::fprintf(stderr, "dicomq-super: %s\n", err.c_str());
      return 111;
    }
    std::printf("held %s (from %s)\n", id.c_str(), from.c_str());
    return 0;
  }

  if (verb == "release")
  {
    if (from != "hold")
    {
      std::fprintf(stderr, "dicomq-super: '%s' is not in hold/\n", id.c_str());
      return 111;
    }
    std::string target;
    for (const auto& f : env.fields)  // most recent held-from wins
      if (f.first == "held-from")
        target = f.second;
    bool known = false;
    for (const auto& rel : allQueues(sp))
      if (rel == target)
        known = true;
    if (!parsed || !known)
    {
      std::fprintf(stderr,
          "dicomq-super: '%s' has no usable held-from line; use requeue\n",
          id.c_str());
      return 111;
    }
    if (!movePairAnnotated(sp, fromDir, sp.root + "/" + target, id, env, err))
    {
      std::fprintf(stderr, "dicomq-super: %s\n", err.c_str());
      return 111;
    }
    std::printf("released %s (to %s)\n", id.c_str(), target.c_str());
    return 0;
  }

  if (verb == "requeue")
  {
    if (from == "queue/todo")
      return 0;  // idempotent
    if (!movePairRaw(sp, fromDir, sp.queueTodo(), id, err))
    {
      std::fprintf(stderr, "dicomq-super: %s\n", err.c_str());
      return 111;
    }
    std::printf("requeued %s (from %s)\n", id.c_str(), from.c_str());
    return 0;
  }

  if (verb == "fail")
  {
    if (from == "failed")
      return 0;  // idempotent
    std::string reason = "failed by operator";
    if (argc - optind > 2)
      reason = argv[optind + 2];
    if (parsed)
    {
      env.add("failed", isoTime(time(nullptr)) + " " + reason);
      if (!movePairAnnotated(sp, fromDir, sp.failedDir(), id, env, err))
      {
        std::fprintf(stderr, "dicomq-super: %s\n", err.c_str());
        return 111;
      }
    }
    else if (!movePairRaw(sp, fromDir, sp.failedDir(), id, err))
    {
      std::fprintf(stderr, "dicomq-super: %s\n", err.c_str());
      return 111;
    }
    std::printf("failed %s (from %s)\n", id.c_str(), from.c_str());
    return 0;
  }

  return usage();
}
