// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DICOMQ_KVFILE_H
#define DICOMQ_KVFILE_H

#include <string>
#include <utility>
#include <vector>

// A small "key: value" text file: ordered lines, keys lowercase
// [a-z0-9-], values single-line UTF-8, keys may repeat. dicomq uses it
// for machine-written config/state files — dest/<DEST>/remote and the
// route/<DEST>/status dead-site cache — not for per-message data (a
// message is a lone <id>.dcm; see DESIGN.md "Where message metadata
// lives").

namespace dicomq {

struct KeyValueFile {
  std::vector<std::pair<std::string, std::string>> fields;

  // First value for key, or "" if absent.
  std::string get(const std::string &key) const;

  // Number of values recorded for key.
  size_t count(const std::string &key) const;

  void add(const std::string &key, const std::string &value);

  // Parse path. Unknown keys are preserved; malformed lines are an
  // error (these files are machine-written).
  static bool read(const std::string &path, KeyValueFile &kv, std::string &err);

  // Serialize to path. NOT durable on its own: write to a tmp path and
  // publish with commitFile() (see writeKeyValueCommitted).
  bool write(const std::string &path, std::string &err) const;
};

} // namespace dicomq

#endif // DICOMQ_KVFILE_H
