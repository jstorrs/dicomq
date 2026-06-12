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
// preference order. Entries are kept verbatim (UID or DCMTK name).
struct AcceptProfile {
  bool acceptAll = false;  // first non-comment line was "*"
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
