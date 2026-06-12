// dicomq-recv — DICOM C-STORE receiver (qmail-smtpd analog).
//
// Contract (DESIGN.md "Receive"):
//   - runs one process per association under a socket supervisor
//     (systemd Accept=yes / s6 / ucspi-tcp); inherits the connected
//     socket on fd 0, never listens, never forks
//   - rejects the association if aet/<called-aet>/ does not exist
//     (unknown recipient, refused at "RCPT TO" time)
//   - negotiates presentation contexts per aet/<called-aet>/accept
//   - per object: write queue/tmp/<id>.dcm + .env, fsync both, commit
//     into queue/todo/ (.env last), THEN send C-STORE success; any
//     failure removes both tmp files and answers with a refused status
//   - the written file has a zeroed preamble and file meta stamped with
//     Source/Sending/ReceivingApplicationEntityTitle (0002,0016/17/18);
//     the envelope, not the preamble, carries queue state (DESIGN.md
//     "Why a sidecar, not the DICOM preamble")
//
// Links DCMTK (dcmnet, dcmtls). The only inbound DICOM-speaking program.

#include <cstdio>

int main(int, char **)
{
  std::fprintf(stderr, "dicomq-recv: not implemented; see dicomq/DESIGN.md\n");
  return 111;
}
