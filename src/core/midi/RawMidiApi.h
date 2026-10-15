/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_RAW_MIDI_API_H
#define LMMS_RAW_MIDI_API_H
#include "lmmsconfig.h"
#include <poll.h>
#ifdef LMMS_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif
#ifdef LMMS_HAVE_SNDIO
#include <sndio.h>
#endif

namespace lmms::detail
{
// Private native transport seams for lifecycle/error fixtures. Production
// defaults call the platform libraries directly; neither owns project models.
#ifdef LMMS_HAVE_ALSA
struct AlsaRawMidiApi
{
	decltype(&snd_rawmidi_open) open = &snd_rawmidi_open;
	decltype(&snd_rawmidi_close) close = &snd_rawmidi_close;
	decltype(&snd_rawmidi_poll_descriptors_count) descriptorCount = &snd_rawmidi_poll_descriptors_count;
	decltype(&snd_rawmidi_poll_descriptors) descriptors = &snd_rawmidi_poll_descriptors;
	decltype(&snd_rawmidi_poll_descriptors_revents) events = &snd_rawmidi_poll_descriptors_revents;
	decltype(&snd_rawmidi_read) read = &snd_rawmidi_read;
	decltype(&snd_rawmidi_status) status = &snd_rawmidi_status;
	decltype(&snd_rawmidi_status_get_xruns) statusXruns = &snd_rawmidi_status_get_xruns;
	decltype(&snd_rawmidi_write) write = &snd_rawmidi_write;
	decltype(&::poll) poll = &::poll;
};
#endif
#ifdef LMMS_HAVE_SNDIO
struct SndioMidiApi
{
	decltype(&mio_open) open = &mio_open;
	decltype(&mio_close) close = &mio_close;
	decltype(&mio_nfds) descriptorCount = &mio_nfds;
	decltype(&mio_pollfd) descriptors = &mio_pollfd;
	decltype(&mio_revents) events = &mio_revents;
	decltype(&mio_read) read = &mio_read;
	decltype(&mio_write) write = &mio_write;
	decltype(&mio_eof) eof = &mio_eof;
	decltype(&::poll) poll = &::poll;
};
#endif
}
#endif
