// dicomq-local — maildir-style local delivery (qmail-local analog).
//
//   dicomq-local <id> <dir> [env]
//
// Contract (DESIGN.md "Local delivery"):
//   - link queue/todo/<id>.dcm into <dir>/new/ (EEXIST = already
//     delivered = success); with "env", copy the envelope to
//     <dir>/new/<id>.env FIRST so the object's appearance remains the
//     commit point
//   - never creates <dir> or <dir>/new/
//   - delivered files share an inode with the spool: consumers may
//     move or delete them, never modify in place
//
// Speaks no DICOM; links only dicomq-common. Runnable by hand.

#include <cstdio>

int main(int, char **)
{
  std::fprintf(stderr, "dicomq-local: not implemented; see dicomq/DESIGN.md\n");
  return 111;
}
