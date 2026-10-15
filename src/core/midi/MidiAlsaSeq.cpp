/*
 * MidiAlsaSeq.cpp - ALSA sequencer client
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include "MidiAlsaSeq.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "Song.h"
#include "MidiPort.h"
#include "MidiEventToByteSeq.h"

#ifdef LMMS_HAVE_ALSA

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>
#include <vector>

namespace lmms
{

namespace
{
constexpr int EventPollTimeOut = 250;
constexpr unsigned int InputBatchSize = 64;

QString portName(snd_seq_client_info_t* client, snd_seq_port_info_t* port)
{
	return QString("%1:%2 %3:%4").arg(snd_seq_port_info_get_client(port))
		.arg(snd_seq_port_info_get_port(port)).arg(snd_seq_client_info_get_name(client))
		.arg(snd_seq_port_info_get_name(port));
}

QString portName(snd_seq_t* handle, const snd_seq_addr_t& address)
{
	const auto fallback = QString("%1:%2").arg(address.client).arg(address.port);
	if (!handle) { return fallback; }
	snd_seq_client_info_t* client;
	snd_seq_port_info_t* port;
	snd_seq_client_info_alloca(&client);
	snd_seq_port_info_alloca(&port);
	if (snd_seq_get_any_port_info(handle, address.client, address.port, port) < 0
		|| snd_seq_get_any_client_info(handle, address.client, client) < 0) { return fallback; }
	return portName(client, port);
}

bool preparePipe(int (&descriptors)[2])
{
	if (::pipe(descriptors) < 0) { return false; }
	for (const auto descriptor : descriptors)
	{
		const auto flags = ::fcntl(descriptor, F_GETFL);
		if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0
			|| ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) { return false; }
	}
	return true;
}
}

MidiAlsaSeq::MidiAlsaSeq() : MidiAlsaSeq(true) { }

MidiAlsaSeq::MidiAlsaSeq(bool connectSequencer) :
	MidiClient(), m_seqHandle(nullptr), m_queueID(-1), m_quit(false), m_portListUpdateTimer(this)
{
	if (!connectSequencer) { return; }
	try
	{
		if (!initialize()) { return; }
		connect(&m_portListUpdateTimer, &QTimer::timeout, this, &MidiAlsaSeq::updatePortList);
		m_tempoConnection = connect(Engine::getSong(), &Song::tempoChanged,
			this, &MidiAlsaSeq::changeQueueTempo, Qt::DirectConnection);
		m_portListUpdateTimer.start(1000);
		start(QThread::IdlePriority);
	}
	catch (...)
	{
		recordError(-ENOMEM);
		m_portListUpdateTimer.stop();
		QObject::disconnect(m_tempoConnection);
	}
}

bool MidiAlsaSeq::initialize()
{
	const auto check = [this](int code) { recordError(code); return code >= 0; };
	if (!check(snd_seq_open(&m_seqHandle, probeDevice().toLatin1().constData(),
		SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK))) { return false; }
	m_clientID = snd_seq_client_id(m_seqHandle);
	if (m_clientID < 0) { recordError(m_clientID); return false; }
	if (!check(snd_seq_set_client_name(m_seqHandle, "LMMS"))) { return false; }
	m_queueID = snd_seq_alloc_queue(m_seqHandle);
	if (m_queueID < 0) { recordError(m_queueID); return false; }
	const auto song = Engine::getSong();
	if (!song || song->getTempo() <= 0) { recordError(-EINVAL); return false; }
	// PPQ must be installed before the queue starts; native LMMS ticks are 48/quarter.
	if (synchronizeQueueTempoLocked(song->getTempo()) < 0
		|| !check(snd_seq_start_queue(m_seqHandle, m_queueID, nullptr))
		|| !check(snd_seq_drain_output(m_seqHandle))) { return false; }
	if (!preparePipe(m_pipe)) { recordError(-errno); return false; }
	updatePortList();
	return true;
}

MidiAlsaSeq::~MidiAlsaSeq()
{
	m_portListUpdateTimer.stop();
	QObject::disconnect(m_tempoConnection);
	m_quit.store(true, std::memory_order_release);
	if (m_pipe[1] >= 0)
	{
		const char wake = 1;
		// Nonblocking write: the stop flag is authoritative even if the pipe is full.
		(void)::write(m_pipe[1], &wake, sizeof(wake));
	}
	// Closing a handle after an ignored timed join can leave run() accessing freed state.
	// Join without m_seqMutex. The reader never enters a model/DSP guard, so callers may hold that guard.
	wait();
	closeResources();
}

void MidiAlsaSeq::closeResources()
{
	const QMutexLocker lock(&m_seqMutex);
	if (m_seqHandle)
	{
		// The reader has joined and new sends observe m_quit. Release voices before
		// freeing the queue/ports, including when MidiPorts outlive this backend.
		for (auto it = m_portIDs.begin(); it != m_portIDs.end(); ++it)
		{
			try { cancelOutputEventsLocked(it.value()[1], it.value().activeOutputChannels); }
			catch (...) { qWarning("ALSA MIDI output cleanup failed while closing the sequencer"); }
		}
		if (m_queueID >= 0)
		{
			recordError(snd_seq_stop_queue(m_seqHandle, m_queueID, nullptr));
			recordError(snd_seq_drain_output(m_seqHandle));
			recordError(snd_seq_free_queue(m_seqHandle, m_queueID));
		}
		recordError(snd_seq_close(m_seqHandle));
		m_seqHandle = nullptr;
	}
	m_queueID = -1;
	m_queueTempo = 0;
	m_queueMicrosecondsPerQuarterNote = 0;
	m_clientID = -1;
	m_portIDs.clear();
	for (auto& descriptor : m_pipe)
	{
		if (descriptor >= 0) { ::close(descriptor); descriptor = -1; }
	}
}

void MidiAlsaSeq::recordError(int code)
{
	if (code >= 0) { return; }
	int expected = 0;
	m_nativeError.compare_exchange_strong(expected, code, std::memory_order_relaxed);
}

MidiClient::InputBackendStatus MidiAlsaSeq::inputBackendStatus() const
{
	const QMutexLocker lock(&m_seqMutex);
	return {true, m_seqHandle && isRunning() && !m_quit.load(std::memory_order_acquire),
		m_nativeError.load(std::memory_order_relaxed)};
}

MidiClient::OutputClockStatus MidiAlsaSeq::outputClockStatus() const
{
	const QMutexLocker lock(&m_seqMutex);
	return {true, m_seqHandle && m_queueID >= 0 && m_queueTempo > 0 && m_queueTempoError == 0
		&& !m_quit.load(std::memory_order_acquire), m_queueMicrosecondsPerQuarterNote ? TicksPerQuarterNote : 0,
		m_queueMicrosecondsPerQuarterNote, m_queueTempoError};
}

QString MidiAlsaSeq::probeDevice()
{
	auto device = ConfigManager::inst()->value("Midialsaseq", "device");
	if (!device.isEmpty()) { return device; }
	if (const auto environmentDevice = getenv("MIDIDEV")) { return environmentDevice; }
	return "default";
}

void MidiAlsaSeq::processOutEvent(const MidiEvent& event, const TimePos& time, const MidiPort* port)
{
	std::uint8_t bytes[3]{};
	if (!writeToByteSeq(event, bytes, sizeof(bytes))) { return; }
	if (static_cast<int>(time) < 0) { throw std::invalid_argument("ALSA MIDI output delay must not be negative"); }
	const QMutexLocker lock(&m_seqMutex);
	if (!m_seqHandle || m_quit.load(std::memory_order_acquire)) { checkOutputResult(-ENODEV); }
	const auto found = m_portIDs.find(const_cast<MidiPort*>(port));
	if (found == m_portIDs.end() || found.value()[1] < 0) { checkOutputResult(-ENXIO); }
	const auto id = found.value()[1];
	if (id > std::numeric_limits<unsigned char>::max()) { checkOutputResult(-ERANGE); }
	if (static_cast<int>(time) > 0)
	{
		const auto song = Engine::getSong();
		if (!song) { checkOutputResult(-ENODEV); }
		if (m_queueTempo != song->getTempo() || m_queueTempoError < 0)
		{
			checkOutputResult(synchronizeQueueTempoLocked(song->getTempo()));
		}
	}
	snd_seq_event_t native;
	snd_seq_ev_clear(&native);
	snd_seq_ev_set_source(&native, id);
	// ALSA removal has no source-port filter. Native port IDs are unique within
	// this client and fit its byte-sized tag; retire their events before ID reuse.
	snd_seq_ev_set_tag(&native, id);
	snd_seq_ev_set_subs(&native);
	if (static_cast<int>(time) == 0) { snd_seq_ev_set_direct(&native); }
	else { snd_seq_ev_schedule_tick(&native, m_queueID, 1, static_cast<int>(time)); }
	switch (event.type())
	{
	case MidiNoteOn: snd_seq_ev_set_noteon(&native, event.channel(), event.key(), event.velocity()); break;
	case MidiNoteOff: snd_seq_ev_set_noteoff(&native, event.channel(), event.key(), event.velocity()); break;
	case MidiKeyPressure: snd_seq_ev_set_keypress(&native, event.channel(), event.key(), event.velocity()); break;
	case MidiControlChange: snd_seq_ev_set_controller(&native, event.channel(), event.controllerNumber(), event.controllerValue()); break;
	case MidiProgramChange: snd_seq_ev_set_pgmchange(&native, event.channel(), event.program()); break;
	case MidiChannelPressure: snd_seq_ev_set_chanpress(&native, event.channel(), event.channelPressure()); break;
	case MidiPitchBend: snd_seq_ev_set_pitchbend(&native, event.channel(), event.pitchBend() - 8192); break;
	default: return;
	}
	if ((event.type() == MidiNoteOn && event.velocity() > 0)
		|| (event.type() == MidiControlChange && event.controllerValue() >= 64
			&& (event.controllerNumber() == MidiControllerSustain || event.controllerNumber() == MidiControllerSostenuto)))
	{
		found.value().activeOutputChannels |= quint16(1u << event.channel());
	}
	// Bypass the userspace output buffer. Positive delays still use the kernel
	// queue; zero-delay cleanup must not wait behind a stopped or failed queue.
	checkOutputResult(m_outputApi.send(m_seqHandle, &native));
}

void MidiAlsaSeq::checkOutputResult(int code)
{
	if (code >= 0) { return; }
	recordError(code);
	throw std::runtime_error(QString("ALSA MIDI output failed: %1 (%2)").arg(snd_strerror(code)).arg(code).toStdString());
}

void MidiAlsaSeq::cancelOutputEventsLocked(int id, quint16& activeChannels)
{
	if (id < 0) { return; }
	if (!m_seqHandle) { checkOutputResult(-ENODEV); }
	if (id > std::numeric_limits<unsigned char>::max()) { checkOutputResult(-ERANGE); }
	snd_seq_remove_events_t* removal;
	snd_seq_remove_events_alloca(&removal);
	const snd_seq_addr_t destination{SND_SEQ_ADDRESS_SUBSCRIBERS, SND_SEQ_ADDRESS_UNKNOWN};
	snd_seq_remove_events_set_condition(removal, SND_SEQ_REMOVE_OUTPUT | SND_SEQ_REMOVE_TAG_MATCH | SND_SEQ_REMOVE_DEST);
	snd_seq_remove_events_set_tag(removal, id);
	snd_seq_remove_events_set_queue(removal, m_queueID);
	snd_seq_remove_events_set_dest(removal, &destination);
	checkOutputResult(m_outputApi.remove(m_seqHandle, removal));
	for (int channel = 0; channel < MidiChannelCount; ++channel)
	{
		if (!(activeChannels & (1u << channel))) { continue; }
		// A canceled note-off may have followed an already delivered note-on.
		// MIDI has no per-source note identity at a shared destination/channel.
		for (const auto controller : {MidiControllerSustain, MidiControllerSostenuto,
			MidiControllerAllNotesOff, MidiControllerAllSoundOff})
		{
			snd_seq_event_t cleanup{};
			snd_seq_ev_set_source(&cleanup, id);
			snd_seq_ev_set_tag(&cleanup, id);
			snd_seq_ev_set_subs(&cleanup);
			snd_seq_ev_set_direct(&cleanup);
			snd_seq_ev_set_controller(&cleanup, channel, controller, 0);
			checkOutputResult(m_outputApi.send(m_seqHandle, &cleanup));
		}
		activeChannels &= ~quint16(1u << channel);
	}
}

void MidiAlsaSeq::cancelOutputEvents(const MidiPort* port)
{
	const QMutexLocker lock(&m_seqMutex);
	const auto found = m_portIDs.find(const_cast<MidiPort*>(port));
	if (found == m_portIDs.end()) { return; }
	cancelOutputEventsLocked(found.value()[1], found.value().activeOutputChannels);
}

void MidiAlsaSeq::applyPortMode(MidiPort* port)
{
	const QMutexLocker lock(&m_seqMutex);
	if (!m_seqHandle || m_quit.load(std::memory_order_acquire)) { return; }
	std::array<unsigned int, 2> capabilities{};
	if (port->isInputEnabled()) { capabilities[0] = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE; }
	if (port->isOutputEnabled()) { capabilities[1] = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ; }
	auto& ids = m_portIDs[port];
	for (int direction = 0; direction < 2; ++direction)
	{
		auto& id = ids[direction];
		if (!capabilities[direction])
		{
			if (id >= 0)
			{
				const auto removed = snd_seq_delete_simple_port(m_seqHandle, id);
				recordError(removed);
				if (removed >= 0) { id = -1; }
			}
			continue;
		}
		if (id < 0)
		{
			id = snd_seq_create_simple_port(m_seqHandle, port->displayName().toUtf8().constData(),
				capabilities[direction], SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
			recordError(id);
			if (direction == 1 && id >= 0)
			{
				try { cancelOutputEventsLocked(id, ids.activeOutputChannels); }
				catch (...)
				{
					// A previously retired port may have left events after a failed
					// cancellation. Do not use a recycled ID until its queue is clear.
					recordError(snd_seq_delete_simple_port(m_seqHandle, id)); id = -1;
					qWarning("ALSA MIDI output port is unavailable after failed cancellation of a recycled ID");
				}
			}
			continue;
		}
		snd_seq_port_info_t* info;
		snd_seq_port_info_alloca(&info);
		const auto result = snd_seq_get_port_info(m_seqHandle, id, info);
		recordError(result);
		if (result < 0) { continue; }
		snd_seq_port_info_set_capability(info, capabilities[direction]);
		recordError(snd_seq_set_port_info(m_seqHandle, id, info));
	}
}

void MidiAlsaSeq::validatePortForTransfer(const MidiPort* port) const
{
	MidiClient::validatePortForTransfer(port);
	const QMutexLocker lock(&m_seqMutex);
	const auto found = m_portIDs.constFind(const_cast<MidiPort*>(port));
	if ((port->isInputEnabled() || port->isOutputEnabled()) &&
		(found == m_portIDs.cend() || (port->isInputEnabled() && found.value()[0] < 0)
			|| (port->isOutputEnabled() && found.value()[1] < 0)))
	{ throw std::runtime_error("ALSA sequencer could not prepare every enabled native MIDI port for transfer"); }
}

void MidiAlsaSeq::applyPortName(MidiPort* port)
{
	const QMutexLocker lock(&m_seqMutex);
	if (!m_seqHandle || m_quit.load(std::memory_order_acquire)) { return; }
	const auto found = m_portIDs.constFind(port);
	if (found == m_portIDs.cend()) { return; }
	for (int direction = 0; direction < 2; ++direction)
	{
		const auto id = found.value()[direction];
		if (id < 0) { continue; }
		snd_seq_port_info_t* info;
		snd_seq_port_info_alloca(&info);
		const auto result = snd_seq_get_port_info(m_seqHandle, id, info);
		recordError(result);
		if (result < 0) { continue; }
		snd_seq_port_info_set_name(info, port->displayName().toUtf8().constData());
		recordError(snd_seq_set_port_info(m_seqHandle, id, info));
	}
}

void MidiAlsaSeq::removePort(MidiPort* port)
{
	const QMutexLocker lock(&m_seqMutex);
	const auto found = m_portIDs.find(port);
	if (found != m_portIDs.end())
	{
		if (m_seqHandle && !m_quit.load(std::memory_order_acquire))
		{
			try { cancelOutputEventsLocked(found.value()[1], found.value().activeOutputChannels); }
			catch (...) { qWarning("ALSA MIDI output cancellation failed while removing a port"); }
			for (int direction = 0; direction < 2; ++direction)
			{
				if (found.value()[direction] >= 0) { recordError(snd_seq_delete_simple_port(m_seqHandle, found.value()[direction])); }
			}
		}
		m_portIDs.erase(found);
	}
	MidiClient::removePort(port);
}

QString MidiAlsaSeq::sourcePortName(const MidiEvent& event) const
{
	if (!event.sourcePortName().isEmpty()) { return event.sourcePortName(); }
	const QMutexLocker lock(&m_seqMutex);
	if (event.sourcePort()) { return portName(m_seqHandle, *static_cast<const snd_seq_addr_t*>(event.sourcePort())); }
	return MidiClient::sourcePortName(event);
}

void MidiAlsaSeq::subscribeReadablePort(MidiPort* port, const QString& destination, bool subscribe)
{
	subscribePort(port, destination, true, subscribe);
}

void MidiAlsaSeq::subscribeWritablePort(MidiPort* port, const QString& destination, bool subscribe)
{
	subscribePort(port, destination, false, subscribe);
}

void MidiAlsaSeq::subscribePort(MidiPort* port, const QString& destination, bool input, bool subscribe)
{
	const QMutexLocker lock(&m_seqMutex);
	if (!m_seqHandle || m_quit.load(std::memory_order_acquire)) { return; }
	const auto found = m_portIDs.constFind(port);
	if (found == m_portIDs.cend()) { return; }
	const auto id = input || found.value()[1] < 0 ? found.value()[0] : found.value()[1];
	if (id < 0) { return; }
	snd_seq_addr_t peer;
	const auto parsed = snd_seq_parse_address(m_seqHandle, &peer, destination.section(' ', 0, 0).toLatin1().constData());
	recordError(parsed);
	if (parsed < 0) { return; }
	snd_seq_port_info_t* info;
	snd_seq_port_info_alloca(&info);
	const auto queried = snd_seq_get_port_info(m_seqHandle, id, info);
	recordError(queried);
	if (queried < 0) { return; }
	snd_seq_port_subscribe_t* subscription;
	snd_seq_port_subscribe_alloca(&subscription);
	const auto address = snd_seq_port_info_get_addr(info);
	snd_seq_port_subscribe_set_sender(subscription, input ? &peer : address);
	snd_seq_port_subscribe_set_dest(subscription, input ? address : &peer);
	const auto existing = snd_seq_get_port_subscription(m_seqHandle, subscription);
	if (subscribe && existing >= 0) { return; }
	if (existing < 0 && existing != -ENOENT) { recordError(existing); return; }
	if (!subscribe && existing == -ENOENT) { return; }
	recordError(subscribe ? snd_seq_subscribe_port(m_seqHandle, subscription)
		: snd_seq_unsubscribe_port(m_seqHandle, subscription));
}

void MidiAlsaSeq::processInputEventLocked(const snd_seq_event_t& native, const QString& sourceName)
{
	if (native.dest.client != m_clientID) { return; }
	MidiPort* destination = nullptr;
	for (auto it = m_portIDs.cbegin(); it != m_portIDs.cend(); ++it)
	{
		if (it.value()[0] >= 0 && it.value()[0] == native.dest.port) { destination = it.key(); break; }
	}
	if (!destination) { return; }
	MidiEvent event;
	TimePos time;
	if (native.type == SND_SEQ_EVENT_NOTEON || native.type == SND_SEQ_EVENT_NOTEOFF || native.type == SND_SEQ_EVENT_KEYPRESS)
	{
		if (native.data.note.channel >= MidiChannelCount || native.data.note.note > 127 || native.data.note.velocity > 127) { return; }
		const auto type = native.type == SND_SEQ_EVENT_NOTEON ? MidiNoteOn
			: native.type == SND_SEQ_EVENT_NOTEOFF ? MidiNoteOff : MidiKeyPressure;
		event = MidiEvent(type, native.data.note.channel, native.data.note.note, native.data.note.velocity);
		if (native.type != SND_SEQ_EVENT_KEYPRESS && (native.flags & SND_SEQ_TIME_STAMP_MASK) == SND_SEQ_TIME_STAMP_TICK)
		{
			if (native.time.tick > static_cast<unsigned int>(std::numeric_limits<int>::max())) { return; }
			time = TimePos(static_cast<int>(native.time.tick));
		}
	}
	else if (native.type == SND_SEQ_EVENT_CONTROLLER || native.type == SND_SEQ_EVENT_PGMCHANGE
		|| native.type == SND_SEQ_EVENT_CHANPRESS || native.type == SND_SEQ_EVENT_PITCHBEND)
	{
		const auto channel = native.data.control.channel;
		const auto first = native.data.control.param;
		const auto value = native.data.control.value;
		if (channel >= MidiChannelCount) { return; }
		switch (native.type)
		{
		case SND_SEQ_EVENT_CONTROLLER:
			if (first > 127 || value < 0 || value > 127) { return; }
			event = MidiEvent(MidiControlChange, channel, first, value); break;
		case SND_SEQ_EVENT_PGMCHANGE:
		case SND_SEQ_EVENT_CHANPRESS:
			if (value < 0 || value > 127) { return; }
			event = MidiEvent(native.type == SND_SEQ_EVENT_PGMCHANGE ? MidiProgramChange : MidiChannelPressure, channel, value, 0); break;
		case SND_SEQ_EVENT_PITCHBEND:
			if (value < -8192 || value > 8191) { return; }
			event = MidiEvent(MidiPitchBend, channel, value + 8192, 0); break;
		default: return;
		}
	}
	else { return; }
	// The destination map remains locked until the shared delivery record is acquired.
	dispatchInputEvent(event, time, destination, sourceName);
}

void MidiAlsaSeq::inputFailed(int code, bool terminal) noexcept
{
	recordError(code);
	failInputEvents(InputDeliveryFailure::BackendInput);
	if (!terminal) { return; }
	m_quit.store(true, std::memory_order_release);
	try
	{
		QMetaObject::invokeMethod(this, [this] {
			m_portListUpdateTimer.stop();
			publishPortLists({}, {});
		}, Qt::QueuedConnection);
	}
	catch (...) { } // Failure state remains observable if event posting also fails.
}

void MidiAlsaSeq::run()
{
	try
	{
		std::vector<pollfd> descriptors;
		{
			const QMutexLocker lock(&m_seqMutex);
			const auto count = snd_seq_poll_descriptors_count(m_seqHandle, POLLIN);
			if (count <= 0) { inputFailed(count < 0 ? count : -EIO, true); return; }
			descriptors.resize(static_cast<std::size_t>(count) + 1);
			descriptors[0].fd = m_pipe[0]; descriptors[0].events = POLLIN;
			const auto filled = snd_seq_poll_descriptors(m_seqHandle, descriptors.data() + 1, count, POLLIN);
			if (filled <= 0 || filled > count) { inputFailed(filled < 0 ? filled : -EIO, true); return; }
			descriptors.resize(static_cast<std::size_t>(filled) + 1);
		}
		bool bufferedInput = false;
		while (!m_quit.load(std::memory_order_acquire))
		{
			const auto result = ::poll(descriptors.data(), descriptors.size(), bufferedInput ? 0 : EventPollTimeOut);
			if (result < 0)
			{
				if (errno == EINTR) { continue; }
				inputFailed(-errno, true); return;
			}
			if (m_quit.load(std::memory_order_acquire)) { return; }
			if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) { inputFailed(-EIO, true); return; }
			const QMutexLocker lock(&m_seqMutex);
			unsigned short events = 0;
			const auto revents = snd_seq_poll_descriptors_revents(m_seqHandle, descriptors.data() + 1,
				static_cast<unsigned int>(descriptors.size() - 1), &events);
			if (revents < 0 || (events & (POLLERR | POLLHUP | POLLNVAL))) { inputFailed(revents < 0 ? revents : -EIO, true); return; }
			if (!bufferedInput && !(events & POLLIN)) { continue; }
			for (unsigned int count = 0; count < InputBatchSize && !m_quit.load(std::memory_order_acquire); ++count)
			{
				snd_seq_event_t* native = nullptr;
				const auto received = snd_seq_event_input(m_seqHandle, &native);
				bufferedInput = received > 0;
				if (received == -EINTR) { continue; }
				if (received == -EAGAIN) { break; }
				if (received < 0)
				{
					inputFailed(received, received != -ENOSPC);
					if (received != -ENOSPC) { return; }
					break;
				}
				if (!native) { inputFailed(-EIO, true); return; }
				processInputEventLocked(*native, portName(m_seqHandle, native->source));
				if (!bufferedInput) { break; }
			}
		}
	}
	catch (...) { inputFailed(-ENOMEM, true); }
}

int MidiAlsaSeq::synchronizeQueueTempoLocked(bpm_t tempo)
{
	const auto failed = [this](int error) {
		m_queueTempo = 0; m_queueTempoError = error; recordError(error); return error;
	};
	if (!m_seqHandle || m_queueID < 0 || m_quit.load(std::memory_order_acquire)) { return failed(-ENODEV); }
	if (tempo == 0) { return failed(-EINVAL); }
	const auto microseconds = 60000000u / tempo;
	snd_seq_queue_tempo_t* native;
	snd_seq_queue_tempo_alloca(&native);
	int result = m_outputApi.getTempo(m_seqHandle, m_queueID, native);
	if (result < 0) { return failed(result); }
	snd_seq_queue_tempo_set_tempo(native, microseconds);
	snd_seq_queue_tempo_set_ppq(native, TicksPerQuarterNote);
	// This is LMMS's own musical clock; an inherited skew must not change its rate.
	snd_seq_queue_tempo_set_skew(native, 0x10000);
	snd_seq_queue_tempo_set_skew_base(native, 0x10000);
	result = m_outputApi.setTempo(m_seqHandle, m_queueID, native);
	if (result < 0) { return failed(result); }
	result = m_outputApi.getTempo(m_seqHandle, m_queueID, native);
	if (result < 0) { return failed(result); }
	if (snd_seq_queue_tempo_get_tempo(native) != microseconds
		|| snd_seq_queue_tempo_get_ppq(native) != TicksPerQuarterNote
		|| snd_seq_queue_tempo_get_skew(native) != snd_seq_queue_tempo_get_skew_base(native)
		|| snd_seq_queue_tempo_get_skew_base(native) == 0)
	{
		return failed(-EIO);
	}
	m_queueTempo = tempo; m_queueMicrosecondsPerQuarterNote = microseconds; m_queueTempoError = 0;
	return 0;
}

void MidiAlsaSeq::changeQueueTempo(bpm_t tempo)
{
	const QMutexLocker lock(&m_seqMutex);
	if (!m_seqHandle || m_quit.load(std::memory_order_acquire)) { return; }
	// Synchronous ioctl: a buffered tempo event could remain unsent after EAGAIN
	// while following notes enter the kernel with the old rate. Do not throw across
	// Song's tempo signal; the next scheduled send retries and surfaces a failure.
	(void)synchronizeQueueTempoLocked(tempo);
}

void MidiAlsaSeq::publishPortLists(const QStringList& readable, const QStringList& writable)
{
	bool readableChanged, writableChanged;
	{
		const QMutexLocker lock(&m_portListMutex);
		readableChanged = m_readablePorts != readable; writableChanged = m_writablePorts != writable;
		m_readablePorts = readable; m_writablePorts = writable;
	}
	if (readableChanged) { emit readablePortsChanged(); }
	if (writableChanged) { emit writablePortsChanged(); }
}

void MidiAlsaSeq::updatePortList()
{
	QStringList readable, writable;
	{
		const QMutexLocker lock(&m_seqMutex);
		if (!m_seqHandle || m_quit.load(std::memory_order_acquire)) { return; }
		snd_seq_client_info_t* client;
		snd_seq_port_info_t* port;
		snd_seq_client_info_alloca(&client);
		snd_seq_port_info_alloca(&port);
		snd_seq_client_info_set_client(client, -1);
		while (snd_seq_query_next_client(m_seqHandle, client) >= 0)
		{
			snd_seq_port_info_set_client(port, snd_seq_client_info_get_client(client));
			snd_seq_port_info_set_port(port, -1);
			while (snd_seq_query_next_port(m_seqHandle, port) >= 0)
			{
				const auto capabilities = snd_seq_port_info_get_capability(port);
				constexpr auto read = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
				constexpr auto write = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
				if ((capabilities & read) == read) { readable.push_back(portName(client, port)); }
				if ((capabilities & write) == write) { writable.push_back(portName(client, port)); }
			}
		}
	}
	publishPortLists(readable, writable);
}

} // namespace lmms
#endif // LMMS_HAVE_ALSA
