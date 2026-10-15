/*
 * MidiClient.cpp - base-class for MIDI-clients like ALSA-sequencer-client
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * This file partly contains code from Fluidsynth, Peter Hanappe
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

#include "MidiClient.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <deque>
#include <utility>
#include <QMetaObject>
#include <QPointer>
#include <QScopeGuard>
#include <QThread>
#include <limits>
#include <stdexcept>
#include "MidiEventToByteSeq.h"

#include "MidiPort.h"
#include "MidiWinMM.h"
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "Engine.h"
#include "FileResourceIO.h"

namespace lmms
{

MidiClient::NativeRetirementStatus MidiClient::nativeRetirementStatus()
{
#ifdef LMMS_HAVE_WINMM
	return MidiWinMM::retiredHandleStatus();
#else
	return {};
#endif
}

struct MidiClient::InputAdmission
{
	// Even generations admit live input; odd generations reserve native state.
	std::atomic<std::uint64_t> generation{0};
};

MidiClient::MidiClient() : m_inputAdmission(std::make_shared<InputAdmission>()) {}

void MidiClient::setExternalInputBlocked(bool blocked) noexcept
{
	// Native engine change ownership serializes these transitions. Driver threads
	// read only this client-owned atomic, never Song or audio-device pointers.
	const auto generation = m_inputAdmission->generation.load(std::memory_order_acquire);
	if (bool(generation & 1) != blocked) { m_inputAdmission->generation.store(generation + 1, std::memory_order_release); }
}

struct MidiClient::InputDelivery
{
	struct Packet { MidiEvent event; TimePos time; std::uint64_t inputGeneration = 0; };
	InputDelivery(MidiPort* target, std::shared_ptr<InputAdmission> state)
		: port(target), livePort(target), admission(std::move(state)), observedGeneration(admission->generation.load()), enabled(target->isInputEnabled()) { }
	MidiPort* const port;
	const QPointer<MidiPort> livePort;
	std::shared_ptr<InputAdmission> admission;
	std::uint64_t observedGeneration;
	std::mutex mutex;
	std::deque<Packet> pending;
	std::uint64_t dropped = 0;
	bool active = true;
	bool enabled;
	bool scheduled = false;
	bool failed = false;
	bool failureDelivered = false;
	InputDeliveryFailure failure = InputDeliveryFailure::None;
	InputDeliveryFailure lastFailure = InputDeliveryFailure::None;
	std::uint64_t failureVersion = 0;
	std::uint64_t recoveryCount = 0;
	bool processing = false;
	bool recovering = false;
	bool transferPaused = false;
	bool requiresBackendObservation = false;
	bool needsNoteCleanup = false;
	bool retirementCleanupPending = false;
	std::uint64_t discardStaleExternal()
	{
		const auto generation = admission->generation.load(std::memory_order_acquire);
		if (generation != observedGeneration)
		{
			dropped += std::erase_if(pending, [generation](const Packet& packet) {
				return packet.event.source() == MidiEvent::Source::External && ((generation & 1) || packet.inputGeneration != generation);
			});
			observedGeneration = generation;
		}
		return generation;
	}
	void reportFailure(InputDeliveryFailure reason)
	{
		if (failureVersion != std::numeric_limits<std::uint64_t>::max()) { ++failureVersion; }
		lastFailure = reason;
		if (reason == InputDeliveryFailure::BackendCallback || reason == InputDeliveryFailure::BackendInput) { requiresBackendObservation = true; }
		if (!failed) { failure = reason; }
		failed = true;
	}
};

MidiClient::~MidiClient()
{
	std::vector<std::shared_ptr<InputDelivery>> retired;
	{
		const auto portsLock = std::lock_guard{m_portsMutex};
		m_retiredForTransfer = true; retired.swap(m_inputDeliveries);
	}
	// Derived destructors must have joined/revoked native input already. First
	// revoke every queued delivery, then detach every model before any processor
	// callback can run and delete/rebind another port in this batch.
	for (const auto& delivery : retired)
	{
		const auto lock = std::lock_guard{delivery->mutex};
		delivery->active = false; delivery->transferPaused = true;
		delivery->dropped += delivery->pending.size(); delivery->pending.clear();
	}
	for (const auto& delivery : retired)
	{
		const auto port = delivery->livePort; if (!port) { continue; }
		try
		{
			const auto outputLock = port->lockOutput();
			// A prepared candidate may register models still owned by another
			// client. Its destruction must not detach or clean those processors.
			if (port->m_midiClient != this) { continue; }
			port->invalidateCilent(); const auto fallback = port->m_midiClient;
			const auto portsLock = std::lock_guard{fallback->m_portsMutex};
			fallback->m_inputDeliveries.push_back(delivery);
			const auto lock = std::lock_guard{delivery->mutex};
			delivery->admission = fallback->m_inputAdmission;
			delivery->observedGeneration = delivery->admission->generation.load();
			delivery->active = true; delivery->enabled = port->isInputEnabled();
			delivery->failed = false; delivery->requiresBackendObservation = false;
			delivery->reportFailure(InputDeliveryFailure::ClientRetired);
			delivery->retirementCleanupPending = delivery->needsNoteCleanup;
			delivery->failureDelivered = !delivery->retirementCleanupPending;
		}
		catch (...) { qWarning("MIDI input retirement could not register its fallback recovery state"); }
	}
	for (const auto& delivery : retired)
	{
		const auto port = delivery->livePort; if (!port) { continue; }
		bool cleanOnOwner = false;
		{
			const auto lock = std::lock_guard{delivery->mutex};
			if (!delivery->active) { continue; }
			delivery->transferPaused = false;
			if (!delivery->retirementCleanupPending) { continue; }
			cleanOnOwner = QThread::currentThread() == port->thread() && Engine::audioEngine()
				&& !delivery->processing && !delivery->recovering;
			if (!cleanOnOwner && !delivery->scheduled)
			{
				try { delivery->scheduled = QMetaObject::invokeMethod(port, [delivery] { drainInput(delivery); }, Qt::QueuedConnection); }
				catch (...) { delivery->scheduled = false; }
				if (!delivery->scheduled) { delivery->reportFailure(InputDeliveryFailure::Enqueue); }
			}
		}
		// No client/entry/output lock is held while entering native models/DSP.
		// A callback already in progress keeps the shared entry and handles the
		// failure after returning; never reenter that processor during retirement.
		if (cleanOnOwner)
		{
			try { drainInput(delivery); }
			catch (...) { qWarning("MIDI input retirement cleanup could not run on the owner"); }
		}
	}
}




void MidiClient::applyPortMode( MidiPort* )
{
}

MidiClient::BackendRoute MidiClient::setBackendRoute(bool, const QString&, bool, const std::function<bool()>&)
{
	throw std::runtime_error("The current MIDI backend does not expose client-wide graph routing");
}

MidiClient::BackendReconnect MidiClient::setBackendReconnect(bool, const QString&, bool)
{
	throw std::runtime_error("The current MIDI backend does not expose client-wide reconnect preferences");
}




void MidiClient::applyPortName( MidiPort* )
{
}




void MidiClient::addPort( MidiPort* port )
{
	const auto lock = std::lock_guard{m_portsMutex};
	if (!port || m_retiredForTransfer || (m_transferringPorts && m_preparingTransferPort != port))
	{ throw std::runtime_error("MIDI client is unavailable for new port registration during transfer"); }
	if (std::any_of(m_inputDeliveries.begin(), m_inputDeliveries.end(), [=](const auto& item) { return item->port == port; }))
	{ throw std::logic_error("MIDI port is already registered with this client"); }
	auto delivery = std::make_shared<InputDelivery>(port, m_inputAdmission); delivery->transferPaused = m_transferringPorts;
	m_inputDeliveries.push_back(std::move(delivery));
}

void MidiClient::validatePortForTransfer(const MidiPort* port) const
{
	if (!inputDeliveryStatus(port).registered) { throw std::runtime_error("Replacement MIDI client did not register the native port"); }
	if (!readyForPortTransfer()) { throw std::runtime_error("Replacement MIDI backend is unavailable"); }
}

bool MidiClient::readyForPortTransfer() const
{
	const auto backend = inputBackendStatus();
	return !backend.observed || backend.active;
}

void MidiClient::finishPortTransferOutput(const std::function<bool()>& cancelled)
{
	if (cancelled && cancelled()) { throw std::runtime_error("MIDI client transfer was cancelled"); }
}

void MidiClient::resumeTransferInput() noexcept
{
	try
	{
		const auto portsLock = std::lock_guard{m_portsMutex};
		for (const auto& delivery : m_inputDeliveries)
		{
			const auto lock = std::lock_guard{delivery->mutex}; delivery->transferPaused = false;
			if (!delivery->active || !delivery->enabled || !delivery->failed || delivery->scheduled || delivery->failureDelivered) { continue; }
			try { delivery->scheduled = QMetaObject::invokeMethod(delivery->port, [delivery] { drainInput(delivery); }, Qt::QueuedConnection); }
			catch (...) { delivery->scheduled = false; }
			if (!delivery->scheduled) { delivery->reportFailure(InputDeliveryFailure::Enqueue); }
		}
	}
	catch (...) { }
}

MidiClient::PortTransferResult MidiClient::transferPortsTo(MidiClient& replacement, const std::function<bool()>& cancelled, bool abandonFailedOutput)
{
	const auto engine = Engine::audioEngine();
	if (!engine || !engine->canTransferMidiClient(this) || !engine->audioDev() || engine->audioDev()->isRunning())
	{ throw std::runtime_error("MIDI client transfer requires stopped/disarmed owner admission and joined audio processing"); }
	if (&replacement == this) { throw std::invalid_argument("MIDI client replacement must be a different owned client"); }
	for (const auto client : {this, &replacement})
	{
		if (const auto object = dynamic_cast<QObject*>(client); object && object->thread() != QThread::currentThread())
		{ throw std::runtime_error("MIDI client transfer requires both backend owners on the engine thread"); }
	}
	FileResourceIO::EventSuspension events;
	struct Entry
	{
		QPointer<MidiPort> port;
		MidiPort* identity;
		std::shared_ptr<InputDelivery> input;
		MidiPort::Map readable, writable;
		MidiPort::OutputLock outputLock;
		bool attempted = false;
	};
	std::vector<Entry> entries;
	const auto abandonmentState = [&] {
		const auto state = outputBackendStatus();
		if (!state.observed || state.active || !state.abandonmentSupported || !state.abandonmentEligible)
		{ throw std::runtime_error("MIDI output abandonment requires a supported terminal backend whose output worker has stopped"); }
		return state;
	};
	const auto check = [&] {
		if (cancelled && cancelled()) { throw std::runtime_error("MIDI client transfer was cancelled"); }
		if (abandonFailedOutput) { abandonmentState(); }
		if (!engine->canTransferMidiClient(this) || engine->audioDev()->isRunning())
		{ throw std::runtime_error("Native engine admission changed during MIDI client transfer"); }
		for (const auto& item : entries)
		{
			if (!item.port || item.port->client() != this)
			{ throw std::runtime_error("A native MIDI port retired or changed client during transfer"); }
		}
	};
	check();
	if (!replacement.readyForPortTransfer()) { throw std::runtime_error("Replacement MIDI backend is unavailable"); }
	{
		const auto lock = std::scoped_lock{m_portsMutex, replacement.m_portsMutex};
		if (m_transferringPorts || m_retiredForTransfer || replacement.m_transferringPorts || replacement.m_retiredForTransfer
			|| !replacement.m_inputDeliveries.empty()) { throw std::runtime_error("MIDI client transfer requires an idle source and an empty replacement"); }
		entries.reserve(m_inputDeliveries.size());
		for (const auto& delivery : m_inputDeliveries)
		{
			const auto inputLock = std::lock_guard{delivery->mutex}; const auto port = delivery->port;
			if (!delivery->active || delivery->processing || delivery->recovering || !port || port->client() != this
				|| QThread::currentThread() != port->thread()) { throw std::runtime_error("MIDI input delivery or port ownership is busy"); }
			entries.push_back({port, port, delivery, {}, {}, {}, false});
		}
		m_transferringPorts = replacement.m_transferringPorts = true;
		for (const auto& item : entries)
		{
			const auto inputLock = std::lock_guard{item.input->mutex}; item.input->transferPaused = true;
			item.input->dropped += item.input->pending.size(); item.input->pending.clear();
			if (item.input->enabled) { item.input->reportFailure(InputDeliveryFailure::ClientTransfer); }
		}
	}
	bool committed = false;
	PortTransferResult result;
	const auto finish = qScopeGuard([&] {
		if (!committed)
		{
			// Even a throwing addPort can leave native/base registrations behind.
			// Always revoke queued delivery before invoking native cleanup.
			for (const auto& item : entries)
			{
				if (!item.attempted) { continue; }
				replacement.MidiClient::removePort(item.identity);
				try { replacement.removePort(item.identity); } catch (...) { }
				if (item.port) { if (const auto object = dynamic_cast<QObject*>(&replacement)) { QObject::disconnect(object, nullptr, item.port, nullptr); } }
			}
		}
		{
			const auto lock = std::scoped_lock{m_portsMutex, replacement.m_portsMutex};
			m_transferringPorts = replacement.m_transferringPorts = false;
			replacement.m_preparingTransferPort = nullptr;
			(committed ? m_retiredForTransfer : replacement.m_retiredForTransfer) = true;
		}
		for (auto& item : entries)
		{
			if (item.outputLock.owns_lock()) { item.outputLock.unlock(); }
			if (item.port) { const auto lock = item.port->lockOutput(); item.port->m_clientTransfer = false; }
		}
		(committed ? replacement : *this).resumeTransferInput();
	});
	for (const auto& item : entries) { const auto lock = item.port->lockOutput(); item.port->m_clientTransfer = true; }
	for (const auto& item : entries)
	{
		check(); if (!abandonFailedOutput) { item.port->releaseOutput(); } check();
		if (item.port->isInputEnabled() && !item.port->inputDeliveryFailed())
		{ throw std::runtime_error("Native MIDI input cleanup could not finish before transfer"); }
		check();
		{ const auto lock = std::lock_guard{item.input->mutex}; item.input->failureDelivered = true; }
	}
	if (!abandonFailedOutput) { finishPortTransferOutput(cancelled); } check();
	for (auto& item : entries)
	{
		check(); item.attempted = true;
		{
			{ const auto lock = std::lock_guard{replacement.m_portsMutex}; replacement.m_preparingTransferPort = item.identity; }
			const auto prepared = qScopeGuard([&] { const auto lock = std::lock_guard{replacement.m_portsMutex}; replacement.m_preparingTransferPort = nullptr; });
			replacement.addPort(item.identity);
		}
		check(); replacement.applyPortMode(item.identity); check(); replacement.applyPortName(item.identity); check();
		if (!replacement.isRaw())
		{
			const auto readable = replacement.readablePorts(); check();
			for (const auto& name : readable)
			{ check(); item.readable.insert(name, item.port->isInputEnabled() && item.port->m_requestedReadablePorts.contains(name)); }
			const auto writable = replacement.writablePorts(); check();
			for (const auto& name : writable)
			{ check(); item.writable.insert(name, item.port->isOutputEnabled() && item.port->m_requestedWritablePorts.contains(name)); }
			for (auto it = item.readable.cbegin(); it != item.readable.cend(); ++it)
			{ check(); if (it.value()) { replacement.subscribeReadablePort(item.identity, it.key(), true); } }
			for (auto it = item.writable.cbegin(); it != item.writable.cend(); ++it)
			{ check(); if (it.value()) { replacement.subscribeWritablePort(item.identity, it.key(), true); } }
			replacement.connectRPChanged(item.identity, SLOT(updateReadablePorts())); check();
			replacement.connectWPChanged(item.identity, SLOT(updateWritablePorts())); check();
		}
		replacement.validatePortForTransfer(item.identity); check();
	}
	for (const auto& item : entries) { replacement.validatePortForTransfer(item.identity); check(); }
	if (!replacement.readyForPortTransfer()) { throw std::runtime_error("Replacement MIDI backend became unavailable during transfer"); }
	check();
	if (abandonFailedOutput)
	{
		result.outputAbandonment.backend = abandonmentState();
		result.outputAbandonment.abandoned = true;
	}
	// No native/plugin/cancellation callbacks after this boundary until every
	// model pointer is committed. Acquire all fallible locks first.
	for (auto& item : entries) { item.outputLock = item.port->lockOutput(); }
	{
		const auto lock = std::scoped_lock{m_portsMutex, replacement.m_portsMutex};
		if (replacement.m_inputDeliveries.size() != entries.size()) { throw std::runtime_error("Replacement MIDI registrations changed during preparation"); }
		for (const auto& delivery : replacement.m_inputDeliveries)
		{
			const auto inputLock = std::lock_guard{delivery->mutex};
			if (delivery->enabled)
			{
				delivery->reportFailure(InputDeliveryFailure::ClientTransfer);
				// Local voices were already cleaned while both deliveries were
				// paused. Do not post a late cleanup against future playback.
				delivery->failureDelivered = true; ++result.inputResetsRequired;
			}
		}
		for (const auto& item : entries)
		{
			const auto inputLock = std::lock_guard{item.input->mutex}; item.input->active = false; item.input->pending.clear();
		}
		for (auto& item : entries)
		{
			if (abandonFailedOutput)
			{
				// Clear only at commit: failed preparation/cancellation must keep
				// the old ledgers. These counts describe uncertain local state,
				// not notes known to have reached or stopped at a physical device.
				result.outputAbandonment.trackedNotes += item.port->m_trackedOutputNotes;
				for (const auto& channel : item.port->m_outputPedals)
				{ for (const bool held : channel) { result.outputAbandonment.pedalHolds += held ? 1u : 0u; } }
				item.port->m_outputNotes = {}; item.port->m_outputPedals = {}; item.port->m_trackedOutputNotes = 0;
			}
			item.port->m_midiClient = &replacement;
			item.port->m_readablePorts.swap(item.readable); item.port->m_writablePorts.swap(item.writable);
		}
		m_inputDeliveries.clear(); m_retiredForTransfer = true; committed = true;
	}
	for (auto& item : entries) { item.outputLock.unlock(); }
	for (const auto& item : entries)
	{
		try { removePort(item.identity); } catch (...) { ++result.retirementExceptions; }
		if (!item.port) { continue; }
		if (const auto object = dynamic_cast<QObject*>(this)) { QObject::disconnect(object, nullptr, item.port, nullptr); }
		try
		{
			// The engine publishes its selected-client metadata before queued UI
			// notification runs. Receipt replay never re-enters this transfer.
			if (!QMetaObject::invokeMethod(item.port, [port = item.port, client = &replacement] {
				if (port && port->client() == client) { emit port->readablePortsChanged(); }
				if (port && port->client() == client) { emit port->writablePortsChanged(); }
				if (port && port->client() == client) { emit port->modeChanged(); }
			}, Qt::QueuedConnection)) { ++result.notificationFailures; }
		}
		catch (...) { ++result.notificationFailures; }
	}
	result.ports = static_cast<unsigned int>(entries.size()); return result;
}




void MidiClient::removePort( MidiPort* port )
{
	if( ! port )
	{
		return;
	}

	const auto portsLock = std::lock_guard{m_portsMutex};
	auto it = std::find_if(m_inputDeliveries.begin(), m_inputDeliveries.end(),
		[port](const auto& delivery) { return delivery->port == port; });
	if( it != m_inputDeliveries.end() )
	{
		const auto delivery = *it;
		const auto deliveryLock = std::lock_guard{delivery->mutex};
		delivery->active = false;
		delivery->pending.clear();
		m_inputDeliveries.erase(it);
	}
}

MidiClient::InputDeliveryStatus MidiClient::inputDeliveryStatus(const MidiPort* port) const
{
	const auto portsLock = std::lock_guard{m_portsMutex};
	for (const auto& delivery : m_inputDeliveries)
	{
		if (delivery->port != port) { continue; }
		const auto deliveryLock = std::lock_guard{delivery->mutex};
		return {static_cast<unsigned int>(delivery->pending.size()), delivery->dropped, delivery->failed, delivery->active, delivery->failure,
			delivery->lastFailure, delivery->failureVersion, delivery->recoveryCount, delivery->processing, delivery->recovering, delivery->requiresBackendObservation,
			delivery->retirementCleanupPending};
	}
	return {};
}

bool MidiClient::resetInputDelivery(MidiPort* port, QString& error, bool& changed)
{
	error.clear(); changed = false;
	if (!port || QThread::currentThread() != port->thread()) { error = "MIDI recovery requires the port's owning thread"; return false; }
	if (!hasQueuedInputDelivery()) { error = "Native MIDI client does not support queued input recovery"; return false; }
	const auto backend = inputBackendStatus();
	if (backend.observed && !backend.active) { error = "Native MIDI input backend is stopped; recreate the backend before recovery"; return false; }
	std::shared_ptr<InputDelivery> target;
	std::uint64_t version;
	bool requiresObservation = false;
	{
		const auto portsLock = std::lock_guard{m_portsMutex};
		for (const auto& delivery : m_inputDeliveries) { if (delivery->port == port) { target = delivery; break; } }
		if (!target) { error = "Native MIDI input port is not registered"; return false; }
		const auto lock = std::lock_guard{target->mutex};
		if (!target->active || !target->enabled) { error = "Native MIDI input port must be enabled and registered"; return false; }
		if (target->processing || target->recovering || target->transferPaused) { error = "Native MIDI input processing, recovery or transfer is already in progress"; return false; }
		if (!target->failed) { return true; }
		if (target->requiresBackendObservation && !backend.observed)
		{
			error = "Backend input health is unknown; recovery of a backend fault requires an observed active backend"; return false;
		}
		if (target->failureVersion == std::numeric_limits<std::uint64_t>::max() || target->recoveryCount == std::numeric_limits<std::uint64_t>::max())
		{
			error = "Native MIDI recovery counters are exhausted; recreate the port"; return false;
		}
		version = target->failureVersion; target->recovering = true; target->pending.clear();
		requiresObservation = target->requiresBackendObservation;
	}
	const QPointer<MidiPort> retainedPort(port);
	const auto finish = qScopeGuard([target] {
		const auto lock = std::lock_guard{target->mutex}; target->recovering = false;
		if (!target->active || (!target->enabled && !target->retirementCleanupPending) || !target->failed || target->scheduled || target->failureDelivered || target->transferPaused) { return; }
		try { target->scheduled = QMetaObject::invokeMethod(target->port, [target] { drainInput(target); }, Qt::QueuedConnection); }
		catch (...) { target->scheduled = false; }
		if (!target->scheduled) { target->reportFailure(InputDeliveryFailure::Enqueue); }
	});
	bool cleaned = false;
	try { cleaned = port->inputDeliveryFailed(); }
	catch (...)
	{
		const auto lock = std::lock_guard{target->mutex}; target->reportFailure(InputDeliveryFailure::Processor);
		error = "Native MIDI all-sound-off failed during recovery"; return false;
	}
	{
		const auto lock = std::lock_guard{target->mutex};
		if (!target->active || !target->enabled || !retainedPort || !cleaned)
		{
			error = "Native MIDI port or processor retired during recovery"; return false;
		}
		if (target->failureVersion != version) { error = "Another native MIDI input failure occurred during recovery"; return false; }
	}
	// The callback/model cleanup may pump events. Verify that the client and backend still exist.
	if (retainedPort->client() != this) { error = "Native MIDI client retired during recovery"; return false; }
	const auto currentBackend = inputBackendStatus();
	if (requiresObservation && !currentBackend.observed) { error = "Native MIDI backend health became unknown during recovery"; return false; }
	if (currentBackend.observed && !currentBackend.active) { error = "Native MIDI input backend stopped during recovery"; return false; }
	{
		const auto lock = std::lock_guard{target->mutex};
		if (!target->active || !target->enabled || target->failureVersion != version)
		{
			error = "Native MIDI input changed while recovery was being completed"; return false;
		}
		target->failed = false; target->failure = InputDeliveryFailure::None; target->failureDelivered = false; target->requiresBackendObservation = false;
		target->needsNoteCleanup = false; target->retirementCleanupPending = false;
		++target->recoveryCount; changed = true;
	}
	return true;
}

void MidiClient::cancelInputEvents(MidiPort* port, const QString& sourceName)
{
	const auto portsLock = std::lock_guard{m_portsMutex};
	for (const auto& delivery : m_inputDeliveries)
	{
		if (delivery->port != port) { continue; }
		const auto deliveryLock = std::lock_guard{delivery->mutex};
		delivery->pending.erase(std::remove_if(delivery->pending.begin(), delivery->pending.end(),
			[&sourceName](const auto& packet) { return packet.event.sourcePortName() == sourceName; }), delivery->pending.end());
	}
}

void MidiClient::setInputDeliveryEnabled(MidiPort* port, bool enabled)
{
	const auto portsLock = std::lock_guard{m_portsMutex};
	for (const auto& delivery : m_inputDeliveries)
	{
		if (delivery->port != port) { continue; }
		const auto deliveryLock = std::lock_guard{delivery->mutex};
		delivery->enabled = enabled;
		delivery->pending.clear();
	}
}

void MidiClient::dispatchInputEvent(const MidiEvent& event, const TimePos& time,
	MidiPort* destination, const QString& sourceName)
{
	// SysEx/meta data can borrow storage. These backends currently deliver scalar events only.
	std::uint8_t bytes[3]{};
	if (event.type() == MidiSystemReset)
	{
		if (event.metaEvent() != MidiMetaInvalid) { return; }
	}
	else if (!writeToByteSeq(event, bytes, sizeof(bytes))) { return; }
	MidiEvent owned(event.type(), event.channel(), event.param(0), event.param(1), nullptr, event.source());
	owned.setSourcePortName(sourceName);
	const auto inputGeneration = m_inputAdmission->generation.load(std::memory_order_acquire);
	const auto portsLock = std::lock_guard{m_portsMutex};
	for (const auto& delivery : m_inputDeliveries)
	{
		if (destination && delivery->port != destination) { continue; }
		const auto deliveryLock = std::lock_guard{delivery->mutex};
		if (!delivery->active || !delivery->enabled) { continue; }
		const auto generation = delivery->discardStaleExternal();
		if (owned.source() == MidiEvent::Source::External && ((generation & 1) || inputGeneration != generation))
		{ ++delivery->dropped; continue; }
		if (delivery->recovering || delivery->transferPaused) { ++delivery->dropped; continue; }
		if (delivery->failed) { ++delivery->dropped; continue; }
		if (delivery->pending.size() == InputQueueLimit)
		{
			delivery->dropped += delivery->pending.size() + 1;
			delivery->pending.clear();
			delivery->reportFailure(InputDeliveryFailure::QueueOverflow);
			continue;
		}
		const auto previousPending = delivery->pending.size();
		try
		{
			delivery->pending.push_back({owned, time, inputGeneration});
			if (delivery->scheduled) { continue; }
			// Registration is held until posting finishes. MidiPort unregisters before destruction.
			delivery->scheduled = QMetaObject::invokeMethod(delivery->port,
				[delivery] { drainInput(delivery); }, Qt::QueuedConnection);
			if (delivery->scheduled) { continue; }
		}
		catch (...) { }
		delivery->dropped += previousPending + 1;
		delivery->pending.clear();
		delivery->reportFailure(InputDeliveryFailure::Enqueue);
	}
}

void MidiClient::failInputEvents(InputDeliveryFailure failure, MidiPort* destination) noexcept
{
	try
	{
		const auto portsLock = std::lock_guard{m_portsMutex};
		for (const auto& delivery : m_inputDeliveries)
		{
			if (destination && delivery->port != destination) { continue; }
			const auto lock = std::lock_guard{delivery->mutex};
			if (!delivery->active || !delivery->enabled) { continue; }
			delivery->dropped += delivery->pending.size(); delivery->pending.clear();
			delivery->reportFailure(failure);
			if (!delivery->scheduled && !delivery->recovering && !delivery->transferPaused)
			{
				delivery->scheduled = QMetaObject::invokeMethod(delivery->port,
					[delivery] { drainInput(delivery); }, Qt::QueuedConnection);
			}
		}
	}
	catch (...) { } // A backend's C callback must never unwind through the driver.
}

void MidiClient::drainInput(const std::shared_ptr<InputDelivery>& delivery)
{
	{
		const auto lock = std::lock_guard{delivery->mutex};
		if (!delivery->active || delivery->processing) { return; }
		if (delivery->recovering || delivery->transferPaused) { delivery->scheduled = false; return; }
		delivery->processing = true;
	}
	bool reposted = false;
	const auto finish = qScopeGuard([&] {
		const auto lock = std::lock_guard{delivery->mutex}; delivery->processing = false;
		if (reposted) { return; }
		if (!delivery->active || delivery->recovering || delivery->transferPaused || (delivery->failed ? delivery->failureDelivered : delivery->pending.empty()))
		{
			delivery->scheduled = false; return;
		}
		// Intake may have arrived between the loop's empty check and this epilogue.
		try { delivery->scheduled = QMetaObject::invokeMethod(delivery->port, [delivery] { drainInput(delivery); }, Qt::QueuedConnection); }
		catch (...) { delivery->scheduled = false; }
		if (!delivery->scheduled)
		{
			delivery->dropped += delivery->pending.size(); delivery->pending.clear(); delivery->reportFailure(InputDeliveryFailure::Enqueue);
		}
	});
	// At most one queued callback per registered port, and a bounded batch per event-loop turn.
	for (unsigned int count = 0; count < 64; ++count)
	{
		InputDelivery::Packet packet;
		std::shared_ptr<InputAdmission> admission;
		bool failed;
		std::uint64_t failureVersion = 0;
		{
			const auto lock = std::lock_guard{delivery->mutex};
			if (!delivery->active || delivery->transferPaused) { return; }
			delivery->discardStaleExternal();
			failed = delivery->failed;
			if (failed)
			{
				if (delivery->failureDelivered) { return; }
				delivery->failureDelivered = true;
				failureVersion = delivery->failureVersion;
			}
			else
			{
				if (delivery->pending.empty()) { return; }
				packet = std::move(delivery->pending.front());
				delivery->pending.pop_front();
				admission = delivery->admission;
			}
		}
		// No registry/backend lock is held while the processor enters native models/DSP.
		try
		{
			const auto changes = Engine::audioEngine()->requestChangesGuard();
			if (failed)
			{
				const bool cleaned = delivery->port->inputDeliveryFailed();
				const auto lock = std::lock_guard{delivery->mutex};
				if (!cleaned) { delivery->reportFailure(InputDeliveryFailure::Processor); }
				else if (delivery->failureVersion == failureVersion)
				{ delivery->needsNoteCleanup = false; delivery->retirementCleanupPending = false; }
				return;
			}
			const auto generation = admission->generation.load(std::memory_order_acquire);
			if (packet.event.source() == MidiEvent::Source::External && ((generation & 1) || packet.inputGeneration != generation))
			{ const auto lock = std::lock_guard{delivery->mutex}; ++delivery->dropped; continue; }
			if ((packet.event.type() == MidiNoteOn && packet.event.velocity() > 0)
				|| (packet.event.type() == MidiControlChange && packet.event.controllerValue() >= 64
					&& (packet.event.controllerNumber() == MidiControllerSustain || packet.event.controllerNumber() == MidiControllerSostenuto)))
			{ const auto lock = std::lock_guard{delivery->mutex}; delivery->needsNoteCleanup = true; }
			delivery->port->processInEvent(packet.event, packet.time);
		}
		catch (...)
		{
			const auto lock = std::lock_guard{delivery->mutex};
			delivery->dropped += delivery->pending.size() + (failed ? 0 : 1); delivery->pending.clear();
			delivery->reportFailure(InputDeliveryFailure::Processor);
			if (failed) { return; }
		}
	}
	const auto lock = std::lock_guard{delivery->mutex};
	if (!delivery->active) { return; }
	if (delivery->pending.empty() && !delivery->failed) { return; }
	try
	{
		if (QMetaObject::invokeMethod(delivery->port, [delivery] { drainInput(delivery); }, Qt::QueuedConnection)) { reposted = true; return; }
	}
	catch (...) { }
	delivery->dropped += delivery->pending.size(); delivery->pending.clear();
	delivery->scheduled = false; delivery->reportFailure(InputDeliveryFailure::Enqueue);
}




void MidiClient::subscribeReadablePort( MidiPort*, const QString& , bool )
{
}




void MidiClient::subscribeWritablePort( MidiPort* , const QString& , bool )
{
}







void MidiClientRaw::parseData( const unsigned char c )
{
	/*********************************************************************/
	/* 'Process' system real-time messages                               */
	/*********************************************************************/
	/* There are not too many real-time messages that are of interest here.
	 * They can occur anywhere, even in the middle of a noteon message!
	 * Real-time range: 0xF8 .. 0xFF
	 * Note: Real-time does not affect (running) status.
	 */
	if( c >= 0xF8 )
	{
		if( c == MidiSystemReset )
		{
			m_midiParseData.m_midiEvent.setType( MidiSystemReset );
			m_midiParseData.m_status = 0;
			processParsedEvent(m_midiParseData.m_midiEvent);
		}
		return;
	}

	/*********************************************************************/
	/* 'Process' system common messages (again, just skip them)          */
	/*********************************************************************/
	/* There are no system common messages that are of interest here.
	 * System common range: 0xF0 .. 0xF7
	 */
	if( c >= 0xF0 )
	{
	/* MIDI spec say: To ignore a non-real-time message, just discard all
	 * data up to the next status byte.  And our parser will ignore data
	 * that is received without a valid status.
	 * Note: system common cancels running status. */
		m_midiParseData.m_status = 0;
		m_midiParseData.m_bytes = 0; m_midiParseData.m_bytesTotal = 0;
		return;
	}

	/*********************************************************************/
	/* Process voice category messages:                                  */
	/*********************************************************************/
	/* Now that we have handled realtime and system common messages, only
	 * voice messages are left.
	 * Only a status byte has bit # 7 set.
	 * So no matter the status of the parser (in case we have lost sync),
	 * as soon as a byte >= 0x80 comes in, we are dealing with a status byte
	 * and start a new event.
	 */
	if( c & 0x80 )
	{
		m_midiParseData.m_channel = c & 0x0F;
		m_midiParseData.m_status = c & 0xF0;
		/* The event consumes x bytes of data...
					(subtract 1 for the status byte) */
		m_midiParseData.m_bytesTotal = eventLength( m_midiParseData.m_status ) - 1;
		/* of which we have read 0 at this time. */
		m_midiParseData.m_bytes = 0;
		return;
	}

	/*********************************************************************/
	/* Process data                                                      */
	/*********************************************************************/
	/* If we made it this far, then the received char belongs to the data
	 * of the last event. */
	if( m_midiParseData.m_status == 0 )
	{
		/* We are not interested in the event currently received.
							Discard the data. */
		return;
	}

	/* Store the first couple of bytes */
	if( m_midiParseData.m_bytes < RAW_MIDI_PARSE_BUF_SIZE )
	{
		m_midiParseData.m_buffer[m_midiParseData.m_bytes] = c;
	}
	++m_midiParseData.m_bytes;

	/* Do we still need more data to get this event complete? */
	if( m_midiParseData.m_bytes < m_midiParseData.m_bytesTotal )
	{
		return;
	}

	/*********************************************************************/
	/* Send the event                                                    */
	/*********************************************************************/
	/* The event is ready-to-go.  About 'running status':
	 *
	 * The MIDI protocol has a built-in compression mechanism. If several
	 * similar events are sent in-a-row, for example note-ons, then the
	 * event type is only sent once. For this case, the last event type
	 * (status) is remembered.
	 * We simply keep the status as it is, just reset the parameter counter.
	 * If another status byte comes in, it will overwrite the status.
	 */
	m_midiParseData.m_midiEvent.setType(static_cast<MidiEventTypes>(m_midiParseData.m_status));
	m_midiParseData.m_midiEvent.setChannel(m_midiParseData.m_channel);
	m_midiParseData.m_bytes = 0; /* Related to running status! */
	switch (m_midiParseData.m_midiEvent.type())
	{
		case MidiNoteOff:
		case MidiNoteOn:
		case MidiKeyPressure:
			m_midiParseData.m_midiEvent.setKey(m_midiParseData.m_buffer[0]);
			m_midiParseData.m_midiEvent.setVelocity(m_midiParseData.m_buffer[1]);
			break;

		case MidiChannelPressure:
		case MidiProgramChange:
			m_midiParseData.m_midiEvent.setParam(0, m_midiParseData.m_buffer[0]);
			m_midiParseData.m_midiEvent.setParam(1, 0);
			break;

		case MidiControlChange:
			m_midiParseData.m_midiEvent.setControllerNumber(m_midiParseData.m_buffer[0]);
			m_midiParseData.m_midiEvent.setControllerValue( m_midiParseData.m_buffer[1]);
			break;

		case MidiPitchBend:
			// Pitch-bend is transmitted with 14-bit precision.
			// Note: '|' does here the same as '+' (no common bits),
			// but might be faster
			m_midiParseData.m_midiEvent.setPitchBend((m_midiParseData.m_buffer[1] * 128) | m_midiParseData.m_buffer[0]);
			break;

		default:
			// Unlikely
			return;
	}

	processParsedEvent(m_midiParseData.m_midiEvent);
}




void MidiClientRaw::processParsedEvent(const MidiEvent& event)
{
	try { dispatchInputEvent(event, TimePos{}); }
	catch (...) { failInputEvents(); }
}




void MidiClientRaw::processOutEvent(const MidiEvent& event, const TimePos&, const MidiPort* port)
{
	// TODO: also evaluate _time and queue event if necessary
	if (event.channel() < 0 || event.channel() >= MidiChannelCount) { return; }
	const auto first = event.param(0); const auto second = event.param(1);
	switch (event.type())
	{
		case MidiNoteOn:
		case MidiNoteOff:
		case MidiKeyPressure:
		case MidiControlChange:
			if (first < 0 || first > 127 || second < 0 || second > 127) { return; }
			break;
		case MidiProgramChange:
		case MidiChannelPressure:
			if (first < 0 || first > 127) { return; }
			break;
		case MidiPitchBend:
			if (first < 0 || first > MidiMaxPitchBend) { return; }
			break;

		default:
			qWarning("MidiClientRaw: unhandled MIDI-event %d\n", (int)event.type());
			return;
	}
	std::array<std::uint8_t, 3> bytes{};
	const auto count = writeToByteSeq(event, bytes.data(), bytes.size());
	const std::lock_guard<std::recursive_mutex> guard(m_outputMutex);
	for (std::size_t index = 0; index < count; ++index) { sendByte(bytes[index]); }
}






// Taken from Nagano Daisuke's USB-MIDI driver
static const auto REMAINS_F0F6 = std::array<unsigned char, 7>
{
	0,	/* 0xF0 */
	2,	/* 0XF1 */
	3,	/* 0XF2 */
	2,	/* 0XF3 */
	2,	/* 0XF4 (Undefined by MIDI Spec, and subject to change) */
	2,	/* 0XF5 (Undefined by MIDI Spec, and subject to change) */
	1	/* 0XF6 */
} ;

static const auto REMAINS_80E0 = std::array<unsigned char, 7>
{
	3,	/* 0x8X Note Off */
	3,	/* 0x9X Note On */
	3,	/* 0xAX Poly-key pressure */
	3,	/* 0xBX Control Change */
	2,	/* 0xCX Program Change */
	2,	/* 0xDX Channel pressure */
	3 	/* 0xEX PitchBend Change */
} ;



// Returns the length of the MIDI message starting with _event.
// Taken from Nagano Daisuke's USB-MIDI driver
int MidiClientRaw::eventLength( const unsigned char event )
{
	if ( event < 0xF0 )
	{
		return REMAINS_80E0[( ( event - 0x80 ) >> 4 ) & 0x0F];
	}
	else if ( event < 0xF7 )
	{
		return REMAINS_F0F6[event - 0xF0];
	}
	return 1;
}

} // namespace lmms
