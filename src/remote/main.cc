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
// moves the object to route/<DEST>/failed/. A delivered message moves to
// route/<DEST>/complete/ (auto-reaped by dicomq-clean), and an unreadable
// one to route/<DEST>/corrupt/; all three sinks are per-destination so a
// fan-out object never aliases across them. The reason for every outcome is
// logged,
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

#include <csignal>
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
#include "common/tls.h"

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

// What became of one message's delivery attempt. These outcomes are
// mutually exclusive (a message is all-or-nothing), so the per-message loop
// dispatches on one value rather than juggling parallel booleans.
enum class Outcome {
  Delivered,      // every object accepted -> complete/
  Demote,         // a reachable destination rejected it -> next retry rung
  Defer,          // our context budget didn't reach its SOP class this batch
  FailPermanent,  // no retry can ever help -> straight to failed/
  Quarantine,     // unreadable on load -> corrupt/
  ConnectionBroke // mid-association failure -> destination state, stop here
};
struct DeliveryResult {
  Outcome outcome;
  std::string reason; // logged (or recorded, for ConnectionBroke); never stored
};

// Presentation-context ids are odd and a Uint8, so 253 is the last usable id
// (255 + 2 would wrap to 1) — and that ceiling is exactly kMaxContexts
// distinct contexts: (253 - 1)/2 + 1 = 127. With many SOP classes in one run
// the budget can run out before every class gets a context; classes left
// unproposed are normally DEFERRED, not failed (a later, smaller batch will
// fit them). The exception is a single message that on its own needs more than
// the budget — no later batch is smaller than one message, so that one must
// fail (FailPermanent), not defer.
static constexpr int kMaxPresentationContextID = 253;
static constexpr size_t kMaxContexts = 127;
// The two encode the same fact (the count of usable odd Uint8 ids), so pin
// them together: a change to either that broke the identity would otherwise
// let openAssociation propose more contexts than deliverMessage's defer-vs-fail
// check accounts for, livelocking an over-budget message instead of failing it.
static_assert(kMaxContexts == (kMaxPresentationContextID - 1) / 2 + 1,
              "kMaxContexts must equal the number of usable presentation "
              "context ids");

static void logmsg(const std::string &m) {
  dicomq::logmsg("dicomq-remote: " + destName, m);
}

// destination-level backoff (dead-site cache): one file, not N messages
static void recordConnectionFailure(const std::string &reason) {
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

  const time_t now = time(nullptr);
  KeyValueFile status;
  status.add("last-failure", isoTime(now) + " " + reason);
  status.add("failures", std::to_string(failures));
  status.add("next-attempt-after", isoTime(now + delay));
  if (!writeKeyValueCommitted(sp, status, sp.routeStatus(destName), err))
    logmsg("cannot write status: " + err);
  logmsg("connection failed (" + reason + "); next attempt in " +
         std::to_string(delay) + "s");
}

// Move a finished forwarding message into one of this destination's terminal
// sinks (complete/, failed/, corrupt/), creating the sink on first use. The
// sinks are per-destination: a fan-out object that ends differently at two
// destinations would otherwise alias in a shared sink — its hardlinks share
// one inode, so the second rename is a POSIX no-op that leaves the source in
// place and that destination re-processes it forever. None of the sinks is
// ever scheduled, so a plain rename (no fresh-mtime copy) is fine.
static bool sinkMessage(const std::string &sinkDir, const std::string &fromDir,
                        const std::string &id, bool isBatch, std::string &err) {
  return mkdirIfMissing(sinkDir, err) &&
         moveMessage(fromDir, sinkDir, id, err, isBatch);
}

// a destination rejected this message: climb one retry rung, or fail it
// at the top. The operator sizes the ladder by which route/<DEST>/retry/<k>
// directories exist — dicomq never creates a rung, so a rejection with no
// next rung is terminal. The move to a rung COPIES the object to a fresh
// inode (and unlinks the source) so this rung's mtime — what the backoff
// clock reads — is private, even though the same .dcm may be hardlinked
// into other destinations' queues. The terminal move to route/<DEST>/failed/
// is a plain rename: failed/ is never scheduled.
static void demote(const WorkItem &w, const std::string &reason) {
  std::string err;
  const int next = w.level + 1;
  const std::string nextDir = sp.routeRetry(destName, next);
  if (!isDir(nextDir)) {
    logmsg("failing " + w.id + ": " + reason + " (no retry/" +
           std::to_string(next) + " rung)");
    if (!sinkMessage(sp.routeFailed(destName), w.dir, w.id, w.isBatch, err))
      logmsg("cannot fail " + w.id + ": " + err);
    return;
  }
  logmsg(w.id + " -> retry/" + std::to_string(next) + ": " + reason);
  // Land on the next rung with a fresh, private mtime (the backoff clock):
  // a single object copies to a new inode; a batch hardlink-trees into a
  // fresh directory whose own mtime is the clock (members share inodes),
  // because the same message may be hardlinked into other destinations.
  if (w.isBatch) {
    if (!linkBatchTree(w.dir, nextDir, w.id, err)) {
      logmsg("cannot demote " + w.id + ": " + err);
      return;
    }
  } else {
    const std::string tmp =
        sp.queueTmp() + "/" + w.id + ".retry" + std::to_string(getpid());
    if (!copyFile(dcmPath(w.dir, w.id), tmp, err) ||
        !commitFile(tmp, dcmPath(nextDir, w.id), err)) {
      logmsg("cannot demote " + w.id + ": " + err);
      unlink(tmp.c_str());
      return;
    }
  }
  if (!discardMessage(sp, w.dir, w.id, w.isBatch, err))
    logmsg("demoted " + w.id + " but cannot remove source: " + err);
}

// Gather every due message across todo/ (always due) and each retry/<k> rung,
// reading the routing fields each object needs from its file-meta header.
// An unreadable message is quarantined here; an empty sealed batch is dropped
// quietly. Fills sopClasses with every SOP class seen (the context budget).
static std::vector<WorkItem> gatherDueWork(std::set<std::string> &sopClasses) {
  const time_t now = time(nullptr);
  std::vector<WorkItem> work;
  std::string err;

  // read the routing fields one object needs from its file-meta header
  auto readObjectMeta = [](const std::string &path, Object &obj) -> bool {
    FileMeta m;
    if (!readFileMeta(path, m) || !m.hasRouting())
      return false;
    obj = {path, m.sopClass, m.sopInstance, m.transferSyntax};
    return true;
  };

  for (const auto &d : routeQueueDirs(sp, destName)) {
    const std::string &dir = d.first;
    const int level = d.second;
    for (const auto &msg : listMessages(dir)) {
      if (!messageDue(dir, msg.id, level, now, msg.isBatch))
        continue; // attempted and still backing off
      WorkItem item;
      item.dir = dir;
      item.level = level;
      item.id = msg.id;
      item.isBatch = msg.isBatch;
      bool ok = true;
      if (msg.isBatch) {
        // a batch is a directory of <objid>.dcm; read every member
        const std::string bdir = messagePath(dir, msg.id, true);
        for (const auto &objid : listIds(bdir)) {
          Object obj;
          if (!readObjectMeta(dcmPath(bdir, objid), obj)) {
            ok = false;
            break;
          }
          item.objects.push_back(obj);
        }
      } else {
        Object obj;
        if (readObjectMeta(dcmPath(dir, msg.id), obj))
          item.objects.push_back(obj);
        else
          ok = false;
      }
      if (ok && item.objects.empty()) {
        // a sealed batch should never be empty; if it is, drop it quietly
        removeMessage(dir, msg.id, err, msg.isBatch);
        continue;
      }
      if (!ok) {
        logmsg("quarantining " + msg.id + ": cannot read file meta");
        if (!sinkMessage(sp.routeCorrupt(destName), dir, msg.id, msg.isBatch,
                         err))
          logmsg("cannot quarantine " + msg.id + ": " + err);
        continue;
      }
      for (const auto &o : item.objects)
        sopClasses.insert(o.sopClass);
      work.push_back(std::move(item));
    }
  }
  return work;
}

// The transfer syntaxes to propose for a set of objects, in preference order:
// the profile's list when it has one (fixed regardless of payload), else each
// object's own stored syntax followed by the two uncompressed defaults.
// Deduplicated. openAssociation feeds it every object in the run to build the
// association's contexts; deliverMessage feeds it one message's objects to
// size that message's OWN context need for the defer-vs-fail decision. Keeping
// both on one rule means the two budget computations cannot silently diverge.
static std::vector<std::string>
syntaxesToPropose(const std::vector<Object> &objects,
                  const ProposeProfile &profile) {
  if (!profile.transferSyntaxes.empty())
    return profile.transferSyntaxes;
  std::vector<std::string> ts;
  auto add = [&ts](const std::string &uid) {
    if (uid.empty())
      return;
    for (const auto &p : ts)
      if (p == uid)
        return;
    ts.push_back(uid);
  };
  for (const auto &o : objects)
    add(o.xferUID);
  add(UID_LittleEndianExplicitTransferSyntax);
  add(UID_LittleEndianImplicitTransferSyntax);
  return ts;
}

// Build presentation contexts and request the association. The syntaxes come
// from syntaxesToPropose() over every object in the run. One context per (SOP
// class, transfer syntax) gives precise control over what an accepted context
// implies. On failure this records the connection failure and returns nullptr;
// on success it returns the open association and fills proposedSops with the
// SOP classes that actually got a context (the rest are deferred, not failed).
static T_ASC_Association *
openAssociation(T_ASC_Network *net, const RemoteConfig &cfg,
                const std::string &callingAET, const ProposeProfile &profile,
                const std::vector<WorkItem> &work,
                const std::set<std::string> &sopClasses, bool useTLS,
                std::set<std::string> &proposedSops) {
  std::vector<Object> allObjects;
  for (const auto &w : work)
    allObjects.insert(allObjects.end(), w.objects.begin(), w.objects.end());
  const std::vector<std::string> proposeTS =
      syntaxesToPropose(allObjects, profile);

  T_ASC_Parameters *params = nullptr;
  ASC_createAssociationParameters(&params, ASC_DEFAULTMAXPDU,
                                  30 /* TCP connect timeout, seconds */);
  ASC_setAPTitles(params, callingAET.c_str(), cfg.aet.c_str(), nullptr);
  char localHost[256] = {0}; // POSIX leaves the result unterminated on
  gethostname(localHost, sizeof(localHost) - 1); // truncation; pre-NUL it
  const std::string peer = cfg.host + ":" + std::to_string(cfg.port);
  ASC_setPresentationAddresses(params, localHost, peer.c_str());
  if (useTLS)
    ASC_setTransportLayerType(params, OFTrue);

  // Spend the context budget (see kMaxContexts): ids are odd and a Uint8, so
  // 253 is the last usable one. Remember which classes we proposed so the
  // delivery loop can tell "we never proposed this class" (defer) from "the
  // destination refused a class we did propose" (a real rejection).
  T_ASC_PresentationContextID pid = 1;
  for (const auto &sop : sopClasses) {
    if (pid > kMaxPresentationContextID)
      break; // context budget reached; remaining classes deferred
    for (const auto &ts : proposeTS) {
      if (pid > kMaxPresentationContextID)
        break;
      const char *tsArr[] = {ts.c_str()};
      ASC_addPresentationContext(params, pid, sop.c_str(), tsArr, 1);
      proposedSops.insert(sop);
      pid += 2;
    }
  }

  T_ASC_Association *assoc = nullptr;
  const OFCondition cond = ASC_requestAssociation(net, params, &assoc);
  if (cond.bad() || ASC_countAcceptedPresentationContexts(params) == 0) {
    recordConnectionFailure(cond.bad() ? cond.text()
                                       : "no presentation context accepted");
    if (assoc) {
      ASC_abortAssociation(assoc);
      ASC_destroyAssociation(&assoc);
    }
    return nullptr;
  }
  return assoc;
}

// Try to C-STORE every object of one message over the open association.
// Performs NO filesystem moves and records NO status — it only reports what
// happened, and the caller acts on the Outcome. A message is all-or-nothing:
// the first object that cannot be sent decides the whole message's fate (the
// destination dedups any objects it already received on SOP Instance UID).
static DeliveryResult
deliverMessage(T_ASC_Association *assoc, const WorkItem &item,
               const ProposeProfile &profile,
               const std::set<std::string> &proposedSops) {
  // Contexts this message needs ON ITS OWN (so the defer-vs-fail decision does
  // not depend on what else shares this run): one per (distinct SOP class x
  // proposed syntax), using the same syntaxesToPropose() rule openAssociation
  // does so the two budget computations stay in lockstep.
  std::set<std::string> itemSops;
  for (const auto &o : item.objects)
    itemSops.insert(o.sopClass);
  const size_t itemTs = syntaxesToPropose(item.objects, profile).size();
  const bool itemCannotFit = itemSops.size() * itemTs > kMaxContexts;

  for (const auto &obj : item.objects) {
    DcmFileFormat ff;
    OFCondition cond = ff.loadFile(obj.path.c_str());
    if (cond.bad())
      return {Outcome::Quarantine, cond.text()};
    DcmDataset *dataset = ff.getDataset();
    const DcmXfer objXfer(dataset->getOriginalXfer());

    // Note: the 3-arg lookup FALLS BACK to any context for the abstract
    // syntax when no exact transfer syntax match exists, so the accepted
    // TS must be compared explicitly — DIMSE_storeUser would otherwise
    // convert silently, bypassing the transcode policy.
    T_ASC_PresentationContextID presID = ASC_findAcceptedPresentationContextID(
        assoc, obj.sopClass.c_str(), objXfer.getXferID());
    if (presID == 0)
      presID =
          ASC_findAcceptedPresentationContextID(assoc, obj.sopClass.c_str());
    if (presID == 0) {
      // No usable context. Distinguish our own context-budget limit (we never
      // proposed this class — defer so a later batch carries it; a deferral
      // must not consume a retry rung) from a destination that refused a class
      // we did propose (a real, permanent rejection).
      if (proposedSops.count(obj.sopClass) == 0) {
        if (itemCannotFit)
          // Even alone this message needs more contexts than fit; no later
          // batch is smaller than one message, so deferring would livelock.
          // This is structural, not transient — fail it outright rather than
          // climb the retry ladder, which exists for transient problems.
          return {Outcome::FailPermanent,
                  "needs " + std::to_string(itemSops.size() * itemTs) +
                      " presentation contexts (" +
                      std::to_string(itemSops.size()) + " SOP classes x " +
                      std::to_string(itemTs) + " syntaxes), over the " +
                      std::to_string(kMaxContexts) +
                      "-context association limit"};
        return {Outcome::Defer, "presentation-context budget exhausted, " +
                                    obj.sopClass + " not proposed this batch"};
      }
      return {Outcome::Demote,
              "no presentation context accepted for " + obj.sopClass};
    }
    T_ASC_PresentationContext pc;
    ASC_findAcceptedPresentationContext(assoc->params, presID, &pc);
    if (strcmp(pc.acceptedTransferSyntax, objXfer.getXferID()) != 0) {
      // stored syntax not accepted: transcode or fail, per policy
      if (profile.transcode == ProposeProfile::Transcode::Never)
        return {Outcome::Demote, std::string("stored syntax ") +
                                     objXfer.getXferID() +
                                     " not accepted and transcode is 'never'"};
      if (profile.transcode == ProposeProfile::Transcode::Lossless &&
          isLossyTransferSyntaxUID(pc.acceptedTransferSyntax))
        return {Outcome::Demote, std::string("accepted syntax ") +
                                     pc.acceptedTransferSyntax +
                                     " is lossy and transcode is 'lossless'"};
      const DcmXfer target(pc.acceptedTransferSyntax);
      const OFCondition xc =
          dataset->chooseRepresentation(target.getXfer(), nullptr);
      if (xc.bad() || !dataset->canWriteXfer(target.getXfer()))
        return {Outcome::Demote,
                std::string("cannot transcode ") + objXfer.getXferID() +
                    " -> " + target.getXferID() +
                    (xc.bad() ? std::string(": ") + xc.text() : "")};
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
      // mid-association breakage is destination state; messages stay where
      // they are so nothing is double-penalized
      return {Outcome::ConnectionBroke, cond.text()};
    if (rsp.DimseStatus != STATUS_Success &&
        (rsp.DimseStatus & 0xf000) != 0xB000) // warnings are delivered
    {
      char status[16];
      snprintf(status, sizeof(status), "0x%04x", rsp.DimseStatus);
      return {Outcome::Demote, std::string("rejected ") + status};
    }
  }
  return {Outcome::Delivered, ""};
}

int main(int argc, char **argv) {
  // ignore SIGPIPE: a peer reset mid-C-STORE returns EPIPE on write rather
  // than killing us by signal, so the failure is handled as destination state
  signal(SIGPIPE, SIG_IGN);

  std::string spoolArg;
  int opt;
  while ((opt = getopt(argc, argv, "s:")) != -1) {
    switch (opt) {
    case 's':
      spoolArg = optarg;
      break;
    default:
      std::fprintf(stderr, "usage: dicomq-remote [-s <spool>] <DEST>\n");
      return 100;
    }
  }
  if (optind + 1 != argc) {
    std::fprintf(stderr, "usage: dicomq-remote [-s <spool>] <DEST>\n");
    return 100;
  }
  destName = argv[optind];
  sp = Spool(spoolArg);

  std::string err;
  RemoteConfig cfg;
  if (!RemoteConfig::load(sp.destDir(destName) + "/remote", cfg, err)) {
    logmsg(err);
    return 100;
  }
  ProposeProfile profile;
  if (!ProposeProfile::load(sp.destDir(destName) + "/propose", profile, err)) {
    logmsg(err);
    return 100;
  }
  const std::string callingAET =
      cfg.callingAET.empty() ? "DICOMQ" : cfg.callingAET;

  // gather due work across todo/ and every retry/<k> rung, reading each
  // message's routing fields from its file-meta header
  std::set<std::string> sopClasses;
  std::vector<WorkItem> work = gatherDueWork(sopClasses);
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
  if (cond.bad()) {
    logmsg(std::string("cannot initialize network: ") + cond.text());
    return 111;
  }

  // dest/<DEST>/tls/ exists => DICOM TLS to this destination. A requestor's own
  // key.pem + cert.pem are optional (load only if present); ca.pem verifies the
  // server.
  const std::string tlsDir = sp.destDir(destName) + "/tls";
  const bool useTLS = isDir(tlsDir);
  if (useTLS) {
    std::string tlsErr;
    DcmTransportLayer *layer =
        makeTLSLayer(NET_REQUESTOR, tlsDir, /*requireOwnCert=*/false, tlsErr);
    if (!layer || ASC_setTransportLayer(net, layer, 1).bad()) {
      logmsg("TLS setup failed for '" + tlsDir +
             "': " + (tlsErr.empty() ? "transport layer" : tlsErr));
      return 100; // config problem, not destination weather
    }
  }

  std::set<std::string> proposedSops;
  T_ASC_Association *assoc = openAssociation(
      net, cfg, callingAET, profile, work, sopClasses, useTLS, proposedSops);
  if (!assoc) {
    ASC_dropNetwork(&net);
    return 0; // connection failure already recorded as destination state
  }

  bool connectionBroke = false;
  for (const auto &item : work) {
    const DeliveryResult r = deliverMessage(assoc, item, profile, proposedSops);
    switch (r.outcome) {
    case Outcome::ConnectionBroke:
      recordConnectionFailure(r.reason);
      connectionBroke = true;
      break;
    case Outcome::Quarantine:
      logmsg("quarantining " + item.id + ": " + r.reason);
      if (!sinkMessage(sp.routeCorrupt(destName), item.dir, item.id,
                       item.isBatch, err))
        logmsg("cannot quarantine " + item.id + ": " + err);
      break;
    case Outcome::Defer:
      logmsg("deferred " + item.id + ": " + r.reason);
      break; // left in place for a later, smaller batch
    case Outcome::FailPermanent:
      // no retry will ever help, so move straight to failed/ instead of
      // consuming the (transient-problem) retry ladder
      logmsg("failing " + item.id + ": " + r.reason);
      if (!sinkMessage(sp.routeFailed(destName), item.dir, item.id,
                       item.isBatch, err))
        logmsg("cannot fail " + item.id + ": " + err);
      break;
    case Outcome::Demote:
      demote(item, r.reason);
      break;
    case Outcome::Delivered:
      if (!sinkMessage(sp.routeComplete(destName), item.dir, item.id,
                       item.isBatch, err))
        logmsg("delivered " + item.id +
               " but cannot move to complete/: " + err);
      else
        logmsg("delivered " + item.id +
               (item.isBatch
                    ? " (" + std::to_string(item.objects.size()) + " objects)"
                    : ""));
      break;
    }
    if (connectionBroke)
      break;
  }

  if (!connectionBroke) {
    ASC_releaseAssociation(assoc);
    unlink(sp.routeStatus(destName).c_str()); // the site is alive
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
