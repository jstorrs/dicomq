// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// dicomq-inject — local submission (qmail-inject analog). Enqueues a
// DICOM file as if it had been received, for testing the queue without
// DICOM networking and for re-queueing objects (e.g. from failed/).
//
//   dicomq-inject [-s <spool>] -c <called-aet> [-a <calling-aet>] <file.dcm>...
//
// Per file: read SOP class/instance and transfer syntax UIDs from the
// file meta header (rejecting non-Part-10 input), stamp the
// Source/Sending/ReceivingApplicationEntityTitle meta tags the receiver
// writes, and save into queue/tmp/ then commit into
// queue/todo/<called-aet>/ with a single atomic rename — the same
// protocol as dicomq-recv. DCMTK preserves any load-bearing source
// preamble (dual TIFF/DICOM) across the load/save. Prints each new
// message id on stdout.
//
// Exit: 0 enqueued; 100 bad usage or unreadable/non-Part-10 input
// (permanent); 111 spool failure (temporary).

#include "dcmtk/config/osconfig.h"

#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/dcmdata/dcmetinf.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "common/dcm.h"
#include "common/message.h"
#include "common/spool.h"

using namespace dicomq;

static void logmsg(const std::string &m) { dicomq::logmsg("dicomq-inject", m); }

static int usage() {
  std::fprintf(stderr, "usage: dicomq-inject [-s <spool>] -c <called-aet> [-a "
                       "<calling-aet>] <file.dcm>...\n");
  return 100;
}

int main(int argc, char **argv) {
  std::string spoolArg, calledAET, callingAET = "LOCAL";
  int opt;
  while ((opt = getopt(argc, argv, "s:c:a:")) != -1) {
    switch (opt) {
    case 's':
      spoolArg = optarg;
      break;
    case 'c':
      calledAET = optarg;
      break;
    case 'a':
      callingAET = optarg;
      break;
    default:
      return usage();
    }
  }
  if (calledAET.empty() || optind >= argc)
    return usage();

  // same gate dicomq-recv applies to an inbound called AET (recv/main.cc):
  // a title that sanitizes to a reserved queue name must not become a
  // queue/todo/<name> destination.
  const std::string dest = sanitizeAET(calledAET);
  if (isReservedName(dest)) {
    logmsg("called AET '" + calledAET + "' resolves to reserved name '" + dest +
           "'");
    return 100;
  }

  const Spool sp(spoolArg);
  std::string err;

  // recv rejects an unknown called AET at association time — the
  // unknown-recipient error happens at "RCPT TO" (DESIGN.md). Apply the
  // same gate at submission: without it a filter's typoed -c enqueues
  // every object only for dicomq-send to escalate them to failed/.
  if (!isDir(sp.aetDir(dest))) {
    logmsg("unknown called AET '" + calledAET + "' (no aet/" + dest + "/)");
    return 100;
  }

  for (int i = optind; i < argc; i++) {
    const char *file = argv[i];

    DcmFileFormat ff;
    OFCondition cond = ff.loadFile(file);
    if (cond.bad()) {
      logmsg("cannot read '" + std::string(file) + "': " + cond.text());
      return 100;
    }
    DcmMetaInfo *meta = ff.getMetaInfo();
    FileMeta fm;
    extractFileMeta(meta, fm);
    if (!fm.hasRouting()) {
      logmsg("'" + std::string(file) +
             "' has no usable file meta header (not Part 10?)");
      return 100;
    }

    // stamp the standard-blessed file meta dicomq-recv writes, so an
    // injected object is self-describing for routing and downstream
    // archives just like a received one
    stampOriginAETs(meta, callingAET, calledAET);

    const std::string id = generateId();
    const std::string aetDir = sp.queueTodoAET(dest);
    const std::string tmpDcm = dcmPath(sp.queueTmp(), id);

    // the same commit protocol as dicomq-recv: save into tmp, then one
    // atomic rename into queue/todo/<called-aet>/ commits the message.
    if (!saveAsReceived(ff, tmpDcm, err) || !mkdirIfMissing(aetDir, err) ||
        !commitFile(tmpDcm, dcmPath(aetDir, id), err)) {
      logmsg(err);
      unlink(tmpDcm.c_str());
      removeMessage(aetDir, id, err);
      return 111;
    }
    std::printf("%s\n", id.c_str());
  }
  return 0;
}
