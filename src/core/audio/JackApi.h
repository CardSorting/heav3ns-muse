/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_JACK_API_H
#define LMMS_JACK_API_H
#include "lmmsconfig.h"
#include <cstdint>
#include <cstddef>
#ifndef LMMS_HAVE_WEAKJACK
#include <jack/jack.h>
#include <jack/midiport.h>
#else
#include <weak_libjack.h>
#endif

namespace lmms::detail
{
// Wrappers resolve weakjack at call time. The fixture replaces transport
// entry points while retaining AudioJack's actual lifecycle and native DSP.
struct JackApi
{
	jack_client_t* (*open)(const char*, jack_options_t, jack_status_t*) = [](const char* n, jack_options_t o, jack_status_t* s) { return jack_client_open(n, o, s); };
	int (*close)(jack_client_t*) = [](jack_client_t* c) { return jack_client_close(c); };
	char* (*clientName)(jack_client_t*) = [](jack_client_t* c) { return jack_get_client_name(c); };
	jack_nframes_t (*bufferSize)(jack_client_t*) = [](jack_client_t* c) { return jack_get_buffer_size(c); };
	jack_nframes_t (*sampleRate)(jack_client_t*) = [](jack_client_t* c) { return jack_get_sample_rate(c); };
	int (*setBufferSizeCallback)(jack_client_t*, JackBufferSizeCallback, void*) = [](jack_client_t* c, JackBufferSizeCallback f, void* a) { return jack_set_buffer_size_callback(c, f, a); };
	int (*setProcessCallback)(jack_client_t*, JackProcessCallback, void*) = [](jack_client_t* c, JackProcessCallback f, void* a) { return jack_set_process_callback(c, f, a); };
	void (*onShutdown)(jack_client_t*, JackShutdownCallback, void*) = [](jack_client_t* c, JackShutdownCallback f, void* a) { jack_on_shutdown(c, f, a); };
	jack_port_t* (*registerPort)(jack_client_t*, const char*, const char*, unsigned long, unsigned long) = [](jack_client_t* c, const char* n, const char* t, unsigned long f, unsigned long b) { return jack_port_register(c, n, t, f, b); };
	int (*unregisterPort)(jack_client_t*, jack_port_t*) = [](jack_client_t* c, jack_port_t* p) { return jack_port_unregister(c, p); };
	void* (*portBuffer)(jack_port_t*, jack_nframes_t) = [](jack_port_t* p, jack_nframes_t n) { return jack_port_get_buffer(p, n); };
	const char* (*portName)(const jack_port_t*) = [](const jack_port_t* p) { return jack_port_name(p); };
	int (*connect)(jack_client_t*, const char*, const char*) = [](jack_client_t* c, const char* a, const char* b) { return jack_connect(c, a, b); };
	int (*disconnect)(jack_client_t*, const char*, const char*) = [](jack_client_t* c, const char* a, const char* b) { return jack_disconnect(c, a, b); };
	const char** (*ports)(jack_client_t*, const char*, const char*, unsigned long) = [](jack_client_t* c, const char* n, const char* t, unsigned long f) { return jack_get_ports(c, n, t, f); };
	void (*freeMemory)(void*) = [](void* p) { jack_free(p); };
	jack_port_t* (*portByName)(jack_client_t*, const char*) = [](jack_client_t* c, const char* n) { return jack_port_by_name(c, n); };
	int (*portFlags)(const jack_port_t*) = [](const jack_port_t* p) { return jack_port_flags(p); };
	const char* (*portType)(const jack_port_t*) = [](const jack_port_t* p) { return jack_port_type(p); };
	int (*portIsMine)(const jack_client_t*, const jack_port_t*) = [](const jack_client_t* c, const jack_port_t* p) { return jack_port_is_mine(c, p); };
	int (*portConnectedTo)(const jack_port_t*, const char*) = [](const jack_port_t* p, const char* n) { return jack_port_connected_to(p, n); };
	int (*portConnected)(const jack_port_t*) = [](const jack_port_t* p) { return jack_port_connected(p); };
	int (*disconnectPort)(jack_client_t*, jack_port_t*) = [](jack_client_t* c, jack_port_t* p) { return jack_port_disconnect(c, p); };
	int (*activate)(jack_client_t*) = [](jack_client_t* c) { return jack_activate(c); };
	int (*deactivate)(jack_client_t*) = [](jack_client_t* c) { return jack_deactivate(c); };
	jack_nframes_t (*midiEventCount)(void*) = [](void* b) { return jack_midi_get_event_count(b); };
	int (*midiEvent)(jack_midi_event_t*, void*, std::uint32_t) = [](jack_midi_event_t* e, void* b, std::uint32_t i) { return jack_midi_event_get(e, b, i); };
	std::uint32_t (*lostMidiEvents)(void*) = [](void* b) { return jack_midi_get_lost_event_count(b); };
	void (*clearMidiBuffer)(void*) = [](void* b) { jack_midi_clear_buffer(b); };
	int (*writeMidiEvent)(void*, jack_nframes_t, const jack_midi_data_t*, std::size_t) = [](void* b, jack_nframes_t t, const jack_midi_data_t* d, std::size_t s) { return jack_midi_event_write(b, t, d, s); };
	jack_nframes_t (*frameTime)(const jack_client_t*) = [](const jack_client_t* c) { return jack_frame_time(c); };
	jack_nframes_t (*lastFrameTime)(const jack_client_t*) = [](const jack_client_t* c) { return jack_last_frame_time(c); };
	int (*renamePort)(jack_client_t*, jack_port_t*, const char*) = [](jack_client_t* c, jack_port_t* p, const char* n)
	{
#ifdef LMMS_HAVE_JACK_PRENAME
		return jack_port_rename(c, p, n);
#else
		(void)c; return jack_port_set_name(p, n);
#endif
	};
};
}
#endif
