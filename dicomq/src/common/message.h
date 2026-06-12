#ifndef DICOMQ_MESSAGE_H
#define DICOMQ_MESSAGE_H

#include <string>

#include "common/envelope.h"
#include "common/spool.h"

// Operations on message pairs (<id>.dcm + <id>.env) that follow the
// commit discipline from DESIGN.md: object first and envelope last on
// the way into a queue, envelope first and object last on the way out.

namespace dicomq {

std::string dcmPath(const std::string& dir, const std::string& id);
std::string envPath(const std::string& dir, const std::string& id);

// Remove a pair from dir: envelope first (uncommitting the message),
// then the object. Missing files are tolerated — removal is idempotent.
bool removePair(const std::string& dir, const std::string& id,
                std::string& err);

// Serialize env into queue/tmp/ and commit it at finalPath (atomic
// replacement if it already exists). Used both to create envelopes and
// to rewrite a route queue's envelope copy after a delivery attempt.
bool writeEnvelopeCommitted(const Spool& sp, const Envelope& env,
                            const std::string& finalPath, std::string& err);

// Move a pair between queues. Raw copies the envelope bytes verbatim
// (works even for unparseable envelopes, and tolerates a missing
// object so half a message can still be quarantined); Annotated writes
// the given envelope instead of the stored one.
bool movePairRaw(const Spool& sp, const std::string& fromDir,
                 const std::string& toDir, const std::string& id,
                 std::string& err);
bool movePairAnnotated(const Spool& sp, const std::string& fromDir,
                       const std::string& toDir, const std::string& id,
                       const Envelope& env, std::string& err);

} // namespace dicomq

#endif // DICOMQ_MESSAGE_H
