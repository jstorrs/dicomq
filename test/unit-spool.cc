// Unit tests for filesystem-helper edge cases in common/spool.cc and the
// "missing optional config" parsing in common/profile.cc — cheap to cover
// here and awkward to reach from the network integration suite:
//   * mkdirIfMissing on an existing non-directory must fail
//   * linkIdempotent must accept a replay (same inode) but reject a
//     pre-existing destination with a different inode
//   * the profile loaders must treat a genuinely absent file as the
//     compiled-in default, but a required config (remote) as an error
//
// Exit 0 = all asserts held; 1 = a failure was printed.

#include "common/profile.h"
#include "common/spool.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <unistd.h>

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

  // --- freeBytes sentinel ---------------------------------------------
  // recv's watermark fails closed on a statvfs failure, which freeBytes
  // reports as -1. Pin that contract: a real spool root measures >= 0, a
  // path that cannot be statted is -1 (so the guard refuses, never writes).
  expect(freeBytes(root) >= 0, "freeBytes of a real directory is non-negative");
  expect(freeBytes(root + "/no-such-path") == -1,
         "freeBytes of a missing path is the -1 'unknown' sentinel");

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
