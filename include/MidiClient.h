/*
 * MidiClient.h - base-class for MIDI clients like ALSA-sequencer-client
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

#ifndef LMMS_MIDI_CLIENT_H
#define LMMS_MIDI_CLIENT_H

#include <QStringList>
#include <vector>
#include <mutex>
#include <memory>
#include <cstdint>
#include <functional>


#include "MidiEvent.h"

class QObject;


namespace lmms
{

class MidiPort;
class TimePos;
class AudioEngine;


// base-class for all MIDI-clients
class MidiClient
{
public:
	MidiClient();
	virtual ~MidiClient();

	// to be implemented by sub-classes
	virtual void processOutEvent( const MidiEvent & _me,
						const TimePos & _time,
						const MidiPort * _port ) = 0;

	// Called before note cleanup or an output route retires. Scheduled backends
	// must discard this port's pending events and release possibly delivered notes
	// whose queued releases were discarded. Throw on failure to preserve routing.
	// Immediate backends have no application-owned schedule to cancel.
	virtual void cancelOutputEvents(const MidiPort*) { }
	// A retiring port cannot retain its ledger after a failed release. Backends
	// that support deferred cleanup must copy channel state without retaining or
	// dereferencing the port after this call. Existing queued backends own their
	// cancellation state already; immediate backends may opt into this hook.
	virtual void retainFailedOutputCleanup(const MidiPort*, std::uint16_t) noexcept { }
	struct OutputClockStatus
	{
		bool observed = false;
		bool configurationVerified = false;
		int ticksPerQuarterNote = 0;
		unsigned int microsecondsPerQuarterNote = 0;
		int nativeErrorCode = 0;
	};
	// Last verified native queue configuration, not a hardware latency measurement.
	virtual OutputClockStatus outputClockStatus() const { return {}; }
	struct OutputBackendStatus
	{
		bool observed = false;
		bool active = false;
		unsigned int pendingEvents = 0;
		unsigned int queueLimit = 0;
		unsigned int cleanupPendingChannels = 0;
		int nativeErrorCode = 0;
		int lastNativeErrorCode = 0;
		// Eligible only when terminal output cannot resume or submit any more
		// bytes before this client is destroyed. Temporary backpressure is not
		// eligible. The default keeps unobserved/unsupported backends protected.
		bool abandonmentSupported = false;
		bool abandonmentEligible = false;
	};
	virtual OutputBackendStatus outputBackendStatus() const { return {}; }
	struct NativeRetirementStatus
	{
		bool observationAvailable = false, retrying = false;
		unsigned int pendingInputHandles = 0, pendingOutputHandles = 0, cleanupPendingChannels = 0;
		int nativeErrorCode = 0, lastNativeErrorCode = 0;
		QString retryOnBackendSelection;
	};
	// Process-local native ownership can outlive the selected client. Discovery
	// observes it without retrying driver calls or changing backend selection.
	static NativeRetirementStatus nativeRetirementStatus();
	struct OutputAbandonment
	{
		bool abandoned = false;
		std::uint64_t trackedNotes = 0, pedalHolds = 0;
		OutputBackendStatus backend;
	};
	struct BackendGraph
	{
		struct Endpoint
		{
			QString name;
			bool connected = false, available = true, reconnect = false, pending = false;
			int nativeErrorCode = 0;
		};
		bool supported = false, observed = false;
		bool reconnectSupported = false, reconnectDeferred = false;
		QString localPort;
		std::vector<Endpoint> endpoints;
		int nativeErrorCode = 0;
	};
	struct BackendRoute
	{
		QString localPort, remotePort;
		bool connected = false, changed = false;
		bool reconnect = false, reconnectChanged = false;
	};
	struct BackendReconnect
	{
		QString remotePort;
		bool enabled = false, changed = false;
	};
	// Client-wide native graph routing, separate from per-model subscriptions.
	virtual BackendGraph inspectBackendGraph(bool) { return {}; }
	virtual BackendRoute setBackendRoute(bool input, const QString& remote, bool enabled,
		const std::function<bool()>& cancelled = {});
	// Retain an already connected exact endpoint, or forget its process-local
	// recovery preference without changing the graph, including while offline.
	virtual BackendReconnect setBackendReconnect(bool input, const QString& remote, bool enabled);

	// inheriting classes can re-implement this for being able to update
	// their internal port-structures etc.
	virtual void applyPortMode( MidiPort * _port );
	virtual void applyPortName( MidiPort * _port );

	virtual void addPort( MidiPort * _port );

	// re-implemented methods HAVE to call removePort() of base-class!!
	virtual void removePort( MidiPort * _port );


	// returns whether client works with raw-MIDI, only needs to be
	// re-implemented by MidiClientRaw for returning true
	virtual bool isRaw() const
	{
		return false;
	}

	// if not raw-client, return all readable/writable ports
	virtual QStringList readablePorts() const
	{
		return QStringList();
	}
	virtual QStringList writablePorts() const
	{
		return QStringList();
	}

	// return name of port which specified MIDI event came from
	virtual QString sourcePortName( const MidiEvent& event ) const
	{
		return event.sourcePortName();
	}

	static constexpr unsigned int InputQueueLimit = 1024;
	enum class InputDeliveryFailure { None, QueueOverflow, Enqueue, Processor, BackendCallback, BackendInput, ClientTransfer, ClientRetired };
	struct InputBackendStatus
	{
		bool observed = false;
		bool active = false;
		int nativeErrorCode = 0;
		// Native byte-stream loss, separate from the application's event queue.
		// Zero is not evidence of loss-free input without a valid observation.
		bool overrunObservationAvailable = false;
		std::uint64_t reportedOverrunBytes = 0;
	};
	virtual InputBackendStatus inputBackendStatus() const { return {}; }
	struct InputDeliveryStatus
	{
		unsigned int pendingEvents = 0;
		std::uint64_t droppedEvents = 0;
		bool failed = false;
		bool registered = false;
		InputDeliveryFailure failure = InputDeliveryFailure::None;
		InputDeliveryFailure lastFailure = InputDeliveryFailure::None;
		std::uint64_t failureVersion = 0;
		std::uint64_t recoveryCount = 0;
		bool processing = false;
		bool recovering = false;
		bool requiresBackendObservation = false;
		// A retired client's possible local activations still need owner-thread
		// cleanup. False does not imply a physical backend was reconnected.
		bool retirementCleanupPending = false;
	};
	InputDeliveryStatus inputDeliveryStatus(const MidiPort* port) const;
	bool resetInputDelivery(MidiPort* port, QString& error, bool& changed);
	virtual bool hasQueuedInputDelivery() const { return false; }
	// Called on the port's thread when input routing changes or its processor retires.
	void cancelInputEvents(MidiPort* port, const QString& sourceName);
	void setInputDeliveryEnabled(MidiPort* port, bool enabled);


	// (un)subscribe given MidiPort to/from destination-port
	virtual void subscribeReadablePort( MidiPort * _port,
						const QString & _dest,
						bool _subscribe = true );
	virtual void subscribeWritablePort( MidiPort * _port,
						const QString & _dest,
						bool _subscribe = true );

	// qobject-derived classes can use this for make a slot being
	// connected to signal of non-raw-MIDI-client if port-lists change
	virtual void connectRPChanged( QObject *, const char * )
	{
	}

	virtual void connectWPChanged( QObject *, const char * )
	{
	}

	// tries to open either MIDI-driver from config-file or (if it fails)
	// any other working
	static MidiClient * openMidiClient();

protected:
	friend class MidiClientTestAccess;
	// A replacement prepares native per-model resources before publishing any
	// new client pointers. Immediate backends complete output cleanup in their
	// existing cancellation calls; scheduled backends must join submission here.
	virtual void validatePortForTransfer(const MidiPort* port) const;
	virtual bool readyForPortTransfer() const;
	virtual void finishPortTransferOutput(const std::function<bool()>& cancelled);
	// Copies supported scalar events and source identity; never enters DSP on a backend thread.
	void dispatchInputEvent(const MidiEvent& event, const TimePos& time,
		MidiPort* destination = nullptr, const QString& sourceName = {});
	void failInputEvents(InputDeliveryFailure failure = InputDeliveryFailure::BackendCallback, MidiPort* destination = nullptr) noexcept;

private:
	friend class AudioEngine;
	friend class MidiClientTransferTestAccess;
	struct PortTransferResult
	{
		unsigned int ports = 0, inputResetsRequired = 0, retirementExceptions = 0, notificationFailures = 0;
		OutputAbandonment outputAbandonment;
	};
	// Owner-only, stopped/disarmed native engine with DSP already joined.
	// Replacement must be empty. On success retire this client before returning
	// to the event loop. Once preparation begins, failure requires discarding
	// the replacement. Neither client may be deleted by cancellation/processor
	// callbacks during this call. Retirement counters count thrown exceptions;
	// zero does not prove native handle closure or physical MIDI delivery.
	PortTransferResult transferPortsTo(MidiClient& replacement, const std::function<bool()>& cancelled = {}, bool abandonFailedOutput = false);
	struct InputDelivery;
	struct InputAdmission;
	void setExternalInputBlocked(bool blocked) noexcept;
	std::shared_ptr<InputAdmission> m_inputAdmission;
	static void drainInput(const std::shared_ptr<InputDelivery>& delivery);
	void resumeTransferInput() noexcept;
	mutable std::mutex m_portsMutex;
	std::vector<std::shared_ptr<InputDelivery>> m_inputDeliveries;
	bool m_transferringPorts = false, m_retiredForTransfer = false;
	MidiPort* m_preparingTransferPort = nullptr;

} ;




const uint32_t RAW_MIDI_PARSE_BUF_SIZE = 16;


class MidiClientRaw : public MidiClient
{
public:
	MidiClientRaw() = default;
	~MidiClientRaw() override = default;
	bool hasQueuedInputDelivery() const override { return true; }

	// we are raw-clients for sure!
	bool isRaw() const override
	{
		return true;
	}


protected:
	// generic raw-MIDI-parser which generates appropriate MIDI-events
	void parseData( const unsigned char c );
	// Call only after the backend has joined readers of the old byte stream.
	void resetRawParser() { m_midiParseData = {}; }

	// to be implemented by actual client-implementation
	virtual void sendByte( const unsigned char c ) = 0;
	// Named native backends may reuse framing while retaining a separate stream per source.
	virtual void processParsedEvent(const MidiEvent& event);


private:
	void processOutEvent( const MidiEvent& event, const TimePos& time, const MidiPort* port ) override;

	// small helper function returning length of a certain event - this
	// is necessary for parsing raw-MIDI-data
	static int eventLength( const unsigned char event );


	// data being used for parsing
	struct midiParserData
	{
		uint8_t m_status;		// identifies the type of event, that
					// is currently received ('Noteon',
					// 'Pitch Bend' etc).
		uint8_t m_channel;	// The channel of the event that is
					// received (in case of a channel event)
		uint32_t m_bytes;		// How many bytes have been read for
					// the current event?
		uint32_t m_bytesTotal;	// How many bytes does the current
					// event type include?
		uint32_t m_buffer[RAW_MIDI_PARSE_BUF_SIZE];
					// buffer for incoming data
		MidiEvent m_midiEvent;	// midi-event
	} m_midiParseData{};
	std::recursive_mutex m_outputMutex;

} ;

} // namespace lmms

#endif // LMMS_MIDI_CLIENT_H
