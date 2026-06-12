// dicomq-remote — C-STORE forwarder for one destination (qmail-remote
// analog).
//
//   dicomq-remote <DEST>
//
// Contract (DESIGN.md "Remote delivery", "Retry and the queue lifetime"):
//   - reads dest/<DEST>/remote (host/port/aet) and dest/<DEST>/propose
//     (transfer syntaxes + transcode policy: never|lossless|as-needed)
//   - opens ONE association and C-STOREs every due message in
//     route/<DEST>/todo/ over it (objects batch per association)
//   - transcoding happens per attempt, into memory or a tmp file; the
//     queued object is never modified
//   - per message: success -> unlink (.env first, .dcm last);
//     failure -> append "attempt:" line, recommit the envelope copy
//     (mtime = last attempt); past the queue lifetime or on permanent
//     rejection -> link .dcm into failed/, write annotated .env beside
//     it, unlink from the route queue
//
// Links DCMTK (dcmnet, dcmtls). The only outbound DICOM-speaking program.

#include <cstdio>

int main(int, char **)
{
  std::fprintf(stderr, "dicomq-remote: not implemented; see dicomq/DESIGN.md\n");
  return 111;
}
