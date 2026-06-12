// dicomq-clean — orphan reaper (qmail-clean analog). Run from a
// timer/cron.
//
// Contract (DESIGN.md "Commit protocols"):
//   - removes queue/tmp/ entries older than the grace period (default
//     36 hours)
//   - removes orphaned .dcm files (no .env beside them) of the same age
//     from queue/todo/ and route/*/todo/ — a .dcm without its envelope
//     is invisible to every consumer and means a crash mid-commit
//   - touches nothing younger, nothing with an envelope, and nothing
//     outside queue/ and route/
//
// Speaks no DICOM; links only dicomq-common.

#include <cstdio>

int main(int, char **)
{
  std::fprintf(stderr, "dicomq-clean: not implemented; see dicomq/DESIGN.md\n");
  return 111;
}
