// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// dicomq-remote — C-STORE forwarder for one destination (qmail-remote
// analog).
//
//   dicomq-remote [-s <spool>] <DEST>
//
// Opens ONE association to dest/<DEST>/remote, proposing presentation
// contexts per dest/<DEST>/propose (transfer syntaxes + transcode
// policy), and C-STOREs every due message in route/<DEST>/todo/ and its
// retry/<k>/ rungs over it. The SOP class/instance and transfer syntax
// each message needs come from its DICOM file-meta header — there is no
// sidecar.
//
// Per message: success unlinks it; a rejection by the reachable
// destination demotes it one retry rung (todo -> retry/1 -> retry/2 ...),
// copying it to a private inode so the rung's mtime — the backoff clock —
// is independent of the object's other queued copies (the same .dcm may
// be hardlinked into other destinations' queues). The operator sizes the
// ladder by which route/<DEST>/retry/<k> directories exist: a rejection
// with no next rung — or any permanent impossibility (no accepted
// context, transcode forbidden or unavailable) at the top of the ladder —
// moves the object to failed/. The reason for every outcome is logged,
// never stored. Transcoding happens per attempt, in memory; the
// queued object is never modified. Policy: 'never' requires the stored
// syntax to be accepted; 'lossless' transcodes but refuses lossy target
// syntaxes (decompressing FROM lossy adds no further loss and is
// allowed); 'as-needed' transcodes to anything the destination accepted.
//
// If dest/<DEST>/tls/ exists, the association uses DICOM TLS (BCP 195):
// tls/ca.pem verifies the server (required when present, otherwise the
// peer certificate is not checked — use a CA), and tls/key.pem +
// tls/cert.pem, when present, authenticate us to the server.
//
// A connection-level failure is destination state, not message state:
// it is recorded once in route/<DEST>/status (last-failure, failures,
// next-attempt-after with exponential backoff capped at 1h) and the
// program exits without moving any message; dicomq-send skips the
// destination until the status file says otherwise. A successful
// association removes the status file.
//
// Exit: 0 ran to completion (individual messages may have failed);
// 100 bad usage/config; 111 spool trouble.

#include "dcmtk/config/osconfig.h"

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
#include <set>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "common/dcm.h"
#include "common/kvfile.h"
#include "common/message.h"
#include "common/profile.h"
#include "common/spool.h"

using namespace dicomq;

static Spool sp;
static std::string destName;

// one DICOM object to C-STORE, with the routing fields read once from its
// file-meta header
struct Object {
  std::string path, sopClass, sopInstance, xferUID;
};

// a due message to deliver: where it sits, its retry rung, and its
// objects — one for a single .dcm, many for a sealed study/series batch.
// A batch delivers all-or-nothing: every object goes over one association,
// and a single rejection demotes the whole batch (the destination dedups
// the objects it already received on SOP Instance UID).
struct WorkItem {
  std::string dir;
  int level;
  std::string id;
  bool isBatch;
  std::vector<Object> objects;
};

static void logmsg(const std::string& m)
{
  std::fprintf(stderr, "dicomq-remote: %s: %s\n", destName.c_str(), m.c_str());
}

// destination-level backoff (dead-site cache): one file, not N messages
static void recordConnectionFailure(const std::string& reason)
{
  std::string err;
  long failures = 0;
  KeyValueFile old;
  if (KeyValueFile::read(sp.routeStatus(destName), old, err))
    failures = atol(old.get("failures").c_str());
  failures++;
  long delay = 60;
  for (long i = 1; i < failures && delay < 3600; i++)
    delay *= 2;
  if (delay > 3600)
    delay = 3600;

  KeyValueFile status;
  status.add("last-failure", isoTime(time(nullptr)) + " " + reason);
  status.add("failures", std::to_string(failures));
  status.add("next-attempt-after", isoTime(time(nullptr) + delay));
  if (!writeKeyValueCommitted(sp, status, sp.routeStatus(destName), err))
    logmsg("cannot write status: " + err);
  logmsg("connection failed (" + reason + "); next attempt in "
         + std::to_string(delay) + "s");
}

// a destination rejected this message: climb one retry rung, or fail it
// at the top. The operator sizes the ladder by which route/<DEST>/retry/<k>
// directories exist — dicomq never creates a rung, so a rejection with no
// next rung is terminal. The move to a rung COPIES the object to a fresh
// inode (and unlinks the source) so this rung's mtime — what the backoff
// clock reads — is private, even though the same .dcm may be hardlinked
// into other destinations' queues. The terminal move to failed/ can be a
// plain rename: failed/ is never scheduled.
static void demote(const WorkItem& w, const std::string& reason)
{
  std::string err;
  const int next = w.level + 1;
  const std::string nextDir = sp.routeRetry(destName, next);
  if (!isDir(nextDir))
  {
    logmsg("failing " + w.id + ": " + reason + " (no retry/"
           + std::to_string(next) + " rung)");
    if (!moveMessage(w.dir, sp.failedDir(), w.id, err, w.isBatch))
      logmsg("cannot fail " + w.id + ": " + err);
    return;
  }
  logmsg(w.id + " -> retry/" + std::to_string(next) + ": " + reason);
  // Land on the next rung with a fresh, private mtime (the backoff clock):
  // a single object copies to a new inode; a batch hardlink-trees into a
  // fresh directory whose own mtime is the clock (members share inodes),
  // because the same message may be hardlinked into other destinations.
  if (w.isBatch)
  {
    if (!linkBatchTree(w.dir, nextDir, w.id, err))
    {
      logmsg("cannot demote " + w.id + ": " + err);
      return;
    }
  }
  else
  {
    const std::string tmp = sp.queueTmp() + "/" + w.id + ".retry"
                            + std::to_string(getpid());
    if (!copyFile(dcmPath(w.dir, w.id), tmp, err)
        || !commitFile(tmp, dcmPath(nextDir, w.id), err))
    {
      logmsg("cannot demote " + w.id + ": " + err);
      unlink(tmp.c_str());
      return;
    }
  }
  if (!removeMessage(w.dir, w.id, err, w.isBatch))
    logmsg("demoted " + w.id + " but cannot remove source: " + err);
}

int main(int argc, char **argv)
{
  std::string spoolArg;
  int opt;
  while ((opt = getopt(argc, argv, "s:")) != -1)
  {
    switch (opt)
    {
      case 's': spoolArg = optarg; break;
      default:
        std::fprintf(stderr, "usage: dicomq-remote [-s <spool>] <DEST>\n");
        return 100;
    }
  }
  if (optind + 1 != argc)
  {
    std::fprintf(stderr, "usage: dicomq-remote [-s <spool>] <DEST>\n");
    return 100;
  }
  destName = argv[optind];
  sp = Spool(spoolArg);

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

  // gather due work across todo/ (always due) and every retry/<k> rung,
  // reading each message's routing fields from its file-meta header
  const time_t now = time(nullptr);
  std::vector<WorkItem> work;
  std::set<std::string> sopClasses;

  // read the routing fields one object needs from its file-meta header
  auto readObjectMeta = [](const std::string& path, Object& obj) -> bool {
    FileMeta m;
    if (!readFileMeta(path, m) || !m.hasRouting())
      return false;
    obj = {path, m.sopClass, m.sopInstance, m.transferSyntax};
    return true;
  };

  auto gather = [&](const std::string& dir, int level) {
    for (const auto& msg : listMessages(dir))
    {
      if (!messageDue(dir, msg.id, level, now, msg.isBatch))
        continue;  // attempted and still backing off
      WorkItem item;
      item.dir = dir;
      item.level = level;
      item.id = msg.id;
      item.isBatch = msg.isBatch;
      bool ok = true;
      if (msg.isBatch)
      {
        // a batch is a directory of <objid>.dcm; read every member
        const std::string bdir = messagePath(dir, msg.id, true);
        for (const auto& objid : listIds(bdir))
        {
          Object obj;
          if (!readObjectMeta(dcmPath(bdir, objid), obj)) { ok = false; break; }
          item.objects.push_back(obj);
        }
      }
      else
      {
        Object obj;
        if (readObjectMeta(dcmPath(dir, msg.id), obj))
          item.objects.push_back(obj);
        else
          ok = false;
      }
      if (ok && item.objects.empty())
      {
        // a sealed batch should never be empty; if it is, drop it quietly
        removeMessage(dir, msg.id, err, msg.isBatch);
        continue;
      }
      if (!ok)
      {
        logmsg("quarantining " + msg.id + ": cannot read file meta");
        if (!moveMessage(dir, sp.corruptDir(), msg.id, err, msg.isBatch))
          logmsg("cannot quarantine " + msg.id + ": " + err);
        continue;
      }
      for (const auto& o : item.objects)
        sopClasses.insert(o.sopClass);
      work.push_back(std::move(item));
    }
  };
  for (const auto& d : routeQueueDirs(sp, destName))
    gather(d.first, d.second);
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
      for (const auto& o : w.objects)
      {
        bool seen = false;
        for (const auto& p : proposeTS)
          seen = seen || p == o.xferUID;
        if (!seen && !o.xferUID.empty())
          proposeTS.push_back(o.xferUID);
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
  for (const auto& item : work)
  {
    // A message delivers all-or-nothing: every object goes over this one
    // association. A single object that is rejected or impossible to send
    // demotes the whole message; an object whose SOP class we never got to
    // propose defers the whole message to a later, smaller batch; an
    // object that cannot be loaded quarantines the whole message. The
    // destination dedups any objects of the message it already received.
    bool itemFailed = false, deferItem = false, quarantined = false;
    std::string failReason;

    for (const auto& obj : item.objects)
    {
      DcmFileFormat ff;
      cond = ff.loadFile(obj.path.c_str());
      if (cond.bad())
      {
        logmsg("quarantining " + item.id + ": " + cond.text());
        if (!moveMessage(item.dir, sp.corruptDir(), item.id, err, item.isBatch))
          logmsg("cannot quarantine " + item.id + ": " + err);
        quarantined = true;
        break;
      }
      DcmDataset *dataset = ff.getDataset();
      const DcmXfer objXfer(dataset->getOriginalXfer());

      // Note: the 3-arg lookup FALLS BACK to any context for the abstract
      // syntax when no exact transfer syntax match exists, so the accepted
      // TS must be compared explicitly — DIMSE_storeUser would otherwise
      // convert silently, bypassing the transcode policy.
      T_ASC_PresentationContextID presID =
          ASC_findAcceptedPresentationContextID(assoc, obj.sopClass.c_str(),
                                                objXfer.getXferID());
      if (presID == 0)
        presID = ASC_findAcceptedPresentationContextID(assoc,
                                                       obj.sopClass.c_str());
      if (presID == 0)
      {
        // No usable context. Distinguish our own context-budget limit (we
        // never proposed this class — defer so a later batch carries it; a
        // deferral must not consume a retry rung) from a destination that
        // refused a class we did propose (a real, permanent rejection).
        if (proposedSops.count(obj.sopClass) == 0)
        {
          logmsg("deferred " + item.id + ": presentation-context budget "
                 "exhausted, " + obj.sopClass + " not proposed this batch");
          deferItem = true;
        }
        else
        {
          itemFailed = true;
          failReason = "no presentation context accepted for " + obj.sopClass;
        }
        break;
      }
      T_ASC_PresentationContext pc;
      ASC_findAcceptedPresentationContext(assoc->params, presID, &pc);
      if (strcmp(pc.acceptedTransferSyntax, objXfer.getXferID()) != 0)
      {
        // stored syntax not accepted: transcode or fail, per policy
        if (profile.transcode == ProposeProfile::Transcode::Never)
        {
          itemFailed = true;
          failReason = std::string("stored syntax ") + objXfer.getXferID()
                       + " not accepted and transcode is 'never'";
          break;
        }
        if (profile.transcode == ProposeProfile::Transcode::Lossless
            && isLossyTransferSyntaxUID(pc.acceptedTransferSyntax))
        {
          itemFailed = true;
          failReason = std::string("accepted syntax ")
                       + pc.acceptedTransferSyntax
                       + " is lossy and transcode is 'lossless'";
          break;
        }
        const DcmXfer target(pc.acceptedTransferSyntax);
        const OFCondition xc = dataset->chooseRepresentation(target.getXfer(),
                                                             nullptr);
        if (xc.bad() || !dataset->canWriteXfer(target.getXfer()))
        {
          itemFailed = true;
          failReason = std::string("cannot transcode ") + objXfer.getXferID()
                       + " -> " + target.getXferID()
                       + (xc.bad() ? std::string(": ") + xc.text() : "");
          break;
        }
      }

      T_DIMSE_C_StoreRQ req;
      memset(&req, 0, sizeof(req));
      req.MessageID = assoc->nextMsgID++;
      OFStandard::strlcpy(req.AffectedSOPClassUID, obj.sopClass.c_str(),
                          sizeof(req.AffectedSOPClassUID));
      OFStandard::strlcpy(req.AffectedSOPInstanceUID, obj.sopInstance.c_str(),
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
        // mid-association breakage is destination state; messages stay
        // where they are so nothing is double-penalized
        recordConnectionFailure(cond.text());
        connectionBroke = true;
        break;
      }
      if (rsp.DimseStatus != STATUS_Success
          && (rsp.DimseStatus & 0xf000) != 0xB000)  // warnings are delivered
      {
        char status[16];
        snprintf(status, sizeof(status), "0x%04x", rsp.DimseStatus);
        logmsg(item.id + " rejected with status " + status);
        itemFailed = true;
        failReason = std::string("rejected ") + status;
        break;
      }
    }

    if (connectionBroke)
      break;
    if (quarantined)
      continue;  // the whole message was moved to corrupt/
    if (deferItem)
      continue;  // left in place for a later batch
    if (itemFailed)
    {
      demote(item, failReason);
      continue;
    }
    if (!removeMessage(item.dir, item.id, err, item.isBatch))
      logmsg("delivered " + item.id + " but cannot dequeue: " + err);
    else
      logmsg("delivered " + item.id
             + (item.isBatch ? " (" + std::to_string(item.objects.size())
                                      + " objects)" : ""));
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
