// dicomq-remote — C-STORE forwarder for one destination (qmail-remote
// analog).
//
//   dicomq-remote [-s <spool>] [-L <lifetime-days>] <DEST>
//
// Opens ONE association to dest/<DEST>/remote, proposing presentation
// contexts per dest/<DEST>/propose (transfer syntaxes + transcode
// policy), and C-STOREs every due message in route/<DEST>/todo/ over it.
//
// Per message: success unlinks the pair; rejection by the reachable
// destination appends an "attempt:" line and recommits the envelope
// copy (its mtime is the last-attempt time); a permanent impossibility
// (no accepted context, transcode forbidden or unavailable) or an
// exhausted queue lifetime (default 7 days) moves it to failed/ with a
// reason. Transcoding happens per attempt, in memory; the queued object
// is never modified. Policy: 'never' requires the stored syntax to be
// accepted; 'lossless' transcodes but refuses lossy target syntaxes
// (decompressing FROM lossy adds no further loss and is allowed);
// 'as-needed' transcodes to anything the destination accepted.
//
// If dest/<DEST>/tls/ exists, the association uses DICOM TLS (BCP 195):
// tls/ca.pem verifies the server (required when present, otherwise the
// peer certificate is not checked — use a CA), and tls/key.pem +
// tls/cert.pem, when present, authenticate us to the server.
//
// A connection-level failure is destination state, not message state:
// it is recorded once in route/<DEST>/status (last-failure, failures,
// next-attempt-after with exponential backoff capped at 1h) and the
// program exits without touching any envelope; dicomq-send skips the
// destination until the status file says otherwise. A successful
// association removes the status file.
//
// Exit: 0 ran to completion (individual messages may have failed);
// 100 bad usage/config; 111 spool trouble.

#include "dcmtk/config/osconfig.h"

#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/dcmdata/dcrledrg.h"
#include "dcmtk/dcmdata/dcrleerg.h"
#include "dcmtk/dcmdata/dcuid.h"
#include "dcmtk/dcmdata/dcxfer.h"
#include "dcmtk/dcmjpeg/djdecode.h"
#include "dcmtk/dcmjpeg/djencode.h"
#include "dcmtk/dcmjpls/djdecode.h"
#include "dcmtk/dcmjpls/djencode.h"
#include "dcmtk/dcmnet/dimse.h"
#include "dcmtk/dcmnet/diutil.h"
#include "dcmtk/dcmtls/tlslayer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "common/envelope.h"
#include "common/message.h"
#include "common/profile.h"
#include "common/spool.h"

using namespace dicomq;

static Spool sp;
static std::string destName;

static void logmsg(const std::string& m)
{
  std::fprintf(stderr, "dicomq-remote: %s: %s\n", destName.c_str(), m.c_str());
}

// destination-level backoff (dead-site cache): one file, not N envelopes
static void recordConnectionFailure(const std::string& reason)
{
  std::string err;
  long failures = 0;
  Envelope old;
  if (Envelope::read(sp.routeStatus(destName), old, err))
    failures = atol(old.get("failures").c_str());
  failures++;
  long delay = 60;
  for (long i = 1; i < failures && delay < 3600; i++)
    delay *= 2;
  if (delay > 3600)
    delay = 3600;

  Envelope status;
  status.add("last-failure", isoTime(time(nullptr)) + " " + reason);
  status.add("failures", std::to_string(failures));
  status.add("next-attempt-after", isoTime(time(nullptr) + delay));
  if (!writeEnvelopeCommitted(sp, status, sp.routeStatus(destName), err))
    logmsg("cannot write status: " + err);
  logmsg("connection failed (" + reason + "); next attempt in "
         + std::to_string(delay) + "s");
}

static void failMessage(const std::string& todo, const std::string& id,
                        Envelope env, const std::string& reason)
{
  std::string err;
  logmsg("failing " + id + ": " + reason);
  env.add("failed", isoTime(time(nullptr)) + " " + destName + ": " + reason);
  if (!movePairAnnotated(sp, todo, sp.failedDir(), id, env, err))
    logmsg("cannot fail " + id + ": " + err);
}

static void recordAttempt(const std::string& todo, const std::string& id,
                          Envelope env, const std::string& what)
{
  std::string err;
  env.add("attempt", isoTime(time(nullptr)) + " " + destName + ": " + what);
  if (!writeEnvelopeCommitted(sp, env, envPath(todo, id), err))
    logmsg("cannot record attempt on " + id + ": " + err);
}

int main(int argc, char **argv)
{
  std::string spoolArg;
  long lifetimeDays = 7;
  int opt;
  while ((opt = getopt(argc, argv, "s:L:")) != -1)
  {
    switch (opt)
    {
      case 's': spoolArg = optarg; break;
      case 'L': lifetimeDays = atol(optarg); break;
      default:
        std::fprintf(stderr,
            "usage: dicomq-remote [-s <spool>] [-L <lifetime-days>] <DEST>\n");
        return 100;
    }
  }
  if (lifetimeDays <= 0)
  {
    // a non-positive lifetime would fail every queued message at once
    std::fprintf(stderr, "dicomq-remote: -L must be a positive number of days\n");
    return 100;
  }
  if (optind + 1 != argc)
  {
    std::fprintf(stderr,
        "usage: dicomq-remote [-s <spool>] [-L <lifetime-days>] <DEST>\n");
    return 100;
  }
  destName = argv[optind];
  sp = Spool(spoolArg);
  const std::string todo = sp.routeTodo(destName);

  std::string err;
  RemoteConfig cfg;
  if (!RemoteConfig::load(sp.destDir(destName) + "/remote", cfg, err))
  {
    logmsg(err);
    return 100;
  }
  ProposeProfile profile;
  if (!ProposeProfile::load(sp.destDir(destName) + "/propose", profile, err))
  {
    logmsg(err);
    return 100;
  }
  const std::string callingAET =
      cfg.callingAET.empty() ? "DICOMQ" : cfg.callingAET;

  // gather due work (and expire the dead) before connecting
  const time_t now = time(nullptr);
  std::vector<std::string> work;
  std::map<std::string, Envelope> envs;
  std::set<std::string> sopClasses;
  for (const auto& id : listIds(todo))
  {
    Envelope env;
    if (!Envelope::read(envPath(todo, id), env, err))
    {
      logmsg("quarantining " + id + ": " + err);
      if (!movePairRaw(sp, todo, sp.corruptDir(), id, err))
        logmsg("cannot quarantine " + id + ": " + err);
      continue;
    }
    if (now - idTime(id) > lifetimeDays * 86400)
    {
      failMessage(todo, id, env, "queue lifetime exhausted");
      continue;
    }
    // never-attempted messages are always due; attempted ones follow
    // the backoff schedule keyed on the envelope copy's mtime
    struct stat st;
    if (env.count("attempt") > 0
        && (stat(envPath(todo, id).c_str(), &st) != 0
            || !isDue(now, st.st_mtime, idTime(id))))
      continue;
    if (env.get("sop-class-uid").empty())
    {
      failMessage(todo, id, env, "envelope lacks sop-class-uid");
      continue;
    }
    sopClasses.insert(env.get("sop-class-uid"));
    envs[id] = env;
    work.push_back(id);
  }
  if (work.empty())
    return 0;

  // decoders for transcoding to uncompressed syntaxes, encoders for
  // transcoding to compressed ones
  DJDecoderRegistration::registerCodecs();
  DJLSDecoderRegistration::registerCodecs();
  DcmRLEDecoderRegistration::registerCodecs();
  DJEncoderRegistration::registerCodecs();
  DJLSEncoderRegistration::registerCodecs();
  DcmRLEEncoderRegistration::registerCodecs();

  OFStandard::initializeNetwork();
  T_ASC_Network *net = nullptr;
  OFCondition cond = ASC_initializeNetwork(NET_REQUESTOR, 0, 30, &net);
  if (cond.bad())
  {
    logmsg(std::string("cannot initialize network: ") + cond.text());
    return 111;
  }

  // dest/<DEST>/tls/ exists => DICOM TLS to this destination
  const std::string tlsDir = sp.destDir(destName) + "/tls";
  const bool useTLS = isDir(tlsDir);
  if (useTLS)
  {
    DcmTLSTransportLayer::initializeOpenSSL();
    DcmTLSTransportLayer *layer =
        new DcmTLSTransportLayer(NET_REQUESTOR, nullptr, OFFalse);
    bool ok = layer->setTLSProfile(TSP_Profile_BCP195).good()
              && layer->activateCipherSuites().good();
    const std::string ca = tlsDir + "/ca.pem";
    if (ok && pathExists(ca))
    {
      ok = layer->addTrustedCertificateFile(ca.c_str(), DCF_Filetype_PEM).good();
      layer->setCertificateVerification(DCV_requireCertificate);
    }
    else
      layer->setCertificateVerification(DCV_ignoreCertificate);
    const std::string key = tlsDir + "/key.pem", cert = tlsDir + "/cert.pem";
    if (ok && pathExists(key))
      ok = layer->setPrivateKeyFile(key.c_str(), DCF_Filetype_PEM).good()
           && layer->setCertificateFile(cert.c_str(), DCF_Filetype_PEM, TSP_Profile_BCP195).good()
           && layer->checkPrivateKeyMatchesCertificate();
    if (!ok || ASC_setTransportLayer(net, layer, 1).bad())
    {
      logmsg("TLS setup failed for '" + tlsDir + "'");
      return 100;  // config problem, not destination weather
    }
  }

  T_ASC_Parameters *params = nullptr;
  ASC_createAssociationParameters(&params, ASC_DEFAULTMAXPDU,
                                  30 /* TCP connect timeout, seconds */);
  ASC_setAPTitles(params, callingAET.c_str(), cfg.aet.c_str(), nullptr);
  char localHost[256] = {0};  // POSIX leaves the result unterminated on
  gethostname(localHost, sizeof(localHost) - 1);  // truncation; pre-NUL it
  const std::string peer = cfg.host + ":" + std::to_string(cfg.port);
  ASC_setPresentationAddresses(params, localHost, peer.c_str());
  if (useTLS)
    ASC_setTransportLayerType(params, OFTrue);

  // one context per (SOP class, transfer syntax): precise control over
  // what an accepted context implies
  std::vector<std::string> proposeTS = profile.transferSyntaxes;
  if (proposeTS.empty())
  {
    for (const auto& w : work)
    {
      const std::string ts = envs[w].get("transfer-syntax-uid");
      bool seen = false;
      for (const auto& p : proposeTS)
        seen = seen || p == ts;
      if (!seen && !ts.empty())
        proposeTS.push_back(ts);
    }
    proposeTS.push_back(UID_LittleEndianExplicitTransferSyntax);
    proposeTS.push_back(UID_LittleEndianImplicitTransferSyntax);
  }
  // The association can carry at most 128 presentation contexts (odd ids
  // 1..255). With many SOP classes in one batch the budget can run out
  // before every class gets a context; classes left unproposed must be
  // DEFERRED, not failed (a later, smaller batch will fit them), so
  // remember which classes we actually proposed.
  std::set<std::string> proposedSops;
  T_ASC_PresentationContextID pid = 1;
  for (const auto& sop : sopClasses)
  {
    if (pid > 253)
      break;  // 128-context PDU limit reached; remaining classes deferred
    for (const auto& ts : proposeTS)
    {
      if (pid > 253)
        break;
      const char *tsArr[] = { ts.c_str() };
      ASC_addPresentationContext(params, pid, sop.c_str(), tsArr, 1);
      proposedSops.insert(sop);
      pid += 2;
    }
  }

  T_ASC_Association *assoc = nullptr;
  cond = ASC_requestAssociation(net, params, &assoc);
  if (cond.bad() || ASC_countAcceptedPresentationContexts(params) == 0)
  {
    recordConnectionFailure(cond.bad() ? cond.text()
                            : "no presentation context accepted");
    if (assoc)
    {
      ASC_abortAssociation(assoc);
      ASC_destroyAssociation(&assoc);
    }
    ASC_dropNetwork(&net);
    return 0;
  }

  bool connectionBroke = false;
  for (const auto& id : work)
  {
    Envelope& env = envs[id];
    const std::string sopClass = env.get("sop-class-uid");
    const std::string sopInstance = env.get("sop-instance-uid");

    DcmFileFormat ff;
    cond = ff.loadFile(dcmPath(todo, id).c_str());
    if (cond.bad())
    {
      logmsg("quarantining " + id + ": " + cond.text());
      if (!movePairRaw(sp, todo, sp.corruptDir(), id, err))
        logmsg("cannot quarantine " + id + ": " + err);
      continue;
    }
    DcmDataset *dataset = ff.getDataset();
    const DcmXfer objXfer(dataset->getOriginalXfer());

    // Note: the 3-arg lookup FALLS BACK to any context for the abstract
    // syntax when no exact transfer syntax match exists, so the accepted
    // TS must be compared explicitly — DIMSE_storeUser would otherwise
    // convert silently, bypassing the transcode policy.
    T_ASC_PresentationContextID presID =
        ASC_findAcceptedPresentationContextID(assoc, sopClass.c_str(),
                                              objXfer.getXferID());
    if (presID == 0)
      presID = ASC_findAcceptedPresentationContextID(assoc, sopClass.c_str());
    if (presID == 0)
    {
      // No usable context. Distinguish our own context-budget limit (we
      // never proposed this class — defer so a later batch can carry it)
      // from a destination that refused a class we did propose (a real,
      // permanent rejection of that SOP class).
      if (proposedSops.count(sopClass) == 0)
        recordAttempt(todo, id, env, "deferred: presentation-context budget "
                      "exhausted, " + sopClass + " not proposed this batch");
      else
        failMessage(todo, id, env, "no presentation context accepted for "
                    + sopClass);
      continue;
    }
    T_ASC_PresentationContext pc;
    ASC_findAcceptedPresentationContext(assoc->params, presID, &pc);
    if (strcmp(pc.acceptedTransferSyntax, objXfer.getXferID()) != 0)
    {
      // stored syntax not accepted: transcode or fail, per policy
      if (profile.transcode == ProposeProfile::Transcode::Never)
      {
        failMessage(todo, id, env, std::string("stored syntax ")
                    + objXfer.getXferID()
                    + " not accepted and transcode is 'never'");
        continue;
      }
      if (profile.transcode == ProposeProfile::Transcode::Lossless
          && isLossyTransferSyntaxUID(pc.acceptedTransferSyntax))
      {
        failMessage(todo, id, env, std::string("accepted syntax ")
                    + pc.acceptedTransferSyntax
                    + " is lossy and transcode is 'lossless'");
        continue;
      }
      const DcmXfer target(pc.acceptedTransferSyntax);
      const OFCondition xc = dataset->chooseRepresentation(target.getXfer(),
                                                           nullptr);
      if (xc.bad() || !dataset->canWriteXfer(target.getXfer()))
      {
        failMessage(todo, id, env, std::string("cannot transcode ")
                    + objXfer.getXferID() + " -> " + target.getXferID()
                    + (xc.bad() ? std::string(": ") + xc.text() : ""));
        continue;
      }
    }

    T_DIMSE_C_StoreRQ req;
    memset(&req, 0, sizeof(req));
    req.MessageID = assoc->nextMsgID++;
    OFStandard::strlcpy(req.AffectedSOPClassUID, sopClass.c_str(),
                        sizeof(req.AffectedSOPClassUID));
    OFStandard::strlcpy(req.AffectedSOPInstanceUID, sopInstance.c_str(),
                        sizeof(req.AffectedSOPInstanceUID));
    req.DataSetType = DIMSE_DATASET_PRESENT;
    req.Priority = DIMSE_PRIORITY_MEDIUM;

    T_DIMSE_C_StoreRSP rsp;
    memset(&rsp, 0, sizeof(rsp));
    DcmDataset *statusDetail = nullptr;
    cond = DIMSE_storeUser(assoc, presID, &req, nullptr, dataset, nullptr,
                           nullptr, DIMSE_BLOCKING, 0, &rsp, &statusDetail);
    delete statusDetail;

    if (cond.bad())
    {
      // mid-association breakage is destination state; envelopes stay
      // untouched so nothing is double-penalized
      recordConnectionFailure(cond.text());
      connectionBroke = true;
      break;
    }
    if (rsp.DimseStatus == STATUS_Success
        || (rsp.DimseStatus & 0xf000) == 0xB000)  // warnings are delivered
    {
      if (!removePair(todo, id, err))
        logmsg("delivered " + id + " but cannot dequeue: " + err);
      else
        logmsg("delivered " + id);
    }
    else
    {
      char status[16];
      snprintf(status, sizeof(status), "0x%04x", rsp.DimseStatus);
      logmsg(id + " rejected with status " + status);
      recordAttempt(todo, id, env, std::string("rejected ") + status);
    }
  }

  if (!connectionBroke)
  {
    ASC_releaseAssociation(assoc);
    unlink(sp.routeStatus(destName).c_str());  // the site is alive
  }
  ASC_destroyAssociation(&assoc);
  ASC_dropNetwork(&net);
  DJDecoderRegistration::cleanup();
  DJLSDecoderRegistration::cleanup();
  DcmRLEDecoderRegistration::cleanup();
  DJEncoderRegistration::cleanup();
  DJLSEncoderRegistration::cleanup();
  DcmRLEEncoderRegistration::cleanup();
  return 0;
}
