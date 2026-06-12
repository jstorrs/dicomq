// dicomq-send — queue runner (qmail-send analog). The one long-running
// process; owns all routing decisions.
//
// Contract (DESIGN.md "Route"):
//   - watches queue/todo/ for *.env (inotify + periodic scan backstop)
//   - per message: read envelope, look up aet/<called-aet>/deliver
//     (default: "maildir ./"); for each instruction:
//       forward <DEST>  -> link .dcm into route/<DEST>/todo/, copy .env
//                          beside it (.env committed last); idempotent
//       maildir <dir>   -> invoke dicomq-local
//     when all instructions committed: unlink from queue/todo/
//     (.env first, .dcm last)
//   - triggers dicomq-remote <DEST> when that queue has due messages;
//     at most one dicomq-remote per destination at a time
//
// Speaks no DICOM; links only dicomq-common.

#include <cstdio>

int main(int, char **)
{
  std::fprintf(stderr, "dicomq-send: not implemented; see dicomq/DESIGN.md\n");
  return 111;
}
