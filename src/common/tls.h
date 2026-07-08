// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DICOMQ_TLS_H
#define DICOMQ_TLS_H

#include <string>

// DICOM TLS (BCP 195) setup, shared by the two programs that speak TLS —
// dicomq-recv (inbound) and dicomq-remote (outbound) — so the two setups
// cannot drift apart. Kept in its own library (dicomq-tls) so only those two
// programs link DCMTK::dcmtls. See dicomq/DESIGN.md "Process and privilege
// model".

class DcmTransportLayer;

namespace dicomq {

// Build a BCP 195 DICOM TLS transport layer from the PEM files in `dir`:
//
//   ca.pem             — when present, peer certificates are REQUIRED and
//                        verified against it; when absent, the peer
//                        certificate is not checked.
//   key.pem + cert.pem — our own identity. Loaded unconditionally when
//                        requireOwnCert is true (an acceptor always presents a
//                        certificate); otherwise loaded only when present (a
//                        requestor authenticates itself only if it was
//                        configured to). Exactly one of the pair present is
//                        an error, never a silent anonymous connection.
//
// role is NET_ACCEPTOR (recv) or NET_REQUESTOR (remote), from dcmnet. On
// success returns the layer for the caller to hand to ASC_setTransportLayer
// (which takes ownership); on failure returns nullptr and sets err.
DcmTransportLayer *makeTLSLayer(int role, const std::string &dir,
                                bool requireOwnCert, std::string &err);

} // namespace dicomq

#endif // DICOMQ_TLS_H
