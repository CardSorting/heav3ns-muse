/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_PULSE_AUDIO_API_H
#define LMMS_PULSE_AUDIO_API_H

#include <pulse/pulseaudio.h>

namespace lmms::detail
{
// Native entry points. The private constructor permits deterministic transport
// failures in tests while retaining the production worker, lifecycle and DSP.
struct PulseAudioApi
{
	decltype(&pa_mainloop_new) mainloopNew = &pa_mainloop_new;
	decltype(&pa_mainloop_free) mainloopFree = &pa_mainloop_free;
	decltype(&pa_mainloop_get_api) mainloopGetApi = &pa_mainloop_get_api;
	decltype(&pa_mainloop_prepare) mainloopPrepare = &pa_mainloop_prepare;
	decltype(&pa_mainloop_poll) mainloopPoll = &pa_mainloop_poll;
	decltype(&pa_mainloop_dispatch) mainloopDispatch = &pa_mainloop_dispatch;
	decltype(&pa_mainloop_wakeup) mainloopWakeup = &pa_mainloop_wakeup;
	decltype(&pa_context_new) contextNew = &pa_context_new;
	decltype(&pa_context_unref) contextUnref = &pa_context_unref;
	decltype(&pa_context_connect) contextConnect = &pa_context_connect;
	decltype(&pa_context_disconnect) contextDisconnect = &pa_context_disconnect;
	decltype(&pa_context_errno) contextError = &pa_context_errno;
	decltype(&pa_context_get_state) contextState = &pa_context_get_state;
	decltype(&pa_context_set_state_callback) contextSetCallback = &pa_context_set_state_callback;
	decltype(&pa_stream_new) streamNew = &pa_stream_new;
	decltype(&pa_stream_unref) streamUnref = &pa_stream_unref;
	decltype(&pa_stream_connect_playback) streamConnect = &pa_stream_connect_playback;
	decltype(&pa_stream_disconnect) streamDisconnect = &pa_stream_disconnect;
	decltype(&pa_stream_get_context) streamContext = &pa_stream_get_context;
	decltype(&pa_stream_get_state) streamState = &pa_stream_get_state;
	decltype(&pa_stream_set_state_callback) streamSetStateCallback = &pa_stream_set_state_callback;
	decltype(&pa_stream_set_write_callback) streamSetWriteCallback = &pa_stream_set_write_callback;
	decltype(&pa_stream_begin_write) streamBeginWrite = &pa_stream_begin_write;
	decltype(&pa_stream_cancel_write) streamCancelWrite = &pa_stream_cancel_write;
	decltype(&pa_stream_write) streamWrite = &pa_stream_write;
};
}
#endif
