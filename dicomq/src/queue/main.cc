// dicomq-queue — queue inspection (postqueue/qshape analog).
//
//   dicomq-queue [-s <spool>] [<DEST>]
//
// Without arguments: one line per queue — counts, due counts, oldest
// message age, hold flags, and destination backoff status. With a
// destination name: one line per queued message (id, age, attempts,
// AETs). Strictly read-only; safe for any user with read access.
//
// Speaks no DICOM; links only dicomq-common.

#include <cstdio>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "common/envelope.h"
#include "common/message.h"
#include "common/spool.h"

using namespace dicomq;

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

static QueueStats scan(const std::string& dir)
{
  QueueStats q;
  const time_t now = time(nullptr);
  for (const auto& id : listIds(dir))
  {
    q.count++;
    const long age = static_cast<long>(now - idTime(id));
    if (age > q.oldest)
      q.oldest = age;
    struct stat st;
    if (stat(envPath(dir, id).c_str(), &st) == 0
        && isDue(now, st.st_mtime, idTime(id)))
      q.due++;
  }
  return q;
}

static void summaryLine(const std::string& label, const QueueStats& q,
                        const std::string& extra)
{
  std::printf("%-16s %4zu message%s", label.c_str(), q.count,
              q.count == 1 ? " " : "s");
  if (q.count)
  {
    std::printf("  (%zu due)  oldest %s", q.due, humanAge(q.oldest).c_str());
  }
  if (!extra.empty())
    std::printf("  %s", extra.c_str());
  std::printf("\n");
}

static void listMessages(const std::string& dir)
{
  const time_t now = time(nullptr);
  for (const auto& id : listIds(dir))
  {
    Envelope env;
    std::string err;
    size_t attempts = 0;
    std::string from = "?", to = "?";
    if (Envelope::read(envPath(dir, id), env, err))
    {
      from = env.get("calling-aet");
      to = env.get("called-aet");
      for (const auto& f : env.fields)
        if (f.first == "attempt")
          attempts++;
    }
    std::printf("%s  age %-4s  attempts %zu  %s -> %s\n", id.c_str(),
                humanAge(static_cast<long>(now - idTime(id))).c_str(),
                attempts, from.c_str(), to.c_str());
  }
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

  const Spool sp(spoolArg);

  if (!destArg.empty())
  {
    listMessages(sp.routeTodo(destArg));
    return 0;
  }

  summaryLine("queue/todo", scan(sp.queueTodo()), "");
  for (const auto& dest : listSubdirs(sp.routeRoot()))
  {
    std::string extra;
    struct stat st;
    if (stat(sp.routeHoldFlag(dest).c_str(), &st) == 0)
      extra += "[held] ";
    Envelope status;
    std::string err;
    if (Envelope::read(sp.routeStatus(dest), status, err))
    {
      extra += "[down until " + status.get("next-attempt-after") + ": "
               + status.get("last-failure") + "]";
    }
    summaryLine("route/" + dest, scan(sp.routeTodo(dest)), extra);
  }
  summaryLine("hold", scan(sp.holdDir()), "");
  summaryLine("corrupt", scan(sp.corruptDir()), "");
  summaryLine("failed", scan(sp.failedDir()), "");
  return 0;
}
