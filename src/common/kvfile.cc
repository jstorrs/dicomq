// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/kvfile.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace dicomq {

namespace fs = std::filesystem;

std::string KeyValueFile::get(const std::string &key) const {
  for (const auto &f : fields)
    if (f.first == key)
      return f.second;
  return "";
}

void KeyValueFile::add(const std::string &key, const std::string &value) {
  fields.emplace_back(key, value);
}

bool KeyValueFile::read(const std::string &path, KeyValueFile &kv,
                        std::string &err) {
  // Distinguish "absent" from other open failures via std::filesystem
  // rather than errno after an ifstream failure (not standard-guaranteed).
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    err = "cannot open '" + path +
          "': " + (ec ? ec.message() : "no such file or directory");
    return false;
  }
  std::ifstream in(path);
  if (!in) {
    err = "cannot open '" + path + "': " + strerror(errno);
    return false;
  }
  kv.fields.clear();
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    const size_t colon = line.find(": ");
    if (colon == std::string::npos || colon == 0) {
      err = "malformed line in '" + path + "': " + line;
      return false;
    }
    kv.fields.emplace_back(line.substr(0, colon), line.substr(colon + 2));
  }
  if (in.bad()) {
    err = "read error on '" + path + "'";
    return false;
  }
  return true;
}

bool KeyValueFile::write(const std::string &path, std::string &err) const {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    err = "cannot create '" + path + "': " + strerror(errno);
    return false;
  }
  for (const auto &f : fields)
    out << f.first << ": " << f.second << '\n';
  out.close();
  if (!out) {
    err = "write error on '" + path + "'";
    return false;
  }
  return true;
}

} // namespace dicomq
