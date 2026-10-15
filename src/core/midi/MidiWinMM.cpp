/*
 * MidiWinMM.cpp - WinMM MIDI client
 *
 * Copyright (c) 2008-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "MidiWinMM.h"
#include "MidiEventToByteSeq.h"

#ifdef LMMS_HAVE_WINMM
#include "WinMmMidiApi.h"
#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <QScopeGuard>


namespace lmms
{

namespace
{
const detail::WinMmMidiApi nativeApi;
struct InputRegistration { MidiWinMM* client; HMIDIIN handle; bool closed = false; };
struct CallbackRegistry
{
	std::mutex mutex;
	std::map<DWORD_PTR, InputRegistration> inputs;
	DWORD_PTR next = 1;
};
CallbackRegistry& callbackRegistry()
{
	// The driver may retain an opaque instance value after a failed close.
	static auto* registry = new CallbackRegistry;
	return *registry;
}

// Register ownership before opening a native device. Retirement therefore
// needs no allocation, and failed close cannot discard the last handle owner.
// Owner generations cannot alias a new client allocated at the same address.
struct NativeHandle
{
	NativeHandle(const detail::WinMmMidiApi& calls, std::uint64_t owner, bool isInput)
		: api(calls), ownerId(owner), input(isInput) { }
	const detail::WinMmMidiApi api;
	const std::uint64_t ownerId;
	const bool input;
	std::uint64_t id = 0;
	HMIDIIN inputHandle = nullptr;
	HMIDIOUT outputHandle = nullptr;
	bool retired = false, serviced = false, resetInput = false, resetOutput = false;
	std::uint16_t cleanupChannels = 0;
	MMRESULT error = MMSYSERR_NOERROR;
};
struct NativeOwnership
{
	std::mutex mutex, serviceMutex;
	std::vector<std::shared_ptr<NativeHandle>> handles;
	std::uint64_t next = 1;
	std::atomic<bool> retrying{false};
	std::atomic<int> lastError{0};
};
NativeOwnership& nativeOwnership()
{
	// Failed close can leave a driver-owned resource live beyond Engine teardown.
	// Keep its call table and revoked callback identity until a later retry.
	static auto* ownership = new NativeOwnership;
	return *ownership;
}
std::uint64_t nextNativeOwner()
{
	auto& state = nativeOwnership(); const auto lock = std::lock_guard{state.mutex};
	if (state.next == std::numeric_limits<std::uint64_t>::max()) { throw std::runtime_error("WinMM native ownership identifiers exhausted"); }
	return state.next++;
}
std::shared_ptr<NativeHandle> prepareNativeHandle(const detail::WinMmMidiApi& api, std::uint64_t owner, bool input)
{
	auto handle = std::make_shared<NativeHandle>(api, owner, input);
	auto& state = nativeOwnership(); const auto lock = std::lock_guard{state.mutex};
	if (state.next == std::numeric_limits<std::uint64_t>::max()) { throw std::runtime_error("WinMM native ownership identifiers exhausted"); }
	handle->id = state.next++; state.handles.push_back(handle); return handle;
}
void markNativeRetired(const std::shared_ptr<NativeHandle>& handle, std::uint16_t channels = 0)
{
	const auto lock = std::lock_guard{nativeOwnership().mutex};
	handle->cleanupChannels |= channels; handle->retired = true;
}
void markNativeRetired(std::uint64_t owner, HMIDIIN input, HMIDIOUT output, std::uint16_t channels = 0, MMRESULT error = MMSYSERR_NOERROR)
{
	auto& state = nativeOwnership(); const auto lock = std::lock_guard{state.mutex};
	for (const auto& handle : state.handles)
	{
		if (handle->ownerId == owner && !handle->retired
			&& ((input && handle->input && handle->inputHandle == input) || (output && !handle->input && handle->outputHandle == output)))
		{
			handle->cleanupChannels |= channels; handle->retired = true; handle->error = error;
			if (error != MMSYSERR_NOERROR) { state.lastError.store(static_cast<int>(error), std::memory_order_relaxed); }
			return;
		}
	}
}
bool hasRetiredHandles(const detail::WinMmMidiApi& api)
{
	auto& state = nativeOwnership(); const auto lock = std::lock_guard{state.mutex};
	return std::any_of(state.handles.begin(), state.handles.end(), [&](const auto& handle) { return handle->retired && handle->api == api; });
}
struct NativeRetirementResult { MMRESULT input = MMSYSERR_NOERROR, output = MMSYSERR_NOERROR; };
NativeRetirementResult serviceNativeRetirement(const detail::WinMmMidiApi& api, std::uint64_t owner = 0) noexcept
{
	NativeRetirementResult result;
	try
	{
		auto& state = nativeOwnership(); const auto service = std::unique_lock{state.serviceMutex, std::try_to_lock};
		if (!service.owns_lock()) { return result; }
		state.retrying.store(true, std::memory_order_relaxed);
		const auto finish = qScopeGuard([&] { state.retrying.store(false, std::memory_order_relaxed); });
		std::uint64_t through = 0, after = 0;
		{ const auto lock = std::lock_guard{state.mutex}; through = state.next - 1; }
		for (;;)
		{
			std::shared_ptr<NativeHandle> handle;
			bool retry = false, resetInput = false, resetOutput = false;
			std::uint16_t cleanup = 0;
			{
				const auto lock = std::lock_guard{state.mutex};
				const auto found = std::find_if(state.handles.begin(), state.handles.end(), [&](const auto& item) {
					return item->id > after && item->id <= through && item->retired && item->api == api && (!owner || item->ownerId == owner);
				});
				if (found == state.handles.end()) { break; }
				handle = *found; after = handle->id;
				retry = handle->serviced; handle->serviced = true;
				resetInput = handle->resetInput; resetOutput = handle->resetOutput; cleanup = handle->cleanupChannels;
			}
			const auto invoke = [&](auto operation) {
				MMRESULT code; try { code = operation(); } catch (...) { code = MMSYSERR_ERROR; }
				if (code != MMSYSERR_NOERROR)
				{
					state.lastError.store(static_cast<int>(code), std::memory_order_relaxed);
					(handle->input ? result.input : result.output) = code;
				}
				return code;
			};
			MMRESULT closed = MMSYSERR_NOERROR;
			// Neither registry mutex is held during a native call. Revoked input
			// tokens stay revoked during reset/close and any synchronous loopback.
			if (handle->input && handle->inputHandle)
			{
				if (resetInput) { invoke([&] { return handle->api.inputReset(handle->inputHandle); }); }
				closed = invoke([&] { return handle->api.inputClose(handle->inputHandle); });
			}
			else if (!handle->input && handle->outputHandle)
			{
				if (resetOutput) { invoke([&] { return handle->api.outputReset(handle->outputHandle); }); }
				if (retry)
				{
					for (int channel = 0; channel < MidiChannelCount; ++channel)
					{
						if (!(cleanup & (1u << channel))) { continue; }
						bool released = true;
						for (const int controller : {MidiControllerAllSoundOff, MidiControllerSustain, MidiControllerSostenuto, MidiControllerAllNotesOff})
						{
							const DWORD message = static_cast<DWORD>(0xb0 | channel | (controller << 8));
							if (invoke([&] { return handle->api.outputShortMessage(handle->outputHandle, message); }) != MMSYSERR_NOERROR) { released = false; }
						}
						if (released) { const auto lock = std::lock_guard{state.mutex}; handle->cleanupChannels &= ~(1u << channel); }
					}
				}
				closed = invoke([&] { return handle->api.outputClose(handle->outputHandle); });
			}
			{
				const auto lock = std::lock_guard{state.mutex};
				handle->error = closed;
				if (closed == MMSYSERR_NOERROR || closed == MMSYSERR_INVALHANDLE)
				{
					// Never retry a handle the native API confirms closed or invalid;
					// its numeric value could subsequently belong to another device.
					std::erase(state.handles, handle);
				}
				else if (!handle->input) { handle->resetOutput = closed == MIDIERR_STILLPLAYING; }
			}
		}
	}
	catch (...)
	{
		// Existing records still own every unresolved handle if bookkeeping
		// cannot proceed. Destruction must not throw through native callbacks.
		nativeOwnership().lastError.store(MMSYSERR_ERROR, std::memory_order_relaxed);
		result.input = result.output = MMSYSERR_ERROR;
	}
	return result;
}
}

MidiClient::NativeRetirementStatus MidiWinMM::retiredHandleStatus()
{
	auto& state = nativeOwnership(); const auto lock = std::lock_guard{state.mutex};
	NativeRetirementStatus result; result.observationAvailable = true;
	result.retryOnBackendSelection = name(); result.retrying = state.retrying.load(std::memory_order_relaxed);
	result.lastNativeErrorCode = state.lastError.load(std::memory_order_relaxed);
	std::uint16_t channels = 0;
	for (const auto& handle : state.handles)
	{
		if (!handle->retired) { continue; }
		if (handle->input && handle->inputHandle) { ++result.pendingInputHandles; }
		if (!handle->input && handle->outputHandle) { ++result.pendingOutputHandles; channels |= handle->cleanupChannels; }
		if (!result.nativeErrorCode && handle->error) { result.nativeErrorCode = static_cast<int>(handle->error); }
	}
	for (int channel = 0; channel < MidiChannelCount; ++channel) { if (channels & (1u << channel)) { ++result.cleanupPendingChannels; } }
	return result;
}

DWORD_PTR MidiWinMM::registerInput(HMIDIIN handle)
{
	auto& registry = callbackRegistry(); const auto lock = std::lock_guard{registry.mutex};
	if (registry.next == std::numeric_limits<DWORD_PTR>::max()) { recordError(MMSYSERR_NOMEM); return 0; }
	const auto token = registry.next++; registry.inputs.emplace(token, InputRegistration{this, handle}); return token;
}

void MidiWinMM::removeInputToken(DWORD_PTR token)
{
	auto& registry = callbackRegistry(); const auto lock = std::lock_guard{registry.mutex}; registry.inputs.erase(token);
}

void MidiWinMM::recordError(MMRESULT result) noexcept
{
	if (result == MMSYSERR_NOERROR) { return; }
	int expected = 0; m_nativeError.compare_exchange_strong(expected, static_cast<int>(result), std::memory_order_relaxed);
}

void MidiWinMM::recordOutputError(MMRESULT result) noexcept
{
	if (result != MMSYSERR_NOERROR) { m_lastOutputError.store(static_cast<int>(result), std::memory_order_relaxed); }
}

MidiClient::OutputBackendStatus MidiWinMM::outputBackendStatus() const
{
	const auto lock = std::lock_guard{m_devicesMutex};
	const int last = m_lastOutputError.load(std::memory_order_relaxed); int current = m_outputDevices.isEmpty() ? last : 0;
	std::uint16_t pending = 0;
	for (const auto& output : m_outputDevices)
	{
		pending |= output.cleanupChannels;
		if (!current && output.result != MMSYSERR_NOERROR) { current = static_cast<int>(output.result); }
	}
	unsigned int channels = 0;
	for (int channel = 0; channel < MidiChannelCount; ++channel) { if (pending & (1u << channel)) { ++channels; } }
	return {true, !m_closing && !m_outputDevices.isEmpty() && !current && !channels, 0, 0, channels, current, last};
}

void MidiWinMM::retainFailedOutputCleanup(const MidiPort* port, std::uint16_t channels) noexcept
{
	if (!channels) { return; }
	try
	{
		const auto outputLock = std::lock_guard{m_outputMutex};
		const auto lock = std::lock_guard{m_devicesMutex};
		for (auto output = m_outputDevices.begin(); output != m_outputDevices.end(); ++output)
		{
			const auto subscribed = m_outputSubs.constFind(output.value().name);
			if (subscribed == m_outputSubs.cend()) { continue; }
			for (const auto identity : subscribed.value())
			{
				if (identity == port) { output.value().cleanupChannels |= channels; break; }
			}
		}
	}
	catch (...) { recordOutputError(MMSYSERR_ERROR); }
}

MMRESULT MidiWinMM::submitShortMessage(HMIDIOUT handle, DWORD message) noexcept
{
	MMRESULT result;
	try { result = m_api.outputShortMessage(handle, message); }
	catch (...) { result = MMSYSERR_ERROR; }
	recordOutputError(result); return result;
}

MMRESULT MidiWinMM::flushOutputCleanup(HMIDIOUT handle, const std::function<bool()>& cancelled)
{
	std::uint16_t pending = 0;
	{
		const auto lock = std::lock_guard{m_devicesMutex};
		const auto output = m_outputDevices.constFind(handle);
		if (output != m_outputDevices.cend()) { pending = output.value().cleanupChannels; }
	}
	if (!pending) { return MMSYSERR_NOERROR; }
	MMRESULT firstFailure = MMSYSERR_NOERROR;
	for (int channel = 0; channel < MidiChannelCount; ++channel)
	{
		const auto bit = static_cast<std::uint16_t>(1u << channel); if (!(pending & bit)) { continue; }
		bool released = true;
		for (const auto controller : {MidiControllerAllSoundOff, MidiControllerSustain, MidiControllerSostenuto, MidiControllerAllNotesOff})
		{
			if (cancelled && cancelled()) { throw std::runtime_error("WinMM output cleanup was cancelled"); }
			const auto message = static_cast<DWORD>(MidiControlChange | channel) | (static_cast<DWORD>(controller) << 8);
			const auto result = submitShortMessage(handle, message);
			if (result != MMSYSERR_NOERROR)
			{
				released = false; if (firstFailure == MMSYSERR_NOERROR) { firstFailure = result; }
				const auto lock = std::lock_guard{m_devicesMutex};
				if (const auto output = m_outputDevices.find(handle); output != m_outputDevices.end()) { output.value().result = firstFailure; }
			}
		}
		if (released)
		{
			const auto lock = std::lock_guard{m_devicesMutex};
			if (const auto output = m_outputDevices.find(handle); output != m_outputDevices.end())
			{
				output.value().cleanupChannels &= ~bit;
				output.value().usedChannels &= ~bit;
			}
		}
	}
	{
		const auto lock = std::lock_guard{m_devicesMutex};
		if (const auto output = m_outputDevices.find(handle); output != m_outputDevices.end()) { output.value().result = firstFailure; }
	}
	return firstFailure;
}

void MidiWinMM::flushAllOutputCleanup(const std::function<bool()>& cancelled)
{
	QList<HMIDIOUT> handles;
	{
		const auto lock = std::lock_guard{m_devicesMutex};
		for (auto output = m_outputDevices.cbegin(); output != m_outputDevices.cend(); ++output)
		{ if (output.value().cleanupChannels) { handles.push_back(output.key()); } }
	}
	MMRESULT firstFailure = MMSYSERR_NOERROR;
	for (const auto handle : handles)
	{
		if (cancelled && cancelled()) { throw std::runtime_error("WinMM output cleanup was cancelled"); }
		const auto result = flushOutputCleanup(handle, cancelled);
		if (result != MMSYSERR_NOERROR && firstFailure == MMSYSERR_NOERROR) { firstFailure = result; }
	}
	if (firstFailure != MMSYSERR_NOERROR)
	{ throw std::runtime_error(QString("Retired WinMM output cleanup failed (MMRESULT %1)").arg(firstFailure).toStdString()); }
}

void MidiWinMM::finishPortTransferOutput(const std::function<bool()>& cancelled)
{
	if (cancelled && cancelled()) { throw std::runtime_error("WinMM output cleanup was cancelled"); }
	const auto outputLock = std::lock_guard{m_outputMutex};
	{
		const auto lock = std::lock_guard{m_devicesMutex};
		// Complete possible live-source cleanup before the ownership commit,
		// so final close need not introduce a new cleanup failure after handoff.
		for (auto output = m_outputDevices.begin(); output != m_outputDevices.end(); ++output)
		{ output.value().cleanupChannels |= output.value().usedChannels; }
	}
	flushAllOutputCleanup(cancelled);
}

MidiClient::InputBackendStatus MidiWinMM::inputBackendStatus() const
{
	const auto lock = std::lock_guard{m_devicesMutex};
	return {true, !m_closing && !m_startedInputs.isEmpty(), m_nativeError.load(std::memory_order_relaxed)};
}

bool MidiWinMM::isAvailable() const
{
	const auto lock = std::lock_guard{m_devicesMutex}; return !m_closing && (!m_startedInputs.isEmpty() || !m_outputDevices.isEmpty());
}

bool MidiWinMM::readyForPortTransfer() const { return isAvailable(); }

MidiWinMM::MidiWinMM() : MidiWinMM(true) { }

MidiWinMM::MidiWinMM(bool openPhysicalDevices) : MidiWinMM(nativeApi, openPhysicalDevices) { }

MidiWinMM::MidiWinMM(const detail::WinMmMidiApi& api, bool openPhysicalDevices) :
	MidiClient(),
	m_api(api),
	m_inputDevices(),
	m_outputDevices(),
	m_inputSubs(),
	m_outputSubs()
{
	m_nativeOwnerId = nextNativeOwner();
	if (openPhysicalDevices)
	{
		try { openDevices(); }
		catch (...) { recordError(MMSYSERR_ERROR); closeDevices(); }
	}
}




MidiWinMM::~MidiWinMM()
{
	closeDevices();
}




void MidiWinMM::processOutEvent( const MidiEvent& event, const TimePos& time, const MidiPort* port )
{
	std::uint8_t bytes[3]{};
	if (!writeToByteSeq(event, bytes, sizeof(bytes))) { return; }
	const DWORD shortMsg = static_cast<DWORD>(bytes[0]) | (static_cast<DWORD>(bytes[1]) << 8) | (static_cast<DWORD>(bytes[2]) << 16);
	const auto outputLock = std::lock_guard{m_outputMutex};
	QList<HMIDIOUT> handles;
	{
		const auto lock = std::lock_guard{m_devicesMutex};
		if (m_closing) { throw std::runtime_error("Native WinMM MIDI output is closed"); }
		QStringList outDevs;
		for (auto it = m_outputSubs.cbegin(); it != m_outputSubs.cend(); ++it)
		{
			for (MidiPort* subscribed : it.value())
			{
				if (subscribed == port) { outDevs += it.key(); break; }
			}
		}
		for (auto it = m_outputDevices.cbegin(); it != m_outputDevices.cend(); ++it)
		{
			if (outDevs.contains(it.value().name)) { handles.push_back(it.key()); }
		}
	}
	// A loopback driver may deliver input while output is sent. Only output lifetime is held here.
	MMRESULT firstFailure = MMSYSERR_NOERROR;
	for (const auto handle : handles)
	{
		// Retired source cleanup must precede new music on this native endpoint.
		auto result = flushOutputCleanup(handle, {});
		if (result == MMSYSERR_NOERROR)
		{
			const bool activation = (event.type() == MidiNoteOn && event.velocity() > 0)
				|| (event.type() == MidiControlChange && event.controllerValue() >= 64
					&& (event.controllerNumber() == MidiControllerSustain || event.controllerNumber() == MidiControllerSostenuto));
			if (activation)
			{
				// Reserve before the API call: a failed/throwing activation may
				// already have reached hardware, and its port can outlive this client.
				const auto lock = std::lock_guard{m_devicesMutex};
				if (const auto output = m_outputDevices.find(handle); output != m_outputDevices.end())
				{ output.value().usedChannels |= static_cast<std::uint16_t>(1u << event.channel()); }
			}
			result = submitShortMessage(handle, shortMsg);
		}
		{
			const auto lock = std::lock_guard{m_devicesMutex};
			if (const auto output = m_outputDevices.find(handle); output != m_outputDevices.end()) { output.value().result = result; }
		}
		if (result != MMSYSERR_NOERROR && firstFailure == MMSYSERR_NOERROR) { firstFailure = result; }
	}
	// Attempt every subscribed output, but retain the port's uncertain notes
	// and pedals if any native destination failed. A retry may clear its error.
	if (firstFailure != MMSYSERR_NOERROR)
	{ throw std::runtime_error(QString("Native WinMM MIDI output failed (MMRESULT %1)").arg(firstFailure).toStdString()); }
}




void MidiWinMM::applyPortMode( MidiPort* port )
{
	const auto lock = std::lock_guard{m_devicesMutex};
	// make sure no subscriptions exist which are not possible with
	// current port-mode
	if( !port->isInputEnabled() )
	{
		for( SubMap::Iterator it = m_inputSubs.begin(); it != m_inputSubs.end(); ++it )
		{
			it.value().removeAll( port );
		}
	}

	if( !port->isOutputEnabled() )
	{
		for( SubMap::Iterator it = m_outputSubs.begin(); it != m_outputSubs.end(); ++it )
		{
			it.value().removeAll( port );
		}
	}
}




void MidiWinMM::removePort( MidiPort* port )
{
	const auto lock = std::lock_guard{m_devicesMutex};
	for( SubMap::Iterator it = m_inputSubs.begin(); it != m_inputSubs.end(); ++it )
	{
		it.value().removeAll( port );
	}

	for( SubMap::Iterator it = m_outputSubs.begin(); it != m_outputSubs.end(); ++it )
	{
		it.value().removeAll( port );
	}

	MidiClient::removePort( port );
}




QString MidiWinMM::sourcePortName( const MidiEvent& event ) const
{
	if (!event.sourcePortName().isEmpty()) { return event.sourcePortName(); }
	const auto lock = std::lock_guard{m_devicesMutex};
	if( event.sourcePort() )
	{
		return m_inputDevices.value( *static_cast<const HMIDIIN *>( event.sourcePort() ) );
	}

	return MidiClient::sourcePortName( event );
}




void MidiWinMM::subscribeReadablePort( MidiPort* port, const QString& dest, bool subscribe )
{
	const auto lock = std::lock_guard{m_devicesMutex};
	if( subscribe && port->isInputEnabled() == false )
	{
		qWarning( "port %s can't be (un)subscribed!\n", port->displayName().toLatin1().constData() );
		return;
	}

	m_inputSubs[dest].removeAll( port );
	if( subscribe )
	{
		m_inputSubs[dest].push_back( port );
	}
}




void MidiWinMM::subscribeWritablePort( MidiPort* port, const QString& dest, bool subscribe )
{
	const auto lock = std::lock_guard{m_devicesMutex};
	if( subscribe && port->isOutputEnabled() == false )
	{
		qWarning( "port %s can't be (un)subscribed!\n", port->displayName().toLatin1().constData() );
		return;
	}

	m_outputSubs[dest].removeAll( port );
	if( subscribe )
	{
		m_outputSubs[dest].push_back( port );
	}
}




void WINAPI CALLBACK MidiWinMM::inputCallback( HMIDIIN hm, UINT msg, DWORD_PTR inst, DWORD_PTR param1, DWORD_PTR param2 ) noexcept
{
	try
	{
		auto& registry = callbackRegistry(); const auto lock = std::lock_guard{registry.mutex};
		const auto found = registry.inputs.find(inst);
		if (found == registry.inputs.end() || found->second.handle != hm) { return; }
		auto client = found->second.client;
		if (found->second.closed) { return; }
		if (msg == MIM_CLOSE) { found->second.closed = true; }
		try
		{
			if (msg == MIM_DATA || msg == MIM_MOREDATA) { client->handleInputEvent(hm, static_cast<DWORD>(param1)); }
			else if (msg == MIM_ERROR || msg == MIM_LONGERROR || msg == MIM_CLOSE) { client->handleInputFailure(hm, msg == MIM_CLOSE); }
		}
		catch (...) { client->recordError(MMSYSERR_ERROR); client->failInputEvents(); }
	}
	catch (...) { } // Never unwind through the driver's C callback.
}

void MidiWinMM::handleInputFailure(HMIDIIN handle, bool closed)
{
	const auto lock = std::lock_guard{m_devicesMutex};
	const auto name = m_inputDevices.value(handle);
	if (name.isEmpty()) { return; }
	if (closed)
	{
		m_startedInputs.remove(handle); m_inputDevices.remove(handle);
		QMetaObject::invokeMethod(this, [this] { emit readablePortsChanged(); }, Qt::QueuedConnection);
	}
	for (MidiPort* port : m_inputSubs.value(name)) { failInputEvents(InputDeliveryFailure::BackendInput, port); }
}




void MidiWinMM::handleInputEvent( HMIDIIN hm, DWORD ev )
{
	const int cmd = ev & 0xff;
	if( cmd == MidiActiveSensing )
	{
		return;
	}
	const int par1 = ( ev >> 8 ) & 0xff;
	const int par2 = (ev >> 16) & 0xff;
	const MidiEventTypes cmdtype = static_cast<MidiEventTypes>( cmd & 0xf0 );
	const int chan = cmd & 0x0f;

	const auto lock = std::lock_guard{m_devicesMutex};
	const QString d = m_inputDevices.value( hm );
	if( d.isEmpty() || !m_inputSubs.contains( d ) )
	{
		return;
	}

	MidiEvent event;
	switch (cmdtype)
	{
		case MidiNoteOn: case MidiNoteOff: case MidiKeyPressure: case MidiControlChange:
			event = MidiEvent(cmdtype, chan, par1, par2); break;
		case MidiProgramChange: case MidiChannelPressure:
			event = MidiEvent(cmdtype, chan, par1, 0); break;
		case MidiPitchBend:
			if (par1 > 127 || par2 > 127) { return; }
			event = MidiEvent(cmdtype, chan, par1 + par2 * 128, 0); break;
		default: return;
	}
	std::uint8_t bytes[3]{};
	if (!writeToByteSeq(event, bytes, sizeof(bytes))) { return; }
	// The callback's HMIDIIN local is never retained by an event.
	for (MidiPort* port : m_inputSubs.value(d))
	{
		dispatchInputEvent(event, TimePos{}, port, d);
	}
}




void MidiWinMM::updateDeviceList()
{
	closeDevices();
	openDevices();

	emit readablePortsChanged();
	emit writablePortsChanged();
}



void MidiWinMM::closeDevices()
{
	const auto outputLock = std::lock_guard{m_outputMutex};
	{
		const auto lock = std::lock_guard{m_devicesMutex};
		m_closing = true; m_startedInputs.clear();
		for (auto output = m_outputDevices.begin(); output != m_outputDevices.end(); ++output)
		{ output.value().cleanupChannels |= output.value().usedChannels; }
	}
	{
		auto& registry = callbackRegistry(); const auto lock = std::lock_guard{registry.mutex};
		for (auto it = registry.inputs.begin(); it != registry.inputs.end();)
		{
			if (it->second.client == this) { it = registry.inputs.erase(it); } else { ++it; }
		}
	}
	// A device refresh can keep the client and its ports alive. Do not let
	// queued events from the old registrations cross that close/reopen boundary.
	failInputEvents(InputDeliveryFailure::BackendInput);
	// Tokens are already revoked, so cleanup loopback cannot reach retiring input.
	// Include possible live-port activations before closing; recovery is best effort.
	try { flushAllOutputCleanup({}); }
	catch (...) { qWarning("Retired WinMM output cleanup failed before native close"); }
	QMap<HMIDIIN, QString> inputs;
	QMap<HMIDIOUT, OutputDevice> outputs;
	{
		const auto lock = std::lock_guard{m_devicesMutex};
		m_inputSubs.clear(); m_outputSubs.clear();
		m_startedInputs.clear();
		inputs.swap(m_inputDevices); outputs.swap(m_outputDevices);
	}
	for (auto input = inputs.cbegin(); input != inputs.cend(); ++input) { markNativeRetired(m_nativeOwnerId, input.key(), nullptr); }
	for (auto output = outputs.cbegin(); output != outputs.cend(); ++output)
	{ markNativeRetired(m_nativeOwnerId, nullptr, output.key(), output.value().cleanupChannels, output.value().result); }
	// Ownership was registered before open. Failed close keeps that record and
	// its copied native call table after the client and its port maps are gone.
	const auto retired = serviceNativeRetirement(m_api, m_nativeOwnerId);
	recordError(retired.input); recordOutputError(retired.output);
}




void MidiWinMM::openDevices()
{
	const auto retired = serviceNativeRetirement(m_api);
	recordError(retired.input); recordOutputError(retired.output);
	if (hasRetiredHandles(m_api) || nativeOwnership().retrying.load(std::memory_order_relaxed))
	{
		const auto lock = std::lock_guard{m_devicesMutex}; m_closing = true;
		return; // No duplicate native opens while any previous ownership is unresolved.
	}
	{ const auto lock = std::lock_guard{m_devicesMutex}; m_closing = false; }
	for( unsigned int i = 0; i < m_api.inputCount(); ++i )
	{
		if (hasRetiredHandles(m_api)) { closeDevices(); return; }
		MIDIINCAPSW c{};
		const auto capabilities = m_api.inputCapabilities(i, &c, sizeof(c)); recordError(capabilities);
		if (capabilities != MMSYSERR_NOERROR) { continue; }
		const auto token = registerInput(nullptr); if (!token) { continue; }
		auto revoke = qScopeGuard([&] { removeInputToken(token); });
		const auto handle = prepareNativeHandle(m_api, m_nativeOwnerId, true); auto& hm = handle->inputHandle;
		bool published = false;
		const auto cleanup = qScopeGuard([&] {
			if (published) { return; }
			removeInputToken(token);
			markNativeRetired(handle); const auto result = serviceNativeRetirement(m_api, m_nativeOwnerId);
			recordError(result.input); recordOutputError(result.output);
		});
		MMRESULT res = m_api.inputOpen(&hm, i, (DWORD_PTR)&inputCallback, token, CALLBACK_FUNCTION | MIDI_IO_STATUS);
		if (res == MMSYSERR_NOERROR && !hm) { res = MMSYSERR_INVALHANDLE; }
		recordError(res);
		if( res == MMSYSERR_NOERROR )
		{
			{ auto& registry = callbackRegistry(); const auto lock = std::lock_guard{registry.mutex}; registry.inputs.at(token).handle = hm; }
			{
				const auto lock = std::lock_guard{m_devicesMutex};
				m_inputDevices[hm] = QString::fromWCharArray(c.szPname); published = true;
			}
			handle->resetInput = true; // Also reset if a native start unwinds after partial activation.
			const auto started = m_api.inputStart(hm); recordError(started);
			if (started != MMSYSERR_NOERROR)
			{
				handle->resetInput = false;
				removeInputToken(token);
				{ const auto lock = std::lock_guard{m_devicesMutex}; m_inputDevices.remove(hm); }
				markNativeRetired(handle); const auto result = serviceNativeRetirement(m_api, m_nativeOwnerId);
				recordError(result.input); recordOutputError(result.output);
				published = true; // The retirement registry now owns cleanup.
				if (hasRetiredHandles(m_api)) { closeDevices(); return; }
			}
			else
			{
				auto& registry = callbackRegistry(); const auto callbackLock = std::lock_guard{registry.mutex};
				const auto registered = registry.inputs.find(token);
				if (registered != registry.inputs.end() && !registered->second.closed)
				{
					const auto lock = std::lock_guard{m_devicesMutex};
					m_startedInputs.insert(hm);
				}
				revoke.dismiss();
			}
		}
	}

	for( unsigned int i = 0; i < m_api.outputCount(); ++i )
	{
		if (hasRetiredHandles(m_api)) { closeDevices(); return; }
		MIDIOUTCAPSW c{};
		const auto capabilities = m_api.outputCapabilities(i, &c, sizeof(c)); recordOutputError(capabilities);
		if (capabilities != MMSYSERR_NOERROR) { continue; }
		const auto handle = prepareNativeHandle(m_api, m_nativeOwnerId, false); auto& hm = handle->outputHandle;
		bool published = false;
		const auto cleanup = qScopeGuard([&] {
			if (!published)
			{
				markNativeRetired(handle); const auto result = serviceNativeRetirement(m_api, m_nativeOwnerId);
				recordError(result.input); recordOutputError(result.output);
			}
		});
		MMRESULT res = m_api.outputOpen(&hm, i, 0, 0, CALLBACK_NULL);
		if (res == MMSYSERR_NOERROR && !hm) { res = MMSYSERR_INVALHANDLE; }
		recordOutputError(res);
		if( res == MMSYSERR_NOERROR )
		{
			const auto lock = std::lock_guard{m_devicesMutex};
			m_outputDevices[hm] = {QString::fromWCharArray(c.szPname), MMSYSERR_NOERROR};
			published = true;
		}
	}
	if (hasRetiredHandles(m_api)) { closeDevices(); }
}


} // namespace lmms

#endif // LMMS_HAVE_WINMM
