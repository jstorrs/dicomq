// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/profile.h"

#include "common/kvfile.h"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace dicomq {

namespace fs = std::filesystem;

// Parse an integer that must consume the WHOLE token. Operator-facing config
// values go through this rather than atoi/atol, which silently accept trailing
// garbage ("30x" -> 30, "5400junk" -> 5400) and so would mask typos. Returns
// false on empty, malformed, partial, or out-of-range input.
static bool parseWholeInt(const std::string &s, long &out) {
  const char *begin = s.data();
  const char *end = begin + s.size();
  long value = 0;
  const auto res = std::from_chars(begin, end, value);
  if (res.ec != std::errc() || res.ptr != end)
    return false;
  out = value;
  return true;
}

// Shared line reader for profile files: strips comments, trims
// whitespace, and skips blank lines. A '#' at line start or preceded by
// whitespace begins a comment to end of line (so inline comments in the
// copy-pasteable config examples parse); a '#' embedded in a token is
// kept, so paths may contain one. A genuinely absent file is reported via
// `missing` when missingOk (the caller then uses a compiled-in default);
// otherwise it is an error. Existence is decided with std::filesystem so
// the "missing optional config" case never rides on errno being set
// after an ifstream open failure (which the standard does not promise).
static bool readProfileLines(const std::string &path,
                             std::vector<std::string> &lines, bool missingOk,
                             bool &missing, std::string &err) {
  missing = false;
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    if (ec) {
      err = "cannot stat '" + path + "': " + ec.message();
      return false;
    }
    if (missingOk) {
      missing = true;
      return true;
    }
    err = "cannot open '" + path + "': no such file or directory";
    return false;
  }
  std::ifstream in(path);
  if (!in) {
    err = "cannot open '" + path + "': " + strerror(errno);
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    // strip an inline comment: a '#' at line start or preceded by
    // whitespace begins a comment; one embedded in a token is kept
    for (size_t h = line.find('#'); h != std::string::npos;
         h = line.find('#', h + 1)) {
      if (h == 0 || line[h - 1] == ' ' || line[h - 1] == '\t') {
        line.erase(h);
        break;
      }
    }
    const size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos)
      continue; // blank or comment-only line
    const size_t e = line.find_last_not_of(" \t");
    lines.push_back(line.substr(b, e - b + 1));
  }
  if (in.bad()) {
    err = "read error on '" + path + "'";
    return false;
  }
  return true;
}

std::string transferSyntaxUID(const std::string &nameOrUID) {
  if (!nameOrUID.empty() && isdigit(static_cast<unsigned char>(nameOrUID[0])) &&
      nameOrUID.find('.') != std::string::npos)
    return nameOrUID; // already a UID

  static const struct {
    const char *name, *uid;
  } aliases[] = {
      {"ImplicitVRLittleEndian", "1.2.840.10008.1.2"},
      {"ExplicitVRLittleEndian", "1.2.840.10008.1.2.1"},
      {"DeflatedExplicitVRLittleEndian", "1.2.840.10008.1.2.1.99"},
      {"ExplicitVRBigEndian", "1.2.840.10008.1.2.2"},
      {"JPEGBaseline", "1.2.840.10008.1.2.4.50"},
      {"JPEGExtended", "1.2.840.10008.1.2.4.51"},
      {"JPEGLossless", "1.2.840.10008.1.2.4.57"},
      {"JPEGLosslessSV1", "1.2.840.10008.1.2.4.70"},
      {"JPEGLSLossless", "1.2.840.10008.1.2.4.80"},
      {"JPEGLSLossy", "1.2.840.10008.1.2.4.81"},
      {"JPEG2000LosslessOnly", "1.2.840.10008.1.2.4.90"},
      {"JPEG2000", "1.2.840.10008.1.2.4.91"},
      {"MPEG2MainProfile", "1.2.840.10008.1.2.4.100"},
      {"MPEG4HighProfile", "1.2.840.10008.1.2.4.102"},
      {"HEVCMainProfile", "1.2.840.10008.1.2.4.107"},
      {"RLELossless", "1.2.840.10008.1.2.5"},
  };
  for (const auto &a : aliases)
    if (nameOrUID == a.name)
      return a.uid;
  return "";
}

bool isLossyTransferSyntaxUID(const std::string &uid) {
  static const char *lossy[] = {
      "1.2.840.10008.1.2.4.50", // JPEG baseline
      "1.2.840.10008.1.2.4.51", // JPEG extended
      "1.2.840.10008.1.2.4.81", // JPEG-LS near-lossless
      "1.2.840.10008.1.2.4.91", // JPEG 2000 (may be lossy)
      "1.2.840.10008.1.2.4.93", // JPEG 2000 part 2 (may be lossy)
  };
  for (const char *u : lossy)
    if (uid == u)
      return true;
  // all MPEG/HEVC video syntaxes (1.2.840.10008.1.2.4.100..107) are lossy
  return uid.rfind("1.2.840.10008.1.2.4.10", 0) == 0;
}

bool AcceptProfile::load(const std::string &path, AcceptProfile &p,
                         std::string &err) {
  p = AcceptProfile();
  std::vector<std::string> lines;
  bool missing = false;
  if (!readProfileLines(path, lines, /*missingOk=*/true, missing, err))
    return false;
  if (missing)
    return true; // compiled-in default
  for (const auto &line : lines) {
    if (line == "*") {
      p.acceptAll = true;
      continue;
    }
    const std::string uid = transferSyntaxUID(line);
    if (uid.empty()) {
      err = "unknown transfer syntax in '" + path + "': " + line;
      return false;
    }
    p.transferSyntaxes.push_back(uid);
  }
  return true;
}

bool ProposeProfile::load(const std::string &path, ProposeProfile &p,
                          std::string &err) {
  p = ProposeProfile();
  std::vector<std::string> lines;
  bool missing = false;
  if (!readProfileLines(path, lines, /*missingOk=*/true, missing, err))
    return false;
  if (missing)
    return true; // default: propose stored syntax + uncompressed, no transcode
  for (const auto &line : lines) {
    if (line.rfind("transcode: ", 0) == 0) {
      const std::string v = line.substr(11);
      if (v == "never")
        p.transcode = Transcode::Never;
      else if (v == "lossless")
        p.transcode = Transcode::Lossless;
      else if (v == "as-needed")
        p.transcode = Transcode::AsNeeded;
      else {
        err = "unknown transcode policy in '" + path + "': " + v;
        return false;
      }
    } else {
      const std::string uid = transferSyntaxUID(line);
      if (uid.empty()) {
        err = "unknown transfer syntax in '" + path + "': " + line;
        return false;
      }
      p.transferSyntaxes.push_back(uid);
    }
  }
  return true;
}

bool loadDeliver(const std::string &path, std::vector<DeliverInstruction> &out,
                 std::string &err) {
  out.clear();
  std::vector<std::string> lines;
  bool missing = false;
  if (!readProfileLines(path, lines, /*missingOk=*/true, missing, err))
    return false;
  if (missing) {
    DeliverInstruction def;
    def.kind = DeliverInstruction::Kind::Maildir;
    def.arg = "./";
    out.push_back(def);
    return true;
  }
  for (const auto &line : lines) {
    DeliverInstruction in;
    // split into at most 3 whitespace-separated words
    std::vector<std::string> words;
    size_t pos = 0;
    while (pos < line.size() && words.size() < 4) {
      const size_t start = line.find_first_not_of(" \t", pos);
      if (start == std::string::npos)
        break;
      size_t end = line.find_first_of(" \t", start);
      if (end == std::string::npos)
        end = line.size();
      words.push_back(line.substr(start, end - start));
      pos = end;
    }
    if (words.size() == 2 && words[0] == "forward") {
      in.kind = DeliverInstruction::Kind::Forward;
      in.arg = words[1];
    } else if (words.size() == 2 && words[0] == "maildir") {
      in.kind = DeliverInstruction::Kind::Maildir;
      in.arg = words[1];
    } else {
      err = "malformed instruction in '" + path + "': " + line;
      return false;
    }
    out.push_back(in);
  }
  if (out.empty()) {
    err = "'" + path + "' contains no instructions";
    return false;
  }
  return true;
}

bool GroupConfig::load(const std::string &path, GroupConfig &g,
                       std::string &err) {
  g = GroupConfig();
  std::vector<std::string> lines;
  bool missing = false;
  if (!readProfileLines(path, lines, /*missingOk=*/true, missing, err))
    return false;
  if (missing)
    return true; // per-object delivery (mode None)
  if (lines.size() != 1) {
    err = "'" + path + "' must hold exactly one '<mode> <seconds>' line";
    return false;
  }
  const std::string &line = lines[0];
  const size_t sp = line.find_first_of(" \t");
  const std::string mode = line.substr(0, sp);
  if (mode == "study")
    g.mode = Mode::Study;
  else if (mode == "series")
    g.mode = Mode::Series;
  else {
    err = "unknown group mode in '" + path + "': " + mode +
          " (expected study or series)";
    return false;
  }
  const std::string secs = sp == std::string::npos
                               ? std::string()
                               : line.substr(line.find_first_not_of(" \t", sp));
  if (!parseWholeInt(secs, g.quiescenceSeconds) || g.quiescenceSeconds <= 0) {
    err =
        "'" + path + "' needs a positive integer quiescence timeout in seconds";
    return false;
  }
  return true;
}

bool RemoteConfig::load(const std::string &path, RemoteConfig &c,
                        std::string &err) {
  c = RemoteConfig();
  KeyValueFile kv;
  if (!KeyValueFile::read(path, kv, err))
    return false;
  c.host = kv.get("host");
  c.aet = kv.get("aet");
  c.callingAET = kv.get("calling-aet");
  const std::string port = kv.get("port");
  if (!port.empty()) {
    long parsed = 0;
    if (!parseWholeInt(port, parsed) || parsed < 1 || parsed > 65535) {
      err = "'" + path + "' has an invalid port (want 1-65535): " + port;
      return false;
    }
    c.port = static_cast<int>(parsed);
  }
  if (c.host.empty() || c.aet.empty() || c.port <= 0 || c.port > 65535) {
    err = "'" + path + "' must define host, aet, and a valid port";
    return false;
  }
  return true;
}

} // namespace dicomq
