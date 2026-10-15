/* Internal native decoder entry points for backend compatibility verification.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_SAMPLE_DECODER_BACKENDS_H
#define LMMS_SAMPLE_DECODER_BACKENDS_H

#include "SampleDecoder.h"
#include "lmms_export.h"
#include "lmmsconfig.h"

namespace lmms::detail
{
#ifdef LMMS_HAVE_OGGVORBIS
LMMS_EXPORT std::optional<SampleDecoder::Result> decodeSampleOggVorbis(const QString& audioFile);
LMMS_EXPORT std::optional<SampleDecoder::Result> decodeSampleOggVorbis(const QString& audioFile, bool* recognized);
#endif
}

#endif
