/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_SF2_BANK_SNAPSHOT_H
#define LMMS_SF2_BANK_SNAPSHOT_H

#include "FileResourceSnapshot.h"

namespace lmms
{

// SoundFont lifetime management retains these through native lazy unloading.
using Sf2BankSnapshot = FileResourceSnapshot;

} // namespace lmms

#endif // LMMS_SF2_BANK_SNAPSHOT_H
