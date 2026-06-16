// Unit tests for pure helpers in common/spool.cc + common/message.cc and the
// "missing optional config" parsing in common/profile.cc — cheap to cover
// here and awkward to reach from the network integration suite:
//   * mkdirIfMissing on an existing non-directory must fail
//   * linkIdempotent must accept a replay (same inode) but reject a
//     pre-existing destination with a different inode
//   * copyFile is all-or-nothing (no partial dst on a write error)
//   * freeBytes reports -1 for an unstattable path (the watermark's fail-closed
//     sentinel), sanitizeAET/isReservedName enforce the path-name rules,
//     id<->time and ISO-time round-trip, and retryBackoff/messageDue encode the
//     retry schedule — all subtle, routing/security-relevant, and silent if
//     they regress
//   * the profile loaders must treat a genuinely absent file as the
//     compiled-in default, but a required config (remote) as an error
//
// Exit 0 = all asserts held; 1 = a failure was printed.

#include "common/message.h"
#include "common/profile.h"
#include "common/spool.h"

#include <csignal>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>

#include <sys/resource.h>
#include <unistd.h>
#include <utime.h>

using namespace dicomq;

static int failures = 0;

static void expect(bool cond, const char *what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s\n", what);
    failures++;
  }
}

// A throwaway directory unique to this process; cleaned up at the end.
static std::string scratchRoot() {
  const char *t = std::getenv("TMPDIR");
  std::string base = (t && *t) ? t : "/tmp";
  if (!base.empty() && base.back() == '/')
    base.pop_back();
  return base + "/dicomq-unit-spool." + std::to_string(getpid());
}

static void writeFile(const std::string &path, const std::string &body) {
  std::ofstream o(path, std::ios::trunc);
  o << body;
}

int main() {
  std::string err;
  const std::string root = scratchRoot();
  // root itself is created by an operator in the real spool; here we own it.
  expect(mkdirIfMissing(root, err), "mkdirIfMissing creates a fresh dir");

  // --- mkdirIfMissing -------------------------------------------------
  const std::string d = root + "/leaf";
  expect(mkdirIfMissing(d, err) && isDir(d), "creates a missing directory");
  expect(mkdirIfMissing(d, err), "idempotent on an existing directory");

  const std::string f = root + "/regular";
  writeFile(f, "i am a file\n");
  err.clear();
  expect(!mkdirIfMissing(f, err) && !err.empty(),
         "fails when the path exists as a regular file");

  // --- linkIdempotent -------------------------------------------------
  const std::string src = root + "/src";
  writeFile(src, "payload\n");
  const std::string dstNew = root + "/dst-new";
  err.clear();
  expect(linkIdempotent(src, dstNew, err), "links to a fresh destination");
  err.clear();
  expect(linkIdempotent(src, dstNew, err),
         "replay to the same inode is success");

  // A pre-existing destination that is a *different* file must be rejected
  // — accepting it would let the source be dequeued over a wrong file.
  const std::string other = root + "/other";
  writeFile(other, "different payload\n");
  err.clear();
  expect(!linkIdempotent(src, other, err) && !err.empty(),
         "rejects an existing destination with a different inode");

  // --- routeQueueDirs walks rungs in ascending numeric order ----------
  {
    const Spool sp(root);
    const std::string dest = "PACS1";
    expect(mkdirIfMissing(sp.routeRoot(), err) &&
               mkdirIfMissing(sp.routeDir(dest), err) &&
               mkdirIfMissing(sp.routeRetryRoot(dest), err),
           "route skeleton for the rung test");
    // rungs out of lexical order (10 < 2 as strings), plus noise that must
    // be ignored: a non-canonical "02" and a malformed "2x"
    for (const char *r : {"1", "2", "10", "02", "2x"})
      mkdirIfMissing(sp.routeRetryRoot(dest) + "/" + r, err);

    const auto dirs = routeQueueDirs(sp, dest);
    // expected: todo(0), retry/1, retry/2, retry/10 — "02"/"2x" dropped
    const bool ok = dirs.size() == 4 && dirs[0].second == 0 &&
                    dirs[1].second == 1 && dirs[2].second == 2 &&
                    dirs[3].second == 10 &&
                    dirs[3].first == sp.routeRetry(dest, 10);
    expect(ok, "routeQueueDirs sorts rungs numerically, dropping malformed");
  }

  // --- copyFile is all-or-nothing -------------------------------------
  {
    const std::string cpSrc = root + "/cp-src";
    const std::string cpDst = root + "/cp-dst";
    writeFile(cpSrc, "payload to copy\n");
    err.clear();
    expect(copyFile(cpSrc, cpDst, err) && isDir(root), "copyFile succeeds");
    {
      std::ifstream in(cpDst);
      std::string got((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
      expect(got == "payload to copy\n", "copyFile reproduces the source");
    }
    // a missing source fails and creates no destination
    const std::string cpDst2 = root + "/cp-dst2";
    err.clear();
    expect(!copyFile(root + "/no-such-src", cpDst2, err) && !pathExists(cpDst2),
           "copyFile of a missing source leaves no destination");

    // a write that cannot complete (tiny RLIMIT_FSIZE; SIGXFSZ ignored so the
    // write returns EFBIG rather than killing us) must leave no partial file
    signal(SIGXFSZ, SIG_IGN);
    struct rlimit orig{};
    if (getrlimit(RLIMIT_FSIZE, &orig) == 0) {
      writeFile(cpSrc, std::string(4096, 'x')); // larger than the cap below
      const std::string partial = root + "/cp-partial";
      struct rlimit cap = orig;
      cap.rlim_cur = 64;
      if (setrlimit(RLIMIT_FSIZE, &cap) == 0) {
        err.clear();
        const bool ok = copyFile(cpSrc, partial, err);
        setrlimit(RLIMIT_FSIZE, &orig); // restore before any other write
        expect(!ok, "copyFile fails when the write cannot complete");
        expect(!pathExists(partial),
               "copyFile leaves no partial dst on a write error");
        unlink(partial.c_str());
      } else {
        setrlimit(RLIMIT_FSIZE, &orig);
      }
    }
    unlink(cpSrc.c_str());
    unlink(cpDst.c_str());
  }

  // --- freeBytes sentinel ---------------------------------------------
  // recv's watermark fails closed on a statvfs failure, which freeBytes
  // reports as -1. Pin that contract: a real spool root measures >= 0, a
  // path that cannot be statted is -1 (so the guard refuses, never writes).
  expect(freeBytes(root) >= 0, "freeBytes of a real directory is non-negative");
  expect(freeBytes(root + "/no-such-path") == -1,
         "freeBytes of a missing path is the -1 'unknown' sentinel");

  // --- sanitizeAET: trimming, allowed charset, no path escape ---------
  expect(sanitizeAET(" a.b ") == "a_b", "sanitizeAET trims and replaces '.'");
  expect(sanitizeAET("") == "_", "empty AET sanitizes to '_'");
  expect(sanitizeAET("   ") == "_", "all-whitespace AET sanitizes to '_'");
  expect(sanitizeAET("PACS-1") == "PACS-1", "alphanumerics and '-' are kept");
  expect(sanitizeAET("a/b") == "a_b", "'/' is replaced");
  expect(sanitizeAET("../x").find('/') == std::string::npos,
         "no '/' survives sanitize (no path escape)");

  // --- isReservedName: the queue's own dir names, case-insensitive -----
  expect(isReservedName("tmp") && isReservedName("TMP") &&
             isReservedName("New") && isReservedName("todo"),
         "tmp/new/todo are reserved, case-insensitively");
  expect(!isReservedName("ARCHIVE") && !isReservedName("temp"),
         "ordinary names are not reserved");

  // --- id <-> time, and ISO time round-trip ---------------------------
  {
    const time_t now = time(nullptr);
    const time_t t = idTime(generateId());
    expect(t != 0 && t >= now - 2 && t <= now + 2,
           "idTime of a fresh id is within seconds of now");
    expect(idTime("not-an-id") == 0, "idTime of a non-id is 0");
    expect(parseIsoTime(isoTime(now)) == now, "parseIsoTime inverts isoTime");
    expect(parseIsoTime("garbage") == 0, "parseIsoTime rejects garbage");
  }

  // --- retryBackoff: quadratic growth to a 6-hour cap -----------------
  expect(retryBackoff(0) == 0, "rung 0 (todo) is always due");
  expect(retryBackoff(-3) == 0, "a non-positive rung backoff is 0");
  expect(retryBackoff(1) == 420, "rung 1 backoff is ~7 minutes");
  expect(retryBackoff(2) == 1680, "rung 2 backoff is quadratic (420*2^2)");
  expect(retryBackoff(8) == 21600 && retryBackoff(8) > retryBackoff(7),
         "backoff grows then caps at 6 hours");

  // --- messageDue: rung 0 always due; rung k gated on mtime+backoff ----
  {
    const std::string mdDir = root + "/md";
    expect(mkdirIfMissing(mdDir, err), "messageDue fixture dir");
    const std::string mid = "20200101000000000.1.000000";
    writeFile(dcmPath(mdDir, mid), "x");
    const time_t now = time(nullptr);
    expect(messageDue(mdDir, mid, 0, now), "rung 0 is always due");
    struct utimbuf fresh{now, now};
    utime(dcmPath(mdDir, mid).c_str(), &fresh);
    expect(!messageDue(mdDir, mid, 1, now),
           "a just-landed retry/1 message is not yet due");
    struct utimbuf aged{now - 100000, now - 100000};
    utime(dcmPath(mdDir, mid).c_str(), &aged);
    expect(messageDue(mdDir, mid, 1, now),
           "a retry/1 message past its backoff is due");
    unlink(dcmPath(mdDir, mid).c_str());
    expect(!messageDue(mdDir, mid, 1, now), "a vanished message is not due");
    rmdir(mdDir.c_str());
  }

  // --- missing optional config vs required config ---------------------
  {
    const std::string absent = root + "/does-not-exist";

    AcceptProfile ap;
    err.clear();
    expect(AcceptProfile::load(absent, ap, err) && !ap.acceptAll &&
               ap.transferSyntaxes.empty(),
           "absent accept profile is the compiled-in default");

    std::vector<DeliverInstruction> di;
    err.clear();
    expect(loadDeliver(absent, di, err) && di.size() == 1 &&
               di[0].kind == DeliverInstruction::Kind::Maildir &&
               di[0].arg == "./",
           "absent deliver profile defaults to 'maildir ./'");

    RemoteConfig rc;
    err.clear();
    expect(!RemoteConfig::load(absent, rc, err) && !err.empty(),
           "absent remote config is a hard error");
  }

  // Best-effort cleanup (ignore errors; the OS reaps TMPDIR anyway).
  unlink(f.c_str());
  unlink(src.c_str());
  unlink(dstNew.c_str());
  unlink(other.c_str());
  rmdir(d.c_str());
  rmdir(root.c_str());

  if (failures == 0)
    std::printf("unit-spool: all checks passed\n");
  return failures ? 1 : 0;
}
