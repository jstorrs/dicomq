// dicomq-super — queue surgery (postsuper analog). Runs as the send
// user.
//
//   dicomq-super hold <id>     move a message from its queue into hold/
//   dicomq-super release <id>  return it to the queue recorded in its
//                              envelope's "held-from:" line
//   dicomq-super requeue <id>  move a message (e.g. from failed/) back
//                              into queue/todo/ for fresh routing
//   dicomq-super fail <id>     move a message into failed/ with an
//                              annotated envelope
//
// Contract (DESIGN.md "Hold and quarantine"):
//   - every move uses the standard discipline: object first and
//     envelope last on the way in, envelope first on the way out;
//     idempotent via unique ids
//   - never deletes anything; removal from hold/, corrupt/, or
//     failed/ is the operator's own rm
//
// Speaks no DICOM; links only dicomq-common.

#include <cstdio>

int main(int, char **)
{
  std::fprintf(stderr, "dicomq-super: not implemented; see dicomq/DESIGN.md\n");
  return 111;
}
