#ifndef DICOMQ_ENVELOPE_H
#define DICOMQ_ENVELOPE_H

#include <string>
#include <utility>
#include <vector>

// The <id>.env file: routing metadata as ordered "key: value" lines.
// Keys are lowercase [a-z0-9-]; values are single-line UTF-8; keys may
// repeat (e.g. "attempt"). See dicomq/DESIGN.md "Messages".

namespace dicomq {

struct Envelope {
  std::vector<std::pair<std::string, std::string>> fields;

  // First value for key, or "" if absent.
  std::string get(const std::string& key) const;

  void add(const std::string& key, const std::string& value);

  // Parse path. Unknown keys are preserved; malformed lines are an
  // error (envelopes are machine-written).
  static bool read(const std::string& path, Envelope& env, std::string& err);

  // Serialize to path. NOT durable on its own: write to a tmp path and
  // publish with commitFile(), envelope always last (its presence is
  // the commit point).
  bool write(const std::string& path, std::string& err) const;
};

} // namespace dicomq

#endif // DICOMQ_ENVELOPE_H
