// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// dicomq-send — queue runner (qmail-send analog). The one long-running
// process; owns all routing decisions.
//
//   dicomq-send [-s <spool>] [-i <seconds>] [--once]
//
// Each pass: route every committed message in queue/todo/<called-AET>/
// per that AET's deliver file (maildir instructions run dicomq-local,
// forward instructions link into route/<DEST>/todo/), then trigger
// dicomq-remote <DEST> for every destination with a due message in its
// todo/ or retry/<k>/ rungs — at most one dicomq-remote per destination
// at a time, none while route/<DEST>/hold exists or route/<DEST>/status
// says the destination is backed off.
//
// A message for an unknown called AET is failed; one whose deliver
// instructions cannot be satisfied right now (missing maildir, unknown
// destination) is deferred in place with a logged reason.
//
// --once performs a single pass and waits for spawned agents — the
// testing and cron-driven mode. Speaks no DICOM — routing is purely by
// directory; the .dcm is never opened.

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <getopt.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <poll.h>
#include <sys/inotify.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "common/message.h"
#include "common/profile.h"
#include "common/spool.h"

using namespace dicomq;

static Spool sp;
static std::map<std::string, pid_t> running; // dest -> dicomq-remote pid

static void logmsg(const std::string &m) { dicomq::logmsg("dicomq-send", m); }

static std::string selfExecutable() {
#ifdef __APPLE__
  char buf[PATH_MAX];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0)
    return buf;
#else
  char buf[PATH_MAX];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    return buf;
  }
#endif
  return "";
}

// prefer the sibling binary (same directory as this executable) so the
// suite works from a build tree; fall back to PATH
static std::string siblingPath(const char *name) {
  const std::string self = selfExecutable();
  const size_t slash = self.rfind('/');
  if (slash != std::string::npos) {
    const std::string candidate = self.substr(0, slash + 1) + name;
    if (pathExists(candidate))
      return candidate;
  }
  return name; // execvp will search PATH
}

static pid_t spawn(const char *prog, const std::vector<std::string> &args) {
  const std::string path = siblingPath(prog);
  std::vector<char *> argv;
  argv.push_back(const_cast<char *>(path.c_str()));
  for (const auto &a : args)
    argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid == 0) {
    execvp(argv[0], argv.data());
    std::fprintf(stderr, "dicomq-send: cannot exec %s: %s\n", argv[0],
                 strerror(errno));
    _exit(111);
  }
  return pid;
}

static int runChild(const char *prog, const std::vector<std::string> &args) {
  const pid_t pid = spawn(prog, args);
  if (pid < 0)
    return 111;
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 111;
}

static std::string resolveMaildir(const std::string &aetDir,
                                  const std::string &arg) {
  if (!arg.empty() && arg[0] == '/')
    return arg;
  return aetDir + "/" + arg;
}

// wait for new work: inotify where available (the kernel queues events
// raised while we were busy routing, so no commit between scan and wait
// is lost), with the periodic scan as backstop everywhere. recv renames
// objects into queue/todo/<AET>/, so we watch the parent for new AET
// subdirs appearing and each subdir for objects moved into it.
#ifdef __linux__
static int inotifyFd = -1;

static void watchAetDir(const std::string &aet) {
  if (inotifyFd >= 0)
    inotify_add_watch(inotifyFd, sp.queueTodoAET(aet).c_str(), IN_MOVED_TO);
}

static void watchQueue() {
  inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotifyFd < 0)
    return;
  if (inotify_add_watch(inotifyFd, sp.queueTodo().c_str(),
                        IN_CREATE | IN_MOVED_TO) < 0) {
    close(inotifyFd);
    inotifyFd = -1;
    return;
  }
  for (const auto &aet : listSubdirs(sp.queueTodo()))
    watchAetDir(aet);
}

static void waitForWork(long intervalSeconds) {
  if (inotifyFd < 0) {
    sleep(static_cast<unsigned>(intervalSeconds));
    return;
  }
  struct pollfd pfd = {inotifyFd, POLLIN, 0};
  poll(&pfd, 1, static_cast<int>(intervalSeconds * 1000));
  // drain events; a newly appeared AET subdir gets its own watch so its
  // *later* objects wake us. Its first object is still caught by the
  // scan that follows, and the periodic scan backstops any dropped event.
  alignas(struct inotify_event) char buf[4096];
  ssize_t n;
  while ((n = read(inotifyFd, buf, sizeof(buf))) > 0) {
    for (char *p = buf; p < buf + n;) {
      const struct inotify_event *ev =
          reinterpret_cast<const struct inotify_event *>(p);
      if (ev->len > 0 && (ev->mask & IN_ISDIR) &&
          (ev->mask & (IN_CREATE | IN_MOVED_TO)))
        watchAetDir(ev->name);
      p += sizeof(struct inotify_event) + ev->len;
    }
  }
}
#else
static void watchQueue() {}
static void waitForWork(long intervalSeconds) {
  sleep(static_cast<unsigned>(intervalSeconds));
}
#endif

// seal quiescent study/series accumulation directories. A batch is sealed
// purely on its directory mtime — the time its last object arrived — so
// this opens no DICOM (the grouping was decided by dicomq-recv). One
// atomic rename frees the UID rendezvous name and commits the batch as a
// directory-message at once; a straggler arriving afterward simply starts
// the next batch (docs/study-mode.md "the batch boundary is race-free").
static void sweepAccum() {
  const time_t now = time(nullptr);
  for (const auto &aet : listSubdirs(sp.accumRoot())) {
    std::string err;
    GroupConfig group;
    const bool loaded =
        GroupConfig::load(sp.aetDir(aet) + "/group", group, err);
    if (!loaded || !group.enabled()) {
      // Not (or no longer) a grouping AET: leave its dirs alone. But objects
      // already accumulated here can no longer be sealed and would strand
      // silently — surface a backlog so a removed/broken group config is
      // visible (dicomq-queue also reports accum/ counts).
      size_t stranded = 0;
      for (const auto &uid : listSubdirs(sp.accumAET(aet)))
        stranded += listIds(sp.accumGroup(aet, uid)).size();
      if (stranded > 0)
        logmsg("accum/" + aet + ": " + std::to_string(stranded) +
               " object(s) cannot be sealed — group config " +
               (loaded ? "is disabled" : "is invalid: " + err) +
               "; restore it to drain the backlog");
      continue;
    }

    for (const auto &uid : listSubdirs(sp.accumAET(aet))) {
      const std::string dir = sp.accumGroup(aet, uid);
      struct stat st;
      if (stat(dir.c_str(), &st) != 0)
        continue;
      if (now - st.st_mtime < group.quiescenceSeconds)
        continue; // still accumulating: quiet for less than T

      const size_t n = listIds(dir).size();
      if (n == 0) {
        rmdir(dir.c_str()); // empty and quiescent: tidy up, ignore failure
        continue;
      }

      const std::string todoAet = sp.queueTodoAET(aet);
      if (!mkdirIfMissing(todoAet, err)) {
        logmsg("cannot seal " + aet + "/" + uid + ": " + err);
        continue;
      }
      const std::string id = generateId();
      if (rename(dir.c_str(), (todoAet + "/" + id).c_str()) != 0) {
        logmsg("cannot seal " + aet + "/" + uid + ": " + strerror(errno));
        continue;
      }
      if (!fsyncPath(todoAet, err) || !fsyncPath(sp.accumAET(aet), err))
        logmsg("sealed " + id + " but cannot fsync: " + err);
      logmsg("sealed " + aet + "/" + uid + " as batch " + id + " (" +
             std::to_string(n) + " objects)");
    }
  }
}

// route one message from queue/todo/<aet>/; on success it leaves there.
// The called AET is the subdir name, so routing opens no DICOM. A message
// is a single .dcm or a sealed batch directory (msg.isBatch).
static void processMessage(const std::string &aet, const Message &msg) {
  std::string err;
  const std::string &id = msg.id;
  const bool batch = msg.isBatch;
  const std::string srcDir = sp.queueTodoAET(aet);
  const std::string aetDir = sp.aetDir(aet);

  // recv only creates queue/todo/<AET>/ under a validated aet/<AET>/, so
  // this is a defensive guard for a hand-placed object
  if (!isDir(aetDir)) {
    logmsg("failing " + id + ": unknown called AET '" + aet + "'");
    // failed/ is dicomq's to create on first use (DESIGN.md "Spool layout"),
    // like every per-destination sink — without this an absent failed/ makes
    // the move ENOENT and the message re-fails every scan.
    if (!mkdirIfMissing(sp.failedDir(), err) ||
        !moveMessage(srcDir, sp.failedDir(), id, err, batch))
      logmsg("cannot fail " + id + ": " + err);
    return;
  }

  std::vector<DeliverInstruction> instructions;
  if (!loadDeliver(aetDir + "/deliver", instructions, err)) {
    logmsg("deferring " + id + ": " + err);
    return;
  }

  // validate every instruction before executing any, so a config typo
  // defers the whole message instead of half-delivering it
  for (const auto &in : instructions) {
    if (in.kind == DeliverInstruction::Kind::Forward) {
      if (!isDir(sp.destDir(in.arg)) || !isDir(sp.routeTodo(in.arg))) {
        logmsg("deferring " + id + ": destination '" + in.arg +
               "' has no dest/ config or route/ queue");
        return;
      }
    } else if (!isDir(resolveMaildir(aetDir, in.arg) + "/new")) {
      logmsg("deferring " + id + ": maildir '" +
             resolveMaildir(aetDir, in.arg) + "' has no new/");
      return;
    }
  }

  for (const auto &in : instructions) {
    if (in.kind == DeliverInstruction::Kind::Forward) {
      // fan out into the destination queue — a hardlink for a single
      // object, a hardlink-tree for a batch; already-present means a
      // crashed pass routed it here (idempotent fan-out)
      if (!linkMessage(srcDir, sp.routeTodo(in.arg), id, err, batch)) {
        logmsg("deferring " + id + ": " + err);
        return;
      }
    } else {
      const int rc = runChild("dicomq-local",
                              {id, resolveMaildir(aetDir, in.arg), srcDir});
      if (rc == 100) {
        // permanent failure (a different object already holds the maildir
        // slot, or a bad invocation): re-running cannot help, so escalate to
        // failed/ rather than re-attempt every scan, mirroring the unknown-AET
        // path above. dicomq-local logged the specific reason to stderr.
        logmsg("failing " + id + ": dicomq-local reported a permanent failure");
        if (!mkdirIfMissing(sp.failedDir(), err) ||
            !moveMessage(srcDir, sp.failedDir(), id, err, batch))
          logmsg("cannot fail " + id + ": " + err);
        return;
      }
      if (rc != 0) {
        logmsg("deferring " + id + ": dicomq-local exited " +
               std::to_string(rc));
        return;
      }
    }
  }

  if (!discardMessage(sp, srcDir, id, batch, err))
    logmsg("routed " + id + " but cannot dequeue: " + err);
}

static void reapAgents(bool block) {
  while (!running.empty()) {
    const pid_t pid = waitpid(-1, nullptr, block ? 0 : WNOHANG);
    if (pid <= 0)
      return;
    for (auto it = running.begin(); it != running.end(); ++it)
      if (it->second == pid) {
        running.erase(it);
        break;
      }
  }
}

static bool anyDue(const std::string &dir, int level, time_t now) {
  for (const auto &m : listMessages(dir))
    if (messageDue(dir, m.id, level, now, m.isBatch))
      return true;
  return false;
}

// a destination has work if its todo/ holds anything (level 0 = always
// due) or any retry/<k> rung holds a message past its backoff
static bool destHasDueWork(const std::string &dest, time_t now) {
  for (const auto &d : routeQueueDirs(sp, dest))
    if (anyDue(d.first, d.second, now))
      return true;
  return false;
}

static void maybeTrigger(const std::string &dest) {
  if (running.count(dest))
    return;
  if (pathExists(sp.routeHoldFlag(dest)))
    return;

  if (readDestStatus(sp, dest).backedOff(time(nullptr)))
    return; // destination-level backoff (dead-site cache)

  if (destHasDueWork(dest, time(nullptr))) {
    const pid_t pid = spawn("dicomq-remote", {dest});
    if (pid > 0)
      running[dest] = pid;
  }
}

int main(int argc, char **argv) {
  // ignore SIGPIPE so a write to a closed stdio/agent pipe returns EPIPE
  // rather than killing the one long-running process by signal
  signal(SIGPIPE, SIG_IGN);

  std::string spoolArg;
  long interval = 10;
  bool once = false;
  static const struct option longopts[] = {{"once", no_argument, nullptr, 'o'},
                                           {nullptr, 0, nullptr, 0}};
  const char *usage =
      "usage: dicomq-send [-s <spool>] [-i <seconds>] [--once]\n";
  opterr = 0; // we print our own one-line usage
  int opt;
  while ((opt = getopt_long(argc, argv, "s:i:", longopts, nullptr)) != -1) {
    switch (opt) {
    case 's':
      spoolArg = optarg;
      break;
    case 'i':
      // upper bound keeps interval*1000 (the poll(2) millisecond timeout) well
      // inside int — a value that overflowed it would wrap to a negative
      // timeout, which poll reads as "block forever"
      if (!parseWholeInt(optarg, interval) || interval <= 0 ||
          interval > 86400) {
        std::fprintf(stderr, "dicomq-send: -i must be 1..86400 seconds\n");
        return 100;
      }
      break;
    case 'o':
      once = true;
      break;
    default:
      std::fputs(usage, stderr);
      return 100;
    }
  }
  if (optind != argc) { // dicomq-send takes no positional arguments
    std::fputs(usage, stderr);
    return 100;
  }

  sp = Spool(spoolArg);
  // agents inherit the spool through the environment
  setenv("DICOMQ_SPOOL", sp.root.c_str(), 1);

  if (!isDir(sp.queueTodo()) || !isDir(sp.queueTmp())) {
    std::fprintf(stderr, "dicomq-send: '%s' is not a dicomq spool\n",
                 sp.root.c_str());
    return 111;
  }

  if (!once)
    watchQueue();

  for (;;) {
    reapAgents(false);
    sweepAccum(); // seal quiescent study/series batches before routing
    for (const auto &aet : listSubdirs(sp.queueTodo()))
      for (const auto &msg : listMessages(sp.queueTodoAET(aet)))
        processMessage(aet, msg);
    for (const auto &dest : listSubdirs(sp.routeRoot()))
      maybeTrigger(dest);
    if (once) {
      reapAgents(true);
      return 0;
    }
    waitForWork(interval);
  }
}
