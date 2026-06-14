// SPDX-FileCopyrightText: 2026 Judd Storrs <jstorrs@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DICOMQ_MESSAGE_H
#define DICOMQ_MESSAGE_H

#include <ctime>
#include <string>

#include "common/spool.h"

// Operations on a message — a single <id>.dcm file. The retry state a
// sidecar envelope used to carry now lives in the directory the file
// sits in (route/<DEST>/todo vs retry/<k>), so moving a message between
// queues is one atomic rename: the rename IS the commit (DESIGN.md
// "Commit protocols"). Nothing here knows about DICOM.

namespace dicomq {

std::string dcmPath(const std::string& dir, const std::string& id);

// Move <id>.dcm from fromDir to toDir with a single rename(2). The
// rename is atomic, so the message exists in exactly one directory at
// every instant. Idempotent: if the source is already gone but the
// target exists, a prior (possibly crashed) pass did this — success.
bool moveMessage(const std::string& fromDir, const std::string& toDir,
                 const std::string& id, std::string& err);

// Hardlink <id>.dcm from fromDir into toDir (fan-out); EEXIST means a
// prior pass already linked it. The source stays in place.
bool linkMessage(const std::string& fromDir, const std::string& toDir,
                 const std::string& id, std::string& err);

// Remove <id>.dcm from dir; a missing file is tolerated (idempotent).
bool removeMessage(const std::string& dir, const std::string& id,
                   std::string& err);

// Whether message <id> in dir, sitting at retry rung retryLevel, is due
// for a delivery attempt at time now. Rung 0 (todo/) is always due; rung
// k>=1 is due when now - mtime(<id>.dcm) >= retryBackoff(k). A vanished
// file reads as not-due.
bool messageDue(const std::string& dir, const std::string& id,
                int retryLevel, time_t now);

} // namespace dicomq

#endif // DICOMQ_MESSAGE_H
