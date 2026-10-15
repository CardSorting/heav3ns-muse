/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_SNDIO_API_H
#define LMMS_SNDIO_API_H
#include "lmmsconfig.h"
#include <poll.h>
#include <sndio.h>

namespace lmms::detail
{
// Private transport seam for native lifecycle/DSP fault fixtures.
struct SndioApi
{
	decltype(&sio_open) open = &sio_open;
	decltype(&sio_close) close = &sio_close;
	decltype(&sio_initpar) initParameters = &sio_initpar;
	decltype(&sio_setpar) setParameters = &sio_setpar;
	decltype(&sio_getpar) getParameters = &sio_getpar;
	decltype(&sio_start) start = &sio_start;
#ifdef LMMS_HAVE_SIO_FLUSH
	decltype(&sio_flush) stop = &sio_flush;
#else
	// Older libraries retain their draining stop semantics.
	decltype(&sio_stop) stop = &sio_stop;
#endif
	decltype(&sio_write) write = &sio_write;
	decltype(&sio_nfds) descriptorCount = &sio_nfds;
	decltype(&sio_pollfd) descriptors = &sio_pollfd;
	decltype(&sio_revents) events = &sio_revents;
	decltype(&sio_eof) eof = &sio_eof;
	decltype(&::poll) poll = &::poll;
};
}
#endif
