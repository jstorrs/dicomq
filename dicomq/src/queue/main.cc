// dicomq-queue — queue inspection (postqueue/qshape analog).
//
//   dicomq-queue [<DEST>]
//
// Contract (DESIGN.md "Components"):
//   - shows what is queued where: per-queue message counts and age
//     histograms for queue/todo/, each route/<DEST>/todo/, hold/,
//     corrupt/, and failed/; surfaces route/<DEST>/status (dead-site
//     backoff) and hold flags
//   - strictly read-only: opens nothing for writing, takes no locks,
//     safe to run as any user with read access to the spool
//
// Speaks no DICOM; links only dicomq-common.

#include <cstdio>

int main(int, char **)
{
  std::fprintf(stderr, "dicomq-queue: not implemented; see dicomq/DESIGN.md\n");
  return 111;
}
