// dicomq-inject — local submission (qmail-inject analog). Enqueues a
// DICOM file as if it had been received, for testing the queue without
// DICOM networking and for re-queueing objects (e.g. from failed/).
//
//   dicomq-inject -c <called-aet> [-a <calling-aet>] <file.dcm>...
//
// Contract:
//   - per file: read SOP class/instance and transfer syntax UIDs from
//     the file meta header, fabricate an envelope (peer: local), write
//     object + envelope into queue/tmp/, commit into queue/todo/
//     (.env last) — the same commit protocol as dicomq-recv
//   - the source file is copied, never moved or linked: the spool owns
//     its objects
//   - the source file's preamble and meta are preserved as-is (some
//     real files carry load-bearing preambles, e.g. dual TIFF/DICOM)
//
// Needs dcmdata only (file meta parsing), no networking.

#include <cstdio>

int main(int, char **)
{
  std::fprintf(stderr, "dicomq-inject: not implemented; see dicomq/DESIGN.md\n");
  return 111;
}
