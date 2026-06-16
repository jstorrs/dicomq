// Unit tests for the pure helpers in common/profile.cc — transfer
// syntax name/UID resolution and lossy classification. These paths are
// not reachable from the network integration tests (they need a peer
// that accepts video syntaxes), so they are exercised directly here.
//
// Exit 0 = all asserts held; 1 = a failure was printed.

#include "common/profile.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <unistd.h>

using namespace dicomq;

static int failures = 0;

static void expectEq(const std::string &got, const std::string &want,
                     const char *what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL %s: got '%s' want '%s'\n", what, got.c_str(),
                 want.c_str());
    failures++;
  }
}

static void expectLossy(const std::string &uid, bool want, const char *what) {
  if (isLossyTransferSyntaxUID(uid) != want) {
    std::fprintf(stderr, "FAIL %s: isLossy('%s') != %d\n", what, uid.c_str(),
                 want);
    failures++;
  }
}

int main() {
  // name -> UID resolution, and UID passthrough
  expectEq(transferSyntaxUID("ExplicitVRLittleEndian"), "1.2.840.10008.1.2.1",
           "name resolves to UID");
  expectEq(transferSyntaxUID("1.2.840.10008.1.2.1"), "1.2.840.10008.1.2.1",
           "UID passes through");
  expectEq(transferSyntaxUID("NoSuchSyntax"), "", "unknown name is empty");

  // lossless still-image and uncompressed syntaxes are NOT lossy
  expectLossy("1.2.840.10008.1.2", false, "implicit VR");
  expectLossy("1.2.840.10008.1.2.1", false, "explicit VR");
  expectLossy("1.2.840.10008.1.2.4.70", false, "JPEG lossless SV1");
  expectLossy("1.2.840.10008.1.2.4.80", false, "JPEG-LS lossless");
  expectLossy("1.2.840.10008.1.2.4.90", false, "JPEG 2000 lossless only");
  expectLossy("1.2.840.10008.1.2.5", false, "RLE lossless");

  // still-image lossy syntaxes ARE lossy
  expectLossy("1.2.840.10008.1.2.4.50", true, "JPEG baseline");
  expectLossy("1.2.840.10008.1.2.4.51", true, "JPEG extended");
  expectLossy("1.2.840.10008.1.2.4.81", true, "JPEG-LS near-lossless");
  expectLossy("1.2.840.10008.1.2.4.91", true, "JPEG 2000 (may be lossy)");

  // every MPEG/HEVC video syntax (…4.100..107) is lossy — the case the
  // off-by-three length in the prefix compare used to miss entirely
  expectLossy("1.2.840.10008.1.2.4.100", true, "MPEG2 main");
  expectLossy("1.2.840.10008.1.2.4.101", true, "MPEG2 high");
  expectLossy("1.2.840.10008.1.2.4.102", true, "MPEG4 high");
  expectLossy("1.2.840.10008.1.2.4.104", true, "MPEG4 BD");
  expectLossy("1.2.840.10008.1.2.4.107", true, "HEVC main");

  // GroupConfig parsing (aet/<AET>/group): study/series accumulation knob
  {
    const std::string tmp =
        "/tmp/dicomq-group-test." + std::to_string(getpid());
    auto put = [&](const std::string &body) {
      std::ofstream o(tmp, std::ios::trunc);
      o << body;
    };
    auto expect = [&](bool cond, const char *what) {
      if (!cond) {
        std::fprintf(stderr, "FAIL group %s\n", what);
        failures++;
      }
    };
    std::string err;
    GroupConfig g;

    // missing file => mode None, not an error
    expect(GroupConfig::load(tmp + ".absent", g, err) && !g.enabled(),
           "missing file is per-object (None)");

    put("study 120\n");
    g = GroupConfig();
    expect(GroupConfig::load(tmp, g, err) &&
               g.mode == GroupConfig::Mode::Study && g.quiescenceSeconds == 120,
           "study 120 parses");

    put("series 90\n");
    g = GroupConfig();
    expect(GroupConfig::load(tmp, g, err) &&
               g.mode == GroupConfig::Mode::Series && g.quiescenceSeconds == 90,
           "series 90 parses");

    put("bogus 5\n");
    expect(!GroupConfig::load(tmp, g, err), "unknown mode is rejected");

    put("study\n");
    expect(!GroupConfig::load(tmp, g, err), "missing seconds is rejected");

    put("study 0\n");
    expect(!GroupConfig::load(tmp, g, err), "non-positive timeout is rejected");

    // inline '#' comments are stripped — the copy-pasteable README/DESIGN
    // propose example must parse verbatim (full-line and inline comments)
    put("# a propose profile\n"
        "JPEGLSLossless\n"
        "ExplicitVRLittleEndian\n"
        "transcode: lossless        # never | lossless | as-needed\n");
    ProposeProfile pp;
    expect(ProposeProfile::load(tmp, pp, err) &&
               pp.transcode == ProposeProfile::Transcode::Lossless &&
               pp.transferSyntaxes.size() == 2,
           "inline comments are stripped");

    unlink(tmp.c_str());
  }

  if (failures == 0)
    std::printf("unit-profile: all checks passed\n");
  return failures ? 1 : 0;
}
