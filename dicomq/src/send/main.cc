// dicomq-send — queue runner (qmail-send analog). The one long-running
// process; owns all routing decisions.
//
//   dicomq-send [-s <spool>] [-i <scan-interval-seconds>] [--once]
//
// Each pass: route every committed message in queue/todo/ per its
// called AET's deliver file (maildir instructions run dicomq-local,
// forward instructions link into route/<DEST>/todo/), then trigger
// dicomq-remote <DEST> for every destination with due messages — at
// most one dicomq-remote per destination at a time, none while
// route/<DEST>/hold exists or route/<DEST>/status says the destination
// is backed off.
//
// Unparseable envelopes are quarantined to corrupt/; a message for an
// unknown called AET is failed; a message whose deliver instructions
// cannot be satisfied right now (missing maildir, unknown destination)
// is deferred in place with a logged reason.
//
// --once performs a single pass and waits for spawned agents — the
// testing and cron-driven mode. Speaks no DICOM.

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <poll.h>
#include <sys/inotify.h>
#endif

#include "common/envelope.h"
#include "common/message.h"
#include "common/profile.h"
#include "common/spool.h"

using namespace dicomq;

static Spool sp;
static std::map<std::string, pid_t> running;  // dest -> dicomq-remote pid

static void logmsg(const std::string& m)
{
  std::fprintf(stderr, "dicomq-send: %s\n", m.c_str());
}

static bool isDir(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool fileExists(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

// prefer the sibling binary (same directory as this executable) so the
// suite works from a build tree; fall back to PATH
static std::string siblingPath(const char *name)
{
  char self[PATH_MAX];
  const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (n > 0)
  {
    self[n] = '\0';
    std::string dir(self);
    const size_t slash = dir.rfind('/');
    if (slash != std::string::npos)
    {
      const std::string candidate = dir.substr(0, slash + 1) + name;
      if (fileExists(candidate))
        return candidate;
    }
  }
  return name;  // execvp will search PATH
}

static pid_t spawn(const char *prog, const std::vector<std::string>& args)
{
  const std::string path = siblingPath(prog);
  std::vector<char *> argv;
  argv.push_back(const_cast<char *>(path.c_str()));
  for (const auto& a : args)
    argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid == 0)
  {
    execvp(argv[0], argv.data());
    std::fprintf(stderr, "dicomq-send: cannot exec %s: %s\n", argv[0],
                 strerror(errno));
    _exit(111);
  }
  return pid;
}

static int runChild(const char *prog, const std::vector<std::string>& args)
{
  const pid_t pid = spawn(prog, args);
  if (pid < 0)
    return 111;
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 111;
}

static std::string resolveMaildir(const std::string& aetDir,
                                  const std::string& arg)
{
  if (!arg.empty() && arg[0] == '/')
    return arg;
  return aetDir + "/" + arg;
}

// wait for new work: inotify on queue/todo/ where available (the kernel
// queues events raised while we were busy routing, so no commit between
// scan and wait is lost), with the periodic scan as backstop everywhere
#ifdef __linux__
static int inotifyFd = -1;

static void watchTodo(const std::string& dir)
{
  inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotifyFd >= 0
      && inotify_add_watch(inotifyFd, dir.c_str(), IN_MOVED_TO) < 0)
  {
    close(inotifyFd);
    inotifyFd = -1;
  }
}

static void waitForWork(long intervalSeconds)
{
  if (inotifyFd < 0)
  {
    sleep(static_cast<unsigned>(intervalSeconds));
    return;
  }
  struct pollfd pfd = { inotifyFd, POLLIN, 0 };
  poll(&pfd, 1, static_cast<int>(intervalSeconds * 1000));
  char buf[4096];
  while (read(inotifyFd, buf, sizeof(buf)) > 0)
    ;  // drain; the scan that follows picks up everything
}
#else
static void watchTodo(const std::string&) {}
static void waitForWork(long intervalSeconds)
{
  sleep(static_cast<unsigned>(intervalSeconds));
}
#endif

// route one message; on success it leaves queue/todo/
static void processMessage(const std::string& id)
{
  std::string err;
  Envelope env;
  if (!Envelope::read(envPath(sp.queueTodo(), id), env, err))
  {
    logmsg("quarantining " + id + ": " + err);
    if (!movePairRaw(sp, sp.queueTodo(), sp.corruptDir(), id, err))
      logmsg("cannot quarantine " + id + ": " + err);
    return;
  }

  const std::string called = sanitizeAET(env.get("called-aet"));
  const std::string aetDir = sp.aetDir(called);
  if (env.get("called-aet").empty() || !isDir(aetDir))
  {
    logmsg("failing " + id + ": unknown called AET '" + env.get("called-aet")
           + "'");
    Envelope failed = env;
    failed.add("failed", isoTime(time(nullptr)) + " unknown called AET");
    if (!movePairAnnotated(sp, sp.queueTodo(), sp.failedDir(), id, failed, err))
      logmsg("cannot fail " + id + ": " + err);
    return;
  }

  std::vector<DeliverInstruction> instructions;
  if (!loadDeliver(aetDir + "/deliver", instructions, err))
  {
    logmsg("deferring " + id + ": " + err);
    return;
  }

  // validate every instruction before executing any, so a config typo
  // defers the whole message instead of half-delivering it
  for (const auto& in : instructions)
  {
    if (in.kind == DeliverInstruction::Kind::Forward)
    {
      if (!isDir(sp.destDir(in.arg)) || !isDir(sp.routeTodo(in.arg)))
      {
        logmsg("deferring " + id + ": destination '" + in.arg
               + "' has no dest/ config or route/ queue");
        return;
      }
    }
    else if (!isDir(resolveMaildir(aetDir, in.arg) + "/new"))
    {
      logmsg("deferring " + id + ": maildir '"
             + resolveMaildir(aetDir, in.arg) + "' has no new/");
      return;
    }
  }

  for (const auto& in : instructions)
  {
    if (in.kind == DeliverInstruction::Kind::Forward)
    {
      const std::string todo = sp.routeTodo(in.arg);
      if (!linkIdempotent(dcmPath(sp.queueTodo(), id), dcmPath(todo, id), err))
      {
        logmsg("deferring " + id + ": " + err);
        return;
      }
      // don't clobber an existing copy: a crashed pass may have routed
      // it already and dicomq-remote may have annotated attempts since
      if (!fileExists(envPath(todo, id)))
      {
        const std::string tmp = sp.queueTmp() + "/" + id + ".env.route";
        if (!copyFile(envPath(sp.queueTodo(), id), tmp, err)
            || !commitFile(tmp, envPath(todo, id), err))
        {
          logmsg("deferring " + id + ": " + err);
          unlink(tmp.c_str());
          return;
        }
      }
    }
    else
    {
      std::vector<std::string> args{id, resolveMaildir(aetDir, in.arg)};
      if (in.withEnv)
        args.push_back("env");
      const int rc = runChild("dicomq-local", args);
      if (rc != 0)
      {
        logmsg("deferring " + id + ": dicomq-local exited " +
               std::to_string(rc));
        return;
      }
    }
  }

  if (!removePair(sp.queueTodo(), id, err))
    logmsg("routed " + id + " but cannot dequeue: " + err);
}

static void reapAgents(bool block)
{
  while (!running.empty())
  {
    const pid_t pid = waitpid(-1, nullptr, block ? 0 : WNOHANG);
    if (pid <= 0)
      return;
    for (auto it = running.begin(); it != running.end(); ++it)
      if (it->second == pid)
      {
        running.erase(it);
        break;
      }
  }
}

static void maybeTrigger(const std::string& dest)
{
  if (running.count(dest))
    return;
  if (fileExists(sp.routeHoldFlag(dest)))
    return;

  std::string err;
  Envelope status;
  if (Envelope::read(sp.routeStatus(dest), status, err))
  {
    const time_t next = parseIsoTime(status.get("next-attempt-after"));
    if (next != 0 && time(nullptr) < next)
      return;  // destination-level backoff (dead-site cache)
  }

  const time_t now = time(nullptr);
  for (const auto& id : listIds(sp.routeTodo(dest)))
  {
    // never-attempted messages are always due; attempted ones follow
    // the backoff schedule keyed on the envelope copy's mtime
    Envelope env;
    bool due = true;
    if (Envelope::read(envPath(sp.routeTodo(dest), id), env, err)
        && env.count("attempt") > 0)
    {
      struct stat st;
      due = stat(envPath(sp.routeTodo(dest), id).c_str(), &st) == 0
            && isDue(now, st.st_mtime, idTime(id));
    }
    if (due)
    {
      const pid_t pid = spawn("dicomq-remote", {dest});
      if (pid > 0)
        running[dest] = pid;
      return;
    }
  }
}

int main(int argc, char **argv)
{
  std::string spoolArg;
  long interval = 10;
  bool once = false;
  for (int i = 1; i < argc; i++)
  {
    const std::string a = argv[i];
    if (a == "-s" && i + 1 < argc)
      spoolArg = argv[++i];
    else if (a == "-i" && i + 1 < argc)
      interval = atol(argv[++i]);
    else if (a == "--once")
      once = true;
    else
    {
      std::fprintf(stderr,
          "usage: dicomq-send [-s <spool>] [-i <seconds>] [--once]\n");
      return 100;
    }
  }

  sp = Spool(spoolArg);
  // agents inherit the spool through the environment
  setenv("DICOMQ_SPOOL", sp.root.c_str(), 1);

  if (!isDir(sp.queueTodo()) || !isDir(sp.queueTmp()))
  {
    std::fprintf(stderr, "dicomq-send: '%s' is not a dicomq spool\n",
                 sp.root.c_str());
    return 111;
  }

  if (!once)
    watchTodo(sp.queueTodo());

  for (;;)
  {
    reapAgents(false);
    for (const auto& id : listIds(sp.queueTodo()))
      processMessage(id);
    for (const auto& dest : listSubdirs(sp.routeRoot()))
      maybeTrigger(dest);
    if (once)
    {
      reapAgents(true);
      return 0;
    }
    waitForWork(interval);
  }
}
