/*
 * AudioJack.cpp - support for JACK transport
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

#include "AudioJack.h"

#ifdef LMMS_HAVE_JACK

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QDebug>
#include <QScopeGuard>
#include <QThread>
#include <QToolButton>
#include <QStringList>

#include "AudioEngine.h"
#include "AudioBusHandle.h"
#include "ConfigManager.h"
#include "Song.h"
#include "Track.h"
#include "PatternStore.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include <cerrno>
#include <stdexcept>
#include "MidiJack.h"
#include "JackCallbackGate.h"
#include "JackApi.h"
#include "JackGraph.h"
#include "JackAudioBuses.h"

#include <cstdio>
#include <algorithm>

namespace lmms {
static_assert(std::atomic_bool::is_always_lock_free && std::atomic<MidiJack*>::is_always_lock_free);
namespace {

const detail::JackApi nativeApi;

const auto audioJackClass = QString{"audiojack"};
const auto clientNameKey = QString{"clientname"};
const auto disconnectedRepresentation = QString{"-"};

QString getOutputKeyByChannel(size_t channel)
{
	return "output" + QString::number(channel + 1);
}

QString getInputKeyByChannel(size_t channel)
{
	return "input" + QString::number(channel + 1);
}

void printJackStatus(jack_status_t status)
{
	std::fprintf(stderr, "Status: 0x%2.0x\n", static_cast<unsigned int>(status));

	if (status & JackFailure)
	{
		std::fprintf(stderr, "Overall operation failed. JACK dependencies might need to be installed.\n");
	}

	if (status & JackServerFailed)
	{
		std::fprintf(stderr, "Could not connect to JACK server.\n");
	}
}

QString buildChannelSuffix(ch_cnt_t ch)
{
	return (ch % 2 ? "R" : "L") + QString::number(ch / 2 + 1);
}

QString buildOutputName(ch_cnt_t ch)
{
	return QString("master out ") + buildChannelSuffix(ch);
}

QString buildInputName(ch_cnt_t ch)
{
	return QString("master in ") + buildChannelSuffix(ch);
}

} // namespace

AudioJack::AudioJack(bool& successful, AudioEngine* audioEngineParam)
	: AudioJack(successful, audioEngineParam, nativeApi)
{
}

AudioJack::AudioJack(bool& successful, AudioEngine* audioEngineParam, const detail::JackApi& api)
	: AudioDevice(
		DEFAULT_CHANNELS,
		audioEngineParam)
	, m_api(api)
	, m_clientName(ConfigManager::inst()->value(audioJackClass, clientNameKey, "lmms").toUtf8())
	, m_client(nullptr)
	, m_active(false)
	, m_midiClient(nullptr)
	, m_callbackGate(std::make_unique<detail::JackCallbackGate>())
	, m_midiGate(std::make_unique<detail::JackCallbackGate>())
	, m_buses(std::make_unique<detail::JackAudioBuses>(*this))
	, m_tempOutBufs(new jack_default_audio_sample_t*[channels()])
{
	successful = false;
	successful = initJackClient();
	connect(&m_healthTimer, &QTimer::timeout, this, &AudioJack::pollServerHealth);
	if (successful) { m_healthTimer.start(100); }

	m_supportsCapture = true;
}




AudioJack::~AudioJack()
{
	m_healthTimer.stop(); stopProcessing(); m_midiGate->pauseAndWait();
	if (const auto midi = m_midiClient.exchange(nullptr)) { midi->audioDeviceRetired(); }
	closeJackClient();
}

void AudioJack::closeJackClient()
{
	m_callbackGate->pauseAndWait(); m_midiGate->pauseAndWait();
	m_buses->detachClient();
	if (const auto midi = m_midiClient.load()) { midi->releaseShared(false); }
	if (m_client)
	{
		try { if (m_active && !serverDisconnected()) { m_api.deactivate(m_client); } }
		catch (...) { qWarning() << "JACK deactivation threw during retirement"; }
		try { if (const auto error = m_api.close(m_client)) { qWarning() << "JACK client close returned" << error; } }
		catch (...) { qWarning() << "JACK client close threw during retirement"; }
		m_client = nullptr;
	}
	m_active = false; m_outputPorts.clear(); m_inputPorts.clear(); m_inputFrameBuffer.clear();
}

bool AudioJack::reopenJackClient()
{
	closeJackClient(); // Own the old handle until native callback retirement.
	m_serverDisconnected.store(false, std::memory_order_release);
	bool successful = false;
	const auto cleanup = qScopeGuard([&]
	{
		if (!successful) { closeJackClient(); m_serverDisconnected.store(true, std::memory_order_release); }
	});
	if (!initJackClient(true)) { return false; }
	if (const auto midi = m_midiClient.load())
	{
		if (!midi->prepareShared(this, m_client)) { return false; }
		m_midiGate->resume();
	}
	if (serverDisconnected()) { return false; }
	m_reportedDisconnect = false; successful = true; return true;
}

void AudioJack::pollServerHealth()
{
	if (!serverDisconnected()) { m_buses->poll(); }
	if (!serverDisconnected() || m_recovering || m_graphBusy) { return; }
	m_recovering = true; const auto done = qScopeGuard([&] { m_recovering = false; });
	if (!m_reportedDisconnect)
	{
		reportProcessingFailure(ProcessingFailure::Disconnected, -ENOTCONN);
		m_reportedDisconnect = true;
		// A notification listener may replace/delete this device. Deliver it
		// after this recovery frame has returned, on the owning Qt thread.
		QMetaObject::invokeMethod(this, [this] { emit zombified(); }, Qt::QueuedConnection);
	}
	// An export reservation may temporarily own this stopped device. Its restore
	// path will reconnect it; a timer must not start a second native DSP clock.
	if (!m_processingRequested || audioEngine()->audioDev() != this
		|| audioEngine()->hasStoredAudioDevice() || audioEngine()->isChangingAudioDevice()
		|| (Engine::getSong() && Engine::getSong()->isExporting())) { return; }
	if (m_retryClock.isValid() && m_retryClock.elapsed() < 1000) { return; }
	m_retryClock.start();
	try { AudioDevice::startProcessing(); }
	catch (...) { reportProcessingFailure(ProcessingFailure::Exception, -EIO); }
}

AudioJack* AudioJack::addMidiClient(MidiJack* midi)
{
	if (!midi || !m_client || serverDisconnected() || m_midiClient.load()) { return nullptr; }
	m_midiGate->pauseAndWait();
	try
	{
		if (!midi->prepareShared(this, m_client)) { return nullptr; }
		m_midiClient.store(midi, std::memory_order_release);
		m_midiGate->resume(); return this;
	}
	catch (...)
	{
		if (!midi->releaseShared(true)) { retireFailedMidiPorts(); }
		midi->m_jackAudio = nullptr; throw;
	}
}

void AudioJack::removeMidiClient(MidiJack* midi)
{
	if (!midi || m_midiClient.load() != midi) { return; }
	// Only MIDI readers are joined. An unrelated native DSP callback can keep
	// rendering while its former MIDI client's port and parser are retired.
	m_midiGate->pauseAndWait();
	m_midiClient.store(nullptr, std::memory_order_release);
	const bool retired = midi->releaseShared(!serverDisconnected()); midi->m_jackAudio = nullptr;
	if (!retired) { retireFailedMidiPorts(); }
}

bool AudioJack::recoverMidiClient(MidiJack* midi)
{
	if (!midi || !m_client || serverDisconnected() || m_midiClient.load() != midi) { return false; }
	m_midiGate->pauseAndWait();
	if (!midi->releaseShared(true)) { retireFailedMidiPorts(); return false; }
	if (!midi->prepareShared(this, m_client)) { return false; }
	m_midiGate->resume(); return true;
}

void AudioJack::retireFailedMidiPorts()
{
	// An unregister failure leaves native port ownership uncertain. Close the
	// owning client before replacing ports; never abandon a live output buffer.
	m_serverDisconnected.store(true, std::memory_order_release);
	reportProcessingFailure(ProcessingFailure::Disconnected, -EIO);
	closeJackClient();
}

bool AudioJack::initJackClient(bool preserveRate)
{
	bool successful = false;
	const auto cleanup = qScopeGuard([&]
	{
		if (successful) { return; }
		if (m_client) { m_api.close(m_client); m_client = nullptr; }
		m_outputPorts.clear(); m_inputPorts.clear(); m_inputFrameBuffer.clear();
	});
	m_outputPorts.clear(); m_inputPorts.clear();
	const auto clientName = m_clientName.isEmpty() ? QByteArray("lmms") : m_clientName;

	// Initial selection keeps native autostart behavior; recovery only reconnects.
	jack_status_t status{};
	m_client = m_api.open(clientName.constData(), preserveRate ? JackNoStartServer : JackNullOption, &status);
	if (m_client == nullptr)
	{
		std::fprintf(stderr, "jack_client_open() failed, ");
		printJackStatus(status);

		return false;
	}
	if (status & JackNameNotUnique)
	{
		std::fprintf(stderr, "there's already a client with name '%s', so unique "
					"name '%s' was assigned\n",
					clientName.constData(), m_api.clientName(m_client));
	}

	resizeInputBuffer(m_api.bufferSize(m_client));

	// set buffer-size callback
	if (m_api.setBufferSizeCallback(m_client,
		[](jack_nframes_t nframes, void* udata) -> int {
			const auto device = static_cast<AudioJack*>(udata);
			try { device->resizeInputBuffer(nframes); return 0; }
			catch (...) { device->reportProcessingFailure(ProcessingFailure::Exception); return 1; }
		},
		this)) { return false; }

	// set process-callback
	if (m_api.setProcessCallback(m_client, staticProcessCallback, this)) { return false; }

	// set shutdown-callback
	m_api.onShutdown(m_client, shutdownCallback, this);

	const auto rate = m_api.sampleRate(m_client);
	if (!rate || m_inputFrameBuffer.empty() || (preserveRate && rate != sampleRate())) { return false; }
	if (rate != sampleRate()) { setSampleRate(rate); }

	for (ch_cnt_t ch = 0; ch < channels(); ++ch)
	{
		const QString name = buildOutputName(ch);
		m_outputPorts.push_back(
			m_api.registerPort(m_client, name.toUtf8().constData(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0));

		const QString input_name = buildInputName(ch);
		m_inputPorts.push_back(m_api.registerPort(m_client, input_name.toUtf8().constData(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0));

		if (m_outputPorts.back() == nullptr || m_inputPorts.back() == nullptr)
		{
			std::fprintf(stderr, "no more JACK-ports available!\n");
			return false;
		}
	}

	if (serverDisconnected()) { return false; }
	successful = true; return true;
}




void AudioJack::resizeInputBuffer(jack_nframes_t nframes)
{
	m_inputFrameBuffer.resize(nframes);
}

void AudioJack::attemptToConnect(size_t index, const char *lmms_port_type, const char *source_port, const char *destination_port)
{
	std::fprintf(stderr, "Attempting to reconnect %s port %u: %s -> %s", lmms_port_type, static_cast<unsigned int>(index), source_port, destination_port);
	if (!m_api.connect(m_client, source_port, destination_port))
	{
		std::fprintf(stderr, " - Success!\n");
	}
	else
	{
		std::fprintf(stderr, " - Failure\n");
	}
}

void AudioJack::attemptToReconnectOutput(size_t outputIndex, const QString& targetPort)
{
	if (outputIndex >= m_outputPorts.size()) { return; }

	if (targetPort == disconnectedRepresentation)
	{
		std::fprintf(stderr, "Output port %u is not connected.\n", static_cast<unsigned int>(outputIndex));
		return;
	}

	auto outputName = m_api.portName(m_outputPorts[outputIndex]);
	auto targetName = targetPort.toUtf8();

	attemptToConnect(outputIndex, "output", outputName, targetName.constData());
}

void AudioJack::attemptToReconnectInput(size_t inputIndex, const QString& sourcePort)
{
	if (inputIndex >= m_inputPorts.size()) { return; }

	if (sourcePort == disconnectedRepresentation)
	{
		std::fprintf(stderr, "Input port %u is not connected.\n", static_cast<unsigned int>(inputIndex));
		return;
	}

	auto inputName = m_api.portName(m_inputPorts[inputIndex]);
	auto sourceName = sourcePort.toUtf8();

	attemptToConnect(inputIndex, "input", sourceName.constData(), inputName);
}


void AudioJack::startProcessingImpl()
{
	if (!audioEngine()->canJoinBackendCallbacks())
	{ throw std::logic_error("JACK processing must start on the owner thread outside a native model edit"); }
	if (m_graphBusy) { throw std::runtime_error("JACK graph operation is active; processing cannot be restarted inside it"); }
	m_processingRequested = true;
	m_callbackGate->pauseAndWait();
	const bool reconnecting = serverDisconnected() || !m_client;
	bool prepared = false;
	const auto retry = qScopeGuard([&]
	{
		// Failed activation can leave native JACK partially active. Retire it
		// before retrying, including a failed first activation.
		if (!prepared) { m_callbackGate->pause(); m_serverDisconnected.store(true, std::memory_order_release); }
	});
	if (reconnecting && !reopenJackClient())
	{ reportProcessingFailure(ProcessingFailure::Start, -EIO); return; }
	if (m_active && m_client) { prepared = true; m_callbackGate->resume(); return; }
	if (m_client == nullptr) { reportProcessingFailure(ProcessingFailure::Start); return; }
	m_buses->prepareClient();
	if (!m_client || serverDisconnected()) { reportProcessingFailure(ProcessingFailure::Start, -EIO); return; }

	if (const int error = m_api.activate(m_client))
	{
		std::fprintf(stderr, "cannot activate client\n");
		reportProcessingFailure(ProcessingFailure::Start, error);
		return;
	}

	m_active = true;

	// try to sync JACK's and LMMS's buffer-size
	//	jack_set_buffer_size( m_client, audioEngine()->framesPerPeriod() );

	const auto cm = ConfigManager::inst();

	const auto numberOfChannels = channels();
	for (size_t i = 0; i < numberOfChannels; ++i)
	{
		attemptToReconnectOutput(i, cm->value(audioJackClass, getOutputKeyByChannel(i)));
	}

	for (size_t i = 0; i < numberOfChannels; ++i)
	{
		attemptToReconnectInput(i, cm->value(audioJackClass, getInputKeyByChannel(i)));
	}
	if (serverDisconnected()) { reportProcessingFailure(ProcessingFailure::Disconnected, -ENOTCONN); }
	if (isRunning()) { prepared = true; m_callbackGate->resume(); }
}




void AudioJack::stopProcessingImpl()
{
	m_processingRequested = false;
	// Keep JACK's ports and shared MIDI connection intact. Inactive callbacks
	// only clear output; all admitted project/DSP/capture work finishes here.
	m_callbackGate->pauseAndWait();
	const detail::JackCallbackGate::Lease midiCallback(*m_midiGate);
	if (const auto midi = midiCallback ? m_midiClient.load(std::memory_order_acquire) : nullptr)
	{ midi->suspendOutput(); }
}

void AudioJack::requireGraphOwner() const
{
	if (QThread::currentThread() != thread()) { throw std::runtime_error("JACK audio graph operations require the device owner's thread"); }
	if (m_graphBusy) { throw std::runtime_error("Another JACK audio graph operation is active"); }
}

bool AudioJack::graphConnected(jack_port_t* local, const QByteArray& remote)
{
	const auto connected = m_api.portConnectedTo(local, remote.constData());
	if (connected < 0) { m_graphError = connected; throw std::runtime_error("JACK audio connection observation failed"); }
	return connected != 0;
}

AudioDevice::BackendGraph AudioJack::inspectBackendGraph(bool input)
{
	requireGraphOwner(); FileResourceIO::EventSuspension events;
	m_graphBusy = true; const auto idle = qScopeGuard([&] { m_graphBusy = false; });
	return inspectGraphPorts(input, input ? m_inputPorts : m_outputPorts, true);
}

AudioDevice::BackendGraph AudioJack::inspectGraphPorts(bool input, const std::vector<jack_port_t*>& local, bool startup)
{
	BackendGraph result; result.supported = true;
	if (!m_client || serverDisconnected()) { result.nativeErrorCode = -ENOTCONN; return result; }
	try
	{
		for (std::size_t index = 0; index < local.size(); ++index)
		{
			const auto configured = startup ? ConfigManager::inst()->value(audioJackClass, input ? getInputKeyByChannel(index) : getOutputKeyByChannel(index)) : QString{};
			result.channels.push_back({static_cast<unsigned int>(index), detail::jackGraphName(m_api.portName(local[index])), configured.left(1024), configured.size() > 1024});
		}
		const auto flags = input ? JackPortIsOutput : JackPortIsInput;
		const auto names = m_api.ports(m_client, nullptr, JACK_DEFAULT_AUDIO_TYPE, flags);
		const auto release = qScopeGuard([&] { if (names) { m_api.freeMemory(names); } });
		std::size_t bytes = 0;
		for (std::size_t index = 0; names && names[index]; ++index)
		{
			if (index >= 1024) { throw std::runtime_error("JACK audio endpoint inventory exceeds 1024 entries"); }
			const auto name = detail::jackGraphName(names[index]); const auto encoded = name.toUtf8(); bytes += encoded.size();
			if (bytes > 256 * 1024) { throw std::runtime_error("JACK audio endpoint names exceed 256 KiB"); }
			const auto peer = m_api.portByName(m_client, encoded.constData());
			if (!peer || m_api.portIsMine(m_client, peer)) { continue; }
			if (!(m_api.portFlags(peer) & flags) || detail::jackGraphName(m_api.portType(peer)) != JACK_DEFAULT_AUDIO_TYPE) { continue; }
			BackendGraph::Endpoint endpoint{name, {}};
			for (std::size_t channel = 0; channel < local.size(); ++channel)
			{ if (graphConnected(local[channel], encoded)) { endpoint.connectedChannels.push_back(static_cast<unsigned int>(channel)); } }
			result.endpoints.push_back(std::move(endpoint));
		}
		if (serverDisconnected()) { throw std::runtime_error("JACK disconnected during audio endpoint discovery"); }
		std::sort(result.endpoints.begin(), result.endpoints.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
		result.endpoints.erase(std::unique(result.endpoints.begin(), result.endpoints.end(),
			[](const auto& a, const auto& b) { return a.name == b.name; }), result.endpoints.end());
		result.observed = true; result.nativeErrorCode = m_graphError; return result;
	}
	catch (...) { if (!m_graphError) { m_graphError = -EIO; } throw; }
}

AudioDevice::BackendRoute AudioJack::setBackendRoute(bool input, unsigned int channel, const QString& remote, bool enabled,
	const std::function<bool()>& cancelled)
{
	requireGraphOwner();
	if (channel >= static_cast<unsigned int>(channels())) { throw std::out_of_range("Audio channel is outside the native master port range"); }
	const auto& ports = input ? m_inputPorts : m_outputPorts;
	return setGraphPortRoute(input, channel < ports.size() ? ports[channel] : nullptr, remote, enabled, cancelled);
}

AudioDevice::BackendRoute AudioJack::setGraphPortRoute(bool input, jack_port_t* local, const QString& remote, bool enabled,
	const std::function<bool()>& cancelled)
{
	requireGraphOwner(); FileResourceIO::EventSuspension events;
	const auto encoded = detail::jackRouteName(remote); const auto engine = audioEngine();
	if (engine->audioDev() != this || !engine->canChangeBackendGraph())
	{ throw std::runtime_error("Native audio graph changes require the current device, stopped/disarmed transport and no active device transition or render"); }
	m_graphBusy = true; const auto idle = qScopeGuard([&] { m_graphBusy = false; });
	m_graphError = 0; bool completed = false;
	const auto failed = qScopeGuard([&] { if (!completed && !m_graphError) { m_graphError = -EIO; } });
	if (!m_client || serverDisconnected() || !local) { throw std::runtime_error("JACK audio graph is unavailable"); }
	if (cancelled && cancelled()) { throw std::runtime_error("JACK audio routing was cancelled"); }
	const auto localName = detail::jackGraphName(m_api.portName(local)); const auto localBytes = localName.toUtf8();
	const auto before = graphConnected(local, encoded);
	if (before == enabled)
	{
		if (serverDisconnected()) { throw std::runtime_error("JACK disconnected during audio connection observation"); }
		if (cancelled && cancelled()) { throw std::runtime_error("JACK audio routing was cancelled"); }
		completed = true; return {localName, remote, enabled, false, false};
	}
	const auto peer = m_api.portByName(m_client, encoded.constData());
	if (!peer || m_api.portIsMine(m_client, peer) || detail::jackGraphName(m_api.portType(peer)) != JACK_DEFAULT_AUDIO_TYPE
		|| !(m_api.portFlags(peer) & (input ? JackPortIsOutput : JackPortIsInput)) || detail::jackGraphName(m_api.portName(peer)) != remote)
	{ m_graphError = -EINVAL; throw std::invalid_argument("JACK peer must be an available external audio port with the opposite direction"); }
	const bool resumeAudio = !m_callbackGate->paused() && isRunning();
	m_callbackGate->pauseAndWait();
	const auto resume = qScopeGuard([&] {
		if (resumeAudio && isRunning() && m_client && !serverDisconnected()) { m_callbackGate->resume(); }
	});
	// No callback can now append capture. Even a reverted input graph must not
	// deliver queued frames whose source identity predates this change attempt.
	if (input) { engine->discardInputBuffers(); }
	const auto change = [&](bool connect) {
		const auto source = input ? encoded.constData() : localBytes.constData();
		const auto destination = input ? localBytes.constData() : encoded.constData();
		return connect ? m_api.connect(m_client, source, destination) : m_api.disconnect(m_client, source, destination);
	};
	const auto restore = [&] {
		try
		{
			if (!m_client || serverDisconnected()) { return false; }
			if (graphConnected(local, encoded) != before) { change(before); }
			return graphConnected(local, encoded) == before;
		}
		catch (...) { return false; }
	};
	const auto failureText = [&](const QString& reason) {
		const bool restored = restore();
		if (!m_graphError) { m_graphError = -EIO; }
		if (!restored)
		{
			m_processingRequested = false; // uncertainty must not trigger a later timer restart
			reportProcessingFailure(ProcessingFailure::Exception, m_graphError);
		}
		return (reason + (restored ? "; previous audio connection state verified" : "; restoration unconfirmed; processing remains stopped; inspect the native audio graph")
			+ (input ? "; queued audio input was invalidated" : "")).toStdString();
	};
	try
	{
		if (cancelled && cancelled()) { throw std::runtime_error("JACK audio routing was cancelled before changing the graph"); }
		if (serverDisconnected()) { throw std::runtime_error("JACK disconnected before changing the audio graph"); }
		const auto code = change(enabled);
		if (code && !(enabled && code == EEXIST)) { m_graphError = code; throw std::runtime_error("Native JACK audio connection change failed"); }
		if (graphConnected(local, encoded) != enabled || serverDisconnected())
		{ m_graphError = -EIO; throw std::runtime_error("JACK did not confirm the requested audio connection state"); }
		if (cancelled && cancelled()) { throw std::runtime_error("JACK audio routing was cancelled after changing the graph"); }
		m_graphError = 0; completed = true; return {localName, remote, enabled, true, resumeAudio};
	}
	catch (const std::exception& error) { throw std::runtime_error(failureText(QString::fromUtf8(error.what()))); }
	catch (...) { throw std::runtime_error(failureText("JACK audio routing failed")); }
}

bool AudioJack::isProjectBus(const AudioBusHandle* bus) const
{
	if (!bus || !bus->m_renderTrack) { return false; }
	const auto container = bus->m_renderTrack->trackContainer();
	return container && (container == Engine::getSong() || container == Engine::patternStore());
}

void AudioJack::registerPort(AudioBusHandle* port)
{
	if (!isProjectBus(port)) { return; }
	if (QThread::currentThread() != thread()) { throw std::logic_error("Native JACK buses must be registered on the owner thread"); }
	m_buses->add(port->externalOutput(), port->name());
}

void AudioJack::unregisterPort(AudioBusHandle* port)
{
	const std::shared_ptr<const AudioBusOutput> source = port ? port->m_externalOutput : nullptr;
	if (!source) { return; }
	if (QThread::currentThread() != thread())
	{
		// Retained source identity prevents a delayed removal from affecting a
		// new track constructed at the former bus address. No native join here.
		QMetaObject::invokeMethod(this, [this, source] { m_buses->remove(source); }, Qt::QueuedConnection); return;
	}
	m_buses->remove(source);
}

void AudioJack::renamePort(AudioBusHandle* port)
{
	const std::shared_ptr<const AudioBusOutput> source = port ? port->m_externalOutput : nullptr;
	if (!source) { return; }
	const auto label = port->name();
	if (QThread::currentThread() != thread())
	{ QMetaObject::invokeMethod(this, [this, source, label] { m_buses->rename(source, label); }, Qt::QueuedConnection); return; }
	m_buses->rename(source, label);
}

AudioDevice::BusGraph AudioJack::inspectBusGraph(AudioBusHandle* bus)
{
	requireGraphOwner(); FileResourceIO::EventSuspension events;
	if (!isProjectBus(bus)) { throw std::invalid_argument("Native JACK track ports require a song or pattern track audio bus"); }
	m_graphBusy = true; const auto idle = qScopeGuard([&] { m_graphBusy = false; });
	return m_buses->inspect(bus->externalOutput(), bus->extOutputEnabled());
}

void AudioJack::configureBusOutput(AudioBusHandle* bus, bool enabled, const std::function<bool()>& cancelled)
{
	requireGraphOwner(); FileResourceIO::EventSuspension events;
	if (audioEngine()->audioDev() != this || !audioEngine()->canChangeBackendGraph())
	{ throw std::runtime_error("Native track port updates require the current device, stopped/disarmed transport and no active model edit, device transition or render"); }
	if (!isProjectBus(bus)) { throw std::invalid_argument("Native JACK track ports require a song or pattern track audio bus"); }
	if (cancelled && cancelled()) { throw std::runtime_error("JACK track port update was cancelled"); }
	m_graphBusy = true; const auto idle = qScopeGuard([&] { m_graphBusy = false; });
	const auto source = bus->externalOutput(); const bool previous = bus->extOutputEnabled();
	{
		const auto guard = audioEngine()->requestChangesGuard(); bus->setExtOutputEnabled(enabled);
		// Synchronize intent even when the model flag was already equal. A
		// retained device may have been detached while another backend owned it.
		if (enabled) { m_buses->add(source, bus->name()); } else { m_buses->remove(source); }
	}
	bool cancellation = false;
	const auto check = [&] {
		try { if (cancelled && cancelled()) { cancellation = true; return true; } }
		catch (...) { cancellation = true; throw; }
		return false;
	};
	try { m_buses->synchronize(source, check); }
	catch (...)
	{
		if (cancellation)
		{
			{
				const auto guard = audioEngine()->requestChangesGuard(); bus->setExtOutputEnabled(previous);
				if (previous) { m_buses->add(source, bus->name()); } else { m_buses->remove(source); }
			}
			try { m_buses->restoreSnapshot(); } catch (...) { }
		}
		throw;
	}
}

AudioDevice::BackendRoute AudioJack::setBusRoute(AudioBusHandle* bus, unsigned int channel, const QString& remote,
	bool enabled, const std::function<bool()>& cancelled)
{
	requireGraphOwner(); FileResourceIO::EventSuspension events;
	if (!isProjectBus(bus)) { throw std::invalid_argument("Native JACK track ports require a song or pattern track audio bus"); }
	if (channel >= 2) { throw std::out_of_range("Track audio channel must be 0 or 1"); }
	const auto ports = m_buses->routePorts(bus->externalOutput());
	return setGraphPortRoute(false, ports[channel], remote, enabled, cancelled);
}

int AudioJack::processCallback(jack_nframes_t nframes, bool processing, detail::JackBusSnapshot* buses, bool& busesCleared)
{
	if (nframes == 0) { return 0; }
	bool midiOutputReady = false;
	// Submit after native DSP has generated this cycle's output. Take the MIDI
	// lease only here, so detaching MIDI does not wait for unrelated DSP work.
	// An early return/exception still clears the buffer and cancels queued notes.
	const auto midiOutput = qScopeGuard([&]
	{
		const detail::JackCallbackGate::Lease midiCallback(*m_midiGate);
		if (const auto midi = midiCallback ? m_midiClient.load(std::memory_order_acquire) : nullptr)
		{ midi->JackMidiWrite(nframes, midiOutputReady); }
	});
	for (int c = 0; c < channels(); ++c)
	{
		m_tempOutBufs[c] = (jack_default_audio_sample_t*)m_api.portBuffer(m_outputPorts[c], nframes);
		if (m_tempOutBufs[c]) { std::fill_n(m_tempOutBufs[c], nframes, 0.f); }
	}

	busesCleared = m_buses->prepare(buses, nframes);
	const auto silenceFailure = qScopeGuard([&]
	{
		if (!processing || midiOutputReady) { return; }
		for (int channel = 0; channel < channels(); ++channel)
		{ if (m_tempOutBufs[channel]) { std::fill_n(m_tempOutBufs[channel], nframes, 0.f); } }
		busesCleared = m_buses->silence(buses, nframes);
	});
	if (!busesCleared) { reportProcessingFailure(ProcessingFailure::InvalidCallback); return 0; }

	if (!processing) { return 0; }
	if (nframes > m_inputFrameBuffer.size()
		|| std::any_of(m_tempOutBufs.get(), m_tempOutBufs.get() + channels(), [](const auto buffer) { return buffer == nullptr; }))
	{ reportProcessingFailure(ProcessingFailure::InvalidCallback); return 0; }

	{
		const detail::JackCallbackGate::Lease midiCallback(*m_midiGate);
		if (const auto midi = midiCallback ? m_midiClient.load(std::memory_order_acquire) : nullptr)
		{ midi->JackMidiRead(nframes); }
	}
	if (buses && !buses->destinations.empty())
	{ audioEngine()->renderNextBuffer({m_tempOutBufs.get(), channels(), nframes}, buses->destinations); }
	else { audioEngine()->renderNextBuffer({m_tempOutBufs.get(), channels(), nframes}); }
	if (!isRunning() || m_callbackGate->paused())
	{
		for (int c = 0; c < channels(); ++c) { std::fill_n(m_tempOutBufs[c], nframes, 0.f); }
		return 0;
	}

	for (int c = 0; c < channels(); ++c)
	{
		jack_default_audio_sample_t* jack_input_buffer = (jack_default_audio_sample_t*) m_api.portBuffer(m_inputPorts[c], nframes);
		if (!jack_input_buffer) { reportProcessingFailure(ProcessingFailure::InvalidCallback); return 0; }

		for (jack_nframes_t frame = 0; frame < nframes; frame++)
		{
			m_inputFrameBuffer[frame][c] = static_cast<sample_t>(jack_input_buffer[frame]);
		}
	}
	audioEngine()->pushInputFrames (m_inputFrameBuffer.data(), nframes);
	midiOutputReady = isRunning() && !m_callbackGate->paused();
	return 0;
}




int AudioJack::staticProcessCallback(jack_nframes_t nframes, void* udata)
{
	const auto device = static_cast<AudioJack*>(udata);
	if (device->serverDisconnected()) { return 0; }
	const detail::JackCallbackGate::Lease callback(*device->m_callbackGate);
	detail::JackAudioBuses::Read buses(*device->m_buses); bool busesCleared = false;
	const auto observed = qScopeGuard([&] { if (busesCleared) { buses.confirm(); } });
	try { return device->processCallback(nframes, callback && device->isRunning(), buses.get(), busesCleared); }
	catch (...)
	{
		device->reportProcessingFailure(ProcessingFailure::Exception);
		// Port buffers belong to this cycle. Do not reuse an earlier cycle's
		// cached pointers if buffer acquisition or native processing threw.
		for (const auto port : device->m_outputPorts)
		{
			try
			{
				if (const auto buffer = static_cast<jack_default_audio_sample_t*>(device->m_api.portBuffer(port, nframes)))
				{ std::fill_n(buffer, nframes, 0.f); }
			}
			catch (...) { }
		}
		busesCleared = device->m_buses->silence(buses.get(), nframes);
		return 0;
	}
}




void AudioJack::shutdownCallback(void* udata)
{
	const auto device = static_cast<AudioJack*>(udata);
	device->m_callbackGate->pause();
	device->m_serverDisconnected.store(true, std::memory_order_release);
	const detail::JackCallbackGate::Lease midiCallback(*device->m_midiGate);
	if (const auto midi = midiCallback ? device->m_midiClient.load(std::memory_order_acquire) : nullptr)
	{ midi->notifyServerShutdown(); }
}

AudioJack::setupWidget::setupWidget(QWidget* parent)
	: AudioDeviceSetupWidget(AudioJack::name(), parent)
{
	// TODO: Once backend can be changed without restarting LMMS, add a button to start/stop
	//       the JACK server and an indicator for the JACK server status.

	jack_status_t status;
	m_client = jack_client_open("LMMS-Setup Dialog", JackNoStartServer, &status);
	if (!m_client)
	{
		// Failure is expected when not using the JACK backend
	}

	QFormLayout * form = new QFormLayout(this);

	// Set the field growth policy to allow fields to expand horizontally
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	const auto cm = ConfigManager::inst();
	QString cn = cm->value(audioJackClass, clientNameKey);
	if (cn.isEmpty()) { cn = "lmms"; }
	m_clientName = new QLineEdit(cn, this);

	form->addRow(tr("Client name"), m_clientName);

	auto buildToolButton = [](QWidget* parent, const QString& currentSelection, const std::vector<std::string>& names, const QString& filteredLMMSClientName)
	{
		auto toolButton = new QToolButton(parent);
		// Make sure that the tool button will fill out the available space in the form layout
		toolButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		toolButton->setPopupMode(QToolButton::InstantPopup);
		toolButton->setText(currentSelection);
		auto menu = AudioJack::setupWidget::buildMenu(toolButton, names, filteredLMMSClientName);
		toolButton->setMenu(menu);

		return toolButton;
	};

	// Outputs
	const auto audioOutputNames = getAudioOutputNames();

	constexpr size_t numberOfOutputChannels = 2;
	for (size_t i = 0; i < numberOfOutputChannels; ++i)
	{
		const auto outputKey = getOutputKeyByChannel(i);
		const auto outputValue = cm->value(audioJackClass, outputKey);
		auto outputDevice = buildToolButton(this, outputValue, audioOutputNames, cn);
		form->addRow(buildOutputName(i) + ":", outputDevice);
		m_outputDevices.push_back(outputDevice);
	}

	// Inputs
	const auto audioInputNames = getAudioInputNames();

	constexpr size_t numberOfInputChannels = 2;
	for (size_t i = 0; i < numberOfInputChannels; ++i)
	{
		const auto inputKey = getInputKeyByChannel(i);
		const auto inputValue = cm->value(audioJackClass, inputKey);
		auto inputDevice = buildToolButton(this, inputValue, audioInputNames, cn);
		form->addRow(buildInputName(i) + ":", inputDevice);
		m_inputDevices.push_back(inputDevice);
	}

	if (m_client != nullptr)
	{
		jack_deactivate(m_client);
		jack_client_close(m_client);
	}
}


void AudioJack::setupWidget::saveSettings()
{
	ConfigManager::inst()->setValue(audioJackClass, clientNameKey, m_clientName->text());

	for (size_t i = 0; i < m_outputDevices.size(); ++i)
	{
		ConfigManager::inst()->setValue(audioJackClass, getOutputKeyByChannel(i), m_outputDevices[i]->text());	
	}

	for (size_t i = 0; i < m_inputDevices.size(); ++i)
	{
		ConfigManager::inst()->setValue(audioJackClass, getInputKeyByChannel(i), m_inputDevices[i]->text());	
	}
}

std::vector<std::string> AudioJack::setupWidget::getAudioPortNames(JackPortFlags portFlags) const
{
	std::vector<std::string> audioPorts;

	// We are using weak_libjack. If JACK is not installed this will result in the client being nullptr.
	// Because jack_get_ports in weak_libjack does not check for nullptr we have to do this here and fail gracefully,
	// i.e. with an empty list of audio ports.
	if (!m_client)
	{
		return audioPorts;
	}

	const char **inputAudioPorts = jack_get_ports(m_client, nullptr, JACK_DEFAULT_AUDIO_TYPE, portFlags);
	if (inputAudioPorts)
	{
		for (int i = 0; inputAudioPorts[i] != nullptr; ++i)
		{
			auto currentPortName = inputAudioPorts[i];

			audioPorts.push_back(currentPortName);
		}
		jack_free(inputAudioPorts); // Remember to free after use
	}

	return audioPorts;
}

std::vector<std::string> AudioJack::setupWidget::getAudioOutputNames() const
{
	return getAudioPortNames(JackPortIsInput);
}

std::vector<std::string> AudioJack::setupWidget::getAudioInputNames() const
{
	return getAudioPortNames(JackPortIsOutput);
}

QMenu* AudioJack::setupWidget::buildMenu(QToolButton* toolButton, const std::vector<std::string>& names, const QString& filteredLMMSClientName)
{
	auto menu = new QMenu(toolButton);
	QMap<QString, QMenu*> clientNameToSubMenuMap;
	QList<QAction*> topLevelActions;
	for (const auto& currentName : names)
	{
		const auto clientNameWithPortName = QString::fromStdString(currentName);

		auto actionLambda = [toolButton, clientNameWithPortName](bool checked)
		{
			toolButton->setText(clientNameWithPortName);
		};

		// Split into individual client name and port name
		const auto list = clientNameWithPortName.split(":");
		if (list.size() == 2)
		{
			const auto& clientName = list[0];
			const auto& portName = list[1];

			if (clientName == filteredLMMSClientName)
			{
				// Prevent loops by not adding port of the LMMS client to the menu
				continue;
			}

			QMenu* clientSubMenu = nullptr;

			auto it = clientNameToSubMenuMap.find(clientName);
			if (it == clientNameToSubMenuMap.end())
			{
				clientSubMenu = new QMenu(menu);
				clientSubMenu->setTitle(clientName);
				clientNameToSubMenuMap.insert(clientName, clientSubMenu);
			}
			else
			{
				clientSubMenu = *it;
			}

			auto action = new QAction(portName, clientSubMenu);
			connect(action, &QAction::triggered, actionLambda);
			clientSubMenu->addAction(action);
		}
		else
		{
			// We cannot split into client and port name. Add the whole thing to the top level menu
			auto action = new QAction(QString::fromStdString(currentName), menu);
			connect(action, &QAction::triggered, actionLambda);
			topLevelActions.append(action);
		}
	}

	// First add the sub menus. By iterating the map they will be sorted automatically
	for (auto it = clientNameToSubMenuMap.begin(); it != clientNameToSubMenuMap.end(); ++it)
	{
		menu->addMenu(it.value());
	}

	// Now add potential top level actions, i.e. the entries which cannot be split at exactly one ":"
	// They must be sorted explicitly
	std::sort(topLevelActions.begin(), topLevelActions.end(), [](QAction* a, QAction* b) { return a->text() < b->text(); });
	menu->addActions(topLevelActions);

	// Add the menu entry which represents the disconnected state at the very end
	auto disconnectedAction = new QAction(disconnectedRepresentation, menu);
	connect(disconnectedAction, &QAction::triggered, [toolButton](bool checked)	{ toolButton->setText(disconnectedRepresentation); });
	menu->addAction(disconnectedAction);

	return menu;
}


} // namespace lmms

#endif // LMMS_HAVE_JACK
