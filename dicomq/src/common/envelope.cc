#include "common/envelope.h"

#include <cerrno>
#include <cstring>
#include <fstream>

namespace dicomq {

std::string Envelope::get(const std::string& key) const
{
  for (const auto& f : fields)
    if (f.first == key)
      return f.second;
  return "";
}

void Envelope::add(const std::string& key, const std::string& value)
{
  fields.emplace_back(key, value);
}

bool Envelope::read(const std::string& path, Envelope& env, std::string& err)
{
  std::ifstream in(path);
  if (!in)
  {
    err = "cannot open '" + path + "': " + strerror(errno);
    return false;
  }
  env.fields.clear();
  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty())
      continue;
    const size_t colon = line.find(": ");
    if (colon == std::string::npos || colon == 0)
    {
      err = "malformed envelope line in '" + path + "': " + line;
      return false;
    }
    env.fields.emplace_back(line.substr(0, colon), line.substr(colon + 2));
  }
  if (in.bad())
  {
    err = "read error on '" + path + "'";
    return false;
  }
  return true;
}

bool Envelope::write(const std::string& path, std::string& err) const
{
  std::ofstream out(path, std::ios::trunc);
  if (!out)
  {
    err = "cannot create '" + path + "': " + strerror(errno);
    return false;
  }
  for (const auto& f : fields)
    out << f.first << ": " << f.second << '\n';
  out.close();
  if (!out)
  {
    err = "write error on '" + path + "'";
    return false;
  }
  return true;
}

} // namespace dicomq
