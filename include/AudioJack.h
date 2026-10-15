/*
 * AudioJack.h - support for JACK-transport
 *
 * Copyright (c) 2005-2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_AUDIO_JACK_H
#define LMMS_AUDIO_JACK_H

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_JACK
#ifndef LMMS_HAVE_WEAKJACK
#include <jack/jack.h>
#else
#include <weak_libjack.h>
#endif

#include <atomic>
#include <memory>
#include <QByteArray>
#include <QTimer>
#include <QElapsedTimer>
#include <vector>

#include "AudioDevice.h"
#include "AudioDeviceSetupWidget.h"

class QLineEdit;
class QMenu;
class QToolButton;

namespace lmms
{

class MidiJack;
namespace detail { class JackCallbackGate; class JackAudioBuses; struct JackBusSnapshot; struct JackApi; }


class AudioJack : public QObject, public AudioDevice
{
	Q_OBJECT
public:
	AudioJack(bool& successful, AudioEngine* audioEngine);
	~AudioJack() override;

	// this is to allow the jack midi connection to use the same jack client connection
	// the jack callback is handled here, we call the midi client so that it can read
	// it's midi data during the callback
	AudioJack* addMidiClient(MidiJack* midiClient);
	void removeMidiClient(MidiJack* midiClient);
	jack_client_t* jackClient() { return m_client; };
	bool serverDisconnected() const { return m_serverDisconnected.load(std::memory_order_acquire); }
	BackendGraph inspectBackendGraph(bool input) override;
	BackendRoute setBackendRoute(bool input, unsigned int channel, const QString& remote, bool enabled,
		const std::function<bool()>& cancelled = {}) override;
	BusGraph inspectBusGraph(AudioBusHandle* bus) override;
	void configureBusOutput(AudioBusHandle* bus, bool enabled, const std::function<bool()>& cancelled = {}) override;
	BackendRoute setBusRoute(AudioBusHandle* bus, unsigned int channel, const QString& remote, bool enabled,
		const std::function<bool()>& cancelled = {}) override;

	inline static QString name()
	{
		return QT_TRANSLATE_NOOP("AudioDeviceSetupWidget", "JACK (JACK Audio Connection Kit)");
	}

	class setupWidget : public gui::AudioDeviceSetupWidget
	{
	public:
		setupWidget(QWidget* parent);
		void saveSettings() override;

	private:
		std::vector<std::string> getAudioPortNames(JackPortFlags portFlags) const;
		std::vector<std::string> getAudioInputNames() const;
		std::vector<std::string> getAudioOutputNames() const;
		static QMenu* buildMenu(QToolButton* toolButton, const std::vector<std::string>& names, const QString& filteredLMMSClientName);

	private:
		QLineEdit* m_clientName;
		// Because we do not have access to a JackAudio driver instance we have to be our own client to display inputs and outputs...
		jack_client_t* m_client;

		std::vector<QToolButton*> m_outputDevices;
		std::vector<QToolButton*> m_inputDevices;
	};

private:
	friend class AudioJackTestAccess;
	friend class MidiJack;
	friend class detail::JackAudioBuses;
	AudioJack(bool& successful, AudioEngine* engine, const detail::JackApi& api);
	bool initJackClient(bool preserveRate = false);
	bool reopenJackClient();
	void closeJackClient();
	void pollServerHealth();
	bool recoverMidiClient(MidiJack* midiClient);
	void retireFailedMidiPorts();
	void resizeInputBuffer(jack_nframes_t nframes);
	void requireGraphOwner() const;
	bool graphConnected(jack_port_t* local, const QByteArray& remote);
	BackendGraph inspectGraphPorts(bool input, const std::vector<jack_port_t*>& ports, bool startup);
	BackendRoute setGraphPortRoute(bool input, jack_port_t* local, const QString& remote, bool enabled,
		const std::function<bool()>& cancelled);
	bool isProjectBus(const AudioBusHandle* bus) const;

	void startProcessingImpl() override;
	void stopProcessingImpl() override;

	void attemptToConnect(size_t index, const char *lmms_port_type, const char *source_port, const char *destination_port);
	void attemptToReconnectOutput(size_t outputIndex, const QString& targetPort);
	void attemptToReconnectInput(size_t inputIndex, const QString& sourcePort);

	void registerPort(AudioBusHandle* port) override;
	void unregisterPort(AudioBusHandle* port) override;
	void renamePort(AudioBusHandle* port) override;

	int processCallback(jack_nframes_t nframes, bool processing, detail::JackBusSnapshot* buses, bool& busesCleared);

	static int staticProcessCallback(jack_nframes_t nframes, void* udata);
	static void shutdownCallback(void* _udata);

	const detail::JackApi& m_api;
	const QByteArray m_clientName;
	jack_client_t* m_client;

	bool m_active;

	std::atomic<MidiJack*> m_midiClient;
	std::unique_ptr<detail::JackCallbackGate> m_callbackGate;
	std::unique_ptr<detail::JackCallbackGate> m_midiGate;
	std::unique_ptr<detail::JackAudioBuses> m_buses;
	std::atomic_bool m_serverDisconnected{false};
	bool m_processingRequested = false;
	bool m_reportedDisconnect = false;
	bool m_recovering = false;
	bool m_graphBusy = false; // owner-thread graph operations never dispatch Qt events
	int m_graphError = 0;
	QTimer m_healthTimer;
	QElapsedTimer m_retryClock;
	std::vector<jack_port_t*> m_outputPorts;
	std::vector<jack_port_t*> m_inputPorts;
	std::unique_ptr<jack_default_audio_sample_t*[]> m_tempOutBufs;
	std::vector<SampleFrame> m_inputFrameBuffer;

signals:
	void zombified();
};

} // namespace lmms

#endif // LMMS_HAVE_JACK

#endif // LMMS_AUDIO_JACK_H
