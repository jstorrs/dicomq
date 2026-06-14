// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// dicomq-inject — local submission (qmail-inject analog). Enqueues a
// DICOM file as if it had been received, for testing the queue without
// DICOM networking and for re-queueing objects (e.g. from failed/).
//
//   dicomq-inject [-s <spool>] -c <called-aet> [-a <calling-aet>] <file.dcm>...
//
// Per file: read SOP class/instance and transfer syntax UIDs from the
// file meta header, fabricate an envelope (peer: local), byte-copy the
// object into queue/tmp/ — the source preamble and meta are preserved
// as-is; some real files carry load-bearing preambles (dual TIFF/DICOM)
// — and commit into queue/todo/ (.env last), the same protocol as
// dicomq-recv. Prints each new message id on stdout.
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

#include "common/envelope.h"
#include "common/message.h"
#include "common/spool.h"

using namespace dicomq;

static int usage()
{
  std::fprintf(stderr,
      "usage: dicomq-inject [-s <spool>] -c <called-aet> [-a <calling-aet>] <file.dcm>...\n");
  return 100;
}

int main(int argc, char **argv)
{
  std::string spoolArg, calledAET, callingAET = "LOCAL";
  int opt;
  while ((opt = getopt(argc, argv, "s:c:a:")) != -1)
  {
    switch (opt)
    {
      case 's': spoolArg = optarg; break;
      case 'c': calledAET = optarg; break;
      case 'a': callingAET = optarg; break;
      default: return usage();
    }
  }
  if (calledAET.empty() || optind >= argc)
    return usage();

  const Spool sp(spoolArg);
  std::string err;

  for (int i = optind; i < argc; i++)
  {
    const char *file = argv[i];

    DcmFileFormat ff;
    OFCondition cond = ff.loadFile(file);
    if (cond.bad())
    {
      std::fprintf(stderr, "dicomq-inject: cannot read '%s': %s\n", file,
                   cond.text());
      return 100;
    }
    OFString sopClass, sopInstance, xferUID;
    DcmMetaInfo *meta = ff.getMetaInfo();
    if (!meta
        || meta->findAndGetOFString(DCM_MediaStorageSOPClassUID, sopClass).bad()
        || meta->findAndGetOFString(DCM_MediaStorageSOPInstanceUID, sopInstance).bad()
        || meta->findAndGetOFString(DCM_TransferSyntaxUID, xferUID).bad()
        || sopClass.empty() || sopInstance.empty() || xferUID.empty())
    {
      std::fprintf(stderr,
          "dicomq-inject: '%s' has no usable file meta header (not Part 10?)\n",
          file);
      return 100;
    }

    const std::string id = generateId();
    const std::string tmpDcm = dcmPath(sp.queueTmp(), id);

    Envelope env;
    env.add("id", id);
    env.add("received", isoTimeMillis());
    env.add("peer", "local");
    env.add("calling-aet", callingAET);
    env.add("called-aet", calledAET);
    env.add("sop-class-uid", sopClass.c_str());
    env.add("sop-instance-uid", sopInstance.c_str());
    env.add("transfer-syntax-uid", xferUID.c_str());

    // the same commit protocol as dicomq-recv: object first, envelope
    // last; its appearance in todo/ is the commit point
    if (!copyFile(file, tmpDcm, err)
        || !commitFile(tmpDcm, dcmPath(sp.queueTodo(), id), err)
        || !writeEnvelopeCommitted(sp, env, envPath(sp.queueTodo(), id), err))
    {
      std::fprintf(stderr, "dicomq-inject: %s\n", err.c_str());
      unlink(tmpDcm.c_str());
      removePair(sp.queueTodo(), id, err);
      return 111;
    }
    std::printf("%s\n", id.c_str());
  }
  return 0;
}