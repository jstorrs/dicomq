#include "common/profile.h"

#include "common/envelope.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace dicomq {

// Shared line reader for profile files: trims whitespace, skips blank
// lines and '#' comments. Returns false on open failure with errno in
// err, unless missingOk.
static bool readProfileLines(const std::string& path,
                             std::vector<std::string>& lines,
                             bool missingOk, bool& missing, std::string& err)
{
  missing = false;
  std::ifstream in(path);
  if (!in)
  {
    if (missingOk && errno == ENOENT)
    {
      missing = true;
      return true;
    }
    err = "cannot open '" + path + "': " + strerror(errno);
    return false;
  }
  std::string line;
  while (std::getline(in, line))
  {
    const size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] == '#')
      continue;
    const size_t e = line.find_last_not_of(" \t");
    lines.push_back(line.substr(b, e - b + 1));
  }
  if (in.bad())
  {
    err = "read error on '" + path + "'";
    return false;
  }
  return true;
}

bool AcceptProfile::load(const std::string& path, AcceptProfile& p,
                         std::string& err)
{
  p = AcceptProfile();
  std::vector<std::string> lines;
  bool missing = false;
  if (!readProfileLines(path, lines, /*missingOk=*/true, missing, err))
    return false;
  if (missing)
    return true;  // compiled-in default
  for (const auto& line : lines)
  {
    if (line == "*")
      p.acceptAll = true;
    else
      p.transferSyntaxes.push_back(line);
  }
  return true;
}

bool ProposeProfile::load(const std::string& path, ProposeProfile& p,
                          std::string& err)
{
  p = ProposeProfile();
  std::vector<std::string> lines;
  bool missing = false;
  if (!readProfileLines(path, lines, /*missingOk=*/true, missing, err))
    return false;
  if (missing)
    return true;  // default: propose stored syntax + uncompressed, no transcode
  for (const auto& line : lines)
  {
    if (line.rfind("transcode: ", 0) == 0)
    {
      const std::string v = line.substr(11);
      if (v == "never")
        p.transcode = Transcode::Never;
      else if (v == "lossless")
        p.transcode = Transcode::Lossless;
      else if (v == "as-needed")
        p.transcode = Transcode::AsNeeded;
      else
      {
        err = "unknown transcode policy in '" + path + "': " + v;
        return false;
      }
    }
    else
      p.transferSyntaxes.push_back(line);
  }
  return true;
}

bool RemoteConfig::load(const std::string& path, RemoteConfig& c,
                        std::string& err)
{
  c = RemoteConfig();
  Envelope env;
  if (!Envelope::read(path, env, err))
    return false;
  c.host = env.get("host");
  c.aet = env.get("aet");
  c.callingAET = env.get("calling-aet");
  const std::string port = env.get("port");
  if (!port.empty())
    c.port = atoi(port.c_str());
  if (c.host.empty() || c.aet.empty() || c.port <= 0 || c.port > 65535)
  {
    err = "'" + path + "' must define host, aet, and a valid port";
    return false;
  }
  return true;
}

} // namespace dicomq
