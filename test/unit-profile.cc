// Unit tests for the pure helpers in common/profile.cc — transfer
// syntax name/UID resolution and lossy classification. These paths are
// not reachable from the network integration tests (they need a peer
// that accepts video syntaxes), so they are exercised directly here.
//
// Exit 0 = all asserts held; 1 = a failure was printed.

#include "common/profile.h"

#include <cstdio>
#include <string>

using namespace dicomq;

static int failures = 0;

static void expectEq(const std::string& got, const std::string& want,
                     const char* what)
{
  if (got != want)
  {
    std::fprintf(stderr, "FAIL %s: got '%s' want '%s'\n", what, got.c_str(),
                 want.c_str());
    failures++;
  }
}

static void expectLossy(const std::string& uid, bool want, const char* what)
{
  if (isLossyTransferSyntaxUID(uid) != want)
  {
    std::fprintf(stderr, "FAIL %s: isLossy('%s') != %d\n", what, uid.c_str(),
                 want);
    failures++;
  }
}

int main()
{
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

  if (failures == 0)
    std::printf("unit-profile: all checks passed\n");
  return failures ? 1 : 0;
}
