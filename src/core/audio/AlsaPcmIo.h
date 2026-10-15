/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_ALSA_PCM_IO_H
#define LMMS_ALSA_PCM_IO_H
#include <alsa/asoundlib.h>

namespace lmms::detail
{
// Fault injection is restricted to PCM transfer/lifecycle entry points. Tests
// still open and configure an actual ALSA PCM through AudioAlsa's constructor.
struct AlsaPcmIo
{
	decltype(&snd_pcm_prepare) prepare = &snd_pcm_prepare;
	decltype(&snd_pcm_writei) write = &snd_pcm_writei;
	decltype(&snd_pcm_wait) wait = &snd_pcm_wait;
	decltype(&snd_pcm_resume) resume = &snd_pcm_resume;
	decltype(&snd_pcm_drop) drop = &snd_pcm_drop;
};
}
#endif
