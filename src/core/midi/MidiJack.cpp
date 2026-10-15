/*
 * MidiJack.cpp - MIDI client for Jack
 *
 * Copyright (c) 2015 Shane Ambler <develop/at/shaneware.biz>
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

#include "MidiJack.h"

#ifdef LMMS_HAVE_JACK
#include <QDebug>
#include <QScopeGuard>
#include <QThread>
#include <cerrno>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include "AudioEngine.h"
#include "AudioJack.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "MidiEventToByteSeq.h"
#include "Song.h"
#include "TimePos.h"
#include "../audio/JackApi.h"
#include "../audio/JackCallbackGate.h"
#include "../audio/JackGraph.h"

namespace lmms
{
namespace
{
const detail::JackApi nativeApi;
}
static_assert(std::atomic_bool::is_always_lock_free && std::atomic<int>::is_always_lock_free);

MidiJack::MidiJack()
	: MidiJack(Engine::audioEngine() ? dynamic_cast<AudioJack*>(Engine::audioEngine()->audioDev()) : nullptr, nativeApi)
{
}

MidiJack::MidiJack(AudioJack* audio, const detail::JackApi& api)
	: m_api(api), m_clientName(probeDevice().toUtf8()), m_callbackGate(std::make_unique<detail::JackCallbackGate>()),
	  m_outputGate(std::make_unique<detail::JackCallbackGate>())
{
	bool successful = false;
	try { successful = audio ? audio->addMidiClient(this) != nullptr : openStandalone(); }
	catch (...) { closeStandalone(); inputFailed(-EIO); }
	if (!successful && m_nativeError.load() == 0) { inputFailed(-EIO); }
	connect(&m_healthTimer, &QTimer::timeout, this, &MidiJack::pollHealth);
	m_healthTimer.start(100);
}

MidiJack::~MidiJack()
{
	m_closing = true; m_healthTimer.stop(); m_callbackGate->pauseAndWait();
	if (m_jackAudio) { m_jackAudio->removeMidiClient(this); }
	closeStandalone();
}

bool MidiJack::isAvailable() const
{
	const detail::JackCallbackGate::Lease status(*m_callbackGate);
	if (!status) { return false; }
	return m_available.load(std::memory_order_acquire) && !m_disconnected.load(std::memory_order_acquire)
		&& (!m_jackAudio || !m_jackAudio->serverDisconnected());
}

MidiClient::InputBackendStatus MidiJack::inputBackendStatus() const
{
	const detail::JackCallbackGate::Lease status(*m_callbackGate);
	if (!status) { return {true, false, m_nativeError.load(std::memory_order_acquire)}; }
	const bool active = isAvailable() && (!m_jackAudio || (!m_jackAudio->serverDisconnected() && m_jackAudio->isRunning()));
	return {true, active, m_nativeError.load(std::memory_order_acquire)};
}

jack_client_t* MidiJack::jackClient()
{
	return m_jackAudio ? m_jackAudio->jackClient() : m_jackClient;
}

QString MidiJack::probeDevice()
{
	const auto name = ConfigManager::inst()->value("midijack", "lmms");
	return name.isEmpty() ? QString("lmms") : name;
}

bool MidiJack::openStandalone()
{
	bool successful = false;
	const auto cleanup = qScopeGuard([&] { if (!successful) { closeStandalone(); } });
	m_disconnected.store(false, std::memory_order_release);
	jack_status_t status{};
	m_jackClient = m_api.open(m_clientName.constData(), JackNoStartServer, &status);
	if (!m_jackClient) { inputFailed(status ? static_cast<int>(status) : -ENODEV); return false; }
	if (const auto error = m_api.setProcessCallback(m_jackClient, processCallback, this))
	{ inputFailed(error); return false; }
	m_api.onShutdown(m_jackClient, shutdownCallback, this);
	m_inputPort = m_api.registerPort(m_jackClient, "MIDI in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
	if (!m_inputPort) { inputFailed(-ENOSPC); return false; }
	if (!prepareOutput(m_jackClient)) { inputFailed(-ENOSPC); return false; }
	if (const auto error = m_api.activate(m_jackClient)) { inputFailed(error); return false; }
	m_nativeActive = true;
	if (m_disconnected.load(std::memory_order_acquire)) { return false; }
	resetRawParser(); m_nativeError.store(0); m_available.store(true, std::memory_order_release);
	m_outputGate->resume(); m_callbackGate->resume(); successful = true; return true;
}

void MidiJack::closeStandalone()
{
	m_callbackGate->pauseAndWait(); m_available.store(false, std::memory_order_release);
	m_outputGate->pauseAndWait();
	if (m_jackClient)
	{
		// Close owns port retirement even when activation or the server failed.
		try { if (m_nativeActive && !m_disconnected.load()) { m_api.deactivate(m_jackClient); } }
		catch (...) { m_nativeError.store(-EIO); }
		try { if (const auto error = m_api.close(m_jackClient)) { m_nativeError.store(error); } }
		catch (...) { m_nativeError.store(-EIO); }
		m_jackClient = nullptr;
	}
	m_nativeActive = false;
	if (!m_jackAudio) { m_inputPort = nullptr; m_outputPort = nullptr; m_outputClient = nullptr; }
	resetOutput();
	resetRawParser();
	// Close may deliver the old client's last shutdown notification. Drain it
	// before a new client can publish input so it cannot poison that generation.
	if (m_pendingFailure.exchange(false, std::memory_order_acq_rel)) { failInputEvents(InputDeliveryFailure::BackendInput); }
}

bool MidiJack::prepareShared(AudioJack* audio, jack_client_t* client)
{
	m_callbackGate->pauseAndWait(); m_outputGate->pauseAndWait();
	if (m_closing || !client || m_inputPort || m_outputPort || (m_jackAudio && m_jackAudio != audio)) { inputFailed(-EINVAL); return false; }
	const auto previous = m_jackAudio; bool successful = false; m_jackAudio = audio;
	const auto cleanup = qScopeGuard([&] {
		if (!successful)
		{
			if (!releaseShared(!audio->serverDisconnected())) { audio->retireFailedMidiPorts(); }
			m_jackAudio = previous;
		}
	});
	m_inputPort = m_api.registerPort(client, "MIDI in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
	if (!m_inputPort) { inputFailed(-ENOSPC); return false; }
	if (!prepareOutput(client)) { inputFailed(-ENOSPC); return false; }
	// The owner still needs this identity to rebuild both ports after a shutdown
	// that raced registration. Never turn that shared client into a standalone one.
	if (audio->serverDisconnected()) { inputFailed(-ENOTCONN); successful = true; return true; }
	resetRawParser(); m_nativeError.store(0); m_disconnected.store(false); m_available.store(true, std::memory_order_release);
	m_outputGate->resume(); m_callbackGate->resume(); successful = true; return true;
}

bool MidiJack::releaseShared(bool unregister)
{
	m_callbackGate->pauseAndWait(); m_available.store(false, std::memory_order_release);
	m_outputGate->pauseAndWait();
	bool retired = true;
	if (unregister && m_jackAudio && m_jackAudio->jackClient())
	{
		for (const auto port : {m_inputPort, m_outputPort})
		{
			if (!port) { continue; }
			try { if (const auto error = m_api.unregisterPort(m_jackAudio->jackClient(), port)) { m_nativeError.store(error); retired = false; } }
			catch (...) { m_nativeError.store(-EIO); retired = false; }
		}
	}
	m_inputPort = nullptr; m_outputPort = nullptr; m_outputClient = nullptr; resetOutput(); resetRawParser();
	failInputEvents(InputDeliveryFailure::BackendInput); m_pendingFailure.store(false);
	return retired;
}

void MidiJack::audioDeviceRetired()
{
	if (m_jackAudio) { releaseShared(!m_jackAudio->serverDisconnected()); }
	m_jackAudio = nullptr; m_closing = true; m_healthTimer.stop();
	m_nativeError.store(-ENOTCONN); m_disconnected.store(true);
}

void MidiJack::inputFailed(int error) noexcept
{
	m_callbackGate->pause();
	m_available.store(false, std::memory_order_release);
	m_nativeError.store(error ? error : -EIO, std::memory_order_release);
	m_disconnected.store(true, std::memory_order_release);
	m_pendingFailure.store(true, std::memory_order_release);
}

void MidiJack::notifyServerShutdown() noexcept { m_outputGate->pause(); inputFailed(-ENOTCONN); }
void MidiJack::shutdownCallback(void* data) noexcept { static_cast<MidiJack*>(data)->notifyServerShutdown(); }

void MidiJack::pollHealth()
{
	if (m_closing || m_recovering || m_graphBusy) { return; }
	FileResourceIO::EventSuspension events;
	m_recovering = true; const auto done = qScopeGuard([&] { m_recovering = false; });
	if (m_pendingFailure.exchange(false, std::memory_order_acq_rel))
	{
		m_callbackGate->pauseAndWait(); resetRawParser();
		failInputEvents(InputDeliveryFailure::BackendInput);
		QMetaObject::invokeMethod(this, [this] { emit serverDisconnected(); }, Qt::QueuedConnection);
	}
	if (m_disconnected.load())
	{
		if (m_retryClock.isValid() && m_retryClock.elapsed() < 1000) { return; }
		m_retryClock.start();
		try
		{
			if (m_jackAudio)
			{
				if (!m_jackAudio->serverDisconnected()) { m_jackAudio->recoverMidiClient(this); }
			}
			else { closeStandalone(); openStandalone(); }
		}
		catch (...) { inputFailed(-EIO); }
	}
	recoverRetainedRoute();
}

int MidiJack::processCallback(jack_nframes_t frames, void* data) noexcept
{
	const auto midi = static_cast<MidiJack*>(data);
	try { midi->JackMidiRead(frames); midi->JackMidiWrite(frames); }
	catch (...) { midi->inputFailed(-EIO); }
	return 0;
}

void MidiJack::JackMidiRead(jack_nframes_t frames)
{
	const detail::JackCallbackGate::Lease callback(*m_callbackGate);
	if (!callback || !frames || !isAvailable()) { return; }
	try
	{
		if (!m_inputPort) { inputFailed(-ENOTCONN); return; }
		auto buffer = m_api.portBuffer(m_inputPort, frames);
		if (!buffer) { inputFailed(-EIO); return; }
		if (m_api.lostMidiEvents(buffer)) { inputFailed(-EOVERFLOW); return; }
		const auto count = m_api.midiEventCount(buffer);
		if (count > InputQueueLimit) { inputFailed(-EOVERFLOW); return; }
		// Validate the whole native block before publishing any prefix to the
		// owning thread. A malformed final event cannot leave earlier notes live.
		struct Message { std::array<unsigned char, 3> bytes{}; std::size_t size = 0; };
		std::array<Message, InputQueueLimit> messages;
		std::size_t accepted = 0;
		jack_nframes_t previous = 0;
		for (std::uint32_t index = 0; index < count; ++index)
		{
			jack_midi_event_t event{};
			if (const auto error = m_api.midiEvent(&event, buffer, index)) { inputFailed(error); return; }
			if (event.time >= frames || (index && event.time < previous) || !event.size || !event.buffer)
			{ inputFailed(-EINVAL); return; }
			previous = event.time;
			const auto status = event.buffer[0];
			if (!(status & 0x80)) { inputFailed(-EINVAL); return; }
			// JACK packets are complete, with a status byte on every event.
			// System/common/SysEx events remain unsupported by the raw client.
			if (status >= 0xf0) { continue; }
			const std::size_t expected = (status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0 ? 2 : 3;
			if (event.size != expected) { inputFailed(-EINVAL); return; }
			for (std::size_t byte = 1; byte < event.size; ++byte)
			{ if (event.buffer[byte] & 0x80) { inputFailed(-EINVAL); return; } }
			auto& message = messages[accepted++]; message.size = event.size;
			for (std::size_t byte = 0; byte < event.size; ++byte) { message.bytes[byte] = event.buffer[byte]; }
		}
		for (std::size_t index = 0; index < accepted; ++index)
		{
			if (!isAvailable()) { return; }
			for (std::size_t byte = 0; byte < messages[index].size; ++byte) { parseData(messages[index].bytes[byte]); }
		}
	}
	catch (...) { inputFailed(-EIO); }
}

void MidiJack::requireGraphOwner() const
{
	if (QThread::currentThread() != thread()) { throw std::runtime_error("JACK graph operations require the MIDI owner's thread"); }
	if (m_graphBusy) { throw std::runtime_error("Another JACK graph operation is active"); }
}

bool MidiJack::graphConnected(jack_port_t* local, const QByteArray& remote)
{
	const auto connected = m_api.portConnectedTo(local, remote.constData());
	if (connected < 0) { m_graphError = connected; throw std::runtime_error("JACK connection observation failed"); }
	return connected != 0;
}

void MidiJack::requireGraphPeer(bool input, const QString& remote)
{
	const auto peer = m_api.portByName(m_outputClient, remote.toUtf8().constData());
	if (!peer) { m_graphError = -ENOENT; throw std::invalid_argument("JACK peer is not available"); }
	if (m_api.portIsMine(m_outputClient, peer) || detail::jackGraphName(m_api.portType(peer)) != JACK_DEFAULT_MIDI_TYPE
		|| !(m_api.portFlags(peer) & (input ? JackPortIsOutput : JackPortIsInput)) || detail::jackGraphName(m_api.portName(peer)) != remote)
	{
		m_graphError = -EINVAL;
		throw std::invalid_argument("JACK peer must be an available external MIDI port with the opposite direction");
	}
}

bool MidiJack::canRecoverRoutes() const
{
	const auto engine = Engine::audioEngine();
	return engine && engine->midiClient() == this && (!m_jackAudio || engine->audioDev() == m_jackAudio)
		&& engine->canChangeBackendGraph() && isAvailable();
}

bool MidiJack::forgetRetainedRoute(bool input, const QString& remote)
{
	const auto found = std::find_if(m_retainedRoutes.begin(), m_retainedRoutes.end(), [&](const auto& route) {
		return route.input == input && route.remote == remote;
	});
	if (found == m_retainedRoutes.end()) { return false; }
	m_retainedRoutes.erase(found);
	if (m_retainedCursor >= m_retainedRoutes.size()) { m_retainedCursor = 0; }
	return true;
}

MidiClient::BackendReconnect MidiJack::setBackendReconnect(bool input, const QString& remote, bool enabled)
{
	requireGraphOwner(); const auto encoded = detail::jackRouteName(remote);
	m_graphBusy = true; const auto idle = qScopeGuard([&] { m_graphBusy = false; });
	if (!enabled) { return {remote, false, forgetRetainedRoute(input, remote)}; }
	for (const auto& route : m_retainedRoutes)
	{ if (route.input == input && route.remote == remote) { return {remote, true, false}; } }
	const auto engine = Engine::audioEngine();
	if (!engine || !engine->canChangeBackendGraph())
	{ throw std::runtime_error("Retaining a MIDI route requires stopped/disarmed transport and no device transition or render"); }
	std::size_t bytes = encoded.size();
	for (const auto& route : m_retainedRoutes) { bytes += route.remote.toUtf8().size(); }
	if (m_retainedRoutes.size() >= 64 || bytes > 64 * 1024)
	{ throw std::runtime_error("JACK reconnect preferences exceed 64 routes or 64 KiB of endpoint names"); }
	const detail::JackCallbackGate::Lease graph(*m_outputGate);
	if (!graph || !isAvailable()) { throw std::runtime_error("JACK MIDI graph is unavailable; connect the endpoint before retaining it"); }
	requireGraphPeer(input, remote);
	if (!graphConnected(input ? m_inputPort : m_outputPort, encoded) || !isAvailable())
	{ throw std::runtime_error("Connect and verify the exact MIDI endpoint before retaining it"); }
	RetainedRoute route{input, remote, {}, 0}; route.retry.start(); m_retainedRoutes.push_back(std::move(route));
	return {remote, true, true};
}

void MidiJack::recoverRetainedRoute()
{
	if (m_retainedRoutes.empty()) { return; }
	for (std::size_t offset = 0; offset < m_retainedRoutes.size(); ++offset)
	{
		const auto index = (m_retainedCursor + offset) % m_retainedRoutes.size();
		auto& route = m_retainedRoutes[index];
		if (route.retry.isValid() && route.retry.elapsed() < 1000) { continue; }
		if (!canRecoverRoutes()) { return; }
		m_retainedCursor = (index + 1) % m_retainedRoutes.size(); route.retry.start();
		// setBackendRoute neither adds nor removes preferences when enabling.
		// Reuse its input reset, output drain, verification and rollback rules.
		try { setBackendRoute(route.input, route.remote, true); route.nativeErrorCode = 0; }
		catch (...) { route.nativeErrorCode = m_graphError ? m_graphError : -EIO; }
		return;
	}
}

MidiClient::BackendGraph MidiJack::inspectBackendGraph(bool input)
{
	requireGraphOwner(); m_graphBusy = true; const auto idle = qScopeGuard([&] { m_graphBusy = false; });
	BackendGraph result; result.supported = true; result.reconnectSupported = true;
	for (const auto& route : m_retainedRoutes)
	{
		if (route.input == input) { result.endpoints.push_back({route.remote, false, false, true, true, route.nativeErrorCode}); }
	}
	result.reconnectDeferred = !result.endpoints.empty() && !canRecoverRoutes();
	const auto sortEndpoints = [&] {
		std::sort(result.endpoints.begin(), result.endpoints.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
	};
	const detail::JackCallbackGate::Lease graph(*m_outputGate);
	if (!graph || !isAvailable())
	{ result.nativeErrorCode = m_nativeError.load() ? m_nativeError.load() : -ENOTCONN; sortEndpoints(); return result; }
	try
	{
		const auto local = input ? m_inputPort : m_outputPort;
		result.localPort = detail::jackGraphName(m_api.portName(local));
		const auto flags = input ? JackPortIsOutput : JackPortIsInput;
		const auto names = m_api.ports(m_outputClient, nullptr, JACK_DEFAULT_MIDI_TYPE, flags);
		const auto release = qScopeGuard([&] { if (names) { m_api.freeMemory(names); } });
		std::size_t bytes = 0;
		for (std::size_t index = 0; names && names[index]; ++index)
		{
			if (index >= 1024) { throw std::runtime_error("JACK MIDI endpoint inventory exceeds 1024 entries"); }
			const auto name = detail::jackGraphName(names[index]); const auto encoded = name.toUtf8(); bytes += encoded.size();
			if (bytes > 256 * 1024) { throw std::runtime_error("JACK MIDI endpoint names exceed 256 KiB"); }
			const auto port = m_api.portByName(m_outputClient, encoded.constData());
			if (!port || m_api.portIsMine(m_outputClient, port)) { continue; }
			if (!(m_api.portFlags(port) & flags) || detail::jackGraphName(m_api.portType(port)) != JACK_DEFAULT_MIDI_TYPE) { continue; }
			const auto connected = graphConnected(local, encoded);
			const auto retained = std::find_if(result.endpoints.begin(), result.endpoints.end(), [&](const auto& endpoint) { return endpoint.name == name; });
			if (retained == result.endpoints.end()) { result.endpoints.push_back({name, connected}); }
			else
			{
				retained->available = true; retained->connected = connected; retained->pending = retained->reconnect && !connected;
				if (connected) { retained->nativeErrorCode = 0; }
			}
		}
		if (!isAvailable()) { throw std::runtime_error("JACK disconnected during endpoint discovery"); }
		sortEndpoints();
		result.observed = true; result.nativeErrorCode = m_graphError; return result;
	}
	catch (...) { if (!m_graphError) { m_graphError = -EIO; } throw; }
}

void MidiJack::finishPortTransferOutput(const std::function<bool()>& cancelled)
{
	requireGraphOwner();
	m_graphBusy = true; m_graphChanging.store(true, std::memory_order_release);
	const auto resume = qScopeGuard([&] { m_graphChanging.store(false, std::memory_order_release); m_graphBusy = false; });
	const detail::JackCallbackGate::Lease output(*m_outputGate);
	if (!output || !isAvailable()) { throw std::runtime_error("JACK output is unavailable for MIDI client transfer cleanup"); }
	drainOutputForRouting(cancelled);
}

void MidiJack::drainOutputForRouting(const std::function<bool()>& cancelled)
{
	suspendOutput();
	bool waitingForCycle = false; std::uint64_t submitted = 0; QElapsedTimer deadline; deadline.start();
	while (true)
	{
		if (cancelled && cancelled()) { throw std::runtime_error("JACK routing was cancelled before changing the graph"); }
		if (!isAvailable()) { throw std::runtime_error("JACK disconnected before output cleanup completed"); }
		{
			const std::lock_guard<std::mutex> lock(m_outputMutex);
			if (!m_cleanup && !m_outputError)
			{
				if (!waitingForCycle && (!m_cleanupSubmittedCycle || m_cleanupSubmittedCycle != m_outputCycle)) { return; }
				waitingForCycle = true;
				// A subsequent callback establishes that JACK advanced beyond the
				// cycle carrying cleanup. This is not a hardware acknowledgement.
				if (submitted && m_outputCycle != submitted) { return; }
				if (!submitted) { submitted = m_outputCycle; }
			}
			else { waitingForCycle = true; submitted = 0; }
		}
		if (deadline.elapsed() >= 250) { throw std::runtime_error("JACK output cleanup did not complete within 250 ms; graph unchanged"); }
		QThread::msleep(1);
	}
}

MidiClient::BackendRoute MidiJack::setBackendRoute(bool input, const QString& remote, bool enabled,
	const std::function<bool()>& cancelled)
{
	requireGraphOwner();
	const auto encoded = detail::jackRouteName(remote);
	const auto engine = Engine::audioEngine();
	if (!engine || !engine->canChangeBackendGraph())
	{ throw std::runtime_error("Native MIDI graph changes require stopped/disarmed transport and no active device transition or render"); }
	m_graphBusy = true; const auto idle = qScopeGuard([&] { m_graphBusy = false; });
	m_graphError = 0; bool completed = false;
	const auto failure = qScopeGuard([&] { if (!completed && !m_graphError) { m_graphError = -EIO; } });
	const detail::JackCallbackGate::Lease graph(*m_outputGate);
	if (!graph || !isAvailable()) { throw std::runtime_error("JACK MIDI graph is unavailable"); }
	if (cancelled && cancelled()) { throw std::runtime_error("JACK routing was cancelled"); }
	const auto local = input ? m_inputPort : m_outputPort;
	const auto localName = detail::jackGraphName(m_api.portName(local)); const auto localBytes = localName.toUtf8();
	const auto commit = [&](bool changed) -> BackendRoute {
		completed = true; m_graphError = 0;
		if (!enabled) { return {localName, remote, false, changed, false, forgetRetainedRoute(input, remote)}; }
		for (auto& route : m_retainedRoutes)
		{
			if (route.input == input && route.remote == remote)
			{ route.nativeErrorCode = 0; route.retry.start(); return {localName, remote, true, changed, true, false}; }
		}
		return {localName, remote, true, changed, false, false};
	};
	const auto before = graphConnected(local, encoded);
	if (before == enabled)
	{
		if (!isAvailable()) { throw std::runtime_error("JACK disconnected during connection observation"); }
		if (cancelled && cancelled()) { throw std::runtime_error("JACK routing was cancelled"); }
		return commit(false);
	}
	requireGraphPeer(input, remote);
	bool inputPaused = false;
	const auto resume = qScopeGuard([&] {
		m_graphChanging.store(false, std::memory_order_release);
		if (inputPaused && m_available.load() && !m_disconnected.load() && (!m_jackAudio || !m_jackAudio->serverDisconnected())) { m_callbackGate->resume(); }
	});
	if (input)
	{
		m_callbackGate->pauseAndWait(); inputPaused = true; resetRawParser();
		// The raw input merges sources. Retargeting must release every affected
		// model's old notes; explicit midi.input.reset resumes those deliveries.
		failInputEvents(InputDeliveryFailure::BackendInput);
	}
	else { m_graphChanging.store(true, std::memory_order_release); drainOutputForRouting(cancelled); }
	const auto change = [&](bool connect) {
		const auto source = input ? encoded.constData() : localBytes.constData();
		const auto destination = input ? localBytes.constData() : encoded.constData();
		return connect ? m_api.connect(m_outputClient, source, destination) : m_api.disconnect(m_outputClient, source, destination);
	};
	const auto restore = [&] {
		try
		{
			if (m_disconnected.load() || (m_jackAudio && m_jackAudio->serverDisconnected())) { return false; }
			if (graphConnected(local, encoded) == before) { return true; }
			change(before); return graphConnected(local, encoded) == before;
		}
		catch (...) { return false; }
	};
	try
	{
		if (cancelled && cancelled()) { throw std::runtime_error("JACK routing was cancelled before changing the graph"); }
		const auto code = change(enabled);
		if (code && !(enabled && code == EEXIST)) { m_graphError = code; throw std::runtime_error("Native JACK connection change failed"); }
		if (graphConnected(local, encoded) != enabled || m_disconnected.load() || (m_jackAudio && m_jackAudio->serverDisconnected()))
		{ m_graphError = -EIO; throw std::runtime_error("JACK did not confirm the requested connection state"); }
		if (cancelled && cancelled()) { throw std::runtime_error("JACK routing was cancelled after changing the graph"); }
		if (m_recovering && !input)
		{
			// The destination may have retained notes while this edge was absent.
			// Cleanup drained on the old graph cannot reach it. Queue releases on
			// all raw channels after reconnect; the callback sends them before any
			// new musical events, retaining progress if its buffer is too small.
			const std::lock_guard<std::mutex> lock(m_outputMutex);
			m_cleanup = std::numeric_limits<std::uint64_t>::max(); m_reconnectCleanup = true;
		}
		return commit(true);
	}
	catch (const std::exception& error)
	{
		const auto reason = QString::fromUtf8(error.what()); const bool restored = restore();
		if (!m_graphError) { m_graphError = -EIO; }
		throw std::runtime_error((reason + (restored ? "; previous connection state verified" : "; previous connection state could not be restored; inspect the native graph")
			+ (inputPaused ? "; affected input deliveries require midi.input.reset" : "")).toStdString());
	}
	catch (...)
	{
		const bool restored = restore(); m_graphError = -EIO;
		const QString reason = restored ? "JACK routing failed; previous connection state verified" : "JACK routing failed; restoration unconfirmed; inspect the native graph";
		throw std::runtime_error((reason + (inputPaused ? "; affected input deliveries require midi.input.reset" : "")).toStdString());
	}
}

// Output uses complete normalized packets. The byte-at-a-time raw interface
// cannot preserve packet/source ownership, so this backend overrides it.
void MidiJack::sendByte(const unsigned char) { throw std::logic_error("JACK output requires a complete MIDI packet"); }

bool MidiJack::prepareOutput(jack_client_t* client)
{
	m_outputRate = m_api.sampleRate(client);
	if (!m_outputRate) { return false; }
	m_outputPort = m_api.registerPort(client, "MIDI out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
	if (!m_outputPort) { return false; }
	m_outputClient = client;
	resetOutput();
	const std::lock_guard<std::mutex> lock(m_outputMutex); m_outputError = 0;
	return true;
}

void MidiJack::resetOutput()
{
	const std::lock_guard<std::mutex> lock(m_outputMutex);
	discardOutputLocked(); m_outputSuspended = false; m_asyncOutputError.store(0);
}

void MidiJack::discardOutputLocked()
{
	++m_outputGeneration; m_outputCount = 0; m_outputSequence = 0;
	for (const auto& entry : m_outputChannels)
	{
		for (unsigned int channel = 0; channel < MidiChannelCount; ++channel)
		{ if (entry.second & (1u << channel)) { m_cleanup |= std::uint64_t{0xf} << (channel * 4); } }
	}
}

void MidiJack::suspendOutput()
{
	const std::lock_guard<std::mutex> lock(m_outputMutex);
	if (!m_outputSuspended) { discardOutputLocked(); }
	else { ++m_outputGeneration; m_outputCount = 0; }
	m_outputSuspended = true;
}

void MidiJack::addPort(MidiPort* port)
{
	{ const std::lock_guard<std::mutex> lock(m_outputMutex); m_outputChannels.emplace(port, 0); }
	try { MidiClient::addPort(port); }
	catch (...) { const std::lock_guard<std::mutex> lock(m_outputMutex); m_outputChannels.erase(port); throw; }
}

void MidiJack::cancelOutputLocked(const MidiPort* port)
{
	const auto end = std::remove_if(m_outputQueue.begin(), m_outputQueue.begin() + m_outputCount,
		[port](const OutputMessage& message) { return message.source == port; });
	m_outputCount = static_cast<std::size_t>(end - m_outputQueue.begin());
	const auto found = m_outputChannels.find(port);
	if (found == m_outputChannels.end()) { return; }
	for (unsigned int channel = 0; channel < MidiChannelCount; ++channel)
	{ if (found->second & (1u << channel)) { m_cleanup |= std::uint64_t{0xf} << (channel * 4); } }
}

void MidiJack::cancelOutputEvents(const MidiPort* port)
{
	const std::lock_guard<std::mutex> lock(m_outputMutex);
	cancelOutputLocked(port);
}

void MidiJack::removePort(MidiPort* port)
{
	{
		const std::lock_guard<std::mutex> lock(m_outputMutex);
		cancelOutputLocked(port); m_outputChannels.erase(port);
	}
	MidiClient::removePort(port);
}

void MidiJack::failOutputLocked(int error)
{
	m_outputError = error ? error : -EIO; m_lastOutputError = m_outputError;
	discardOutputLocked();
}

MidiClient::OutputBackendStatus MidiJack::outputBackendStatus() const
{
	const detail::JackCallbackGate::Lease status(*m_outputGate);
	const std::lock_guard<std::mutex> lock(m_outputMutex);
	unsigned int channels = 0;
	for (unsigned int channel = 0; channel < MidiChannelCount; ++channel)
	{ if (m_cleanup & (std::uint64_t{0xf} << (channel * 4))) { ++channels; } }
	const bool connected = status && m_available.load() && !m_disconnected.load()
		&& (!m_jackAudio || (!m_jackAudio->serverDisconnected() && m_jackAudio->isRunning()));
	const auto asynchronousError = m_asyncOutputError.load();
	const auto error = asynchronousError ? asynchronousError : m_outputError;
	return {true, connected && !error && !m_cleanup, static_cast<unsigned int>(m_outputCount),
		OutputQueueLimit, channels, connected ? error : -ENOTCONN, m_lastOutputError};
}

void MidiJack::processOutEvent(const MidiEvent& event, const TimePos& time, const MidiPort* port)
{
	OutputMessage message; message.source = port;
	message.size = writeToByteSeq(event, message.bytes.data(), message.bytes.size());
	if (!message.size) { return; }
	const detail::JackCallbackGate::Lease producer(*m_outputGate);
	std::uint64_t generation;
	{ const std::lock_guard<std::mutex> lock(m_outputMutex); generation = m_outputGeneration; }
	if (!producer || !m_available.load() || m_disconnected.load()) { throw std::runtime_error("JACK MIDI output is disconnected"); }
	const bool cleanup = message.bytes[0] >> 4 == 0x8 || (message.bytes[0] >> 4 == 0xb && message.bytes[2] == 0
		&& (message.bytes[1] == MidiControllerSustain || message.bytes[1] == MidiControllerSostenuto
			|| message.bytes[1] == MidiControllerAllNotesOff || message.bytes[1] == MidiControllerAllSoundOff));
	if (m_graphChanging.load() && !cleanup) { throw std::runtime_error("JACK output routing is changing"); }
	if (!cleanup && m_jackAudio && !m_jackAudio->isRunning()) { throw std::runtime_error("JACK audio processing is stopped"); }
	if (static_cast<int>(time) < 0) { throw std::invalid_argument("JACK MIDI delay must be nonnegative"); }
	const auto song = Engine::getSong();
	const double delay = static_cast<int>(time) == 0 ? 0.0 : song
		? time.getTimeInMilliseconds(song->getTempo()) * m_outputRate / 1000.0 : -1.0;
	if (!std::isfinite(delay) || delay < 0 || delay > std::numeric_limits<std::int32_t>::max())
	{ throw std::out_of_range("JACK MIDI delay exceeds the native frame-clock range"); }
	try { message.due = m_api.frameTime(m_outputClient) + static_cast<jack_nframes_t>(delay); }
	catch (...)
	{
		const std::lock_guard<std::mutex> lock(m_outputMutex); failOutputLocked(-EIO); throw;
	}
	const std::lock_guard<std::mutex> lock(m_outputMutex);
	if (generation != m_outputGeneration) { throw std::runtime_error("JACK MIDI output changed while enqueueing"); }
	if (m_graphChanging.load() && !cleanup) { throw std::runtime_error("JACK output routing changed before MIDI enqueue"); }
	if (!cleanup && m_jackAudio && !m_jackAudio->isRunning()) { throw std::runtime_error("JACK audio processing stopped before MIDI enqueue"); }
	if (m_outputChannels.find(port) == m_outputChannels.end()) { throw std::runtime_error("JACK MIDI source has retired"); }
	if (m_reconnectCleanup)
	{
		if (cleanup) { return; } // the pending all-channel releases cover these events
		throw std::runtime_error("JACK reconnect cleanup is pending; inspect the output backend before sending new MIDI");
	}
	if (m_outputError) { throw std::runtime_error("JACK MIDI output is recovering from a delivery failure"); }
	if (m_outputCount == OutputQueueLimit)
	{
		failOutputLocked(-ENOBUFS); throw std::runtime_error("JACK MIDI output queue is full");
	}
	if (!m_outputCount) { m_outputSequence = 0; }
	if (m_outputSequence == std::numeric_limits<std::uint64_t>::max())
	{ failOutputLocked(-ERANGE); throw std::overflow_error("JACK MIDI output sequence exhausted"); }
	message.sequence = m_outputSequence++;
	m_outputQueue[m_outputCount++] = message;
}

void MidiJack::JackMidiWrite(jack_nframes_t frames, bool processing)
{
	const detail::JackCallbackGate::Lease callback(*m_outputGate);
	if (!callback || !frames) { return; }
	// No waiting or allocation in the output callback. A contended producer
	// defers delivery, but the native buffer still must be cleared this cycle.
	void* buffer = nullptr;
	std::uint64_t cleanupBefore = 0;
	std::unique_lock<std::mutex> lock(m_outputMutex, std::defer_lock);
	try
	{
		buffer = m_api.portBuffer(m_outputPort, frames);
		if (buffer) { m_api.clearMidiBuffer(buffer); }
		if (!lock.try_lock()) { return; }
		if (!buffer) { failOutputLocked(-EIO); return; }
		++m_outputCycle;
		if (const auto error = m_asyncOutputError.exchange(0)) { failOutputLocked(error); }
		const bool suspended = !processing || !m_available.load() || m_disconnected.load() || m_graphChanging.load();
		if (suspended)
		{
			if (!m_outputSuspended) { discardOutputLocked(); }
			else { m_outputCount = 0; }
		}
		m_outputSuspended = suspended;
		const std::array<unsigned char, 4> controllers{MidiControllerSustain, MidiControllerSostenuto,
			MidiControllerAllNotesOff, MidiControllerAllSoundOff};
		cleanupBefore = m_cleanup;
		for (unsigned int channel = 0; channel < MidiChannelCount; ++channel)
		{
			if (!(m_cleanup & (std::uint64_t{0xf} << (channel * 4)))) { continue; }
			for (unsigned int controller = 0; controller < controllers.size(); ++controller)
			{
				const auto bit = std::uint64_t{1} << (channel * 4 + controller);
				if (!(m_cleanup & bit)) { continue; }
				const std::array<unsigned char, 3> bytes{static_cast<unsigned char>(0xb0 | channel), controllers[controller], 0};
				if (const auto error = m_api.writeMidiEvent(buffer, 0, bytes.data(), bytes.size()))
				{ m_outputError = error; m_lastOutputError = error; return; }
				m_cleanup &= ~bit;
			}
			// Shared raw channels have no per-track identity at the destination.
			for (auto& entry : m_outputChannels) { entry.second &= ~std::uint16_t(1u << channel); }
		}
		if (cleanupBefore && !m_cleanup) { m_cleanupSubmittedCycle = m_outputCycle; }
		m_reconnectCleanup = false;
		m_outputError = 0;
		if (suspended) { return; }
		const auto start = m_api.lastFrameTime(m_outputClient);
		const auto offset = [start](const OutputMessage& message) -> std::int64_t {
			const auto distance = static_cast<std::uint32_t>(message.due - start);
			return distance <= std::numeric_limits<std::int32_t>::max() ? std::int64_t(distance)
				: std::int64_t(distance) - (std::int64_t{1} << 32);
		};
		std::sort(m_outputQueue.begin(), m_outputQueue.begin() + m_outputCount,
			[&](const OutputMessage& a, const OutputMessage& b) { return offset(a) == offset(b) ? a.sequence < b.sequence : offset(a) < offset(b); });
		std::size_t sent = 0;
		while (sent < m_outputCount)
		{
			const auto& message = m_outputQueue[sent]; const auto at = offset(message);
			if (at >= 0 && static_cast<jack_nframes_t>(at) >= frames) { break; }
			const auto found = m_outputChannels.find(message.source);
			if (found == m_outputChannels.end()) { ++sent; continue; }
			const auto type = message.bytes[0] & 0xf0; const auto channel = message.bytes[0] & 0x0f;
			if (type == MidiNoteOn || (type == MidiControlChange && message.bytes[2] >= 64
				&& (message.bytes[1] == MidiControllerSustain || message.bytes[1] == MidiControllerSostenuto)))
			{ found->second |= std::uint16_t(1u << channel); }
			if (const auto error = m_api.writeMidiEvent(buffer, static_cast<jack_nframes_t>(std::max<std::int64_t>(0, at)), message.bytes.data(), message.size))
			{
				// Nothing in this cycle is committed before callback return. Remove
				// its prefix and recover possible earlier-cycle notes on later cycles.
				m_api.clearMidiBuffer(buffer); m_cleanup |= cleanupBefore; failOutputLocked(error); return;
			}
			++sent;
		}
		if (sent) { std::move(m_outputQueue.begin() + sent, m_outputQueue.begin() + m_outputCount, m_outputQueue.begin()); }
		m_outputCount -= sent;
	}
	catch (...)
	{
		try { if (buffer) { m_api.clearMidiBuffer(buffer); } } catch (...) { }
		if (lock.owns_lock() || lock.try_lock()) { m_cleanup |= cleanupBefore; failOutputLocked(-EIO); }
		else { m_asyncOutputError.store(-EIO); }
	}
}

} // namespace lmms
#endif // LMMS_HAVE_JACK
