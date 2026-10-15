/* Native PortAudio PCM callback path. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_PORTAUDIO_CALLBACK_H
#define LMMS_PORTAUDIO_CALLBACK_H

#include "lmmsconfig.h"
#ifdef LMMS_HAVE_PORTAUDIO
#include <portaudio.h>
#include "LmmsTypes.h"
#include "lmms_export.h"

namespace lmms
{
class AudioEngine;
namespace detail
{
// This is the production native callback path, also exercised by native tests.
// Interleaved float input maps mono to stereo or retains its first two channels.
LMMS_EXPORT int processPortAudioBuffer(AudioEngine& engine, ch_cnt_t inputChannels, ch_cnt_t outputChannels,
	const void* input, void* output, unsigned long frames, bool inputLost);
}
}
#endif
#endif
