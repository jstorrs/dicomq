#ifndef DICOMQ_PROFILE_H
#define DICOMQ_PROFILE_H

#include <string>
#include <vector>

// Transfer syntax profiles and destination config. Parsing lives here;
// resolving syntax names/UIDs to DCMTK transfer syntaxes is the job of
// the DICOM-side programs (dicomq-recv, dicomq-remote). See
// dicomq/DESIGN.md "Transfer syntax profiles".

namespace dicomq {

// aet/<AET>/accept — what dicomq-recv accepts for a called AET, in
// preference order. Entries are normalized to UIDs at load time.
struct AcceptProfile {
  bool acceptAll = false;  // a non-comment line was "*"
  std::vector<std::string> transferSyntaxes;

  // Missing file is not an error: p is the compiled-in default
  // (acceptAll=false, empty list = receiver's built-in preferences).
  static bool load(const std::string& path, AcceptProfile& p,
                   std::string& err);
};

// dest/<DEST>/propose — what dicomq-remote proposes, plus the transcode
// policy for objects whose stored syntax the destination did not accept.
struct ProposeProfile {
  enum class Transcode { Never, Lossless, AsNeeded };

  std::vector<std::string> transferSyntaxes;
  Transcode transcode = Transcode::Never;

  static bool load(const std::string& path, ProposeProfile& p,
                   std::string& err);
};

// Resolve a transfer syntax profile entry to a UID string: dotted
// numeric entries pass through; well-known names (the DCMTK-flavored
// spellings used in DESIGN.md examples) come from a built-in alias
// table. Returns "" for an unknown name, so config errors surface at
// load time.
std::string transferSyntaxUID(const std::string& nameOrUID);

// Whether converting INTO this transfer syntax discards information —
// the question "transcode: lossless" asks of a target syntax.
// (Decompressing FROM a lossy syntax adds no further loss and is always
// allowed.)
bool isLossyTransferSyntaxUID(const std::string& uid);

// aet/<AET>/deliver — routing instructions, one per line (DESIGN.md
// "Routing instructions"). A missing file yields the default
// instruction: maildir ./ without envelope.
struct DeliverInstruction {
  enum class Kind { Maildir, Forward };
  Kind kind;
  std::string arg;       // maildir path (possibly relative) or DEST name
  bool withEnv = false;  // maildir only: deliver the envelope too
};

bool loadDeliver(const std::string& path,
                 std::vector<DeliverInstruction>& out, std::string& err);

// dest/<DEST>/remote — connection parameters (envelope-format file).
struct RemoteConfig {
  std::string host;
  int port = 104;
  std::string aet;
  std::string callingAET;  // empty = compiled-in default

  static bool load(const std::string& path, RemoteConfig& c,
                   std::string& err);
};

} // namespace dicomq

#endif // DICOMQ_PROFILE_H
