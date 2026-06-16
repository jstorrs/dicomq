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
  // optional (load only if present) for a requestor.
  const std::string key = dir + "/key.pem", cert = dir + "/cert.pem";
  if (requireOwnCert || pathExists(key)) {
    if (layer->setPrivateKeyFile(key.c_str(), DCF_Filetype_PEM).bad() ||
        layer
            ->setCertificateFile(cert.c_str(), DCF_Filetype_PEM,
                                 TSP_Profile_BCP195)
            .bad() ||
        !layer->checkPrivateKeyMatchesCertificate()) {
      err = "cannot load '" + key + "' / '" + cert + "'";
      delete layer;
      return nullptr;
    }
  }
  return layer;
}

} // namespace dicomq
