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

#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/dcmdata/dcmetinf.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "common/dcm.h"
#include "common/message.h"
#include "common/spool.h"

using namespace dicomq;

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
    std::fprintf(stderr,
                 "dicomq-inject: called AET '%s' resolves to reserved name "
                 "'%s'\n",
                 calledAET.c_str(), dest.c_str());
    return 100;
  }

  const Spool sp(spoolArg);
  std::string err;

  for (int i = optind; i < argc; i++) {
    const char *file = argv[i];

    DcmFileFormat ff;
    OFCondition cond = ff.loadFile(file);
    if (cond.bad()) {
      std::fprintf(stderr, "dicomq-inject: cannot read '%s': %s\n", file,
                   cond.text());
      return 100;
    }
    DcmMetaInfo *meta = ff.getMetaInfo();
    FileMeta fm;
    extractFileMeta(meta, fm);
    if (!fm.hasRouting()) {
      std::fprintf(
          stderr,
          "dicomq-inject: '%s' has no usable file meta header (not Part 10?)\n",
          file);
      return 100;
    }

    // stamp the standard-blessed file meta dicomq-recv writes, so an
    // injected object is self-describing for routing and downstream
    // archives just like a received one
    meta->putAndInsertString(DCM_SourceApplicationEntityTitle,
                             callingAET.c_str());
    meta->putAndInsertString(DCM_SendingApplicationEntityTitle,
                             callingAET.c_str());
    meta->putAndInsertString(DCM_ReceivingApplicationEntityTitle,
                             calledAET.c_str());

    const std::string id = generateId();
    const std::string aetDir = sp.queueTodoAET(dest);
    const std::string tmpDcm = dcmPath(sp.queueTmp(), id);

    // the same commit protocol as dicomq-recv: save into tmp, then one
    // atomic rename into queue/todo/<called-aet>/ commits the message.
    // EWM_fileformat preserves the loaded preamble (dual TIFF/DICOM).
    const OFCondition saved = ff.saveFile(
        tmpDcm.c_str(), ff.getDataset()->getOriginalXfer(), EET_ExplicitLength,
        EGL_recalcGL, EPD_withoutPadding, 0, 0, EWM_fileformat);
    if (saved.bad() || !mkdirIfMissing(aetDir, err) ||
        !commitFile(tmpDcm, dcmPath(aetDir, id), err)) {
      std::fprintf(stderr, "dicomq-inject: %s\n",
                   saved.bad() ? saved.text() : err.c_str());
      unlink(tmpDcm.c_str());
      removeMessage(aetDir, id, err);
      return 111;
    }
    std::printf("%s\n", id.c_str());
  }
  return 0;
}
