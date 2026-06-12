// dicomq-recv — DICOM C-STORE receiver (qmail-smtpd analog).
//
//   dicomq-recv [-s <spool>] [-w <watermark-MB>] [--listen <port>] [--once]
//
// Default mode: one process per association under a socket supervisor
// (systemd Accept=yes / s6 / ucspi-tcp); the connected socket is
// inherited on fd 0 and the process exits when the association ends.
// --listen accepts associations itself, sequentially — the test and
// small-site mode (--once exits after the first).
//
// Per association: refuse if spool free space is below the watermark
// (default 1024 MB); reject if aet/<called-aet>/ does not exist;
// negotiate presentation contexts per aet/<called-aet>/accept. Per
// object: write queue/tmp/<id>.dcm with a zeroed preamble and file meta
// stamped with Source/Sending/ReceivingApplicationEntityTitle
// (0002,0016/17/18), fsync, commit into queue/todo/ (.env last), THEN
// answer the C-STORE; any failure removes the tmp files and answers
// with a refused status. The envelope, not the preamble, carries queue
// state (DESIGN.md "Why a sidecar, not the DICOM preamble").
//
// Exit: 0 association handled; 111 startup/network failure.

#include "dcmtk/config/osconfig.h"

#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/dcmdata/dcmetinf.h"
#include "dcmtk/dcmdata/dcuid.h"
#include "dcmtk/dcmdata/dcxfer.h"
#include "dcmtk/dcmnet/diutil.h"
#include "dcmtk/dcmnet/dimse.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/envelope.h"
#include "common/message.h"
#include "common/profile.h"
#include "common/spool.h"

using namespace dicomq;

static Spool sp;
static long long watermarkBytes = 1024LL * 1024 * 1024;

static void logmsg(const std::string& m)
{
  std::fprintf(stderr, "dicomq-recv: %s\n", m.c_str());
}

static bool isDir(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// ---------------------------------------------------------------------------
// per-store state shared with the DIMSE callback

struct StoreContext {
  DcmFileFormat *ff;
  std::string callingAET, calledAET, peer;
};

static void storeCallback(void *cbData, T_DIMSE_StoreProgress *progress,
                          T_DIMSE_C_StoreRQ *req, char * /*imageFileName*/,
                          DcmDataset **imageDataSet, T_DIMSE_C_StoreRSP *rsp,
                          DcmDataset **statusDetail)
{
  if (progress->state != DIMSE_StoreEnd)
    return;
  *statusDetail = nullptr;

  StoreContext *ctx = static_cast<StoreContext *>(cbData);
  if (rsp->DimseStatus != STATUS_Success || !imageDataSet || !*imageDataSet)
    return;  // provider already failing; enqueue nothing

  // the dataset must actually be what the request claims it is
  DIC_UI sopClass, sopInstance;
  if (!DU_findSOPClassAndInstanceInDataSet(*imageDataSet, sopClass,
          sizeof(sopClass), sopInstance, sizeof(sopInstance), OFFalse))
  {
    rsp->DimseStatus = STATUS_STORE_Error_CannotUnderstand;
    return;
  }
  if (strcmp(sopClass, req->AffectedSOPClassUID) != 0
      || strcmp(sopInstance, req->AffectedSOPInstanceUID) != 0)
  {
    rsp->DimseStatus = STATUS_STORE_Error_DataSetDoesNotMatchSOPClass;
    return;
  }

  // standard-blessed in-file metadata: who sent it, who it was for
  DcmMetaInfo *meta = ctx->ff->getMetaInfo();
  meta->putAndInsertString(DCM_SourceApplicationEntityTitle,
                           ctx->callingAET.c_str());
  meta->putAndInsertString(DCM_SendingApplicationEntityTitle,
                           ctx->callingAET.c_str());
  meta->putAndInsertString(DCM_ReceivingApplicationEntityTitle,
                           ctx->calledAET.c_str());

  const E_TransferSyntax xfer = (*imageDataSet)->getOriginalXfer();
  const std::string id = generateId();
  const std::string tmpDcm = dcmPath(sp.queueTmp(), id);
  std::string err;

  OFCondition cond = ctx->ff->saveFile(tmpDcm.c_str(), xfer,
      EET_ExplicitLength, EGL_recalcGL, EPD_withoutPadding, 0, 0,
      EWM_fileformat);
  if (cond.bad())
  {
    logmsg("cannot write " + tmpDcm + ": " + cond.text());
    unlink(tmpDcm.c_str());
    rsp->DimseStatus = STATUS_STORE_Refused_OutOfResources;
    return;
  }

  Envelope env;
  env.add("id", id);
  env.add("received", isoTime(time(nullptr)));
  env.add("peer", ctx->peer);
  env.add("calling-aet", ctx->callingAET);
  env.add("called-aet", ctx->calledAET);
  env.add("sop-class-uid", sopClass);
  env.add("sop-instance-uid", sopInstance);
  env.add("transfer-syntax-uid", DcmXfer(xfer).getXferID());

  // object first, envelope last; only after both are durable may the
  // C-STORE response report success
  if (!commitFile(tmpDcm, dcmPath(sp.queueTodo(), id), err)
      || !writeEnvelopeCommitted(sp, env, envPath(sp.queueTodo(), id), err))
  {
    logmsg("cannot enqueue " + id + ": " + err);
    unlink(tmpDcm.c_str());
    removePair(sp.queueTodo(), id, err);
    rsp->DimseStatus = STATUS_STORE_Refused_OutOfResources;
    return;
  }
  logmsg("queued " + id + " from " + ctx->callingAET + " for "
         + ctx->calledAET);
}

// ---------------------------------------------------------------------------

static void rejectAssociation(T_ASC_Association *assoc,
                              T_ASC_RejectParametersResult result,
                              T_ASC_RejectParametersSource source,
                              T_ASC_RejectParametersReason reason)
{
  T_ASC_RejectParameters rej = { result, source, reason };
  ASC_rejectAssociation(assoc, &rej);
}

static int handleAssociation(T_ASC_Association *assoc)
{
  // free-space watermark: refuse before anything is received
  const long long freeNow = freeBytes(sp.root);
  if (freeNow >= 0 && freeNow < watermarkBytes)
  {
    logmsg("refusing association: " + std::to_string(freeNow)
           + " bytes free is below the watermark");
    rejectAssociation(assoc, ASC_RESULT_REJECTEDTRANSIENT,
                      ASC_SOURCE_SERVICEPROVIDER_PRESENTATION_RELATED,
                      ASC_REASON_SP_PRES_TEMPORARYCONGESTION);
    return 0;
  }

  DIC_AE callingTitle, calledTitle;
  if (ASC_getAPTitles(assoc->params, callingTitle, sizeof(callingTitle),
                      calledTitle, sizeof(calledTitle), nullptr, 0).bad())
  {
    rejectAssociation(assoc, ASC_RESULT_REJECTEDPERMANENT,
                      ASC_SOURCE_SERVICEUSER, ASC_REASON_SU_NOREASON);
    return 0;
  }
  const std::string called = sanitizeAET(calledTitle);

  // unknown recipient: rejected at "RCPT TO", not bounced later
  if (isReservedName(called) || !isDir(sp.aetDir(called)))
  {
    logmsg("rejecting association: unknown called AET '"
           + std::string(calledTitle) + "'");
    rejectAssociation(assoc, ASC_RESULT_REJECTEDPERMANENT,
                      ASC_SOURCE_SERVICEUSER,
                      ASC_REASON_SU_CALLEDAETITLENOTRECOGNIZED);
    return 0;
  }

  AcceptProfile profile;
  std::string err;
  if (!AcceptProfile::load(sp.aetDir(called) + "/accept", profile, err))
  {
    logmsg("rejecting association: " + err);
    rejectAssociation(assoc, ASC_RESULT_REJECTEDTRANSIENT,
                      ASC_SOURCE_SERVICEPROVIDER_PRESENTATION_RELATED,
                      ASC_REASON_SP_PRES_TEMPORARYCONGESTION);
    return 0;
  }

  std::vector<const char *> ts;
  for (const auto& uid : profile.transferSyntaxes)
    ts.push_back(uid.c_str());
  if (profile.acceptAll)
  {
    static const char *all[] = {
      UID_JPEGLSLosslessTransferSyntax,
      UID_JPEG2000LosslessOnlyTransferSyntax,
      UID_JPEGProcess14SV1TransferSyntax,
      UID_JPEGLSLossyTransferSyntax,
      UID_JPEG2000TransferSyntax,
      UID_JPEGProcess1TransferSyntax,
      UID_JPEGProcess2_4TransferSyntax,
      UID_RLELosslessTransferSyntax,
      UID_DeflatedExplicitVRLittleEndianTransferSyntax,
      UID_BigEndianExplicitTransferSyntax,
    };
    for (const char *u : all)
      ts.push_back(u);
  }
  if (ts.empty() || profile.acceptAll)
  {
    ts.push_back(UID_LittleEndianExplicitTransferSyntax);
    ts.push_back(UID_LittleEndianImplicitTransferSyntax);
  }

  const char *abstractSyntaxes[] = { UID_VerificationSOPClass };
  OFCondition cond = ASC_acceptContextsWithPreferredTransferSyntaxes(
      assoc->params, abstractSyntaxes, 1, ts.data(),
      static_cast<int>(ts.size()));
  if (cond.good())
    cond = ASC_acceptContextsWithPreferredTransferSyntaxes(
        assoc->params, dcmAllStorageSOPClassUIDs,
        numberOfDcmAllStorageSOPClassUIDs, ts.data(),
        static_cast<int>(ts.size()));
  if (cond.bad())
  {
    logmsg(std::string("cannot accept presentation contexts: ") + cond.text());
    rejectAssociation(assoc, ASC_RESULT_REJECTEDTRANSIENT,
                      ASC_SOURCE_SERVICEPROVIDER_PRESENTATION_RELATED,
                      ASC_REASON_SP_PRES_TEMPORARYCONGESTION);
    return 0;
  }

  // answer with the called AET: this receiver is whoever was asked for
  ASC_setAPTitles(assoc->params, nullptr, nullptr, calledTitle);
  if (ASC_acknowledgeAssociation(assoc).bad())
    return 111;

  StoreContext ctx;
  ctx.callingAET = callingTitle;
  ctx.calledAET = calledTitle;
  ctx.peer = assoc->params->DULparams.callingPresentationAddress;

  for (;;)
  {
    T_DIMSE_Message msg;
    T_ASC_PresentationContextID presID;
    DcmDataset *statusDetail = nullptr;
    cond = DIMSE_receiveCommand(assoc, DIMSE_BLOCKING, 0, &presID, &msg,
                                &statusDetail);
    delete statusDetail;

    if (cond == DUL_PEERREQUESTEDRELEASE)
    {
      ASC_acknowledgeRelease(assoc);
      return 0;
    }
    if (cond.bad())
    {
      logmsg(std::string("association ended: ") + cond.text());
      return 0;
    }

    if (msg.CommandField == DIMSE_C_ECHO_RQ)
    {
      DIMSE_sendEchoResponse(assoc, presID, &msg.msg.CEchoRQ, STATUS_Success,
                             nullptr);
    }
    else if (msg.CommandField == DIMSE_C_STORE_RQ)
    {
      DcmFileFormat ff;
      DcmDataset *dset = ff.getDataset();
      ctx.ff = &ff;
      cond = DIMSE_storeProvider(assoc, presID, &msg.msg.CStoreRQ, nullptr,
                                 OFTrue, &dset, storeCallback, &ctx,
                                 DIMSE_BLOCKING, 0);
      if (cond.bad())
      {
        logmsg(std::string("store failed: ") + cond.text());
        return 0;
      }
    }
    else
    {
      logmsg("unexpected DIMSE command, aborting");
      ASC_abortAssociation(assoc);
      return 0;
    }
  }
}

int main(int argc, char **argv)
{
  std::string spoolArg;
  long listenPort = 0;
  bool once = false;
  for (int i = 1; i < argc; i++)
  {
    const std::string a = argv[i];
    if (a == "-s" && i + 1 < argc)
      spoolArg = argv[++i];
    else if (a == "-w" && i + 1 < argc)
      watermarkBytes = atoll(argv[++i]) * 1024 * 1024;
    else if (a == "--listen" && i + 1 < argc)
      listenPort = atol(argv[++i]);
    else if (a == "--once")
      once = true;
    else
    {
      std::fprintf(stderr,
          "usage: dicomq-recv [-s <spool>] [-w <watermark-MB>] "
          "[--listen <port>] [--once]\n");
      return 100;
    }
  }

  sp = Spool(spoolArg);
  if (!isDir(sp.queueTmp()) || !isDir(sp.queueTodo()))
  {
    logmsg("'" + sp.root + "' is not a dicomq spool");
    return 111;
  }

  OFStandard::initializeNetwork();

  if (listenPort == 0)
  {
    // supervisor mode: the connected socket is fd 0
    const int sock = dup(0);
    if (sock < 0)
      return 111;
    const int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0)
    {
      dup2(devnull, 0);
      dup2(devnull, 1);
      if (devnull > 1)
        close(devnull);
    }
    dcmExternalSocketHandle.set(sock);
    listenPort = 1024;  // dummy; never bound
    once = true;
  }

  T_ASC_Network *net = nullptr;
  OFCondition cond = ASC_initializeNetwork(NET_ACCEPTOR,
      static_cast<int>(listenPort), 30, &net);
  if (cond.bad())
  {
    logmsg(std::string("cannot initialize network: ") + cond.text());
    return 111;
  }

  int rc = 0;
  do
  {
    T_ASC_Association *assoc = nullptr;
    cond = ASC_receiveAssociation(net, &assoc, ASC_DEFAULTMAXPDU);
    if (cond.bad())
    {
      logmsg(std::string("cannot receive association: ") + cond.text());
      rc = 111;
      break;
    }
    rc = handleAssociation(assoc);
    ASC_dropSCPAssociation(assoc);
    ASC_destroyAssociation(&assoc);
  } while (!once);

  ASC_dropNetwork(&net);
  return rc;
}
