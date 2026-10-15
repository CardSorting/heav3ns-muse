/*
 * MidiJack.h - MIDI client for Jack
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

#ifndef LMMS_MIDI_JACK_H
#define LMMS_MIDI_JACK_H

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_JACK
#ifndef LMMS_HAVE_WEAKJACK
#include <jack/jack.h>
#include <jack/midiport.h>
#else
#include <weak_libjack.h>
#endif

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QByteArray>
#include <atomic>
#include <memory>
#include <array>
#include <mutex>
#include <unordered_map>

#include "MidiClient.h"


namespace lmms
{

class AudioJack;
namespace detail { struct JackApi; class JackCallbackGate; }

class MidiJack : public QObject, public MidiClientRaw
{
        Q_OBJECT
public:
	MidiJack();
	~MidiJack() override;
	bool isAvailable() const;
	InputBackendStatus inputBackendStatus() const override;
	OutputBackendStatus outputBackendStatus() const override;
	BackendGraph inspectBackendGraph(bool input) override;
	BackendRoute setBackendRoute(bool input, const QString& remote, bool enabled,
		const std::function<bool()>& cancelled = {}) override;
	BackendReconnect setBackendReconnect(bool input, const QString& remote, bool enabled) override;
	void processOutEvent(const MidiEvent& event, const TimePos& time, const MidiPort* port) override;
	void cancelOutputEvents(const MidiPort* port) override;
	void addPort(MidiPort* port) override;
	void removePort(MidiPort* port) override;
	static constexpr unsigned int OutputQueueLimit = 1024;

	jack_client_t* jackClient();
	const AudioJack* sharedAudioDevice() const { return m_jackAudio; }

	static QString probeDevice();

	inline static QString name()
	{
		return( QT_TRANSLATE_NOOP( "MidiSetupWidget",
			"Jack-MIDI" ) );
	}

	void JackMidiWrite(jack_nframes_t nframes, bool processing = true);
	void JackMidiRead(jack_nframes_t nframes);
	// JACK's shutdown context only publishes lock-free state.
	void notifyServerShutdown() noexcept;


	inline static QString configSection()
	{
		return "MidiJack";
	}


protected:
	void sendByte( const unsigned char c ) override;


private:
	friend class AudioJack;
	friend class MidiJackTestAccess;
	MidiJack(AudioJack* audio, const detail::JackApi& api);
	bool openStandalone();
	void closeStandalone();
	bool prepareShared(AudioJack* audio, jack_client_t* client);
	bool releaseShared(bool unregister);
	void audioDeviceRetired();
	void pollHealth();
	void inputFailed(int error) noexcept;
	bool prepareOutput(jack_client_t* client);
	void resetOutput();
	void discardOutputLocked();
	void suspendOutput();
	void drainOutputForRouting(const std::function<bool()>& cancelled);
	void finishPortTransferOutput(const std::function<bool()>& cancelled) override;
	bool readyForPortTransfer() const override { return isAvailable(); }
	bool graphConnected(jack_port_t* local, const QByteArray& remote);
	void requireGraphPeer(bool input, const QString& remote);
	void requireGraphOwner() const;
	bool canRecoverRoutes() const;
	void recoverRetainedRoute();
	bool forgetRetainedRoute(bool input, const QString& remote);
	void failOutputLocked(int error);
	void cancelOutputLocked(const MidiPort* port);
	static int processCallback(jack_nframes_t frames, void* data) noexcept;
	static void shutdownCallback(void* data) noexcept;

	const detail::JackApi& m_api;
	const QByteArray m_clientName;
	AudioJack* m_jackAudio = nullptr;
	jack_client_t* m_jackClient = nullptr;
	jack_port_t* m_inputPort = nullptr;
	jack_port_t* m_outputPort = nullptr;
	jack_client_t* m_outputClient = nullptr;
	jack_nframes_t m_outputRate = 0;
	bool m_nativeActive = false;
	bool m_recovering = false;
	bool m_closing = false;
	std::unique_ptr<detail::JackCallbackGate> m_callbackGate;
	std::unique_ptr<detail::JackCallbackGate> m_outputGate;
	struct OutputMessage
	{
		const MidiPort* source = nullptr; // identity only; the callback never dereferences it
		std::array<unsigned char, 3> bytes{};
		std::size_t size = 0;
		jack_nframes_t due = 0;
		std::uint64_t sequence = 0;
	};
	mutable std::mutex m_outputMutex;
	std::array<OutputMessage, OutputQueueLimit> m_outputQueue{};
	std::size_t m_outputCount = 0;
	std::uint64_t m_outputSequence = 0;
	std::uint64_t m_outputGeneration = 0;
	std::uint64_t m_outputCycle = 0;
	std::uint64_t m_cleanupSubmittedCycle = 0;
	std::unordered_map<const MidiPort*, std::uint16_t> m_outputChannels;
	std::uint64_t m_cleanup = 0; // four release controllers per channel
	int m_outputError = 0, m_lastOutputError = 0;
	bool m_outputSuspended = false;
	bool m_reconnectCleanup = false; // under m_outputMutex; reject new notes until releases are submitted
	std::atomic<int> m_asyncOutputError{0};
	std::atomic_bool m_graphChanging{false};
	bool m_graphBusy = false; // owner thread only
	int m_graphError = 0;
	struct RetainedRoute
	{
		bool input;
		QString remote;
		QElapsedTimer retry;
		int nativeErrorCode = 0;
	};
	// Owner-thread state only. One route per health tick, at least 1 s between
	// attempts for the same route. Exact names do not identify physical devices.
	std::vector<RetainedRoute> m_retainedRoutes;
	std::size_t m_retainedCursor = 0;
	QTimer m_healthTimer;
	QElapsedTimer m_retryClock;
	std::atomic_bool m_available{false}, m_disconnected{false}, m_pendingFailure{false};
	std::atomic<int> m_nativeError{0};

signals:
	void serverDisconnected();

};


} // namespace lmms

#endif // LMMS_HAVE_JACK

#endif // LMMS_MIDI_JACK_H
