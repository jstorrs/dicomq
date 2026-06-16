// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DICOMQ_DCM_H
#define DICOMQ_DCM_H

#include <string>

// DICOM helpers shared by the DICOM-aware programs (dicomq-remote,
// -inject, -ctl, -queue). Kept in a separate library from dicomq-common
// so the DICOM-free programs (send, local, clean) need not link DCMTK —
// see the library split in CMakeLists.txt.

class DcmMetaInfo;
class DcmFileFormat;

namespace dicomq {

// The file-meta header fields dicomq reads back from a queued Part-10
// object — every routing decision a routed message needs lives here, not
// in a sidecar (DESIGN.md "Where message metadata lives"). An absent tag
// leaves its field empty.
struct FileMeta {
  std::string sopClass;       // MediaStorageSOPClassUID         (0002,0002)
  std::string sopInstance;    // MediaStorageSOPInstanceUID      (0002,0003)
  std::string transferSyntax; // TransferSyntaxUID               (0002,0010)
  std::string sourceAET;      // SourceApplicationEntityTitle    (0002,0016)
  std::string receivingAET;   // ReceivingApplicationEntityTitle (0002,0018)

  // The three UIDs every routable object must carry to be deliverable.
  bool hasRouting() const {
    return !sopClass.empty() && !sopInstance.empty() && !transferSyntax.empty();
  }
};

// Copy the file-meta fields from an already-loaded header into out (for a
// caller that holds the DcmFileFormat anyway, e.g. dicomq-inject before it
// re-saves). A null info leaves every field empty.
void extractFileMeta(DcmMetaInfo *info, FileMeta &out);

// Read the file-meta header of the Part-10 file at path WITHOUT its pixel
// data (ERM_metaOnly — cheap, no transfer-syntax decode). Returns false if
// the file cannot be opened or parsed as Part 10; an absent individual tag
// is not an error (its field stays empty).
bool readFileMeta(const std::string &path, FileMeta &out);

// Stamp the origin AET tags dicomq-recv and dicomq-inject write so a queued
// object is self-describing for routing (DESIGN.md "Where message metadata
// lives"): Source (0002,0016) and Sending (0002,0017) AET are the peer that
// sent it, Receiving (0002,0018) is the called AET it arrived for.
void stampOriginAETs(DcmMetaInfo *meta, const std::string &sendingAET,
                     const std::string &receivingAET);

// Write ff to a queue/tmp/ path as dicomq stores a received object: Part 10
// in its original transfer syntax, explicit length, recalculated group
// lengths, no padding, preserving any load-bearing preamble (dual
// TIFF/DICOM). This is the one definition of how a queued object is written;
// dicomq-recv and dicomq-inject share it. NOT durable on its own — publish
// with commitFile(). Returns false and sets err on a save failure.
bool saveAsReceived(DcmFileFormat &ff, const std::string &tmpPath,
                    std::string &err);

} // namespace dicomq

#endif // DICOMQ_DCM_H
