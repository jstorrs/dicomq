// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/tls.h"

#include "dcmtk/config/osconfig.h"

#include "dcmtk/dcmtls/tlslayer.h"

#include "common/spool.h" // pathExists

namespace dicomq {

DcmTransportLayer *makeTLSLayer(int role, const std::string &dir,
                                bool requireOwnCert, std::string &err) {
  DcmTLSTransportLayer::initializeOpenSSL();
  DcmTLSTransportLayer *layer = new DcmTLSTransportLayer(
      static_cast<T_ASC_NetworkRole>(role), nullptr, OFFalse);
  if (layer->setTLSProfile(TSP_Profile_BCP195).bad() ||
      layer->activateCipherSuites().bad()) {
    err = "cannot activate the BCP 195 TLS profile";
    delete layer;
    return nullptr;
  }

  // ca.pem present => require and verify the peer certificate against it;
  // absent => do not check the peer certificate (anonymous peer allowed).
  const std::string ca = dir + "/ca.pem";
  if (pathExists(ca)) {
    if (layer->addTrustedCertificateFile(ca.c_str(), DCF_Filetype_PEM).bad()) {
      err = "cannot load '" + ca + "'";
      delete layer;
      return nullptr;
    }
    layer->setCertificateVerification(DCV_requireCertificate);
  } else
    layer->setCertificateVerification(DCV_ignoreCertificate);

  // key.pem + cert.pem are our own identity: mandatory for an acceptor,
  // optional (loaded when present) for a requestor. Exactly one of the
  // pair present is a configuration error, not "connect anonymously" —
  // silently skipping a half-configured identity would make connections
  // the operator believes are authenticated go out without a certificate,
  // diagnosable only from the far end.
  const std::string key = dir + "/key.pem", cert = dir + "/cert.pem";
  const bool haveKey = pathExists(key), haveCert = pathExists(cert);
  std::string problem;
  if (haveKey != haveCert)
    problem = "'" + (haveKey ? cert : key) +
              "' is missing (key.pem and cert.pem configure an identity "
              "together)";
  else if (requireOwnCert || haveKey) {
    if (layer->setPrivateKeyFile(key.c_str(), DCF_Filetype_PEM).bad())
      problem = "cannot load private key '" + key + "'";
    else if (layer
                 ->setCertificateFile(cert.c_str(), DCF_Filetype_PEM,
                                      TSP_Profile_BCP195)
                 .bad())
      problem = "cannot load certificate '" + cert + "'";
    else if (!layer->checkPrivateKeyMatchesCertificate())
      problem = "private key '" + key + "' does not match '" + cert + "'";
  }
  if (!problem.empty()) {
    err = problem;
    delete layer;
    return nullptr;
  }
  return layer;
}

} // namespace dicomq
