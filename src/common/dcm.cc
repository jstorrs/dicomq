// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/dcm.h"

#include "dcmtk/config/osconfig.h"

#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/dcmdata/dcmetinf.h"

namespace dicomq {

static std::string metaString(DcmMetaInfo *info, const DcmTagKey &key) {
  OFString s;
  if (info && info->findAndGetOFString(key, s).good())
    return s.c_str();
  return "";
}

void extractFileMeta(DcmMetaInfo *info, FileMeta &out) {
  out.sopClass = metaString(info, DCM_MediaStorageSOPClassUID);
  out.sopInstance = metaString(info, DCM_MediaStorageSOPInstanceUID);
  out.transferSyntax = metaString(info, DCM_TransferSyntaxUID);
  out.sourceAET = metaString(info, DCM_SourceApplicationEntityTitle);
  out.receivingAET = metaString(info, DCM_ReceivingApplicationEntityTitle);
}

bool readFileMeta(const std::string &path, FileMeta &out) {
  DcmFileFormat ff;
  if (ff.loadFile(path.c_str(), EXS_Unknown, EGL_noChange, DCM_MaxReadLength,
                  ERM_metaOnly)
          .bad())
    return false;
  extractFileMeta(ff.getMetaInfo(), out);
  return true;
}

} // namespace dicomq
