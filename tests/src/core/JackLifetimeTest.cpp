/* Native JACK shared-client ownership tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include "FileResourceIO.h"
#include <QScopeGuard>
#include <QTemporaryDir>
#include <atomic>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <limits>
#include <new>
#include <cstdlib>
#include <set>
#include <utility>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioDummy.h"
#include "AudioBusHandle.h"
#include "AudioBusOutput.h"
#include "AudioEngine.h"
#include "AudioJack.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "MidiEventProcessor.h"
#include "MidiJack.h"
#include "MidiDummy.h"
#include "MidiPort.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PlayHandle.h"
#include "RemotePlugin.h"
#include "SampleClip.h"
#include "SampleBuffer.h"
#include "SampleRecordHandle.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "StudioAgent.h"
#include "TrackRenderScope.h"
#include "../../../src/core/audio/JackApi.h"
#include "../../../src/core/audio/JackCallbackGate.h"
#include "../../../src/core/audio/JackAudioBuses.h"

namespace lmms
{
class AudioJackTestAccess
{
public:
	static std::unique_ptr<AudioJack> create(const detail::JackApi& api)
	{
		bool successful = false; auto result = std::unique_ptr<AudioJack>(new AudioJack(successful, Engine::audioEngine(), api));
		result->m_healthTimer.stop(); if (!successful) { throw std::runtime_error("JACK audio fixture setup failed"); }
		return result;
	}
	static void poll(AudioJack& audio, bool retry = true) { if (retry) { audio.m_retryClock.invalidate(); } audio.pollServerHealth(); }
	static bool midiPaused(const AudioJack& audio) { return audio.m_midiGate->paused(); }
	static bool paused(const AudioJack& audio) { return audio.m_callbackGate->paused(); }
	static const void* busSnapshot(const AudioJack& audio) { return audio.m_buses->m_published.load(); }
	static void expireBuses(AudioJack& audio) { for (auto& item : audio.m_buses->m_records) { item.second->retry.invalidate(); } }
};
class MidiJackTestAccess
{
public:
	static std::unique_ptr<MidiJack> create(AudioJack* audio, const detail::JackApi& api)
	{ auto result = std::unique_ptr<MidiJack>(new MidiJack(audio, api)); result->m_healthTimer.stop(); return result; }
	static void poll(MidiJack& midi, bool retry = true) { if (retry) { midi.m_retryClock.invalidate(); } midi.pollHealth(); }
	static void expireRoutes(MidiJack& midi) { for (auto& route : midi.m_retainedRoutes) { route.retry.invalidate(); } }
	static std::size_t retainedCount(const MidiJack& midi) { return midi.m_retainedRoutes.size(); }
	static bool paused(const MidiJack& midi) { return midi.m_callbackGate->paused(); }
	static bool changingGraph(const MidiJack& midi) { return midi.m_graphChanging.load(); }
	static void withOutputLock(MidiJack& midi, const std::function<void()>& action)
	{ const std::lock_guard<std::mutex> lock(midi.m_outputMutex); action(); }
	static jack_port_t* outputPort(const MidiJack& midi) { return midi.m_outputPort; }
	static MidiClient* replaceEngineClient(MidiClient* client) { return std::exchange(Engine::audioEngine()->m_midiClient, client); }
	static bool cleanupAdvanced(MidiJack& midi)
	{ const std::lock_guard<std::mutex> lock(midi.m_outputMutex); return !midi.m_cleanup && midi.m_outputCycle != midi.m_cleanupSubmittedCycle; }
};
class MidiClientTransferTestAccess
{
public:
	static auto transfer(MidiClient& source, MidiClient& target, const std::function<bool()>& cancelled = {})
	{ return source.transferPortsTo(target, cancelled); }
};
class StudioMidiBackendSelectionTestAccess
{
public:
	static auto select(const QString& name, const std::function<std::unique_ptr<MidiClient>()>& factory)
	{ return Engine::audioEngine()->selectMidiBackend(name, factory); }
};
}
using namespace lmms;
namespace
{
struct Client;
struct Event { jack_nframes_t time; std::vector<unsigned char> bytes; };
struct Port
{
	Client* client;
	bool midi, output = false, registered = true;
	QByteArray name;
	std::array<float, 4096> pcm{};
	std::array<float, 4096> alternatePcm{};
	float* lastPcm = nullptr;
	std::vector<Event> events;
};
struct Client
{
	bool closed = false, active = false;
	jack_options_t options{};
	JackProcessCallback process = nullptr;
	JackShutdownCallback shutdown = nullptr;
	void* processArg = nullptr;
	void* shutdownArg = nullptr;
	std::vector<std::unique_ptr<Port>> ports;
	std::thread worker;
	std::atomic<jack_nframes_t> clock{0};
	std::atomic_bool cycling{false};
	int tick(jack_nframes_t frames = 256) { const auto result = process(frames, processArg); clock += frames; return result; }
	void disconnect() { shutdown(shutdownArg); }
	void launch() { worker = std::thread([this] { tick(); }); }
	void launchCycles() { cycling = true; worker = std::thread([this] { while (cycling.load()) { tick(); QThread::msleep(1); } }); }
	void join() { cycling = false; if (worker.joinable()) { worker.join(); } }
	Port* midiPort(bool output = false) { for (const auto& p : ports) { if (p->midi && p->output == output && p->registered) { return p.get(); } } return nullptr; }
};
struct Transport
{
	static inline Transport* current = nullptr;
	detail::JackApi api;
	QString fault;
	std::vector<std::unique_ptr<Client>> clients;
	std::vector<QByteArray> names;
	Client external;
	std::vector<std::unique_ptr<Port>> peers;
	std::set<std::pair<QByteArray, QByteArray>> connections;
	int graphChanges = 0, graphObservations = 0, lists = 0, freedLists = 0;
	std::vector<Event> outputHistory;
	std::atomic<int> opens{0}, closes{0}, activates{0}, unregisters{0}, badAccess{0}, midiReads{0};
	std::function<void()> onMidiBuffer, onMidiRegister, onOutputBuffer, onOutputClock, onConnect, onDisconnect, onAudioInputBuffer;
	std::function<void()> onBusBuffer, onBusRegister, onBusUnregister;
	int busRegistrations = 0;
	bool alternateBuffers = false;
	bool lateCloseShutdown = false;
	std::size_t outputCapacity = std::numeric_limits<std::size_t>::max();
	static Client& client(jack_client_t* c) { return *reinterpret_cast<Client*>(c); }
	Port* endpoint(const QByteArray& name)
	{
		for (const auto& c : clients) { for (const auto& p : c->ports) { if (!c->closed && p->registered && p->name == name) { return p.get(); } } }
		for (const auto& p : peers) { if (p->registered && p->name == name) { return p.get(); } }
		return nullptr;
	}
	Port* addPeer(const QByteArray& name, bool output, bool midi = true)
	{
		auto p = std::make_unique<Port>(); p->client = &external; p->name = name; p->output = output; p->midi = midi;
		const auto raw = p.get(); peers.push_back(std::move(p)); return raw;
	}
	Transport()
	{
		current = this;
		api.open = [](const char* name, jack_options_t options, jack_status_t* status) -> jack_client_t*
		{
			auto& t = *current; ++t.opens; t.names.emplace_back(name); *status = JackFailure;
			if (t.fault == "throw-open") { throw std::runtime_error("JACK open failure"); }
			if (t.fault == "open") { return nullptr; }
			auto c = std::make_unique<Client>(); c->options = options; auto raw = c.get(); t.clients.push_back(std::move(c));
			*status = static_cast<jack_status_t>(0); return reinterpret_cast<jack_client_t*>(raw);
		};
		api.close = [](jack_client_t* raw)
		{
			auto& c = client(raw); c.join();
			if (c.closed) { ++current->badAccess; }
			if (current->lateCloseShutdown && c.shutdown) { c.disconnect(); }
			c.closed = true; c.active = false; for (auto& p : c.ports) { p->registered = false; }
			for (auto it = current->connections.begin(); it != current->connections.end();)
			{ if (!current->endpoint(it->first) || !current->endpoint(it->second)) { it = current->connections.erase(it); } else { ++it; } }
			++current->closes; return 0;
		};
		api.bufferSize = [](jack_client_t*) -> jack_nframes_t { return 1024; };
		api.sampleRate = [](jack_client_t*) -> jack_nframes_t { return Engine::audioEngine()->baseSampleRate() + (current->fault == "rate" ? 100 : 0); };
		api.setBufferSizeCallback = [](jack_client_t*, JackBufferSizeCallback, void*) { return 0; };
		api.setProcessCallback = [](jack_client_t* raw, JackProcessCallback callback, void* arg)
		{
			if (current->fault == "throw-process") { throw std::runtime_error("JACK process registration"); }
			client(raw).process = callback; client(raw).processArg = arg; return current->fault == "process" ? -77 : 0;
		};
		api.onShutdown = [](jack_client_t* raw, JackShutdownCallback callback, void* arg) { client(raw).shutdown = callback; client(raw).shutdownArg = arg; };
		api.registerPort = [](jack_client_t* raw, const char* name, const char* type, unsigned long flags, unsigned long) -> jack_port_t*
		{
			auto& t = *current; auto& c = client(raw); const bool midi = QByteArray(type) == JACK_DEFAULT_MIDI_TYPE;
			const bool output = flags & JackPortIsOutput;
			const bool bus = QByteArray(name).startsWith("track "); const bool right = QByteArray(name).endsWith(" R");
			if (bus)
			{
				++t.busRegistrations; if (t.onBusRegister) { t.onBusRegister(); }
				if (t.fault == (right ? "bus-right-throw" : "bus-left-throw")) { throw std::runtime_error("JACK track registration fixture"); }
				if (t.fault == (right ? "bus-right-port" : "bus-left-port") || (right && t.fault.startsWith("bus-cleanup-"))) { return nullptr; }
			}
			if (c.closed) { ++t.badAccess; return nullptr; }
			if (midi && !output && t.onMidiRegister) { t.onMidiRegister(); }
			if (midi && output && t.fault == "throw-output-port") { throw std::runtime_error("JACK output port registration"); }
			if (midi && output && t.fault == "output-port") { return nullptr; }
			if (t.fault == "throw-port" || (midi && t.fault == "throw-midi-port")) { throw std::runtime_error("JACK port registration"); }
			if (t.fault == "port" || (midi && t.fault == "midi-port") || (!midi && t.fault == "audio-port")) { return nullptr; }
			auto p = std::make_unique<Port>(); p->client = &c; p->midi = midi; p->output = output;
			p->name = "lmms" + QByteArray::number(static_cast<int>(t.clients.size())) + ":" + name;
			auto result = p.get(); c.ports.push_back(std::move(p));
			return reinterpret_cast<jack_port_t*>(result);
		};
		api.unregisterPort = [](jack_client_t* raw, jack_port_t* port)
		{
			auto p = reinterpret_cast<Port*>(port); if (client(raw).closed || !p || !p->registered || p->client != &client(raw)) { ++current->badAccess; return -EINVAL; }
			const auto fault = current->fault;
			if (p->name.contains(":track "))
			{
				if (current->onBusUnregister) { current->onBusUnregister(); }
				if (fault == "bus-cleanup-error" || fault == "bus-unregister-error") { return -EIO; }
				if (fault == "bus-cleanup-throw" || fault == "bus-unregister-throw") { throw std::runtime_error("JACK track unregister fixture"); }
				if (fault == "bus-unregister-right" && p->name.endsWith(" R")) { return -EIO; }
			}
			if ((p->output && fault == "unregister-output") || (!p->output && fault == "unregister-input")) { return -EIO; }
			if ((p->output && fault == "throw-unregister-output") || (!p->output && fault == "throw-unregister-input")) { throw std::runtime_error("JACK port retirement"); }
			p->registered = false;
			for (auto it = current->connections.begin(); it != current->connections.end();)
			{ if (it->first == p->name || it->second == p->name) { it = current->connections.erase(it); } else { ++it; } }
			++current->unregisters; return 0;
		};
		api.activate = [](jack_client_t* raw)
		{
			++current->activates;
			if (current->fault == "throw-activate") { throw std::runtime_error("JACK activation"); }
			if (current->fault == "activate") { return -78; }
			auto& c = client(raw); c.active = true; c.tick();
			if (current->fault == "shutdown-activate") { c.disconnect(); }
			return 0;
		};
		api.deactivate = [](jack_client_t* raw) { client(raw).join(); client(raw).active = false; return 0; };
		api.portName = [](const jack_port_t* p) { return reinterpret_cast<const Port*>(p)->name.constData(); };
		api.portFlags = [](const jack_port_t* p) -> int { return reinterpret_cast<const Port*>(p)->output ? JackPortIsOutput : JackPortIsInput; };
		api.portType = [](const jack_port_t* p) { return reinterpret_cast<const Port*>(p)->midi ? JACK_DEFAULT_MIDI_TYPE : JACK_DEFAULT_AUDIO_TYPE; };
		api.portIsMine = [](const jack_client_t* c, const jack_port_t* p) -> int { return reinterpret_cast<const Port*>(p)->client == reinterpret_cast<const Client*>(c); };
		api.portByName = [](jack_client_t*, const char* name) -> jack_port_t* {
			if (current->fault == "lookup-exception") { throw std::runtime_error("JACK graph lookup"); }
			return reinterpret_cast<jack_port_t*>(current->endpoint(name));
		};
		api.ports = [](jack_client_t*, const char*, const char* type, unsigned long flags) -> const char** {
			auto& t = *current; if (t.fault == "list-exception") { throw std::runtime_error("JACK endpoint list"); }
			std::vector<const char*> names;
			const auto append = [&](const auto& ports) { for (const auto& p : ports) {
				if (p->registered && !p->client->closed && (!type || QByteArray(type) == (p->midi ? JACK_DEFAULT_MIDI_TYPE : JACK_DEFAULT_AUDIO_TYPE))
					&& (flags & (p->output ? JackPortIsOutput : JackPortIsInput))) { names.push_back(p->name.constData()); }
			} };
			for (const auto& c : t.clients) { append(c->ports); } append(t.peers);
			if (t.fault == "ports-limit" && !names.empty()) { names.resize(1025, names.back()); }
			if (names.empty()) { return nullptr; }
			auto result = static_cast<const char**>(std::malloc((names.size() + 1) * sizeof(const char*)));
			if (!result) { throw std::bad_alloc(); }
			std::copy(names.begin(), names.end(), result); result[names.size()] = nullptr; ++t.lists; return result;
		};
		api.freeMemory = [](void* p) { ++current->freedLists; std::free(p); };
		api.portConnectedTo = [](const jack_port_t* raw, const char* name) -> int {
			++current->graphObservations;
			if (current->fault == "observe") { return -77; }
			const auto p = reinterpret_cast<const Port*>(raw);
			return current->connections.count(p->output ? std::make_pair(p->name, QByteArray(name)) : std::make_pair(QByteArray(name), p->name)) != 0;
		};
		api.portConnected = [](const jack_port_t* raw) -> int {
			if (current->fault == "bus-observe") { return -EIO; }
			const auto p = reinterpret_cast<const Port*>(raw); int count = 0;
			for (const auto& edge : current->connections) { if (edge.first == p->name || edge.second == p->name) { ++count; } }
			return count;
		};
		api.disconnectPort = [](jack_client_t*, jack_port_t* raw) {
			const auto p = reinterpret_cast<Port*>(raw);
			if (current->fault == "bus-disconnect") { return -EIO; }
			for (auto it = current->connections.begin(); it != current->connections.end(); )
			{ if (it->first == p->name || it->second == p->name) { it = current->connections.erase(it); } else { ++it; } }
			return 0;
		};
		api.connect = [](jack_client_t*, const char* source, const char* destination) {
			auto& t = *current; ++t.graphChanges;
			if (t.fault == "connect") { return -71; }
			if (t.fault == "connect-unconfirmed") { return 0; }
			t.connections.emplace(source, destination); if (t.onConnect) { t.onConnect(); }
			if (t.fault == "connect-throw") { throw std::runtime_error("JACK graph connect"); }
			if (t.fault == "shutdown-connect") { t.latest().disconnect(); }
			if (t.fault == "connect-after-error" || t.fault == "rollback") { return -72; }
			return t.fault == "exists" ? EEXIST : 0;
		};
		api.disconnect = [](jack_client_t*, const char* source, const char* destination) {
			auto& t = *current; ++t.graphChanges; if (t.fault == "disconnect" || t.fault == "rollback") { return -74; }
			if (t.onDisconnect) { t.onDisconnect(); }
			t.connections.erase({source, destination}); return t.fault == "disconnect-after-error" ? -75 : 0;
		};
		api.portBuffer = [](jack_port_t* raw, jack_nframes_t frames) -> void*
		{
			auto p = reinterpret_cast<Port*>(raw); auto& t = *current;
			if (!p || !p->registered || p->client->closed) { ++t.badAccess; return nullptr; }
			if (!p->midi)
			{
				if (!p->output && t.onAudioInputBuffer) { t.onAudioInputBuffer(); }
				if (p->name.contains(":track "))
				{
					if (t.onBusBuffer) { t.onBusBuffer(); }
					if (t.fault == "bus-buffer-throw" && p->name.endsWith(" R")) { throw std::runtime_error("JACK track buffer fixture"); }
					if (t.fault == "bus-buffer-null" && p->name.endsWith(" R")) { return nullptr; }
				}
				if (!p->output && t.fault == "bus-input-null") { return nullptr; }
				if (!p->output && t.fault == "bus-input-throw") { throw std::runtime_error("JACK track capture fixture"); }
				p->lastPcm = t.alternateBuffers && ((p->client->clock.load() / 256) % 2) ? p->alternatePcm.data() : p->pcm.data();
				return frames <= p->pcm.size() ? p->lastPcm : nullptr;
			}
			if (p->output)
			{
				if (t.onOutputBuffer) { t.onOutputBuffer(); }
				if (t.fault == "throw-output-buffer") { throw std::runtime_error("JACK output buffer"); }
				return t.fault == "null-output-buffer" ? nullptr : p;
			}
			++t.midiReads; if (t.onMidiBuffer) { t.onMidiBuffer(); }
			if (t.fault == "throw-buffer") { throw std::runtime_error("JACK MIDI buffer"); }
			return t.fault == "null-buffer" ? nullptr : p;
		};
		api.midiEventCount = [](void* buffer) -> jack_nframes_t { return current->fault == "count" ? MidiClient::InputQueueLimit + 1 : static_cast<jack_nframes_t>(static_cast<Port*>(buffer)->events.size()); };
		api.lostMidiEvents = [](void*) -> std::uint32_t { return current->fault == "lost" ? 1 : 0; };
		api.midiEvent = [](jack_midi_event_t* result, void* buffer, std::uint32_t index)
		{
			auto& t = *current; if (t.fault == "throw-event") { throw std::runtime_error("JACK MIDI event"); }
			if (t.fault == "get") { return ENODATA; }
			auto& e = static_cast<Port*>(buffer)->events.at(index); result->time = e.time; result->size = e.bytes.size(); result->buffer = e.bytes.data();
			if (index == 1 && t.fault == "null-bytes") { result->buffer = nullptr; }
			return 0;
		};
		api.clearMidiBuffer = [](void* raw)
		{
			auto& p = *static_cast<Port*>(raw);
			if (!p.output || !p.registered || p.client->closed) { ++current->badAccess; }
			p.events.clear();
			if (current->fault == "throw-clear") { throw std::runtime_error("JACK clear output"); }
		};
		api.writeMidiEvent = [](void* raw, jack_nframes_t at, const jack_midi_data_t* bytes, std::size_t size)
		{
			auto& p = *static_cast<Port*>(raw); auto& t = *current;
			if (!p.output || !p.registered || p.client->closed || at >= 256 || (p.events.size() && at < p.events.back().time))
			{ ++t.badAccess; return EINVAL; }
			if (t.fault == "throw-write" && !p.events.empty()) { throw std::runtime_error("JACK write output"); }
			if (p.events.size() >= t.outputCapacity || (t.fault == "write" && !p.events.empty())) { return ENOBUFS; }
			p.events.push_back({at, std::vector<unsigned char>(bytes, bytes + size)}); t.outputHistory.push_back(p.events.back()); return 0;
		};
		api.frameTime = [](const jack_client_t* raw) -> jack_nframes_t
		{
			if (current->onOutputClock) { current->onOutputClock(); }
			if (current->fault == "throw-clock") { throw std::runtime_error("JACK frame time"); }
			return reinterpret_cast<const Client*>(raw)->clock;
		};
		api.lastFrameTime = [](const jack_client_t* raw) -> jack_nframes_t
		{
			if (current->fault == "throw-cycle-clock") { throw std::runtime_error("JACK cycle time"); }
			return reinterpret_cast<const Client*>(raw)->clock;
		};
	}
	~Transport() { for (auto& c : clients) { c->join(); } current = nullptr; }
	Client& latest() { return *clients.back(); }
};
class Processor : public MidiEventProcessor
{
public:
	std::vector<MidiEvent> events;
	std::vector<QThread*> threads;
	void processInEvent(const MidiEvent& event, const TimePos&, f_cnt_t) override { events.push_back(event); threads.push_back(QThread::currentThread()); }
	void processOutEvent(const MidiEvent&, const TimePos&, f_cnt_t) override { }
};
class EngineMidiScope
{
	MidiClient* previous;
public:
	explicit EngineMidiScope(MidiClient* client) : previous(MidiJackTestAccess::replaceEngineClient(client)) { }
	~EngineMidiScope() { MidiJackTestAccess::replaceEngineClient(previous); }
};
QJsonObject graphRequest(const QString& id, const QString& op, const QJsonObject& args)
{ return {{"id", id}, {"op", op}, {"args", args}}; }
AudioJack* installAudio(Transport& transport)
{
	auto candidate = AudioJackTestAccess::create(transport.api); const auto audio = candidate.get();
	Engine::audioEngine()->setAudioDevice(candidate.release(), false); return audio;
}
class NativeBusPlay final : public PlayHandle
{
public:
	explicit NativeBusPlay(SampleTrack* track) : PlayHandle(Type::SamplePlayHandle), m_track(track)
	{ setAudioBusHandle(track->audioBusHandle()); }
	std::atomic<int> calls{0};
	std::atomic_bool failing{false};
	bool ramp = false;
	static float value(f_cnt_t frame) { return static_cast<float>(frame % 1009 + 1) / 8192.f; }
	bool isFinished() const override { return false; }
	bool affinityMatters() const override { return true; }
	bool isFromTrack(const Track* track) const override { return track == m_track; }
	void play(SampleFrame* output) override
	{
		++calls;
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		for (f_cnt_t frame = 0; frame < frames; ++frame)
		{
			const auto v = ramp ? value(m_produced + frame) : .25f;
			output[frame] = SampleFrame(v, -v / 2.f);
		}
		m_produced += frames;
		if (failing) { throw std::runtime_error("Native JACK bus DSP fixture"); }
	}
private:
	SampleTrack* m_track;
	f_cnt_t m_produced = 0;
};
class PreviewContainer final : public TrackContainer
{
public:
	PreviewContainer() { setType(Type::Song); }
	QString nodeName() const override { return "preview-fixture"; }
};
SampleTrack* busTrack(TrackContainer* container = nullptr)
{ return static_cast<SampleTrack*>(Track::create(Track::Type::Sample, container ? container : Engine::getSong())); }
std::array<Port*, 2> busPorts(Transport& transport, const AudioDevice::BusGraph& graph)
{
	if (graph.graph.channels.size() != 2) { throw std::runtime_error("Expected two published native bus ports"); }
	return {transport.endpoint(graph.graph.channels[0].localPort.toUtf8()), transport.endpoint(graph.graph.channels[1].localPort.toUtf8())};
}
bool waitFlag(const std::atomic_bool& flag)
{
	QElapsedTimer wait; wait.start();
	while (!flag.load() && wait.elapsed() < 2000) { std::this_thread::yield(); }
	return flag.load();
}
QJsonObject graphDecision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output;
	for (const auto& value : calls)
	{
		const auto item = value.toObject(); const auto args = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact));
		if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", args}}); }
		else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", args}}}}); }
	}
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
}

class JackLifetimeTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); NotePlayHandleManager::init(); }
	void cleanupTestCase() { NotePlayHandleManager::free(); }
	void init()
	{
		ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); Engine::getSong()->clearProject();
		for (const auto key : {"input1", "input2", "output1", "output2"}) { ConfigManager::inst()->setValue("audiojack", key, "-"); }
	}
	void cleanup() { if (Engine::audioEngine()) { Engine::destroy(); } }

	void standaloneAdmissionOwnsEveryPartialResource_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto f : {"none", "open", "process", "port", "activate", "throw-open", "throw-process", "throw-port", "throw-activate", "shutdown-activate", "output-port", "throw-output-port"})
		{ QTest::newRow(f) << QString(f); }
	}
	void standaloneAdmissionOwnsEveryPartialResource()
	{
		QFETCH(QString, fault); Transport t; t.fault = fault; auto midi = MidiJackTestAccess::create(nullptr, t.api);
		QVERIFY(midi->inputBackendStatus().observed); QCOMPARE(midi->isAvailable(), fault == "none");
		QCOMPARE(midi->inputBackendStatus().active, fault == "none"); QCOMPARE(t.midiReads.load(), 0);
		if (fault != "none") { QVERIFY(midi->inputBackendStatus().nativeErrorCode != 0); }
		midi.reset(); QCOMPARE(t.closes.load(), static_cast<int>(t.clients.size())); QCOMPARE(t.badAccess.load(), 0);
	}

	void sharedAttachPublishesOnlyPreparedPortsAndDoesNotActivateAudio_data()
	{ QTest::addColumn<bool>("running"); QTest::newRow("prepared") << false; QTest::newRow("active") << true; }
	void sharedAttachPublishesOnlyPreparedPortsAndDoesNotActivateAudio()
	{
		QFETCH(bool, running); Transport t; auto audio = AudioJackTestAccess::create(t.api); if (running) { audio->startProcessing(); }
		t.onMidiRegister = [&] { t.latest().tick(); };
		auto midi = MidiJackTestAccess::create(audio.get(), t.api); QVERIFY(midi->isAvailable()); QCOMPARE(t.midiReads.load(), 0);
		QCOMPARE(t.activates.load(), running ? 1 : 0); QCOMPARE(midi->inputBackendStatus().active, running);
		if (!running) { audio->startProcessing(); }
		t.latest().tick(); QCOMPARE(t.midiReads.load(), 1); QCOMPARE(midi->sharedAudioDevice(), audio.get());
		midi.reset(); QCOMPARE(t.unregisters.load(), 2); QCOMPARE(t.closes.load(), 0); QVERIFY(audio->isRunning());
		t.latest().tick(); QCOMPARE(t.midiReads.load(), 1); audio.reset(); QCOMPARE(t.closes.load(), 1); QCOMPARE(t.badAccess.load(), 0);
	}

	void nativePacketsReachTheOwningMidiProcessorAfterCallbackStorageExpires()
	{
		Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor; MidiPort port("JACK input", midi.get(), &processor); port.setReadable(true);
		auto input = t.latest().midiPort(); QVERIFY(input);
		input->events = {{0, {0x82, 60, 80}}, {1, {0x92, 61, 90}}, {2, {0xa2, 62, 70}}, {3, {0xb2, 74, 99}},
			{4, {0xc2, 17}}, {5, {0xd2, 55}}, {6, {0xe2, 0x7f, 0x7f}}, {7, {0xf0, 1, 2, 0xf7}}, {8, {0xf8}}};
		t.latest().launch(); t.latest().join(); input->events.clear(); QVERIFY(processor.events.empty());
		QTRY_COMPARE(processor.events.size(), std::size_t{7});
		for (int i = 0; i < 7; ++i) { QCOMPARE(int(processor.events[i].type()), 0x80 + 0x10 * i); QCOMPARE(processor.events[i].channel(), 2); QCOMPARE(processor.threads[i], QThread::currentThread()); }
		QCOMPARE(processor.events[1].key(), 61); QCOMPARE(processor.events[1].velocity(), 90); QCOMPARE(processor.events[6].pitchBend(), 16383);
	}

	void malformedNativeBlocksCannotPublishAValidPrefix_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto f : {"null-buffer", "lost", "count", "get", "empty", "null-bytes", "time", "order", "running-status", "data-status", "size", "throw-buffer", "throw-event"})
		{ QTest::newRow(f) << QString(f); }
	}
	void malformedNativeBlocksCannotPublishAValidPrefix()
	{
		QFETCH(QString, fault); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor; MidiPort port("invalid JACK", midi.get(), &processor); port.setReadable(true);
		auto p = t.latest().midiPort(); p->events = {{10, {0x90, 60, 100}}, {11, {0x90, 61, 100}}}; t.fault = fault;
		auto& bad = p->events.back();
		if (fault == "empty") { bad.bytes.clear(); } if (fault == "time") { bad.time = 256; } if (fault == "order") { bad.time = 9; }
		if (fault == "running-status") { bad.bytes[0] = 60; } if (fault == "data-status") { bad.bytes[1] = 0x80; } if (fault == "size") { bad.bytes.push_back(1); }
		t.latest().launch(); t.latest().join(); QVERIFY(!midi->isAvailable()); QVERIFY(midi->inputBackendStatus().nativeErrorCode != 0);
		MidiJackTestAccess::poll(*midi); QCoreApplication::processEvents();
		for (const auto& event : processor.events) { QVERIFY(event.type() != MidiNoteOn); }
		QVERIFY(midi->inputDeliveryStatus(&port).failed); t.fault.clear(); MidiJackTestAccess::poll(*midi); QVERIFY(midi->isAvailable());
		QString error; bool changed = false; QVERIFY2(midi->resetInputDelivery(&port, error, changed), qPrintable(error)); QVERIFY(changed);
		processor.events.clear(); t.latest().midiPort()->events = {{0, {0x90, 64, 100}}}; t.latest().launch(); t.latest().join();
		QTRY_COMPARE(processor.events.size(), std::size_t{1}); QCOMPARE(processor.events.front().key(), 64); QCOMPARE(t.badAccess.load(), 0);
	}

	void midiDestructionJoinsItsReaderBeforeUnregistering()
	{
		Transport t; auto audio = AudioJackTestAccess::create(t.api); auto midi = MidiJackTestAccess::create(audio.get(), t.api); audio->startProcessing();
		std::atomic_bool entered{false}, release{false}, observed{false}; std::thread helper;
		t.onMidiBuffer = [&] { entered = true; while (!release.load()) { QThread::msleep(1); } };
		const auto cleanup = qScopeGuard([&] { release = true; t.latest().join(); if (helper.joinable()) { helper.join(); } });
		t.latest().launch(); QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 2000); const auto raw = midi.get();
		helper = std::thread([&]
		{
			QElapsedTimer deadline; deadline.start(); while (!MidiJackTestAccess::paused(*raw) && deadline.elapsed() < 2000) { QThread::msleep(1); }
			observed = MidiJackTestAccess::paused(*raw) && t.unregisters.load() == 0; release = true;
		});
		midi.reset(); helper.join(); t.latest().join(); QVERIFY(observed.load()); QCOMPARE(t.unregisters.load(), 2); QCOMPARE(t.badAccess.load(), 0);
	}

	void midiDetachDoesNotWaitForUnrelatedNativeDsp()
	{
		Transport t; auto audio = AudioJackTestAccess::create(t.api); auto midi = MidiJackTestAccess::create(audio.get(), t.api); audio->startProcessing();
		std::atomic_bool entered{false}, release{false};
		const auto observed = connect(Engine::audioEngine(), &AudioEngine::nextAudioBuffer, Engine::audioEngine(), [&](const SampleFrame*)
		{ entered = true; while (!release.load()) { QThread::msleep(1); } }, Qt::DirectConnection);
		const auto cleanup = qScopeGuard([&] { release = true; t.latest().join(); QObject::disconnect(observed); });
		t.latest().launch(); QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 2000);
		midi.reset(); QCOMPARE(t.unregisters.load(), 2); QVERIFY(!release.load()); release = true; t.latest().join(); QCOMPARE(t.badAccess.load(), 0);
	}

	void sharedShutdownRebuildsPortsOrKeepsFailureRetryable_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto f : {"none", "open", "audio-port", "midi-port", "activate", "rate", "throw-open", "throw-midi-port", "output-port", "throw-output-port"}) { QTest::newRow(f) << QString(f); }
	}
	void sharedShutdownRebuildsPortsOrKeepsFailureRetryable()
	{
		QFETCH(QString, fault); Transport t; auto audio = AudioJackTestAccess::create(t.api); auto midi = MidiJackTestAccess::create(audio.get(), t.api); audio->startProcessing();
		Processor processor; MidiPort port("reconnected JACK", midi.get(), &processor); port.setReadable(true);
		const auto priorClient = midi->jackClient(); const auto priorPort = t.latest().midiPort(); const auto rate = audio->sampleRate();
		QSignalSpy notice(audio.get(), &AudioJack::zombified); t.latest().disconnect();
		QCOMPARE(t.closes.load(), 0); QCOMPARE(midi->jackClient(), priorClient); QVERIFY(!midi->isAvailable()); QCOMPARE(notice.count(), 0);
		AudioJackTestAccess::poll(*audio); QVERIFY(!audio->isRunning()); t.fault = fault;
		try { audio->startProcessing(); } catch (const std::exception&) { }
		if (fault != "none") { QVERIFY(!audio->isRunning()); QVERIFY(audio->serverDisconnected()); t.fault.clear(); audio->startProcessing(); }
		QVERIFY(audio->isRunning()); QVERIFY(midi->isAvailable()); QCOMPARE(midi->sharedAudioDevice(), audio.get());
		QVERIFY(midi->jackClient() != priorClient); QVERIFY(t.latest().midiPort() != priorPort); QVERIFY(!priorPort->registered);
		QCOMPARE(audio->sampleRate(), rate); QCOMPARE(t.latest().options, JackNoStartServer);
		QString error; bool changed = false; QVERIFY2(midi->resetInputDelivery(&port, error, changed), qPrintable(error)); QVERIFY(changed);
		processor.events.clear(); t.latest().midiPort()->events = {{0, {0x90, 67, 100}}}; t.latest().launch(); t.latest().join();
		QTRY_COMPARE(processor.events.size(), std::size_t{1}); QCOMPARE(processor.events.front().key(), 67); QCOMPARE(t.badAccess.load(), 0);
	}

	void standaloneShutdownDrainsLateOldNotifications_data()
	{ QTest::addColumn<QString>("fault"); for (const auto f : {"none", "open", "activate"}) { QTest::newRow(f) << QString(f); } }
	void standaloneShutdownDrainsLateOldNotifications()
	{
		QFETCH(QString, fault); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor; MidiPort port("standalone recovery", midi.get(), &processor); port.setReadable(true);
		const auto name = t.names.front(); ConfigManager::inst()->setValue("midijack", "lmms", "later-configuration");
		t.latest().disconnect(); QVERIFY(!midi->isAvailable()); QCOMPARE(t.closes.load(), 0); t.fault = fault; t.lateCloseShutdown = true;
		MidiJackTestAccess::poll(*midi); if (fault != "none") { QVERIFY(!midi->isAvailable()); t.fault.clear(); MidiJackTestAccess::poll(*midi); }
		QVERIFY(midi->isAvailable()); QCOMPARE(t.names.back(), name); QString error; bool changed = false; QVERIFY2(midi->resetInputDelivery(&port, error, changed), qPrintable(error));
		const auto version = midi->inputDeliveryStatus(&port).failureVersion; MidiJackTestAccess::poll(*midi);
		QVERIFY(!midi->inputDeliveryStatus(&port).failed); QCOMPARE(midi->inputDeliveryStatus(&port).failureVersion, version); QCOMPARE(t.badAccess.load(), 0);
	}

	void recoveryTimerPreservesStoppedAndReservedDevices_data()
	{ QTest::addColumn<bool>("reserved"); QTest::newRow("stopped") << false; QTest::newRow("export-reservation") << true; }
	void recoveryTimerPreservesStoppedAndReservedDevices()
	{
		QFETCH(bool, reserved); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		auto candidate = AudioJackTestAccess::create(t.api); auto audio = candidate.get(); engine->setAudioDevice(candidate.release(), false);
		auto midi = MidiJackTestAccess::create(audio, t.api); audio->startProcessing(); if (reserved) { QVERIFY(engine->storeAudioDevice()); } else { audio->stopProcessing(); }
		t.latest().disconnect(); AudioJackTestAccess::poll(*audio); QCOMPARE(t.opens.load(), 1); QVERIFY(!audio->isRunning());
		if (reserved) { QVERIFY(engine->restoreAudioDevice()); }
		audio->startProcessing(); QVERIFY(audio->isRunning()); QVERIFY(midi->isAvailable()); QCOMPARE(t.opens.load(), 2);
	}

	void retiringAudioInvalidatesALiveSharedMidiOwner()
	{
		Transport t; auto audio = AudioJackTestAccess::create(t.api); auto midi = MidiJackTestAccess::create(audio.get(), t.api); audio->startProcessing();
		audio.reset(); QVERIFY(!midi->sharedAudioDevice()); QVERIFY(!midi->isAvailable()); QVERIFY(!midi->inputBackendStatus().active);
		MidiJackTestAccess::poll(*midi); QCOMPARE(t.opens.load(), 1); QCOMPARE(t.closes.load(), 1); midi.reset(); QCOMPARE(t.closes.load(), 1); QCOMPARE(t.badAccess.load(), 0);
	}

	void ownerTimerRetriesWithoutSpinningAndRestoresSharedMidi()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		auto candidate = AudioJackTestAccess::create(t.api); auto audio = candidate.get(); engine->setAudioDevice(candidate.release(), false);
		auto midi = MidiJackTestAccess::create(audio, t.api); audio->startProcessing(); t.latest().disconnect(); t.fault = "open";
		AudioJackTestAccess::poll(*audio); QVERIFY(!audio->isRunning()); QCOMPARE(t.opens.load(), 2); QCOMPARE(t.closes.load(), 1);
		AudioJackTestAccess::poll(*audio, false); QCOMPARE(t.opens.load(), 2);
		t.fault.clear(); AudioJackTestAccess::poll(*audio); QVERIFY(audio->isRunning()); QVERIFY(midi->isAvailable());
		QCOMPARE(t.opens.load(), 3); QCOMPARE(t.latest().options, JackNoStartServer); QCOMPARE(t.badAccess.load(), 0);
	}

	void sharedMidiFaultRecoveryKeepsTheAudioClientAndDspActive()
	{
		Transport t; auto audio = AudioJackTestAccess::create(t.api); auto midi = MidiJackTestAccess::create(audio.get(), t.api); audio->startProcessing();
		Processor processor; MidiPort port("MIDI-only recovery", midi.get(), &processor); port.setReadable(true);
		const auto previousPort = t.latest().midiPort(); t.fault = "lost"; t.latest().launch(); t.latest().join();
		QVERIFY(audio->isRunning()); QVERIFY(!midi->isAvailable()); t.fault = "midi-port"; MidiJackTestAccess::poll(*midi);
		QVERIFY(!midi->isAvailable()); QVERIFY(audio->isRunning()); QCOMPARE(t.unregisters.load(), 2); QCOMPARE(t.closes.load(), 0);
		t.fault.clear(); MidiJackTestAccess::poll(*midi); QVERIFY(midi->isAvailable()); QVERIFY(t.latest().midiPort() != previousPort);
		QCOMPARE(t.opens.load(), 1); QCOMPARE(t.activates.load(), 1);
		QString error; bool changed = false; QVERIFY2(midi->resetInputDelivery(&port, error, changed), qPrintable(error));
		processor.events.clear(); t.latest().midiPort()->events = {{0, {0x90, 55, 100}}}; t.latest().launch(); t.latest().join();
		QTRY_COMPARE(processor.events.size(), std::size_t{1}); QCOMPARE(processor.events.front().key(), 55); QCOMPARE(t.badAccess.load(), 0);
	}

	void shutdownDuringSharedRegistrationCannotAdvertiseHealthyInput()
	{
		Transport t; auto audio = AudioJackTestAccess::create(t.api); audio->startProcessing();
		t.onMidiRegister = [&] { t.latest().disconnect(); };
		auto midi = MidiJackTestAccess::create(audio.get(), t.api); QVERIFY(!midi->isAvailable()); QVERIFY(!midi->inputBackendStatus().active);
		t.onMidiRegister = {}; AudioJackTestAccess::poll(*audio); audio->startProcessing();
		QVERIFY(audio->isRunning()); QVERIFY(midi->isAvailable()); QCOMPARE(t.closes.load(), 1); QCOMPARE(t.badAccess.load(), 0);
	}

	void outputPacketsPreserveFramingAndOrder_data()
	{ QTest::addColumn<bool>("shared"); QTest::newRow("standalone") << false; QTest::newRow("shared") << true; }
	void outputPacketsPreserveFramingAndOrder()
	{
		QFETCH(bool, shared); Transport t; auto audio = shared ? AudioJackTestAccess::create(t.api) : nullptr;
		auto midi = MidiJackTestAccess::create(audio.get(), t.api); if (audio) { audio->startProcessing(); }
		Processor processor; MidiPort port("output framing", midi.get(), &processor);
		const std::array<MidiEvent, 8> events{MidiEvent(MidiNoteOff, 2, 60, 81), MidiEvent(MidiNoteOn, 2, 61, 91),
			MidiEvent(MidiKeyPressure, 2, 62, 71), MidiEvent(MidiControlChange, 2, 74, 99), MidiEvent(MidiProgramChange, 2, 17),
			MidiEvent(MidiChannelPressure, 2, 55), MidiEvent(MidiPitchBend, 2, 16383), MidiEvent(MidiNoteOn, 2, 63, 0)};
		for (const auto& event : events) { midi->processOutEvent(event, {}, &port); }
		QCOMPARE(midi->outputBackendStatus().pendingEvents, 8u); t.latest().tick();
		const auto output = t.latest().midiPort(true); QVERIFY(output); QCOMPARE(output->events.size(), std::size_t{8});
		const std::vector<std::vector<unsigned char>> expected{{0x82,60,81},{0x92,61,91},{0xa2,62,71},{0xb2,74,99},{0xc2,17},{0xd2,55},{0xe2,127,127},{0x82,63,0}};
		for (std::size_t index = 0; index < expected.size(); ++index)
		{ QVERIFY(output->events[index].bytes == expected[index]); QCOMPARE(output->events[index].time, jack_nframes_t{0}); }
		QCOMPARE(midi->outputBackendStatus().pendingEvents, 0u); t.latest().tick(); QVERIFY(output->events.empty());
		QVERIFY(midi->outputBackendStatus().active); QCOMPARE(t.badAccess.load(), 0);
	}

	void nativeMidiPortRoutesTrackAndInstrumentOutput_data()
	{ QTest::addColumn<bool>("instrument"); QTest::newRow("track") << false; QTest::newRow("instrument") << true; }
	void nativeMidiPortRoutesTrackAndInstrumentOutput()
	{
		QFETCH(bool, instrument); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor;
		MidiPort port("native output source", midi.get(), &processor); port.setWritable(true); port.setFixedOutputNote(62); port.setFixedOutputVelocity(91);
		const auto source = instrument ? MidiPort::OutputSource::Instrument : MidiPort::OutputSource::Track; port.setOutputSource(source);
		port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, source); t.latest().tick();
		auto output = t.latest().midiPort(true); QCOMPARE(output->events.size(), std::size_t{1});
		QVERIFY(output->events.front().bytes == std::vector<unsigned char>({0x90,62,91})); QCOMPARE(port.trackedOutputNotes(), 1u);
		port.setWritable(false); QCOMPARE(port.trackedOutputNotes(), 0u); t.latest().tick();
		QVERIFY(std::none_of(output->events.begin(), output->events.end(), [](const Event& e) { return (e.bytes[0] & 0xf0) == 0x90; }));
		QCOMPARE(midi->outputBackendStatus().cleanupPendingChannels, 0u);
	}

	void outputSchedulingUsesNativeClockAndSurvivesWrap_data()
	{ QTest::addColumn<QString>("mode"); for (const auto value : {"normal", "wrap", "tempo-change", "late"}) { QTest::newRow(value) << QString(value); } }
	void outputSchedulingUsesNativeClockAndSurvivesWrap()
	{
		QFETCH(QString, mode); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor; MidiPort port("scheduled", midi.get(), &processor);
		Engine::getSong()->setTempo(120);
		if (mode == "wrap") { t.latest().clock = std::numeric_limits<jack_nframes_t>::max() - 199; }
		const auto origin = t.latest().clock.load();
		const auto delay = static_cast<jack_nframes_t>(TimePos(1).getTimeInMilliseconds(120) * t.api.sampleRate(midi->jackClient()) / 1000.0);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 61, 100), TimePos(1), &port);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &port);
		if (mode == "tempo-change") { Engine::getSong()->setTempo(240); }
		if (mode == "late") { t.latest().clock = origin + delay + 1000; }
		std::vector<std::pair<jack_nframes_t, int>> received;
		for (int cycle = 0; cycle < 20 && midi->outputBackendStatus().pendingEvents; ++cycle)
		{
			const auto cycleStart = t.latest().clock.load(); t.latest().tick();
			for (const auto& e : t.latest().midiPort(true)->events) { received.emplace_back(cycleStart + e.time, e.bytes[1]); }
		}
		QCOMPARE(received.size(), std::size_t{2}); QCOMPARE(received[0].second, 60); QCOMPARE(received[1].second, 61);
		if (mode == "late") { QCOMPARE(received[0].first, origin + delay + 1000); QCOMPARE(received[1].first, received[0].first); }
		else { QCOMPARE(received[0].first, origin); QCOMPARE(received[1].first, origin + delay); }
		QCOMPARE(t.badAccess.load(), 0);
	}

	void invalidOutputCannotReachNativeBuffers_data()
	{ QTest::addColumn<QString>("kind"); for (const auto value : {"negative-channel", "channel", "key", "velocity", "bend", "system", "negative-delay", "long-delay"}) { QTest::newRow(value) << QString(value); } }
	void invalidOutputCannotReachNativeBuffers()
	{
		QFETCH(QString, kind); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor; MidiPort port("invalid output", midi.get(), &processor);
		MidiEvent event(MidiNoteOn, 0, 60, 100); TimePos delay;
		if (kind == "negative-channel") { event = MidiEvent(MidiNoteOn, -1, 60, 100); }
		if (kind == "channel") { event = MidiEvent(MidiNoteOn, 16, 60, 100); }
		if (kind == "key") { event = MidiEvent(MidiNoteOn, 0, 128, 100); }
		if (kind == "velocity") { event = MidiEvent(MidiNoteOn, 0, 60, 128); }
		if (kind == "bend") { event = MidiEvent(MidiPitchBend, 0, 16384, 0); }
		if (kind == "system") { event = MidiEvent(static_cast<MidiEventTypes>(0xf0), 0, 1, 2); }
		if (kind == "negative-delay") { delay = TimePos(-1); }
		if (kind == "long-delay") { delay = TimePos(std::numeric_limits<int>::max()); }
		bool rejected = false; try { midi->processOutEvent(event, delay, &port); } catch (const std::exception&) { rejected = true; }
		QCOMPARE(rejected, kind.endsWith("delay")); QCOMPARE(midi->outputBackendStatus().pendingEvents, 0u);
		t.latest().tick(); QVERIFY(t.latest().midiPort(true)->events.empty()); QCOMPARE(t.badAccess.load(), 0);
	}

	void cancellationReleasesDeliveredNotesAndKeepsOtherSources()
	{
		Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor;
		MidiPort a("source a", midi.get(), &processor), b("source b", midi.get(), &processor);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &a); t.latest().tick(); t.latest().tick();
		midi->processOutEvent(MidiEvent(MidiNoteOff, 0, 60, 0), TimePos(48), &a);
		midi->processOutEvent(MidiEvent(MidiProgramChange, 1, 20), {}, &b); midi->cancelOutputEvents(&a);
		QCOMPARE(midi->outputBackendStatus().pendingEvents, 1u); QCOMPARE(midi->outputBackendStatus().cleanupPendingChannels, 1u);
		t.latest().tick(); const auto& events = t.latest().midiPort(true)->events; QCOMPARE(events.size(), std::size_t{5});
		const std::array<int, 4> controls{64,66,123,120};
		for (std::size_t index = 0; index < controls.size(); ++index) { QCOMPARE(int(events[index].bytes[1]), controls[index]); QCOMPARE(events[index].bytes[2], static_cast<unsigned char>(0)); }
		QVERIFY(events.back().bytes == std::vector<unsigned char>({0xc1,20})); QCOMPARE(midi->outputBackendStatus().cleanupPendingChannels, 0u);
	}

	void retiredPortIdentityCannotReplayQueuedNotes()
	{
		Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor;
		alignas(MidiPort) std::array<unsigned char, sizeof(MidiPort)> storage{};
		auto port = new (storage.data()) MidiPort("retired", midi.get(), &processor);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), TimePos(1), port); port->~MidiPort();
		auto replacement = new (storage.data()) MidiPort("replacement", midi.get(), &processor);
		const auto cleanup = qScopeGuard([&] { replacement->~MidiPort(); });
		midi->processOutEvent(MidiEvent(MidiProgramChange, 0, 42, 0), {}, replacement); t.latest().tick();
		QCOMPARE(t.latest().midiPort(true)->events.size(), std::size_t{1}); QCOMPARE(t.latest().midiPort(true)->events.front().bytes[1], static_cast<unsigned char>(42));
		for (int cycle = 0; cycle < 20; ++cycle) { t.latest().tick(); QVERIFY(t.latest().midiPort(true)->events.empty()); }
	}

	void queueOverflowIsReportedAndDrainsPossibleNotes()
	{
		Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor; MidiPort port("overflow", midi.get(), &processor);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &port); t.latest().tick();
		for (unsigned int i = 0; i < MidiJack::OutputQueueLimit; ++i) { midi->processOutEvent(MidiEvent(MidiProgramChange, 0, i % 128, 0), TimePos(48), &port); }
		QCOMPARE(midi->outputBackendStatus().pendingEvents, MidiJack::OutputQueueLimit);
		QVERIFY_EXCEPTION_THROWN(midi->processOutEvent(MidiEvent(MidiNoteOff, 0, 60, 0), {}, &port), std::runtime_error);
		auto status = midi->outputBackendStatus(); QCOMPARE(status.nativeErrorCode, -ENOBUFS); QCOMPARE(status.pendingEvents, 0u); QCOMPARE(status.cleanupPendingChannels, 1u);
		t.latest().tick(); QCOMPARE(t.latest().midiPort(true)->events.size(), std::size_t{4}); status = midi->outputBackendStatus();
		QVERIFY(status.active); QCOMPARE(status.lastNativeErrorCode, -ENOBUFS);
		midi->processOutEvent(MidiEvent(MidiProgramChange, 0, 10, 0), {}, &port); t.latest().tick(); QCOMPARE(t.latest().midiPort(true)->events.size(), std::size_t{1});
	}

	void nativeOutputFailureDiscardsTheCycleAndRecovers_data()
	{ QTest::addColumn<QString>("fault"); for (const auto value : {"null-output-buffer", "throw-output-buffer", "throw-clear", "write", "throw-write", "throw-clock", "throw-cycle-clock"}) { QTest::newRow(value) << QString(value); } }
	void nativeOutputFailureDiscardsTheCycleAndRecovers()
	{
		QFETCH(QString, fault); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor; MidiPort port("output failure", midi.get(), &processor);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &port); t.latest().tick(); t.latest().tick();
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 61, 100), {}, &port); midi->processOutEvent(MidiEvent(MidiNoteOff, 0, 60, 0), {}, &port);
		t.fault = fault;
		if (fault == "throw-clock") { QVERIFY_EXCEPTION_THROWN(midi->processOutEvent(MidiEvent(MidiProgramChange, 0, 1, 0), {}, &port), std::runtime_error); }
		else { t.latest().tick(); }
		QVERIFY(midi->outputBackendStatus().nativeErrorCode != 0); QVERIFY(midi->isAvailable());
		QCOMPARE(midi->outputBackendStatus().pendingEvents, 0u); QVERIFY(t.latest().midiPort(true)->events.empty());
		t.fault.clear(); t.latest().tick(); QCOMPARE(midi->outputBackendStatus().cleanupPendingChannels, 0u); QVERIFY(midi->outputBackendStatus().active);
		QVERIFY(std::none_of(t.latest().midiPort(true)->events.begin(), t.latest().midiPort(true)->events.end(), [](const Event& e) { return (e.bytes[0] & 0xf0) == 0x90; }));
		QVERIFY(midi->outputBackendStatus().lastNativeErrorCode != 0); QCOMPARE(t.badAccess.load(), 0);
	}

	void cleanupProgressSurvivesSmallNativeBuffers_data()
	{ QTest::addColumn<int>("capacity"); QTest::newRow("one") << 1; QTest::newRow("two") << 2; QTest::newRow("three") << 3; }
	void cleanupProgressSurvivesSmallNativeBuffers()
	{
		QFETCH(int, capacity); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor; MidiPort port("small buffer", midi.get(), &processor);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &port); t.latest().tick(); midi->cancelOutputEvents(&port); t.outputCapacity = capacity;
		std::vector<unsigned char> delivered;
		for (int cycle = 0; cycle < 8 && midi->outputBackendStatus().cleanupPendingChannels; ++cycle)
		{ t.latest().tick(); for (const auto& e : t.latest().midiPort(true)->events) { delivered.push_back(e.bytes[1]); } }
		QVERIFY(delivered == std::vector<unsigned char>({64,66,123,120})); QVERIFY(midi->outputBackendStatus().active); QCOMPARE(t.badAccess.load(), 0);
	}

	void callbackContentionClearsBuffersAndDefersDelivery()
	{
		Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor; MidiPort port("contention", midi.get(), &processor);
		midi->processOutEvent(MidiEvent(MidiProgramChange, 0, 10, 0), {}, &port); t.latest().tick();
		midi->processOutEvent(MidiEvent(MidiProgramChange, 0, 11, 0), {}, &port);
		MidiJackTestAccess::withOutputLock(*midi, [&] { t.latest().launch(); t.latest().join(); });
		QVERIFY(t.latest().midiPort(true)->events.empty()); QCOMPARE(midi->outputBackendStatus().pendingEvents, 1u);
		t.latest().tick(); QCOMPARE(t.latest().midiPort(true)->events.front().bytes[1], static_cast<unsigned char>(11));
	}

	void stoppedSharedAudioDrainsCleanupWithoutReplayingNotes_data()
	{ QTest::addColumn<bool>("reserved"); QTest::newRow("stopped") << false; QTest::newRow("export-reservation") << true; }
	void stoppedSharedAudioDrainsCleanupWithoutReplayingNotes()
	{
		QFETCH(bool, reserved); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		auto candidate = AudioJackTestAccess::create(t.api); auto audio = candidate.get(); engine->setAudioDevice(candidate.release(), false);
		auto midi = MidiJackTestAccess::create(audio, t.api); audio->startProcessing(); Processor processor; MidiPort port("stopped output", midi.get(), &processor);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &port); t.latest().tick();
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 61, 100), TimePos(1), &port); t.outputCapacity = 1;
		if (reserved) { QVERIFY(engine->storeAudioDevice()); } else { audio->stopProcessing(); }
		std::vector<unsigned char> cleanup;
		for (int cycle = 0; cycle < 6; ++cycle) { t.latest().tick(); for (const auto& e : t.latest().midiPort(true)->events) { cleanup.push_back(e.bytes[1]); QVERIFY((e.bytes[0] & 0xf0) == 0xb0); } }
		QVERIFY(cleanup == std::vector<unsigned char>({64,66,123,120})); QCOMPARE(midi->outputBackendStatus().pendingEvents, 0u); QVERIFY(!midi->outputBackendStatus().active);
		if (reserved) { QVERIFY(engine->restoreAudioDevice()); } else { audio->startProcessing(); }
		for (int cycle = 0; cycle < 10; ++cycle) { t.latest().tick(); QVERIFY(t.latest().midiPort(true)->events.empty()); }
	}

	void immediateStopRestartCannotReplayThePreviousSchedule_data()
	{ QTest::addColumn<bool>("inFlight"); QTest::newRow("queued") << false; QTest::newRow("in-flight-producer") << true; }
	void immediateStopRestartCannotReplayThePreviousSchedule()
	{
		QFETCH(bool, inFlight);
		Transport t; auto audio = AudioJackTestAccess::create(t.api); auto midi = MidiJackTestAccess::create(audio.get(), t.api); audio->startProcessing();
		Processor processor; MidiPort port("immediate restart", midi.get(), &processor);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &port); t.latest().tick();
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 61, 100), TimePos(1), &port);
		std::atomic_bool entered{false}, release{false}; std::atomic<int> rejected{0}; std::thread producer;
		const auto join = qScopeGuard([&] { release = true; if (producer.joinable()) { producer.join(); } });
		if (inFlight)
		{
			t.onOutputClock = [&] { entered = true; while (!release.load()) { std::this_thread::yield(); } };
			producer = std::thread([&] { try { midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 63, 100), {}, &port); } catch (...) { ++rejected; } });
			QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 2000);
		}
		audio->stopProcessing();
		QCOMPARE(midi->outputBackendStatus().pendingEvents, 0u); QCOMPARE(midi->outputBackendStatus().cleanupPendingChannels, 1u);
		QVERIFY_EXCEPTION_THROWN(midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 62, 100), {}, &port), std::runtime_error);
		audio->startProcessing();
		release = true; if (producer.joinable()) { producer.join(); } t.onOutputClock = {}; QCOMPARE(rejected.load(), inFlight ? 1 : 0);
		for (int cycle = 0; cycle < 20; ++cycle)
		{ t.latest().tick(); for (const auto& e : t.latest().midiPort(true)->events) { QVERIFY((e.bytes[0] & 0xf0) != 0x90); } }
		QVERIFY(midi->outputBackendStatus().active); QCOMPARE(t.badAccess.load(), 0);
	}

	void reconnectDiscardsOldOutputAndRebuildsItsNativePort_data()
	{ QTest::addColumn<bool>("shared"); QTest::newRow("standalone") << false; QTest::newRow("shared") << true; }
	void reconnectDiscardsOldOutputAndRebuildsItsNativePort()
	{
		QFETCH(bool, shared); Transport t; auto audio = shared ? AudioJackTestAccess::create(t.api) : nullptr;
		auto midi = MidiJackTestAccess::create(audio.get(), t.api); if (audio) { audio->startProcessing(); }
		Processor processor; MidiPort port("reconnect output", midi.get(), &processor);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &port); t.latest().tick();
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 61, 100), TimePos(1), &port); const auto oldOutput = t.latest().midiPort(true); t.latest().disconnect();
		QVERIFY(!midi->outputBackendStatus().active); QVERIFY_EXCEPTION_THROWN(midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 62, 100), {}, &port), std::runtime_error);
		if (audio) { AudioJackTestAccess::poll(*audio); audio->startProcessing(); } else { MidiJackTestAccess::poll(*midi); }
		QVERIFY(midi->isAvailable()); QVERIFY(!oldOutput->registered); QVERIFY(t.latest().midiPort(true) != oldOutput);
		QCOMPARE(midi->outputBackendStatus().pendingEvents, 0u); t.latest().tick();
		QVERIFY(std::none_of(t.latest().midiPort(true)->events.begin(), t.latest().midiPort(true)->events.end(), [](const Event& e) { return (e.bytes[0] & 0xf0) == 0x90; }));
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 63, 100), {}, &port); t.latest().tick();
		QCOMPARE(t.latest().midiPort(true)->events.back().bytes[1], static_cast<unsigned char>(63)); QCOMPARE(t.badAccess.load(), 0);
	}

	void failedSharedOutputRegistrationRetiresItsInputPort_data()
	{ QTest::addColumn<QString>("fault"); QTest::newRow("null") << QString("output-port"); QTest::newRow("exception") << QString("throw-output-port"); }
	void failedSharedOutputRegistrationRetiresItsInputPort()
	{
		QFETCH(QString, fault); Transport t; auto audio = AudioJackTestAccess::create(t.api); audio->startProcessing(); t.fault = fault;
		auto midi = MidiJackTestAccess::create(audio.get(), t.api); QVERIFY(!midi->isAvailable()); QCOMPARE(t.unregisters.load(), 1);
		QVERIFY(!t.latest().midiPort()); QVERIFY(!t.latest().midiPort(true)); QVERIFY(audio->isRunning()); midi.reset(); QCOMPARE(t.badAccess.load(), 0);
	}

	void failedUnregistrationClosesTheOwningClientBeforeRecovery_data()
	{ QTest::addColumn<QString>("fault"); for (const auto value : {"unregister-input", "unregister-output", "throw-unregister-input", "throw-unregister-output"}) { QTest::newRow(value) << QString(value); } }
	void failedUnregistrationClosesTheOwningClientBeforeRecovery()
	{
		QFETCH(QString, fault); Transport t; auto audio = AudioJackTestAccess::create(t.api); auto midi = MidiJackTestAccess::create(audio.get(), t.api); audio->startProcessing();
		const auto input = t.latest().midiPort(), output = t.latest().midiPort(true);
		t.fault = "lost"; t.latest().tick(); t.fault = fault; MidiJackTestAccess::poll(*midi);
		QVERIFY(!audio->isRunning()); QVERIFY(audio->serverDisconnected()); QCOMPARE(t.closes.load(), 1); QVERIFY(!input->registered); QVERIFY(!output->registered);
		t.fault.clear(); audio->startProcessing(); QVERIFY(audio->isRunning()); QVERIFY(midi->isAvailable());
		QVERIFY(t.latest().midiPort() != input); QVERIFY(t.latest().midiPort(true) != output); QCOMPARE(t.badAccess.load(), 0);
	}

	void simultaneousProducersKeepPacketsWhole()
	{
		Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); Processor processor; MidiPort a("thread a", midi.get(), &processor), b("thread b", midi.get(), &processor);
		std::atomic<int> failures{0};
		const auto send = [&](MidiPort* port, int channel) { try { for (int i = 0; i < 256; ++i) { midi->processOutEvent(MidiEvent(MidiControlChange, channel, 74, i % 128), {}, port); } } catch (...) { ++failures; } };
		std::thread first(send, &a, 0), second(send, &b, 1); first.join(); second.join(); QCOMPARE(failures.load(), 0); t.latest().tick();
		const auto& events = t.latest().midiPort(true)->events; QCOMPARE(events.size(), std::size_t{512}); std::array<int, 2> counts{};
		for (const auto& e : events) { QCOMPARE(e.bytes.size(), std::size_t{3}); QCOMPARE(e.bytes[1], static_cast<unsigned char>(74)); const auto channel = e.bytes[0] & 0xf; QVERIFY(channel < 2); QCOMPARE(int(e.bytes[2]), counts[channel]++ % 128); }
		QCOMPARE(counts[0], 256); QCOMPARE(counts[1], 256);
	}

	void outputDestructionJoinsNativeBufferUsers()
	{
		Transport t; auto audio = AudioJackTestAccess::create(t.api); auto midi = MidiJackTestAccess::create(audio.get(), t.api); audio->startProcessing();
		std::atomic_bool entered{false}, release{false}, joinedBeforeRetirement{false};
		t.onOutputBuffer = [&] { entered = true; while (!release.load()) { std::this_thread::yield(); } };
		const auto cleanup = qScopeGuard([&] { release = true; t.latest().join(); }); t.latest().launch(); QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 2000);
		const auto raw = midi.get(); std::thread helper([&] { while (!MidiJackTestAccess::paused(*raw)) { std::this_thread::yield(); } joinedBeforeRetirement = t.unregisters.load() == 0; release = true; });
		midi.reset(); helper.join(); t.latest().join(); QVERIFY(joinedBeforeRetirement.load()); QCOMPARE(t.unregisters.load(), 2); QCOMPARE(t.badAccess.load(), 0);
	}

	void graphDiscoveryFiltersDirectionsAndPagesNativeNames_data()
	{ QTest::addColumn<bool>("shared"); QTest::newRow("standalone") << false; QTest::newRow("shared") << true; }
	void graphDiscoveryFiltersDirectionsAndPagesNativeNames()
	{
		QFETCH(bool, shared); Transport t; auto audio = shared ? AudioJackTestAccess::create(t.api) : nullptr;
		auto midi = MidiJackTestAccess::create(audio.get(), t.api); if (audio) { audio->startProcessing(); } EngineMidiScope selected(midi.get());
		t.addPeer("z-keyboard:out", true); t.addPeer("a-keyboard:out", true); t.addPeer(QString::fromUtf8("synth-\xc3\xa9:in").toUtf8(), false); t.addPeer("audio:in", false, false);
		const auto first = StudioCapabilities::execute("midi.backend.inspect", {{"direction", "input"}, {"limit", 1}});
		QVERIFY(first.value("ok").toBool()); const auto result = first.value("result").toObject(); QVERIFY(result.value("supported").toBool()); QVERIFY(result.value("observed").toBool());
		QCOMPARE(result.value("total").toInt(), 2); QCOMPARE(result.value("ports").toArray()[0].toObject().value("name").toString(), QString("a-keyboard:out")); QVERIFY(result.value("hasMore").toBool());
		const auto second = StudioCapabilities::execute("midi.backend.inspect", {{"direction", "input"}, {"offset", 1}, {"limit", 1}}).value("result").toObject();
		QCOMPARE(second.value("ports").toArray()[0].toObject().value("name").toString(), QString("z-keyboard:out")); QVERIFY(!second.value("hasMore").toBool());
		const auto sinks = midi->inspectBackendGraph(false); QCOMPARE(sinks.endpoints.size(), std::size_t{1}); QVERIFY(sinks.endpoints[0].name.contains(QChar(0xe9)));
		QCOMPARE(t.lists, t.freedLists); QCOMPARE(t.graphChanges, 0);
	}

	void graphDiscoveryFreesNativeListsOnFaults_data()
	{ QTest::addColumn<QString>("fault"); for (const auto value : {"list-exception", "lookup-exception", "ports-limit", "invalid-utf8", "long-name", "name-budget"}) { QTest::newRow(value) << QString(value); } }
	void graphDiscoveryFreesNativeListsOnFaults()
	{
		QFETCH(QString, fault); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); t.addPeer("sink:in", false);
		if (fault == "invalid-utf8") { t.addPeer(QByteArray("bad:") + char(0xff), false); }
		if (fault == "long-name") { t.addPeer(QByteArray(4096, 'x'), false); }
		if (fault == "name-budget") { for (int i = 0; i < 900; ++i) { t.addPeer(QByteArray::number(i) + ":" + QByteArray(300, 'x'), false); } }
		t.fault = fault; QVERIFY_EXCEPTION_THROWN(midi->inspectBackendGraph(false), std::runtime_error); QCOMPARE(t.lists, t.freedLists); QVERIFY(midi->isAvailable());
	}

	void graphRoutesUseTheCorrectDirectionAndAreIdempotent_data()
	{ QTest::addColumn<bool>("input"); QTest::newRow("input") << true; QTest::newRow("output") << false; }
	void graphRoutesUseTheCorrectDirectionAndAreIdempotent()
	{
		QFETCH(bool, input); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); t.addPeer("peer:port", input); EngineMidiScope selected(midi.get());
		const QJsonObject args{{"direction", input ? "input" : "output"}, {"port", "peer:port"}, {"enabled", true}};
		const auto connected = StudioCapabilities::execute("midi.backend.route", args); QVERIFY2(connected.value("ok").toBool(), qPrintable(connected.value("error").toString()));
		const auto local = t.latest().midiPort(!input)->name; const auto edge = input ? std::make_pair(QByteArray("peer:port"), local) : std::make_pair(local, QByteArray("peer:port"));
		QVERIFY(t.connections.count(edge)); QVERIFY(connected.value("result").toObject().value("connectionVerified").toBool());
		const auto repeated = StudioCapabilities::execute("midi.backend.route", args); QVERIFY(repeated.value("ok").toBool()); QVERIFY(!repeated.value("result").toObject().value("changed").toBool()); QCOMPARE(t.graphChanges, 1);
		QVERIFY(midi->inspectBackendGraph(input).endpoints[0].connected); const auto removed = midi->setBackendRoute(input, "peer:port", false);
		QVERIFY(removed.changed); QVERIFY(!removed.connected); QVERIFY(!t.connections.count(edge)); QCOMPARE(t.graphChanges, 2);
		QVERIFY(!StudioCapabilities::mutatesProject("midi.backend.route"));
	}

	void graphRouteRejectsInvalidPeersBeforeNativeMutation_data()
	{ QTest::addColumn<QString>("kind"); for (const auto value : {"missing", "direction", "audio", "own", "empty", "nul", "long", "surrogate"}) { QTest::newRow(value) << QString(value); } }
	void graphRouteRejectsInvalidPeersBeforeNativeMutation()
	{
		QFETCH(QString, kind); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); QString name = "peer:port";
		if (kind == "direction") { t.addPeer(name.toUtf8(), true); } if (kind == "audio") { t.addPeer(name.toUtf8(), false, false); }
		if (kind == "own") { name = QString::fromUtf8(t.latest().midiPort()->name); }
		if (kind == "empty") { name.clear(); } if (kind == "nul") { name += QChar(0); } if (kind == "long") { name = QString(4096, 'x'); }
		if (kind == "surrogate") { name = QString(QChar(0xd800)); }
		QVERIFY_EXCEPTION_THROWN(midi->setBackendRoute(false, name, true), std::invalid_argument); QCOMPARE(t.graphChanges, 0); QVERIFY(midi->isAvailable());
	}

	void graphMutationFailuresVerifyRollbackOrReportUncertainty_data()
	{ QTest::addColumn<QString>("fault"); for (const auto value : {"connect", "connect-after-error", "connect-throw", "connect-unconfirmed", "rollback", "disconnect", "disconnect-after-error", "observe", "shutdown-connect"}) { QTest::newRow(value) << QString(value); } }
	void graphMutationFailuresVerifyRollbackOrReportUncertainty()
	{
		QFETCH(QString, fault); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); t.addPeer("sink:in", false);
		const auto edge = std::make_pair(t.latest().midiPort(true)->name, QByteArray("sink:in")); const bool before = fault.startsWith("disconnect");
		if (before) { t.connections.insert(edge); } t.fault = fault; QString error;
		try { midi->setBackendRoute(false, "sink:in", !before); } catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
		QVERIFY(!error.isEmpty());
		if (fault == "rollback" || fault == "shutdown-connect") { QVERIFY(t.connections.count(edge)); QVERIFY(error.contains("could not be restored")); }
		else { QCOMPARE(t.connections.count(edge) != 0, before); }
		if (fault == "shutdown-connect") { QVERIFY(!midi->isAvailable()); }
	}

	void graphExistingConnectionRaceRequiresPositiveObservation()
	{
		Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); t.addPeer("sink:in", false); t.fault = "exists";
		const auto result = midi->setBackendRoute(false, "sink:in", true); QVERIFY(result.connected); QVERIFY(result.changed); QCOMPARE(t.graphChanges, 1);
	}

	void graphInputChangesReleaseOldDeliveryAndRequireReset()
	{
		Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); t.addPeer("keys:out", true); Processor processor; MidiPort port("input graph", midi.get(), &processor); port.setReadable(true);
		t.latest().midiPort()->events = {{0, {0x90,60,100}}}; t.latest().tick(); QTRY_COMPARE(processor.events.size(), std::size_t{1});
		QVERIFY(midi->setBackendRoute(true, "keys:out", true).changed); QVERIFY(midi->inputDeliveryStatus(&port).failed); QVERIFY(midi->inputBackendStatus().active);
		t.latest().midiPort()->events = {{0, {0x90,61,100}}}; t.latest().tick(); QCoreApplication::processEvents();
		QCOMPARE(static_cast<int>(std::count_if(processor.events.begin(), processor.events.end(), [](const auto& e) { return e.type() == MidiNoteOn; })), 1);
		QString error; bool changed = false; QVERIFY2(midi->resetInputDelivery(&port, error, changed), qPrintable(error)); QVERIFY(changed);
		t.latest().tick(); QCoreApplication::processEvents();
		QCOMPARE(static_cast<int>(std::count_if(processor.events.begin(), processor.events.end(), [](const auto& e) { return e.type() == MidiNoteOn; })), 2);
		QVERIFY(!midi->setBackendRoute(true, "keys:out", true).changed); QVERIFY(!midi->inputDeliveryStatus(&port).failed);
	}

	void graphOutputDisconnectWaitsForSubmittedCleanup_data()
	{ QTest::addColumn<bool>("shared"); QTest::newRow("standalone") << false; QTest::newRow("shared") << true; }
	void graphOutputDisconnectWaitsForSubmittedCleanup()
	{
		QFETCH(bool, shared); Transport t; auto audio = shared ? AudioJackTestAccess::create(t.api) : nullptr;
		auto midi = MidiJackTestAccess::create(audio.get(), t.api); if (audio) { audio->startProcessing(); } t.addPeer("sink:in", false);
		midi->setBackendRoute(false, "sink:in", true); Processor processor; MidiPort port("routed output", midi.get(), &processor);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &port); t.latest().tick(); t.outputHistory.clear();
		std::atomic_bool running{true}; bool advanced = false; t.onDisconnect = [&] { advanced = MidiJackTestAccess::cleanupAdvanced(*midi); };
		t.latest().worker = std::thread([&] { while (running.load()) { t.latest().tick(); std::this_thread::yield(); } });
		const auto join = qScopeGuard([&] { running = false; t.latest().join(); });
		const auto result = midi->setBackendRoute(false, "sink:in", false); running = false; t.latest().join(); QVERIFY(result.changed); QVERIFY(advanced);
		std::vector<unsigned char> controllers; for (const auto& event : t.outputHistory) { if ((event.bytes[0] & 0xf0) == 0xb0) { controllers.push_back(event.bytes[1]); } }
		QVERIFY(controllers == std::vector<unsigned char>({64,66,123,120})); QVERIFY(t.connections.empty());
	}

	void graphCleanupTimeoutAndCancellationKeepTheConnection_data()
	{ QTest::addColumn<bool>("cancelled"); QTest::newRow("timeout") << false; QTest::newRow("cancelled") << true; }
	void graphCleanupTimeoutAndCancellationKeepTheConnection()
	{
		QFETCH(bool, cancelled); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); t.addPeer("sink:in", false); midi->setBackendRoute(false, "sink:in", true);
		Processor processor; MidiPort port("drain refusal", midi.get(), &processor); midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &port); t.latest().tick();
		int checks = 0; QString error;
		try { midi->setBackendRoute(false, "sink:in", false, [&] { return cancelled && ++checks >= 3; }); } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); }
		QVERIFY(error.contains(cancelled ? "cancelled" : "250 ms")); QCOMPARE(t.graphChanges, 1); QCOMPARE(t.connections.size(), std::size_t{1});
		t.latest().tick(); t.latest().tick(); QVERIFY(midi->setBackendRoute(false, "sink:in", false).changed); QVERIFY(t.connections.empty());
	}

	void graphCancellationAfterMutationRestoresTheNativeEdge()
	{
		Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); t.addPeer("sink:in", false);
		QVERIFY_EXCEPTION_THROWN(midi->setBackendRoute(false, "sink:in", true, [&] { return !t.connections.empty(); }), std::runtime_error);
		QVERIFY(t.connections.empty()); QCOMPARE(t.graphChanges, 2);
	}

	void graphForeignAndNestedCallsCannotMutate()
	{
		Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); t.addPeer("sink:in", false); std::atomic<int> refused{0};
		std::thread foreign([&] {
			try { midi->inspectBackendGraph(false); } catch (...) { ++refused; } try { midi->setBackendRoute(false, "sink:in", true); } catch (...) { ++refused; }
			try { midi->setBackendReconnect(false, "sink:in", false); } catch (...) { ++refused; }
		}); foreign.join();
		QCOMPARE(refused.load(), 3); QCOMPARE(t.graphChanges, 0);
		t.onConnect = [&] {
			try { midi->setBackendRoute(false, "sink:in", false); } catch (...) { ++refused; }
			try { midi->setBackendReconnect(false, "sink:in", false); } catch (...) { ++refused; }
		};
		QVERIFY(midi->setBackendRoute(false, "sink:in", true).connected); QCOMPARE(refused.load(), 5); QCOMPARE(t.graphChanges, 1);
	}

	void graphStudioRejectsBusyAndInvalidRequests_data()
	{ QTest::addColumn<QString>("busy"); for (const auto value : {"playing", "paused", "reserved", "recording", "direction", "type", "unknown-field"}) { QTest::newRow(value) << QString(value); } }
	void graphStudioRejectsBusyAndInvalidRequests()
	{
		QFETCH(QString, busy); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); t.addPeer("sink:in", false); EngineMidiScope selected(midi.get());
		const auto song = Engine::getSong(); const auto engine = Engine::audioEngine(); QJsonObject args{{"direction", "output"}, {"port", "sink:in"}, {"enabled", true}};
		const auto cleanup = qScopeGuard([&] { song->stop(); if (engine->hasStoredAudioDevice()) { engine->restoreAudioDevice(); } });
		if (busy == "playing" || busy == "paused") { song->playSong(); if (busy == "paused") { song->togglePause(); } }
		if (busy == "recording") { song->record(); } if (busy == "reserved") { QVERIFY(engine->storeAudioDevice()); }
		if (busy == "direction") { args.insert("direction", "wrong"); } if (busy == "type") { args.insert("enabled", 1); } if (busy == "unknown-field") { args.insert("track", 0); }
		QVERIFY(!StudioCapabilities::execute("midi.backend.route", args).value("ok").toBool()); QCOMPARE(t.graphChanges, 0);
	}

	void graphReceiptsDoNotReconnectAfterSessionOrBackendRestart_data()
	{ QTest::addColumn<bool>("reconnect"); QTest::newRow("session-reopen") << false; QTest::newRow("backend-reconnect") << true; }
	void graphReceiptsDoNotReconnectAfterSessionOrBackendRestart()
	{
		QFETCH(bool, reconnect); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); t.addPeer("sink:in", false); EngineMidiScope selected(midi.get());
		QTemporaryDir dir; QString error; auto studio = std::make_unique<StudioRuntime>(dir.path()); QVERIFY2(studio->open(error), qPrintable(error));
		const auto request = graphRequest("route", "midi.backend.route", {{"direction", "output"}, {"port", "sink:in"}, {"enabled", true}});
		const auto response = studio->process(request); QVERIFY(response.value("ok").toBool()); studio.reset();
		if (reconnect) { t.latest().disconnect(); MidiJackTestAccess::poll(*midi); QVERIFY(midi->isAvailable()); } else { midi->setBackendRoute(false, "sink:in", false); }
		QVERIFY(t.connections.empty()); const auto changes = t.graphChanges;
		studio = std::make_unique<StudioRuntime>(dir.path()); QVERIFY2(studio->open(error), qPrintable(error));
		auto replay = studio->process(request); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, response); QCOMPARE(t.graphChanges, changes); QVERIFY(t.connections.empty());
	}

	void graphScriptedAgentDiscoversRoutesAndObserves_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void graphScriptedAgentDiscoversRoutesAndObserves()
	{
		QFETCH(QString, protocol); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); t.addPeer("sink:in", false); EngineMidiScope selected(midi.get());
		QTemporaryDir dir; QString error; int round = 0;
		const auto call = [](const QString& id, const QString& name, const QJsonObject& args) { return QJsonObject{{"id", id}, {"name", name}, {"args", args}}; };
		StudioRuntime studio(dir.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (++round == 1) { return graphDecision(protocol, {call("discover", "midi_backend_inspect", {{"direction", "output"}})}); }
			if (round == 2) { return graphDecision(protocol, {call("connect", "midi_backend_route", {{"direction", "output"}, {"port", "sink:in"}, {"enabled", true}}),
				call("retain", "midi_backend_reconnect", {{"direction", "output"}, {"port", "sink:in"}, {"enabled", true}}), call("observe", "midi_backend_inspect", {{"direction", "output"}})}); }
			return graphDecision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Native client graph connected, retained and observed"}, {"evidence", QJsonArray{"discover", "connect", "retain", "observe"}}})});
		});
		QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(graphRequest("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-graph-fixture"}}}})).value("ok").toBool());
		QVERIFY(studio.process(graphRequest("chat", "session.chat", {{"text", "Discover MIDI destinations, connect the studio output to sink:in, retain it for reconnect and inspect the connection."}})).value("ok").toBool());
		for (int iteration = 0; iteration < 12; ++iteration) { studio.advanceAgent(); }
		QFile state(QDir(dir.path()).filePath("session.json")); QVERIFY(state.open(QIODevice::ReadOnly));
		QCOMPARE(QJsonDocument::fromJson(state.readAll()).object().value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete"));
		QCOMPARE(round, 3); QCOMPARE(t.graphChanges, 1); const auto endpoint = midi->inspectBackendGraph(false).endpoints[0]; QVERIFY(endpoint.connected && endpoint.reconnect);
	}

	void graphUnsupportedBackendIsExplicit()
	{
		MidiDummy unavailable; EngineMidiScope selected(&unavailable);
		const auto result = StudioCapabilities::execute("midi.backend.inspect", {{"direction", "output"}});
		QVERIFY(result.value("ok").toBool()); QVERIFY(!result.value("result").toObject().value("supported").toBool());
		QVERIFY(!StudioCapabilities::execute("midi.backend.route", {{"direction", "output"}, {"port", "sink:in"}, {"enabled", true}}).value("ok").toBool());
		QVERIFY(!StudioCapabilities::execute("midi.backend.reconnect", {{"direction", "output"}, {"port", "sink:in"}, {"enabled", false}}).value("ok").toBool());
		QVERIFY(!result.value("result").toObject().value("reconnectSupported").toBool());
	}

	void graphReconnectPreferencesRequireObservedEdgesAndBounds_data()
	{
		QTest::addColumn<QString>("kind");
		for (const auto value : {"unconnected", "missing", "direction", "own", "invalid-name", "route-limit", "byte-limit"}) { QTest::newRow(value) << QString(value); }
	}
	void graphReconnectPreferencesRequireObservedEdgesAndBounds()
	{
		QFETCH(QString, kind); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); EngineMidiScope selected(midi.get());
		QString name = "peer:port"; int retained = 0;
		if (kind == "unconnected" || kind == "direction") { t.addPeer(name.toUtf8(), kind == "direction"); }
		if (kind == "own") { name = midi->inspectBackendGraph(false).localPort; }
		if (kind == "invalid-name") { name = QString("bad") + QChar(0) + "port"; }
		if (kind == "route-limit" || kind == "byte-limit")
		{
			retained = kind == "route-limit" ? 64 : 21;
			for (int index = 0; index <= retained; ++index)
			{
				name = "sink" + QString::number(index) + ":" + QString(kind == "byte-limit" ? 3000 : 2, QChar('a'));
				t.addPeer(name.toUtf8(), false); midi->setBackendRoute(false, name, true);
				if (index < retained)
				{
					QVERIFY(midi->setBackendReconnect(false, name, true).changed);
					QVERIFY(!midi->setBackendReconnect(false, name, true).changed);
				}
			}
		}
		const auto changes = t.graphChanges;
		QVERIFY_EXCEPTION_THROWN(midi->setBackendReconnect(false, name, true), std::exception);
		QCOMPARE(t.graphChanges, changes); QCOMPARE(MidiJackTestAccess::retainedCount(*midi), std::size_t(retained));
		QVERIFY(!StudioCapabilities::mutatesProject("midi.backend.reconnect"));
	}

	void graphReconnectRestoresOnlyRetainedRoutes_data()
	{
		QTest::addColumn<bool>("shared"); QTest::addColumn<bool>("input");
		QTest::newRow("standalone-input") << false << true; QTest::newRow("standalone-output") << false << false;
		QTest::newRow("shared-input") << true << true; QTest::newRow("shared-output") << true << false;
	}
	void graphReconnectRestoresOnlyRetainedRoutes()
	{
		QFETCH(bool, shared); QFETCH(bool, input); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); });
		AudioJack* audio = nullptr;
		if (shared) { auto candidate = AudioJackTestAccess::create(t.api); audio = candidate.get(); Engine::audioEngine()->setAudioDevice(candidate.release(), false); }
		auto midi = MidiJackTestAccess::create(audio, t.api); EngineMidiScope selected(midi.get()); if (audio) { audio->startProcessing(); }
		t.addPeer("kept:port", input); t.addPeer("temporary:port", input);
		midi->setBackendRoute(input, "kept:port", true); midi->setBackendRoute(input, "temporary:port", true);
		QVERIFY(midi->setBackendReconnect(input, "kept:port", true).changed); const auto oldLocal = midi->inspectBackendGraph(input).localPort;
		t.latest().disconnect(); if (audio) { AudioJackTestAccess::poll(*audio); audio->startProcessing(); }
		MidiJackTestAccess::expireRoutes(*midi); MidiJackTestAccess::poll(*midi); QVERIFY(midi->isAvailable());
		const auto graph = midi->inspectBackendGraph(input); QVERIFY(graph.observed); QVERIFY(graph.reconnectSupported); QVERIFY(!graph.reconnectDeferred);
		QVERIFY(graph.localPort != oldLocal); QCOMPARE(graph.endpoints.size(), std::size_t{2});
		QVERIFY(graph.endpoints[0].connected); QVERIFY(graph.endpoints[0].reconnect); QVERIFY(!graph.endpoints[0].pending);
		QVERIFY(!graph.endpoints[1].connected); QVERIFY(!graph.endpoints[1].reconnect); QCOMPARE(t.connections.size(), std::size_t{1});
		QCOMPARE(t.badAccess.load(), 0);
		for (std::size_t index = shared ? 1 : 0; index < t.clients.size(); ++index) { QVERIFY(t.clients[index]->options & JackNoStartServer); }
	}

	void graphReconnectMissingEndpointsRemainInspectable_data()
	{ QTest::addColumn<bool>("input"); QTest::newRow("input") << true; QTest::newRow("output") << false; }
	void graphReconnectMissingEndpointsRemainInspectable()
	{
		QFETCH(bool, input); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); EngineMidiScope selected(midi.get());
		auto peer = t.addPeer("kept:port", input); midi->setBackendRoute(input, "kept:port", true); midi->setBackendReconnect(input, "kept:port", true);
		peer->registered = false; t.connections.clear(); MidiJackTestAccess::expireRoutes(*midi); MidiJackTestAccess::poll(*midi);
		const auto response = StudioCapabilities::execute("midi.backend.inspect", {{"direction", input ? "input" : "output"}, {"limit", 1}});
		QVERIFY(response.value("ok").toBool()); const auto graph = response.value("result").toObject(); QCOMPARE(graph.value("total").toInt(), 1);
		const auto endpoint = graph.value("ports").toArray()[0].toObject(); QVERIFY(!endpoint.value("available").toBool());
		QVERIFY(endpoint.value("reconnect").toBool()); QVERIFY(endpoint.value("pending").toBool()); QCOMPARE(endpoint.value("nativeErrorCode").toInt(), -ENOENT);
		peer->registered = true; MidiJackTestAccess::poll(*midi, false); QVERIFY(t.connections.empty());
		MidiJackTestAccess::expireRoutes(*midi); MidiJackTestAccess::poll(*midi, false);
		const auto recovered = midi->inspectBackendGraph(input).endpoints[0]; QVERIFY(recovered.available && recovered.connected && recovered.reconnect && !recovered.pending);
		QCOMPARE(recovered.nativeErrorCode, 0); QCOMPARE(t.graphChanges, 2);
	}

	void graphReconnectDefersForNativeReservations_data()
	{
		QTest::addColumn<QString>("busy");
		for (const auto value : {"playing", "paused", "recording", "sample-armed", "pattern-sample-armed", "automation-armed",
			"automation-reserved", "note-recording", "export-reserved", "stem", "remote-execution", "different-client"}) { QTest::newRow(value) << QString(value); }
	}
	void graphReconnectDefersForNativeReservations()
	{
		QFETCH(QString, busy); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); EngineMidiScope selected(midi.get());
		t.addPeer("sink:in", false); midi->setBackendRoute(false, "sink:in", true); midi->setBackendReconnect(false, "sink:in", true); t.connections.clear();
		const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
		auto container = busy == "pattern-sample-armed" ? static_cast<TrackContainer*>(Engine::patternStore()) : static_cast<TrackContainer*>(song);
		auto track = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, container)); auto sample = static_cast<SampleClip*>(track->createClip(0));
		auto curve = static_cast<AutomationClip*>(song->globalAutomationTrack()->createClip(0));
		std::unique_ptr<TrackRenderScope> selection; std::uint64_t context = 0; MidiDummy other;
		const auto clearAdmission = [&] {
			MidiJackTestAccess::replaceEngineClient(midi.get()); if (context) { RemotePlugin::finishRenderExecution(context); RemotePlugin::endRenderContext(context); context = 0; }
			selection.reset(); curve->setRecording(false); curve->discardRecordingTake(); sample->setRecord(false); track->endNoteRecording(this); song->stop();
			if (engine->hasStoredAudioDevice()) { engine->restoreAudioDevice(); }
		};
		const auto release = qScopeGuard(clearAdmission);
		if (busy == "playing" || busy == "paused") { song->playSong(); if (busy == "paused") { song->togglePause(); } }
		if (busy == "recording") { song->record(); } if (busy == "sample-armed" || busy == "pattern-sample-armed") { sample->setRecord(true); }
		if (busy == "automation-armed") { curve->setRecording(true); } if (busy == "automation-reserved") { curve->reserveRecordingTake(32, 1); }
		if (busy == "note-recording") { QVERIFY(track->beginNoteRecording(this)); } if (busy == "export-reserved") { QVERIFY(engine->storeAudioDevice()); }
		if (busy == "stem") { selection = std::make_unique<TrackRenderScope>(track); } if (busy == "remote-execution") { context = RemotePlugin::beginRenderContext(); QVERIFY(context); }
		if (busy == "different-client") { MidiJackTestAccess::replaceEngineClient(&other); }
		MidiJackTestAccess::expireRoutes(*midi); MidiJackTestAccess::poll(*midi); QCOMPARE(t.graphChanges, 1); QVERIFY(t.connections.empty());
		const auto graph = midi->inspectBackendGraph(false); QVERIFY(graph.reconnectDeferred); QVERIFY(graph.endpoints[0].pending);
		clearAdmission(); MidiJackTestAccess::poll(*midi, false);
		QCOMPARE(t.graphChanges, 2); QVERIFY(midi->inspectBackendGraph(false).endpoints[0].connected);
	}

	void graphReconnectFailuresRetainIntentAndRetry_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto value : {"connect", "connect-after-error", "connect-throw", "connect-unconfirmed", "observe", "lookup-exception", "shutdown-connect"}) { QTest::newRow(value) << QString(value); }
	}
	void graphReconnectFailuresRetainIntentAndRetry()
	{
		QFETCH(QString, fault); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); EngineMidiScope selected(midi.get());
		t.addPeer("sink:in", false); midi->setBackendRoute(false, "sink:in", true); midi->setBackendReconnect(false, "sink:in", true); t.connections.clear();
		t.fault = fault; MidiJackTestAccess::expireRoutes(*midi); MidiJackTestAccess::poll(*midi); t.fault.clear();
		const auto failed = midi->inspectBackendGraph(false); QVERIFY(failed.endpoints[0].reconnect); QVERIFY(failed.endpoints[0].pending);
		QVERIFY(failed.endpoints[0].nativeErrorCode != 0); QCOMPARE(MidiJackTestAccess::retainedCount(*midi), std::size_t{1});
		MidiJackTestAccess::expireRoutes(*midi); MidiJackTestAccess::poll(*midi);
		const auto recovered = midi->inspectBackendGraph(false); QVERIFY(recovered.observed); QVERIFY(recovered.endpoints[0].connected);
		QVERIFY(!recovered.endpoints[0].pending); QCOMPARE(recovered.endpoints[0].nativeErrorCode, 0); QCOMPARE(t.badAccess.load(), 0);
	}

	void graphReconnectInputRemainsLatchedUntilExplicitReset()
	{
		Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); EngineMidiScope selected(midi.get()); t.addPeer("keys:out", true);
		Processor processor; MidiPort port("retained input", midi.get(), &processor); port.setReadable(true);
		midi->setBackendRoute(true, "keys:out", true); midi->setBackendReconnect(true, "keys:out", true);
		QString error; bool changed = false; QVERIFY2(midi->resetInputDelivery(&port, error, changed), qPrintable(error));
		processor.events.clear();
		t.latest().midiPort()->events = {{0, {0x90,60,100}}}; t.latest().tick(); QTRY_COMPARE(processor.events.size(), std::size_t{1});
		t.connections.clear(); MidiJackTestAccess::expireRoutes(*midi); MidiJackTestAccess::poll(*midi);
		QVERIFY(midi->inspectBackendGraph(true).endpoints[0].connected); QVERIFY(midi->inputDeliveryStatus(&port).failed);
		t.latest().midiPort()->events = {{0, {0x90,61,100}}}; t.latest().tick(); QCoreApplication::processEvents();
		QCOMPARE(std::count_if(processor.events.begin(), processor.events.end(), [](const auto& event) { return event.type() == MidiNoteOn; }), std::ptrdiff_t{1});
		QVERIFY2(midi->resetInputDelivery(&port, error, changed), qPrintable(error)); QVERIFY(changed); t.latest().tick(); QCoreApplication::processEvents();
		QCOMPARE(std::count_if(processor.events.begin(), processor.events.end(), [](const auto& event) { return event.type() == MidiNoteOn; }), std::ptrdiff_t{2});
	}

	void graphReconnectOutputCleanupPrecedesNewEdge_data()
	{ QTest::addColumn<bool>("stalled"); QTest::newRow("advancing") << false; QTest::newRow("stalled") << true; }
	void graphReconnectOutputCleanupPrecedesNewEdge()
	{
		QFETCH(bool, stalled); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); EngineMidiScope selected(midi.get()); t.addPeer("sink:in", false);
		midi->setBackendRoute(false, "sink:in", true); midi->setBackendReconnect(false, "sink:in", true); Processor processor; MidiPort port("retained output", midi.get(), &processor);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &port); t.latest().tick();
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 61, 100), TimePos(10000), &port); t.outputHistory.clear(); t.connections.clear();
		std::atomic_bool running{true}; bool advanced = false; t.onConnect = [&] { advanced = MidiJackTestAccess::cleanupAdvanced(*midi); };
		if (!stalled) { t.latest().worker = std::thread([&] { while (running.load()) { t.latest().tick(); std::this_thread::yield(); } }); }
		const auto join = qScopeGuard([&] { running = false; t.latest().join(); });
		MidiJackTestAccess::expireRoutes(*midi); MidiJackTestAccess::poll(*midi); running = false; t.latest().join();
		const auto endpoint = midi->inspectBackendGraph(false).endpoints[0]; QVERIFY(endpoint.reconnect); QCOMPARE(endpoint.connected, !stalled);
		QCOMPARE(advanced, !stalled); QCOMPARE(midi->outputBackendStatus().pendingEvents, 0u);
		if (stalled) { QVERIFY(endpoint.nativeErrorCode != 0); QCOMPARE(t.graphChanges, 1); }
		else
		{
			t.latest().tick(); t.latest().tick();
			std::vector<unsigned char> controllers; for (const auto& event : t.outputHistory) { QVERIFY((event.bytes[0] & 0xf0) == 0xb0); controllers.push_back(event.bytes[1]); }
			QCOMPARE(controllers.size(), std::size_t{68});
			for (std::size_t index = 0; index < controllers.size(); ++index) { QCOMPARE(controllers[index], (std::array<unsigned char, 4>{64,66,123,120}[index % 4])); }
			QCOMPARE(midi->outputBackendStatus().cleanupPendingChannels, 0u); QCOMPARE(t.graphChanges, 2);
		}
	}

	void graphReconnectCleanupBlocksNewNotesUntilAllChannelsAreReleased_data()
	{ QTest::addColumn<int>("capacity"); QTest::newRow("one-event") << 1; QTest::newRow("three-events") << 3; QTest::newRow("full-cleanup") << 64; }
	void graphReconnectCleanupBlocksNewNotesUntilAllChannelsAreReleased()
	{
		QFETCH(int, capacity); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); EngineMidiScope selected(midi.get()); t.addPeer("sink:in", false);
		midi->setBackendRoute(false, "sink:in", true); midi->setBackendReconnect(false, "sink:in", true);
		Processor processor; MidiPort port("recovered output admission", midi.get(), &processor); t.connections.clear();
		MidiJackTestAccess::expireRoutes(*midi); MidiJackTestAccess::poll(*midi); QVERIFY(midi->inspectBackendGraph(false).endpoints[0].connected);
		QCOMPARE(midi->outputBackendStatus().cleanupPendingChannels, 16u); QVERIFY(!midi->outputBackendStatus().active);
		t.outputCapacity = capacity; t.outputHistory.clear();
		for (int cycle = 0; cycle < 65 && midi->outputBackendStatus().cleanupPendingChannels; ++cycle)
		{
			QVERIFY_EXCEPTION_THROWN(midi->processOutEvent(MidiEvent(MidiNoteOn, 2, 60, 100), {}, &port), std::runtime_error);
			midi->processOutEvent(MidiEvent(MidiNoteOff, 2, 60, 0), {}, &port); QCOMPARE(midi->outputBackendStatus().pendingEvents, 0u); t.latest().tick();
		}
		QCOMPARE(t.outputHistory.size(), std::size_t{64}); QVERIFY(midi->outputBackendStatus().active);
		for (std::size_t index = 0; index < t.outputHistory.size(); ++index)
		{
			const auto& event = t.outputHistory[index]; QCOMPARE(event.bytes[0], static_cast<unsigned char>(0xb0 | (index / 4)));
			QCOMPARE(event.bytes[1], (std::array<unsigned char, 4>{64,66,123,120}[index % 4])); QCOMPARE(event.bytes[2], static_cast<unsigned char>(0));
		}
		midi->processOutEvent(MidiEvent(MidiNoteOn, 2, 60, 100), {}, &port); t.latest().tick();
		QCOMPARE(t.outputHistory.size(), std::size_t{65}); QVERIFY(t.outputHistory.back().bytes == std::vector<unsigned char>({0x92,60,100}));
	}

	void graphReconnectForgettingWorksOfflineAndWhilePlaying_data()
	{
		QTest::addColumn<QString>("state");
		for (const auto value : {"offline", "playing", "connected", "failed-disconnect", "successful-disconnect"}) { QTest::newRow(value) << QString(value); }
	}
	void graphReconnectForgettingWorksOfflineAndWhilePlaying()
	{
		QFETCH(QString, state); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); EngineMidiScope selected(midi.get()); t.addPeer("sink:in", false);
		midi->setBackendRoute(false, "sink:in", true); midi->setBackendReconnect(false, "sink:in", true);
		const auto stop = qScopeGuard([] { Engine::getSong()->stop(); });
		if (state == "offline") { t.latest().disconnect(); } if (state == "playing") { Engine::getSong()->playSong(); }
		if (state == "failed-disconnect")
		{
			t.fault = "disconnect"; QVERIFY_EXCEPTION_THROWN(midi->setBackendRoute(false, "sink:in", false), std::runtime_error); t.fault.clear();
			QVERIFY(midi->inspectBackendGraph(false).endpoints[0].reconnect);
		}
		if (state == "successful-disconnect")
		{
			const auto result = midi->setBackendRoute(false, "sink:in", false); QVERIFY(result.reconnectChanged); QVERIFY(!result.reconnect); QVERIFY(t.connections.empty());
		}
		const auto changes = t.graphChanges; const auto edges = t.connections;
		const auto response = StudioCapabilities::execute("midi.backend.reconnect", {{"direction", "output"}, {"port", "sink:in"}, {"enabled", false}});
		QVERIFY(response.value("ok").toBool()); const auto result = response.value("result").toObject();
		QCOMPARE(result.value("changed").toBool(), state != "successful-disconnect"); QVERIFY(!result.value("graphChanged").toBool()); QVERIFY(!result.value("reconnect").toBool());
		QCOMPARE(t.graphChanges, changes); QVERIFY(t.connections == edges); QCOMPARE(MidiJackTestAccess::retainedCount(*midi), std::size_t{0});
		Engine::getSong()->stop(); t.connections.clear(); MidiJackTestAccess::poll(*midi); QCOMPARE(t.graphChanges, changes);
	}

	void graphReconnectThrottlesAttemptsWithoutStarvingOtherEndpoints()
	{
		Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); EngineMidiScope selected(midi.get());
		for (const auto name : {"a:missing", "b:in", "c:in"})
		{ t.addPeer(name, false); midi->setBackendRoute(false, name, true); midi->setBackendReconnect(false, name, true); }
		t.peers[0]->registered = false; t.connections.clear(); MidiJackTestAccess::expireRoutes(*midi);
		MidiJackTestAccess::poll(*midi, false); QCOMPARE(t.graphChanges, 3);
		MidiJackTestAccess::poll(*midi, false); QCOMPARE(t.graphChanges, 4); t.latest().tick(); t.latest().tick();
		MidiJackTestAccess::poll(*midi, false); QCOMPARE(t.graphChanges, 5); t.latest().tick(); t.latest().tick();
		const auto observations = t.graphObservations;
		for (int iteration = 0; iteration < 20; ++iteration) { MidiJackTestAccess::poll(*midi, false); }
		QCOMPARE(t.graphChanges, 5); QCOMPARE(t.graphObservations, observations); const auto graph = midi->inspectBackendGraph(false);
		QVERIFY(graph.endpoints[0].pending); QVERIFY(graph.endpoints[1].connected); QVERIFY(graph.endpoints[2].connected);
	}

	void graphReconnectReceiptsCannotRecreatePreferences_data()
	{ QTest::addColumn<bool>("replaceClient"); QTest::newRow("session-reopen") << false; QTest::newRow("new-native-client") << true; }
	void graphReconnectReceiptsCannotRecreatePreferences()
	{
		QFETCH(bool, replaceClient); Transport t; auto midi = MidiJackTestAccess::create(nullptr, t.api); EngineMidiScope selected(midi.get()); t.addPeer("sink:in", false);
		QTemporaryDir dir; QString error; auto studio = std::make_unique<StudioRuntime>(dir.path()); QVERIFY2(studio->open(error), qPrintable(error));
		QVERIFY(studio->process(graphRequest("connect", "midi.backend.route", {{"direction", "output"}, {"port", "sink:in"}, {"enabled", true}})).value("ok").toBool());
		const auto request = graphRequest("retain", "midi.backend.reconnect", {{"direction", "output"}, {"port", "sink:in"}, {"enabled", true}});
		const auto response = studio->process(request); QVERIFY(response.value("ok").toBool()); QCOMPARE(MidiJackTestAccess::retainedCount(*midi), std::size_t{1}); studio.reset();
		if (replaceClient)
		{
			auto replacement = MidiJackTestAccess::create(nullptr, t.api); MidiJackTestAccess::replaceEngineClient(replacement.get()); midi = std::move(replacement);
		}
		else { midi->setBackendReconnect(false, "sink:in", false); }
		const auto changes = t.graphChanges; studio = std::make_unique<StudioRuntime>(dir.path()); QVERIFY2(studio->open(error), qPrintable(error));
		auto replay = studio->process(request); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, response);
		QCOMPARE(MidiJackTestAccess::retainedCount(*midi), std::size_t{0}); MidiJackTestAccess::poll(*midi); QCOMPARE(t.graphChanges, changes);
	}

	void optionalRealServerAcceptsBothSharedRetirementOrders_data()
	{ QTest::addColumn<bool>("audioFirst"); QTest::newRow("audio-first") << true; QTest::newRow("midi-first") << false; }
	void optionalRealServerAcceptsBothSharedRetirementOrders()
	{
		QFETCH(bool, audioFirst); if (qgetenv("LMMS_TEST_JACK") != "1") { QSKIP("Set LMMS_TEST_JACK=1 with a running JACK server"); }
		QVERIFY(StudioCapabilities::execute("audio.backend.select", {{"name", AudioJack::name()}}).value("ok").toBool());
		auto midi = std::make_unique<MidiJack>(); QVERIFY(midi->isAvailable()); QVERIFY(midi->sharedAudioDevice());
		if (!audioFirst) { midi.reset(); }
		QVERIFY(StudioCapabilities::execute("audio.backend.select", {{"name", AudioDummy::name()}}).value("ok").toBool());
		if (midi) { QVERIFY(!midi->sharedAudioDevice()); QVERIFY(!midi->isAvailable()); }
	}

	void audioGraphDiscoveryFiltersTypesDirectionsAndPagesChannels_data()
	{ QTest::addColumn<bool>("input"); QTest::newRow("sources") << true; QTest::newRow("destinations") << false; }
	void audioGraphDiscoveryFiltersTypesDirectionsAndPagesChannels()
	{
		QFETCH(bool, input); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		t.addPeer(QString::fromUtf8("a:\xe9\x9f\xb3\xe5\xa3\xb0").toUtf8(), input, false); t.addPeer("z:audio", input, false);
		t.addPeer("wrong:direction", !input, false); t.addPeer("midi:port", input); t.addPeer("retired:audio", input, false)->registered = false;
		audio->setBackendRoute(input, 1, QString::fromUtf8("a:\xe9\x9f\xb3\xe5\xa3\xb0"), true);
		ConfigManager::inst()->setValue("audiojack", input ? "input1" : "output1", QString(5000, QChar('a')));
		const auto response = StudioCapabilities::execute("audio.backend.inspect", {{"direction", input ? "input" : "output"}, {"limit", 1}});
		QVERIFY(response.value("ok").toBool()); const auto graph = response.value("result").toObject(); QCOMPARE(graph.value("scope").toString(), QString("master"));
		QVERIFY(graph.value("supported").toBool()); QVERIFY(graph.value("observed").toBool()); QVERIFY(!graph.value("deviceReceiptVerified").toBool());
		QCOMPARE(graph.value("total").toInt(), 2); const auto channels = graph.value("channels").toArray(); QCOMPARE(channels.size(), 2);
		QCOMPARE(channels[1].toObject().value("index").toInt(), 1); QVERIFY(channels[1].toObject().value("localPort").toString().contains(input ? "master in R1" : "master out R1"));
		QCOMPARE(channels[1].toObject().value("startupPeer").toString(), QString("-"));
		QVERIFY(channels[0].toObject().value("startupPeerTruncated").toBool()); QCOMPARE(channels[0].toObject().value("startupPeer").toString().size(), 1024);
		const auto endpoint = graph.value("ports").toArray()[0].toObject(); QCOMPARE(endpoint.value("name").toString(), QString::fromUtf8("a:\xe9\x9f\xb3\xe5\xa3\xb0"));
		QCOMPARE(endpoint.value("connectedChannels").toArray(), QJsonArray{1}); QVERIFY(graph.value("hasMore").toBool());
		const auto next = StudioCapabilities::execute("audio.backend.inspect", {{"direction", input ? "input" : "output"}, {"offset", 1}, {"limit", 1}}).value("result").toObject();
		QCOMPARE(next.value("ports").toArray()[0].toObject().value("name").toString(), QString("z:audio")); QVERIFY(!next.value("hasMore").toBool());
		QCOMPARE(t.lists, t.freedLists); QVERIFY(!audio->isRunning());
	}

	void audioGraphDiscoveryFreesNativeListsOnFailure_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto value : {"list-exception", "lookup-exception", "observe", "ports-limit", "byte-limit", "invalid-utf8", "long-name"}) { QTest::newRow(value) << QString(value); }
	}
	void audioGraphDiscoveryFreesNativeListsOnFailure()
	{
		QFETCH(QString, fault); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		if (fault == "byte-limit") { for (int index = 0; index < 90; ++index) { t.addPeer(QByteArray::number(index) + ":" + QByteArray(3000, 'a'), false, false); } }
		else { t.addPeer(fault == "invalid-utf8" ? QByteArray("peer:\xff", 6) : fault == "long-name" ? QByteArray(4096, 'a') : QByteArray("peer:audio"), false, false); }
		t.fault = fault; QVERIFY_EXCEPTION_THROWN(audio->inspectBackendGraph(false), std::exception); QCOMPARE(t.lists, t.freedLists); QCOMPARE(t.graphChanges, 0);
	}

	void audioGraphRoutesOnlyOneChannelAndPreservesProcessing_data()
	{
		QTest::addColumn<bool>("input"); QTest::addColumn<bool>("running");
		QTest::newRow("input-running") << true << true; QTest::newRow("input-stopped") << true << false;
		QTest::newRow("output-running") << false << true; QTest::newRow("output-stopped") << false << false;
	}
	void audioGraphRoutesOnlyOneChannelAndPreservesProcessing()
	{
		QFETCH(bool, input); QFETCH(bool, running); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		t.addPeer("peer:audio", input, false); if (running) { audio->startProcessing(); }
		QJsonObject args{{"direction", input ? "input" : "output"}, {"channel", 1}, {"port", "peer:audio"}, {"enabled", true}};
		const auto before = Engine::audioEngine()->inputErrorEpoch(); const auto response = StudioCapabilities::execute("audio.backend.route", args);
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject();
		QVERIFY(result.value("changed").toBool()); QCOMPARE(result.value("processingInterrupted").toBool(), running); QCOMPARE(result.value("inputBuffersInvalidated").toBool(), input);
		QCOMPARE(audio->isRunning(), running); QCOMPARE(AudioJackTestAccess::paused(*audio), !running); QCOMPARE(Engine::audioEngine()->inputErrorEpoch() != before, input);
		const auto graph = audio->inspectBackendGraph(input); QVERIFY(graph.endpoints[0].connectedChannels == std::vector<unsigned int>{1});
		const auto repeated = StudioCapabilities::execute("audio.backend.route", args); QVERIFY(repeated.value("ok").toBool()); QVERIFY(!repeated.value("result").toObject().value("changed").toBool());
		QCOMPARE(t.graphChanges, 1); args.insert("enabled", false); QVERIFY(StudioCapabilities::execute("audio.backend.route", args).value("ok").toBool());
		QVERIFY(t.connections.empty()); QCOMPARE(t.graphChanges, 2); QCOMPARE(audio->isRunning(), running); QVERIFY(!StudioCapabilities::mutatesProject("audio.backend.route"));
	}

	void audioGraphInvalidPeersAndRequestsCannotMutate_data()
	{
		QTest::addColumn<QString>("kind");
		for (const auto value : {"missing", "direction", "midi", "own", "empty", "nul", "long", "surrogate", "negative-channel", "channel-range", "fractional-channel", "wrong-direction", "enabled-type", "unknown-field"})
		{ QTest::newRow(value) << QString(value); }
	}
	void audioGraphInvalidPeersAndRequestsCannotMutate()
	{
		QFETCH(QString, kind); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); audio->startProcessing();
		QString name = "peer:audio"; if (kind != "missing") { t.addPeer(name.toUtf8(), kind == "direction", kind == "midi"); }
		if (kind == "own") { name = audio->inspectBackendGraph(true).channels[0].localPort; }
		if (kind == "empty") { name.clear(); } if (kind == "nul") { name += QChar(0); } if (kind == "long") { name = QString(4096, QChar('a')); }
		if (kind == "surrogate") { name = QString(QChar(0xd800)); }
		QJsonObject args{{"direction", "output"}, {"channel", 0}, {"port", name}, {"enabled", true}};
		if (kind == "negative-channel") { args.insert("channel", -1); } if (kind == "channel-range") { args.insert("channel", 2); }
		if (kind == "fractional-channel") { args.insert("channel", 0.5); } if (kind == "wrong-direction") { args.insert("direction", "send"); }
		if (kind == "enabled-type") { args.insert("enabled", 1); } if (kind == "unknown-field") { args.insert("track", 0); }
		QVERIFY(!StudioCapabilities::execute("audio.backend.route", args).value("ok").toBool()); QCOMPARE(t.graphChanges, 0); QVERIFY(audio->isRunning()); QVERIFY(!AudioJackTestAccess::paused(*audio));
	}

	void audioGraphFailureVerifiesRollbackOrStopsProcessing_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto value : {"connect", "connect-after-error", "connect-throw", "connect-unconfirmed", "rollback", "disconnect", "disconnect-after-error", "observe", "shutdown-connect"})
		{ QTest::newRow(value) << QString(value); }
	}
	void audioGraphFailureVerifiesRollbackOrStopsProcessing()
	{
		QFETCH(QString, fault); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); audio->startProcessing(); t.addPeer("sink:audio", false, false);
		const bool before = fault.startsWith("disconnect"); if (before) { audio->setBackendRoute(false, 0, "sink:audio", true); }
		t.fault = fault; QString error; try { audio->setBackendRoute(false, 0, "sink:audio", !before); } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); }
		QVERIFY(!error.isEmpty()); const bool uncertain = fault == "rollback" || fault == "shutdown-connect";
		if (fault == "observe") { QVERIFY(error.contains("observation")); }
		else { QVERIFY2(error.contains(uncertain ? "restoration unconfirmed" : "previous audio connection state verified"), qPrintable(error)); }
		QCOMPARE(audio->isRunning(), !uncertain); QCOMPARE(AudioJackTestAccess::paused(*audio), uncertain);
		if (!uncertain) { QCOMPARE(!t.connections.empty(), before); }
		else
		{
			QCOMPARE(audio->processingStatus().failure, AudioDevice::ProcessingFailure::Exception);
			const auto opens = t.opens.load(); AudioJackTestAccess::poll(*audio); QVERIFY(!audio->isRunning()); QCOMPARE(t.opens.load(), opens);
		}
		t.fault.clear(); if (uncertain) { audio->startProcessing(); } QVERIFY(audio->isRunning());
	}

	void audioGraphInputAttemptsDiscardPreviouslyQueuedCapture_data()
	{ QTest::addColumn<bool>("fail"); QTest::newRow("changed") << false; QTest::newRow("rolled-back") << true; }
	void audioGraphInputAttemptsDiscardPreviouslyQueuedCapture()
	{
		QFETCH(bool, fail); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); t.addPeer("capture:audio", true, false);
		const auto engine = Engine::audioEngine(); std::array<SampleFrame, 8> samples{}; engine->pushInputFrames(samples.data(), static_cast<f_cnt_t>(samples.size())); engine->renderNextPeriod();
		QVERIFY(engine->inputBufferFrames() > 0); const auto epoch = engine->inputErrorEpoch(); t.fault = fail ? "connect-after-error" : "";
		if (fail) { QVERIFY_EXCEPTION_THROWN(audio->setBackendRoute(true, 0, "capture:audio", true), std::runtime_error); }
		else { QVERIFY(audio->setBackendRoute(true, 0, "capture:audio", true).changed); }
		QVERIFY(engine->inputErrorEpoch() > epoch); QCOMPARE(engine->inputBufferFrames(), f_cnt_t{0}); QVERIFY(!audio->isRunning());
	}

	void audioGraphCancellationRestoresTheEdgeAndProcessing_data()
	{ QTest::addColumn<bool>("after"); QTest::newRow("before-native-change") << false; QTest::newRow("after-native-change") << true; }
	void audioGraphCancellationRestoresTheEdgeAndProcessing()
	{
		QFETCH(bool, after); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); audio->startProcessing(); t.addPeer("sink:audio", false, false);
		int checks = 0;
		QVERIFY_EXCEPTION_THROWN(audio->setBackendRoute(false, 0, "sink:audio", true, [&] { return after ? !t.connections.empty() : ++checks == 2; }), std::runtime_error);
		QVERIFY(t.connections.empty()); QCOMPARE(t.graphChanges, after ? 2 : 0); QVERIFY(audio->isRunning()); QVERIFY(!AudioJackTestAccess::paused(*audio));
	}

	void audioGraphMutationJoinsCaptureWithoutHoldingTheModelLock()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); audio->startProcessing(); t.addPeer("sink:audio", false, false);
		std::atomic_bool entered{false}, release{false}; bool sawPaused = false; int changesBeforeRelease = -1; std::thread unblock;
		t.onAudioInputBuffer = [&] { entered = true; while (!release.load()) { std::this_thread::yield(); } };
		const auto join = qScopeGuard([&] { release = true; t.latest().join(); if (unblock.joinable()) { unblock.join(); } });
		t.latest().launch(); QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 2000);
		unblock = std::thread([&] {
			QElapsedTimer timeout; timeout.start(); while (!AudioJackTestAccess::paused(*audio) && timeout.elapsed() < 2000) { std::this_thread::yield(); }
			sawPaused = AudioJackTestAccess::paused(*audio); changesBeforeRelease = t.graphChanges; release = true;
		});
		QVERIFY(audio->setBackendRoute(false, 0, "sink:audio", true).changed); unblock.join(); t.latest().join();
		QVERIFY(sawPaused); QCOMPARE(changesBeforeRelease, 0); QCOMPARE(t.graphChanges, 1); QVERIFY(audio->isRunning()); QCOMPARE(t.badAccess.load(), 0);
	}

	void audioGraphPauseSilencesMasterAndDrainsSharedMidiWithoutRendering()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		auto midi = MidiJackTestAccess::create(audio, t.api); EngineMidiScope selected(midi.get()); audio->startProcessing(); t.addPeer("sink:audio", false, false);
		Processor processor; MidiPort port("audio route shared MIDI", midi.get(), &processor); midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &port); t.latest().tick();
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 61, 100), TimePos(10000), &port); t.outputHistory.clear();
		int periods = 0; const auto observed = connect(Engine::audioEngine(), &AudioEngine::nextAudioBuffer, this, [&](const SampleFrame*) { ++periods; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(observed); }); bool paused = false, silent = true;
		t.onConnect = [&] {
			paused = AudioJackTestAccess::paused(*audio); t.latest().tick();
			for (const auto& p : t.latest().ports) { if (!p->midi && p->output) { silent = silent && std::all_of(p->pcm.begin(), p->pcm.begin() + 256, [](float value) { return value == 0.f; }); } }
		};
		QVERIFY(audio->setBackendRoute(false, 0, "sink:audio", true).changed); QVERIFY(paused && silent); QCOMPARE(periods, 0); QVERIFY(audio->isRunning());
		QCOMPARE(midi->outputBackendStatus().pendingEvents, 0u); QCOMPARE(t.outputHistory.size(), std::size_t{4});
		for (const auto& event : t.outputHistory) { QVERIFY((event.bytes[0] & 0xf0) == 0xb0); }
		QVERIFY(midi->inputBackendStatus().active); QCOMPARE(t.badAccess.load(), 0);
	}

	void audioGraphOwnerAndReentryGuardsRefuseChanges()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); t.addPeer("sink:audio", false, false);
		std::atomic<int> refused{0}; std::thread foreign([&] {
			try { audio->inspectBackendGraph(false); } catch (...) { ++refused; }
			try { audio->setBackendRoute(false, 0, "sink:audio", true); } catch (...) { ++refused; }
		}); foreign.join(); QCOMPARE(refused.load(), 2); QCOMPARE(t.graphChanges, 0);
		t.onConnect = [&] { try { audio->setBackendRoute(false, 0, "sink:audio", false); } catch (...) { ++refused; } };
		QVERIFY(audio->setBackendRoute(false, 0, "sink:audio", true).connected); QCOMPARE(refused.load(), 3); QCOMPARE(t.graphChanges, 1);
	}

	void audioGraphOfflineAndUnsupportedObservationsAreExplicit_data()
	{ QTest::addColumn<bool>("jack"); QTest::newRow("dummy") << false; QTest::newRow("offline-jack") << true; }
	void audioGraphOfflineAndUnsupportedObservationsAreExplicit()
	{
		QFETCH(bool, jack); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); });
		if (jack) { installAudio(t); t.latest().disconnect(); }
		const auto response = StudioCapabilities::execute("audio.backend.inspect", {{"direction", "output"}}); QVERIFY(response.value("ok").toBool());
		const auto graph = response.value("result").toObject(); QCOMPARE(graph.value("supported").toBool(), jack); QVERIFY(!graph.value("observed").toBool());
		QVERIFY(graph.value("ports").toArray().isEmpty()); QVERIFY(graph.value("channels").toArray().isEmpty());
		QVERIFY(!StudioCapabilities::execute("audio.backend.route", {{"direction", "output"}, {"channel", 0}, {"port", "sink:audio"}, {"enabled", true}}).value("ok").toBool());
		QCOMPARE(t.graphChanges, 0);
	}

	void audioGraphNativeReservationsRefuseMutation_data()
	{
		QTest::addColumn<QString>("busy");
		for (const auto value : {"playing", "paused", "recording", "sample-armed", "pattern-sample-armed", "automation-armed",
			"automation-reserved", "note-recording", "export-reserved", "stem", "remote-execution", "different-device"}) { QTest::newRow(value) << QString(value); }
	}
	void audioGraphNativeReservationsRefuseMutation()
	{
		QFETCH(QString, busy); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); t.addPeer("sink:audio", false, false);
		const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
		auto container = busy == "pattern-sample-armed" ? static_cast<TrackContainer*>(Engine::patternStore()) : static_cast<TrackContainer*>(song);
		auto track = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, container)); auto sample = static_cast<SampleClip*>(track->createClip(0));
		auto curve = static_cast<AutomationClip*>(song->globalAutomationTrack()->createClip(0)); std::unique_ptr<TrackRenderScope> selection;
		std::uint64_t context = 0; auto other = busy == "different-device" ? AudioJackTestAccess::create(t.api) : nullptr;
		const auto release = qScopeGuard([&] {
			if (context) { RemotePlugin::finishRenderExecution(context); RemotePlugin::endRenderContext(context); }
			selection.reset(); curve->setRecording(false); curve->discardRecordingTake(); sample->setRecord(false); track->endNoteRecording(this); song->stop();
			if (engine->hasStoredAudioDevice()) { engine->restoreAudioDevice(); }
		});
		if (busy == "playing" || busy == "paused") { song->playSong(); if (busy == "paused") { song->togglePause(); } }
		if (busy == "recording") { song->record(); } if (busy == "sample-armed" || busy == "pattern-sample-armed") { sample->setRecord(true); }
		if (busy == "automation-armed") { curve->setRecording(true); } if (busy == "automation-reserved") { curve->reserveRecordingTake(32, 1); }
		if (busy == "note-recording") { QVERIFY(track->beginNoteRecording(this)); } if (busy == "export-reserved") { QVERIFY(engine->storeAudioDevice()); }
		if (busy == "stem") { selection = std::make_unique<TrackRenderScope>(track); } if (busy == "remote-execution") { context = RemotePlugin::beginRenderContext(); QVERIFY(context); }
		QVERIFY_EXCEPTION_THROWN((other ? other.get() : audio)->setBackendRoute(false, 0, "sink:audio", true), std::runtime_error);
		QCOMPARE(t.graphChanges, 0); QVERIFY(t.connections.empty());
	}

	void audioGraphExistingEdgeRaceNeedsPositiveObservation()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); t.addPeer("sink:audio", false, false);
		t.fault = "exists"; const auto result = audio->setBackendRoute(false, 0, "sink:audio", true); QVERIFY(result.connected && result.changed);
		QVERIFY(audio->inspectBackendGraph(false).endpoints[0].connectedChannels == std::vector<unsigned int>{0}); QCOMPARE(t.graphChanges, 1);
	}

	void audioGraphReceiptsDoNotRewriteRestartedStartupRoutes_data()
	{ QTest::addColumn<bool>("configured"); QTest::newRow("manual-connect-is-not-replayed") << false; QTest::newRow("configured-peer-reappears") << true; }
	void audioGraphReceiptsDoNotRewriteRestartedStartupRoutes()
	{
		QFETCH(bool, configured); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		const auto peer = QString::fromUtf8("interface:\xe9\x9f\xb3\xe5\xa3\xb0"); t.addPeer(peer.toUtf8(), false, false); ConfigManager::inst()->setValue("audiojack", "output1", configured ? peer : QString("-"));
		audio->startProcessing(); QCOMPARE(!t.connections.empty(), configured);
		QTemporaryDir dir; QString error; auto studio = std::make_unique<StudioRuntime>(dir.path()); QVERIFY2(studio->open(error), qPrintable(error));
		const auto readState = [&] { QFile file(QDir(dir.path()).filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return QJsonObject{}; } return QJsonDocument::fromJson(file.readAll()).object(); };
		const auto before = readState(); QVERIFY(!before.isEmpty()); const auto request = graphRequest("route", "audio.backend.route", {{"direction", "output"}, {"channel", 0}, {"port", peer}, {"enabled", !configured}});
		const auto response = studio->process(request); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto after = readState();
		QVERIFY(!after.isEmpty()); for (const auto key : {"revision", "history", "historyIndex"}) { QCOMPARE(after.value(key), before.value(key)); }
		studio.reset(); t.latest().disconnect(); AudioJackTestAccess::poll(*audio); QVERIFY(audio->isRunning());
		const auto graph = audio->inspectBackendGraph(false); QCOMPARE(!graph.endpoints[0].connectedChannels.empty(), configured); QCOMPARE(graph.channels[0].startupPeer, configured ? peer : QString("-"));
		const auto changes = t.graphChanges; studio = std::make_unique<StudioRuntime>(dir.path()); QVERIFY2(studio->open(error), qPrintable(error));
		auto replay = studio->process(request); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, response); QCOMPARE(t.graphChanges, changes);
		QCOMPARE(!audio->inspectBackendGraph(false).endpoints[0].connectedChannels.empty(), configured);
	}

	void audioGraphScriptedAgentDiscoversRoutesAndObserves_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void audioGraphScriptedAgentDiscoversRoutesAndObserves()
	{
		QFETCH(QString, protocol); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); t.addPeer("sink:audio", false, false);
		QTemporaryDir dir; QString error; int round = 0;
		const auto call = [](const QString& id, const QString& name, const QJsonObject& args) { return QJsonObject{{"id", id}, {"name", name}, {"args", args}}; };
		StudioRuntime studio(dir.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (++round == 1) { return graphDecision(protocol, {call("discover", "audio_backend_inspect", {{"direction", "output"}})}); }
			if (round == 2) { return graphDecision(protocol, {call("connect", "audio_backend_route", {{"direction", "output"}, {"channel", 1}, {"port", "sink:audio"}, {"enabled", true}}), call("observe", "audio_backend_inspect", {{"direction", "output"}})}); }
			return graphDecision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Native master audio channel connected and observed"}, {"evidence", QJsonArray{"discover", "connect", "observe"}}})});
		});
		QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(graphRequest("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-audio-graph-fixture"}}}})).value("ok").toBool());
		QVERIFY(studio.process(graphRequest("chat", "session.chat", {{"text", "Discover native audio destinations, connect master output channel 1 to sink:audio, and inspect the graph."}})).value("ok").toBool());
		for (int iteration = 0; iteration < 12; ++iteration) { studio.advanceAgent(); }
		QFile state(QDir(dir.path()).filePath("session.json")); QVERIFY(state.open(QIODevice::ReadOnly));
		QCOMPARE(QJsonDocument::fromJson(state.readAll()).object().value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete"));
		QCOMPARE(round, 3); QCOMPARE(t.graphChanges, 1); QVERIFY(audio->inspectBackendGraph(false).endpoints[0].connectedChannels == std::vector<unsigned int>{1});
	}

	void audioGraphNativeSamplePlaybackAndCaptureRemainUsable()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
		t.addPeer("sink:audio", false, false); t.addPeer("source:audio", true, false);
		for (unsigned int channel = 0; channel < 2; ++channel) { audio->setBackendRoute(false, channel, "sink:audio", true); audio->setBackendRoute(true, channel, "source:audio", true); }
		auto track = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, song)); auto clip = static_cast<SampleClip*>(track->createClip(0));
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32768, SampleFrame(.25f, -.125f)), engine->outputSampleRate()));
		song->setMasterVolume(100); song->playSong(); audio->startProcessing(); float left = 0, right = 0;
		for (int cycle = 0; cycle < 8; ++cycle)
		{
			t.latest().tick();
			for (const auto& port : t.latest().ports) { if (!port->midi && port->output) {
				for (int frame = 0; frame < 256; ++frame) { if (port->name.endsWith("L1")) { left = std::max(left, port->pcm[frame]); } else { right = std::min(right, port->pcm[frame]); } }
			} }
		}
		QCOMPARE(left, .25f); QCOMPARE(right, -.125f); song->stop(); engine->discardInputBuffers();
		auto recorded = static_cast<SampleClip*>(track->createClip(0));
		for (const auto& port : t.latest().ports) { if (!port->midi && !port->output) { std::fill_n(port->pcm.begin(), 256, port->name.endsWith("L1") ? .125f : -.25f); } }
		QVERIFY(engine->addPlayHandle(new SampleRecordHandle(recorded, 256))); t.latest().tick(); t.latest().tick(); engine->finishSampleRecordings();
		QString error; QVERIFY2(SampleRecordHandle::finalizePendingRecordings(error), qPrintable(error)); QCOMPARE(recorded->sample().sampleSize(), std::size_t{256});
		for (std::size_t frame = 0; frame < 256; ++frame) { QCOMPARE(recorded->sample().data()[frame].left(), .125f); QCOMPARE(recorded->sample().data()[frame].right(), -.25f); }
		QCOMPARE(t.connections.size(), std::size_t{4}); QCOMPARE(t.badAccess.load(), 0);
	}

	void audioGraphQueuedSelectionFollowsDeviceInsteadOfMixerIndices_data()
	{ QTest::addColumn<bool>("replaceDevice"); QTest::newRow("mixer-channel-removed") << false; QTest::newRow("device-replaced") << true; }
	void audioGraphQueuedSelectionFollowsDeviceInsteadOfMixerIndices()
	{
		QFETCH(bool, replaceDevice); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); installAudio(t); t.addPeer("sink:audio", false, false);
		QVERIFY(StudioCapabilities::execute("mixer.add", {{"name", "unrelated mixer channel"}}).value("ok").toBool());
		const QJsonObject args{{"direction", "output"}, {"channel", 1}, {"port", "sink:audio"}, {"enabled", true}};
		const auto selected = StudioCapabilities::selection("audio.backend.route", args);
		if (replaceDevice) { QVERIFY(StudioCapabilities::execute("audio.backend.select", {{"name", AudioDummy::name()}}).value("ok").toBool()); }
		else { QVERIFY(StudioCapabilities::execute("mixer.remove", {{"channel", 1}}).value("ok").toBool()); }
		QCOMPARE(selected.changed(StudioCapabilities::selection("audio.backend.route", args)), replaceDevice);
		if (!replaceDevice) { QVERIFY(StudioCapabilities::execute("audio.backend.route", args).value("ok").toBool()); QCOMPARE(t.graphChanges, 1); }
	}

	void busGraphCallbacksAlignTrackAndMasterAcrossNativeBufferChanges_data()
	{
		QTest::addColumn<int>("chunk");
		for (const int frames : {1, 13, 255, 256, 777}) { QTest::newRow(qPrintable(QString::number(frames))) << frames; }
	}
	void busGraphCallbacksAlignTrackAndMasterAcrossNativeBufferChanges()
	{
		QFETCH(int, chunk); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		const auto engine = Engine::audioEngine(); const auto track = busTrack(); const auto bus = track->audioBusHandle();
		audio->configureBusOutput(bus, true); const auto ports = busPorts(t, audio->inspectBusGraph(bus));
		auto play = new NativeBusPlay(track); play->ramp = true; QVERIFY(engine->addPlayHandle(play)); engine->setMasterGain(.5f);
		t.alternateBuffers = true; audio->startProcessing(); const auto period = engine->framesPerPeriod(), total = period * 5 + 19;
		for (f_cnt_t offset = 0; offset < total; )
		{
			const auto frames = std::min<f_cnt_t>(chunk, total - offset);
			for (const auto& p : t.latest().ports) { if (!p->midi && p->output) { p->pcm.fill(37.f); p->alternatePcm.fill(37.f); } }
			QCOMPARE(t.latest().tick(frames), 0);
			for (f_cnt_t frame = 0; frame < frames; ++frame)
			{
				const auto v = offset + frame < period ? 0.f : NativeBusPlay::value(offset + frame - period);
				QCOMPARE(ports[0]->lastPcm[frame], v); QCOMPARE(ports[1]->lastPcm[frame], -v / 2.f);
				for (const auto& p : t.latest().ports) { if (p->output && !p->midi && !p->name.contains(":track "))
				{ QCOMPARE(p->lastPcm[frame], p->name.endsWith("L1") ? v / 2.f : -v / 4.f); } }
			}
			for (const auto& p : t.latest().ports) { if (!p->midi && p->output) { QCOMPARE(p->lastPcm[frames], 37.f); } }
			offset += frames;
		}
		QCOMPARE(play->calls.load(), static_cast<int>((total + period - 1) / period)); QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphStableNamesPreserveEdgesThroughDuplicateAndLongLabels()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); t.addPeer("sink:audio", false, false);
		const auto first = busTrack(), second = busTrack(Engine::patternStore()); first->setName("same"); second->setName("same");
		audio->configureBusOutput(first->audioBusHandle(), true); audio->configureBusOutput(second->audioBusHandle(), true);
		const auto a = audio->inspectBusGraph(first->audioBusHandle()), b = audio->inspectBusGraph(second->audioBusHandle());
		QVERIFY(a.id != b.id); QVERIFY(a.graph.channels[0].localPort != b.graph.channels[0].localPort);
		audio->setBusRoute(first->audioBusHandle(), 0, "sink:audio", true);
		const auto label = QString::fromUtf8("\xe9\x9f\xb3\xe8\xbb\x8c") + QString(2000, QChar('x')); first->setName(label);
		const auto renamed = audio->inspectBusGraph(first->audioBusHandle()); QCOMPARE(renamed.id, a.id);
		QCOMPARE(renamed.graph.channels[0].localPort, a.graph.channels[0].localPort); QCOMPARE(renamed.label, label.left(1024)); QVERIFY(renamed.labelTruncated);
		QVERIFY(renamed.graph.endpoints[0].connectedChannels == std::vector<unsigned int>{0}); QCOMPARE(t.busRegistrations, 4); QCOMPARE(t.graphChanges, 1);
	}

	void busGraphModelEditsStageIntentWithoutNativeCallsOrJoins()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); SampleTrack* track = nullptr;
		{
			const auto guard = Engine::audioEngine()->requestChangesGuard(); track = busTrack(); track->setName("staged");
			AudioBusHandle internal("internal preview bus"); AudioJackTestAccess::poll(*audio); QCOMPARE(t.busRegistrations, 0);
			PreviewContainer preview; const auto previewTrack = busTrack(&preview);
			QVERIFY_EXCEPTION_THROWN(audio->inspectBusGraph(previewTrack->audioBusHandle()), std::invalid_argument);
			QCOMPARE(t.busRegistrations, 0);
			QVERIFY_EXCEPTION_THROWN(audio->configureBusOutput(track->audioBusHandle(), true), std::runtime_error);
			QVERIFY_EXCEPTION_THROWN(audio->setBackendRoute(false, 0, "sink:audio", true), std::runtime_error);
			QVERIFY_EXCEPTION_THROWN(audio->inspectBusGraph(&internal), std::invalid_argument);
		}
		AudioJackTestAccess::poll(*audio); QCOMPARE(t.busRegistrations, 2); const auto ports = busPorts(t, audio->inspectBusGraph(track->audioBusHandle()));
		{ const auto guard = Engine::audioEngine()->requestChangesGuard(); delete track; QCOMPARE(t.unregisters.load(), 0); }
		AudioJackTestAccess::poll(*audio); QVERIFY(!ports[0]->registered && !ports[1]->registered); QCOMPARE(t.unregisters.load(), 2); QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphDisableClearsAndRetiresOnlyTheSelectedBus_data()
	{ QTest::addColumn<bool>("running"); QTest::newRow("running-dsp") << true; QTest::newRow("stopped-dsp") << false; }
	void busGraphDisableClearsAndRetiresOnlyTheSelectedBus()
	{
		QFETCH(bool, running); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); t.addPeer("sink:audio", false, false);
		const auto track = busTrack(); const auto bus = track->audioBusHandle(); audio->configureBusOutput(bus, true);
		const auto ports = busPorts(t, audio->inspectBusGraph(bus)); auto midi = MidiJackTestAccess::create(audio, t.api);
		audio->setBusRoute(bus, 0, "sink:audio", true); audio->setBackendRoute(false, 0, "sink:audio", true);
		QVERIFY(Engine::audioEngine()->addPlayHandle(new NativeBusPlay(track))); audio->startProcessing(); t.latest().tick(); t.latest().tick();
		if (!running) { audio->stopProcessing(); } t.latest().launchCycles();
		audio->configureBusOutput(bus, false); t.latest().join(); const auto graph = audio->inspectBusGraph(bus);
		QVERIFY(!graph.enabled && !graph.pending && !graph.published); QCOMPARE(audio->isRunning(), running); QVERIFY(midi->isAvailable());
		for (const auto port : ports) { QVERIFY(!port->registered); for (int frame = 0; frame < 256; ++frame) { QCOMPARE(port->lastPcm[frame], 0.f); } }
		QCOMPARE(t.connections.size(), std::size_t{1}); QCOMPARE(t.closes.load(), 0); QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphRetirementJoinsAnOldSnapshotAndDoesNotRebindReusedTracks()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		alignas(SampleTrack) std::byte storage[sizeof(SampleTrack)]; auto track = new (storage) SampleTrack(Engine::getSong());
		const auto releaseTrack = qScopeGuard([&] { if (track) { track->~SampleTrack(); } });
		auto source = track->audioBusHandle()->externalOutput(); audio->configureBusOutput(track->audioBusHandle(), true);
		const auto graph = audio->inspectBusGraph(track->audioBusHandle()); const auto ports = busPorts(t, graph); audio->startProcessing();
		std::atomic_bool entered{false}, release{false}, joined{false}, sawPublication{false}, premature{false};
		t.onBusBuffer = [&] { if (!entered.exchange(true)) { while (!release.load()) { std::this_thread::yield(); } } };
		t.onBusUnregister = [&] { if (!release.load()) { premature = true; } };
		t.latest().launch(); const auto unblock = qScopeGuard([&] { release = true; t.latest().join(); }); QVERIFY(waitFlag(entered));
		const auto oldSnapshot = AudioJackTestAccess::busSnapshot(*audio); track->~SampleTrack(); track = nullptr; QVERIFY(source->retired());
		std::thread helper([&] {
			QElapsedTimer wait; wait.start(); while (AudioJackTestAccess::busSnapshot(*audio) == oldSnapshot && wait.elapsed() < 2000) { std::this_thread::yield(); }
			sawPublication = AudioJackTestAccess::busSnapshot(*audio) != oldSnapshot; release = true; t.latest().join(); joined = true; t.latest().tick();
		});
		AudioJackTestAccess::poll(*audio); helper.join(); QVERIFY(joined && sawPublication && !premature); QVERIFY(!ports[0]->registered && !ports[1]->registered);
		track = new (storage) SampleTrack(Engine::getSong()); audio->configureBusOutput(track->audioBusHandle(), true);
		QVERIFY(track->audioBusHandle()->externalOutput() != source); QVERIFY(audio->inspectBusGraph(track->audioBusHandle()).id != graph.id);
		QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphRegistrationRollsBackOrRetiresUncertainClientOwnership_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto fault : {"bus-left-port", "bus-right-port", "bus-left-throw", "bus-right-throw", "bus-cleanup-error", "bus-cleanup-throw"}) { QTest::newRow(fault) << QString(fault); }
	}
	void busGraphRegistrationRollsBackOrRetiresUncertainClientOwnership()
	{
		QFETCH(QString, fault); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		const auto bus = busTrack()->audioBusHandle(); t.fault = fault;
		QVERIFY_EXCEPTION_THROWN(audio->configureBusOutput(bus, true), std::runtime_error); const auto state = audio->inspectBusGraph(bus);
		QVERIFY(state.enabled && state.pending && !state.published); QVERIFY(state.nativeErrorCode != 0); QVERIFY(!state.error.isEmpty());
		const bool uncertain = fault.startsWith("bus-cleanup-"); QCOMPARE(t.closes.load(), uncertain ? 1 : 0);
		for (const auto& port : t.clients[0]->ports) { if (port->name.contains(":track ")) { QVERIFY(!port->registered); } }
		t.fault.clear(); if (uncertain) { AudioJackTestAccess::poll(*audio); QCOMPARE(t.opens.load(), 1); audio->startProcessing(); }
		else { audio->configureBusOutput(bus, true); }
		QVERIFY(audio->inspectBusGraph(bus).published); QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphRetirementFaultsKeepSilentOwnershipOrCloseTheWholeClient_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto fault : {"bus-disconnect", "bus-observe", "bus-unregister-error", "bus-unregister-right", "bus-unregister-throw"}) { QTest::newRow(fault) << QString(fault); }
	}
	void busGraphRetirementFaultsKeepSilentOwnershipOrCloseTheWholeClient()
	{
		QFETCH(QString, fault); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		const auto bus = busTrack()->audioBusHandle(); audio->configureBusOutput(bus, true); auto midi = MidiJackTestAccess::create(audio, t.api);
		t.addPeer("sink:audio", false, false); audio->setBusRoute(bus, 0, "sink:audio", true); audio->startProcessing(); t.fault = fault; t.latest().launchCycles();
		QVERIFY_EXCEPTION_THROWN(audio->configureBusOutput(bus, false), std::runtime_error); t.latest().join();
		const bool uncertain = fault.startsWith("bus-unregister-"); const auto state = audio->inspectBusGraph(bus);
		QVERIFY(!state.enabled); QCOMPARE(state.published, !uncertain); QCOMPARE(state.pending, !uncertain); QCOMPARE(t.closes.load(), uncertain ? 1 : 0);
		QCOMPARE(midi->isAvailable(), !uncertain); t.fault.clear(); const auto opened = t.opens.load(); AudioJackTestAccess::poll(*audio); QCOMPARE(t.opens.load(), opened);
		if (uncertain) { QVERIFY(!audio->isRunning()); audio->startProcessing(); QVERIFY(audio->isRunning() && midi->isAvailable()); }
		else { t.latest().launchCycles(); audio->configureBusOutput(bus, false); t.latest().join(); }
		QVERIFY(!audio->inspectBusGraph(bus).published); QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphStalledNativeCycleLeavesRetirementPending()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); const auto bus = busTrack()->audioBusHandle();
		audio->configureBusOutput(bus, true); audio->startProcessing(); const auto ports = busPorts(t, audio->inspectBusGraph(bus));
		QElapsedTimer wait; wait.start(); QVERIFY_EXCEPTION_THROWN(audio->configureBusOutput(bus, false), std::runtime_error);
		QVERIFY(wait.elapsed() >= 200); const auto pending = audio->inspectBusGraph(bus); QVERIFY(pending.pending && pending.published && !pending.enabled); QCOMPARE(pending.nativeErrorCode, -ETIMEDOUT);
		for (const auto port : ports) { port->pcm.fill(17.f); } t.latest().tick(); for (const auto port : ports) { QCOMPARE(port->lastPcm[0], 0.f); }
		t.latest().launchCycles(); audio->configureBusOutput(bus, false); t.latest().join(); QVERIFY(!audio->inspectBusGraph(bus).pending); QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphCancellationRestoresRequestedStateAndCleansProvisionalPorts_data()
	{
		QTest::addColumn<int>("at"); QTest::addColumn<bool>("throws");
		for (const int at : {1, 4, 5}) { for (const bool throws : {false, true})
		{ QTest::newRow(qPrintable(QString("check-%1-throw-%2").arg(at).arg(throws))) << at << throws; } }
	}
	void busGraphCancellationRestoresRequestedStateAndCleansProvisionalPorts()
	{
		QFETCH(int, at); QFETCH(bool, throws); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		const auto bus = busTrack()->audioBusHandle(); audio->configureBusOutput(bus, false); int checks = 0;
		QVERIFY_EXCEPTION_THROWN(audio->configureBusOutput(bus, true, [&] {
			if (++checks != at) { return false; } if (throws) { throw std::runtime_error("fixture cancellation"); } return true;
		}), std::runtime_error);
		const auto state = audio->inspectBusGraph(bus); QVERIFY(!state.enabled && !state.published && !state.pending);
		QCOMPARE(t.busRegistrations, at == 1 ? 0 : at == 4 ? 1 : 2); QCOMPARE(t.unregisters.load(), t.busRegistrations);
		audio->configureBusOutput(bus, true); QVERIFY(audio->inspectBusGraph(bus).published); QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphRetirementCancellationPreservesExistingEdges_data()
	{ QTest::addColumn<bool>("throws"); QTest::newRow("boolean") << false; QTest::newRow("exception") << true; }
	void busGraphRetirementCancellationPreservesExistingEdges()
	{
		QFETCH(bool, throws); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		const auto bus = busTrack()->audioBusHandle(); audio->configureBusOutput(bus, true); t.addPeer("sink:audio", false, false);
		audio->setBusRoute(bus, 0, "sink:audio", true); audio->startProcessing(); int checks = 0;
		QVERIFY_EXCEPTION_THROWN(audio->configureBusOutput(bus, false, [&] {
			if (++checks < 3) { return false; } if (throws) { throw std::runtime_error("fixture cancellation"); } return true;
		}), std::runtime_error);
		const auto state = audio->inspectBusGraph(bus); QVERIFY(state.enabled && state.published && !state.pending);
		QVERIFY(state.graph.endpoints[0].connectedChannels == std::vector<unsigned int>{0}); QCOMPARE(t.unregisters.load(), 0); QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphNativeReservationsAndForeignOwnersRefuseMutation_data()
	{
		QTest::addColumn<QString>("busy");
		for (const auto value : {"playing", "paused", "recording", "sample-armed", "pattern-sample-armed", "automation-armed",
			"automation-reserved", "note-recording", "export-reserved", "stem", "remote-execution", "different-device", "model-edit", "foreign-thread"})
		{ QTest::newRow(value) << QString(value); }
	}
	void busGraphNativeReservationsAndForeignOwnersRefuseMutation()
	{
		QFETCH(QString, busy); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); t.addPeer("sink:audio", false, false);
		const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
		const auto track = busTrack(busy == "pattern-sample-armed" ? static_cast<TrackContainer*>(Engine::patternStore()) : song);
		const auto bus = track->audioBusHandle(); audio->configureBusOutput(bus, true);
		auto sample = static_cast<SampleClip*>(track->createClip(0)); auto curve = static_cast<AutomationClip*>(song->globalAutomationTrack()->createClip(0));
		std::unique_ptr<TrackRenderScope> selection; std::uint64_t context = 0; auto other = busy == "different-device" ? AudioJackTestAccess::create(t.api) : nullptr;
		const auto release = qScopeGuard([&] {
			if (context) { RemotePlugin::finishRenderExecution(context); RemotePlugin::endRenderContext(context); }
			selection.reset(); curve->setRecording(false); curve->discardRecordingTake(); sample->setRecord(false); track->endNoteRecording(this); song->stop();
			if (engine->hasStoredAudioDevice()) { engine->restoreAudioDevice(); }
		});
		if (busy == "playing" || busy == "paused") { song->playSong(); if (busy == "paused") { song->togglePause(); } }
		if (busy == "recording") { song->record(); } if (busy == "sample-armed" || busy == "pattern-sample-armed") { sample->setRecord(true); }
		if (busy == "automation-armed") { curve->setRecording(true); } if (busy == "automation-reserved") { curve->reserveRecordingTake(32, 1); }
		if (busy == "note-recording") { QVERIFY(track->beginNoteRecording(this)); } if (busy == "export-reserved") { QVERIFY(engine->storeAudioDevice()); }
		if (busy == "stem") { selection = std::make_unique<TrackRenderScope>(track); } if (busy == "remote-execution") { context = RemotePlugin::beginRenderContext(); QVERIFY(context); }
		std::atomic<int> refused{0}; const auto attempt = [&] {
			try { (other ? other.get() : audio)->configureBusOutput(bus, false); } catch (...) { ++refused; }
			try { (other ? other.get() : audio)->setBusRoute(bus, 0, "sink:audio", true); } catch (...) { ++refused; }
		};
		if (busy == "foreign-thread") { std::thread foreign(attempt); foreign.join(); }
		else if (busy == "model-edit") { const auto guard = engine->requestChangesGuard(); attempt(); }
		else { attempt(); }
		QCOMPARE(refused.load(), 2); QCOMPARE(t.graphChanges, 0); QCOMPARE(t.unregisters.load(), 0); QVERIFY(bus->extOutputEnabled());
	}

	void busGraphCallbackFailuresClearEveryAccessibleOutput_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto fault : {"dsp", "bus-buffer-null", "bus-buffer-throw", "bus-input-null", "bus-input-throw"}) { QTest::newRow(fault) << QString(fault); }
	}
	void busGraphCallbackFailuresClearEveryAccessibleOutput()
	{
		QFETCH(QString, fault); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		const auto track = busTrack(); const auto bus = track->audioBusHandle(); audio->configureBusOutput(bus, true);
		auto play = new NativeBusPlay(track); QVERIFY(Engine::audioEngine()->addPlayHandle(play)); audio->startProcessing(); t.latest().tick(); t.latest().tick();
		for (const auto& p : t.latest().ports) { if (p->output && !p->midi) { p->pcm.fill(31.f); p->lastPcm = nullptr; } }
		if (fault == "dsp") { play->failing = true; } else { t.fault = fault; }
		QCOMPARE(t.latest().tick(), 0);
		for (const auto& p : t.latest().ports) { if (p->output && !p->midi && p->lastPcm)
		{ for (int frame = 0; frame < 256; ++frame) { QCOMPARE(p->lastPcm[frame], 0.f); } } }
		if (fault == "dsp") { QCOMPARE(Engine::audioEngine()->dspStatus().failure, AudioEngine::DspFailure::InstrumentJobException); }
		else { QVERIFY(audio->processingStatus().failure != AudioDevice::ProcessingFailure::None); }
		t.fault.clear(); play->failing = false; QVERIFY(Engine::audioEngine()->resetDspFailure()); audio->startProcessing();
		for (int cycle = 0; cycle < 3; ++cycle) { t.latest().tick(); }
		const auto ports = busPorts(t, audio->inspectBusGraph(bus)); QCOMPARE(ports[0]->lastPcm[0], .25f); QCOMPARE(ports[1]->lastPcm[0], -.125f); QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphClientRecreationRetainsIdentitiesWithoutReplayingEdges()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); t.addPeer("sink:audio", false, false);
		const auto first = busTrack()->audioBusHandle(), disabled = busTrack()->audioBusHandle();
		audio->configureBusOutput(first, true); audio->configureBusOutput(disabled, false); audio->setBusRoute(first, 1, "sink:audio", true);
		const auto before = audio->inspectBusGraph(first); audio->startProcessing(); t.latest().disconnect(); AudioJackTestAccess::poll(*audio);
		QVERIFY(audio->isRunning()); const auto after = audio->inspectBusGraph(first); QCOMPARE(after.id, before.id); QVERIFY(after.enabled && after.published && !after.pending);
		QVERIFY(after.graph.channels[0].localPort.endsWith(":track " + before.id + " L")); QVERIFY(after.graph.endpoints[0].connectedChannels.empty());
		QVERIFY(!audio->inspectBusGraph(disabled).published); QCOMPARE(t.busRegistrations, 4); QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphPublishedBusLimitPreservesPendingTracksAndCanBeFreed()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t);
		std::vector<SampleTrack*> tracks;
		for (int index = 0; index < 256; ++index) { const auto track = busTrack(); tracks.push_back(track); audio->configureBusOutput(track->audioBusHandle(), true); }
		const auto pending = busTrack()->audioBusHandle(); QVERIFY_EXCEPTION_THROWN(audio->configureBusOutput(pending, true), std::runtime_error);
		const auto state = audio->inspectBusGraph(pending); QVERIFY(state.enabled && state.pending && !state.published); QVERIFY(state.error.contains("256")); QCOMPARE(t.busRegistrations, 512);
		audio->configureBusOutput(tracks[0]->audioBusHandle(), false); audio->configureBusOutput(pending, true);
		QVERIFY(audio->inspectBusGraph(pending).published); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t{257}); QCOMPARE(t.closes.load(), 0); QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphQueuedSelectionBindsTheNativeTrackAndDevice_data()
	{
		QTest::addColumn<QString>("change"); for (const auto change : {"mixer", "track", "device"}) { QTest::newRow(change) << QString(change); }
	}
	void busGraphQueuedSelectionBindsTheNativeTrackAndDevice()
	{
		QFETCH(QString, change); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); installAudio(t); const auto track = busTrack();
		QVERIFY(StudioCapabilities::execute("mixer.add", {{"name", "unrelated"}}).value("ok").toBool());
		const QJsonObject args{{"track", 0}, {"channel", 1}, {"port", "sink:audio"}, {"enabled", true}}; const auto before = StudioCapabilities::selection("audio.bus.route", args);
		if (change == "mixer") { QVERIFY(StudioCapabilities::execute("mixer.remove", {{"channel", 1}}).value("ok").toBool()); }
		else if (change == "track") { delete track; busTrack(); }
		else { QVERIFY(StudioCapabilities::execute("audio.backend.select", {{"name", AudioDummy::name()}}).value("ok").toBool()); }
		QCOMPARE(before.changed(StudioCapabilities::selection("audio.bus.route", args)), change != "mixer");
	}

	void busGraphReceiptsPreserveHistoryAndDoNotRestoreNativePorts()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); t.addPeer("sink:audio", false, false);
		QTemporaryDir dir; QString error; StudioRuntime studio(dir.path()); QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(graphRequest("track", "track.add", {{"type", "sample"}})).value("ok").toBool());
		const auto readState = [&] { QFile file(QDir(dir.path()).filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return QJsonObject{}; } return QJsonDocument::fromJson(file.readAll()).object(); };
		const auto before = readState(); QVERIFY(!before.isEmpty());
		const auto publish = graphRequest("publish", "audio.bus.configure", {{"track", 0}, {"enabled", true}});
		const auto route = graphRequest("route", "audio.bus.route", {{"track", 0}, {"channel", 0}, {"port", "sink:audio"}, {"enabled", true}});
		const auto published = studio.process(publish), routed = studio.process(route); QVERIFY(published.value("ok").toBool() && routed.value("ok").toBool());
		const auto after = readState(); for (const auto key : {"revision", "history", "historyIndex"}) { QCOMPARE(after.value(key), before.value(key)); }
		const auto bus = static_cast<SampleTrack*>(Engine::getSong()->tracks()[0])->audioBusHandle(); audio->configureBusOutput(bus, false); const auto registered = t.busRegistrations;
		auto replay = studio.process(publish); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, published);
		replay = studio.process(route); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, routed); QCOMPARE(t.busRegistrations, registered);
		const auto inspected = studio.process(graphRequest("observe", "audio.bus.inspect", {{"track", 0}})); QVERIFY(inspected.value("ok").toBool());
		QVERIFY(!inspected.value("result").toObject().value("published").toBool()); QVERIFY(t.connections.empty());
	}

	void busGraphScriptedAgentCreatesPublishesRoutesAndObserves_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void busGraphScriptedAgentCreatesPublishesRoutesAndObserves()
	{
		QFETCH(QString, protocol); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); t.addPeer("sink:audio", false, false);
		QTemporaryDir dir; QString error; int round = 0;
		const auto call = [](const QString& id, const QString& name, const QJsonObject& args) { return QJsonObject{{"id", id}, {"name", name}, {"args", args}}; };
		StudioRuntime studio(dir.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (++round == 1) { return graphDecision(protocol, {call("track", "track_add", {{"type", "sample"}, {"name", "Native stem"}})}); }
			if (round == 2) { return graphDecision(protocol, {call("publish", "audio_bus_configure", {{"track", 0}, {"enabled", true}}), call("discover", "audio_bus_inspect", {{"track", 0}})}); }
			if (round == 3) { return graphDecision(protocol, {call("connect", "audio_bus_route", {{"track", 0}, {"channel", 1}, {"port", "sink:audio"}, {"enabled", true}}), call("observe", "audio_bus_inspect", {{"track", 0}})}); }
			return graphDecision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Native track output connected and observed"}, {"evidence", QJsonArray{"track", "publish", "discover", "connect", "observe"}}})});
		});
		QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(graphRequest("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-bus-graph-fixture"}}}})).value("ok").toBool());
		QVERIFY(studio.process(graphRequest("chat", "session.chat", {{"text", "Create a sample track, publish its native stereo ports, route its right output to sink:audio, and inspect the graph."}})).value("ok").toBool());
		for (int iteration = 0; iteration < 16; ++iteration) { studio.advanceAgent(); }
		QFile state(QDir(dir.path()).filePath("session.json")); QVERIFY(state.open(QIODevice::ReadOnly));
		QCOMPARE(QJsonDocument::fromJson(state.readAll()).object().value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete"));
		QCOMPARE(round, 4); QCOMPARE(t.graphChanges, 1); const auto bus = static_cast<SampleTrack*>(Engine::getSong()->tracks()[0])->audioBusHandle();
		QVERIFY(audio->inspectBusGraph(bus).graph.endpoints[0].connectedChannels == std::vector<unsigned int>{1});
	}

	void busGraphUnsupportedBackendsAndInvalidTargetsAreExplicit_data()
	{
		QTest::addColumn<QString>("invalid");
		for (const auto invalid : {"dummy", "automation", "container", "track", "negative-channel", "extra-channel", "direction", "missing-peer", "midi-peer", "output-peer", "own-peer", "unpublished"})
		{ QTest::newRow(invalid) << QString(invalid); }
	}
	void busGraphUnsupportedBackendsAndInvalidTargetsAreExplicit()
	{
		QFETCH(QString, invalid); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); });
		const auto audio = invalid == "dummy" ? nullptr : installAudio(t); const auto track = busTrack(); const auto bus = track->audioBusHandle();
		if (invalid == "dummy")
		{
			const auto result = StudioCapabilities::execute("audio.bus.inspect", {{"track", 0}}); QVERIFY(result.value("ok").toBool());
			const auto state = result.value("result").toObject(); QVERIFY(!state.value("supported").toBool() && !state.value("observed").toBool()); QVERIFY(!state.value("published").toBool());
			QVERIFY(!StudioCapabilities::execute("audio.bus.configure", {{"track", 0}, {"enabled", true}}).value("ok").toBool());
		}
		else if (invalid != "unpublished") { audio->configureBusOutput(bus, true); }
		t.addPeer("sink:audio", false, false); t.addPeer("sink:midi", false, true); t.addPeer("source:audio", true, false);
		QJsonObject args{{"track", 0}, {"channel", 0}, {"port", "sink:audio"}, {"enabled", true}};
		if (invalid == "automation") { Track::create(Track::Type::Automation, Engine::getSong()); args.insert("track", 1); }
		if (invalid == "container") { args.insert("container", "preview"); } if (invalid == "track") { args.insert("track", 15); }
		if (invalid == "negative-channel") { args.insert("channel", -1); } if (invalid == "extra-channel") { args.insert("channel", 2); }
		if (invalid == "direction") { args.insert("direction", "input"); } if (invalid == "missing-peer") { args.insert("port", "missing:audio"); }
		if (invalid == "midi-peer") { args.insert("port", "sink:midi"); } if (invalid == "output-peer") { args.insert("port", "source:audio"); }
		if (invalid == "own-peer") { args.insert("port", audio->inspectBackendGraph(true).channels[0].localPort); }
		const auto result = StudioCapabilities::execute("audio.bus.route", args); QVERIFY(!result.value("ok").toBool()); QVERIFY(!result.value("error").toString().isEmpty());
		QCOMPARE(t.graphChanges, 0); QCOMPARE(t.badAccess.load(), 0);
	}

	void transferJackEngineSelectsSharedMidiWhileAudioIsJoined_data()
	{ QTest::addColumn<bool>("running"); QTest::newRow("stopped") << false; QTest::newRow("running") << true; }
	void transferJackEngineSelectsSharedMidiWhileAudioIsJoined()
	{
		QFETCH(bool, running); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); const auto engine = Engine::audioEngine();
		if (running) { audio->startProcessing(); } Processor processor; MidiPort port("engine-shared", engine->midiClient(), &processor, nullptr, MidiPort::Mode::Duplex);
		const auto selected = StudioMidiBackendSelectionTestAccess::select(MidiJack::name(), [&] { return MidiJackTestAccess::create(audio, t.api); });
		QVERIFY(selected.audioRestored); QCOMPARE(audio->isRunning(), running); QCOMPARE(port.client(), engine->midiClient()); QCOMPARE(engine->midiClientName(), MidiJack::name());
		const auto midi = dynamic_cast<MidiJack*>(engine->midiClient()); QVERIFY(midi && midi->isAvailable()); QCOMPARE(midi->sharedAudioDevice(), audio);
		QVERIFY(midi->inputDeliveryStatus(&port).failed); if (!running) { audio->startProcessing(); } QString error; bool changed = false; QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
		port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); t.latest().tick(); t.latest().launchCycles();
		const auto retired = engine->selectMidiBackend(MidiDummy::name()); t.latest().join();
		QVERIFY(retired.audioRestored); QCOMPARE(port.client(), engine->midiClient()); QVERIFY(dynamic_cast<MidiDummy*>(engine->midiClient())); QVERIFY(audio->isRunning());
		QVERIFY(std::any_of(t.outputHistory.begin(), t.outputHistory.end(), [](const auto& event) { return event.bytes == std::vector<unsigned char>{0xb0,120,0}; }));
		QCOMPARE(t.closes.load(), 0); QCOMPARE(t.badAccess.load(), 0); QVERIFY(!engine->hasStoredAudioDevice());
	}

	void transferJackEngineRefusesAnOccupiedSharedAudioConnection_data()
	{ QTest::addColumn<bool>("running"); QTest::newRow("stopped") << false; QTest::newRow("running") << true; }
	void transferJackEngineRefusesAnOccupiedSharedAudioConnection()
	{
		QFETCH(bool, running); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); const auto engine = Engine::audioEngine();
		if (running) { audio->startProcessing(); }
		StudioMidiBackendSelectionTestAccess::select(MidiJack::name(), [&] { return MidiJackTestAccess::create(audio, t.api); }); const auto source = engine->midiClient();
		Processor processor; MidiPort port("occupied", source, &processor, nullptr, MidiPort::Mode::Input);
		QVERIFY_EXCEPTION_THROWN(StudioMidiBackendSelectionTestAccess::select(MidiJack::name(), [&] { return MidiJackTestAccess::create(audio, t.api); }), std::runtime_error);
		QCOMPARE(engine->midiClient(), source); QCOMPARE(port.client(), source); QCOMPARE(audio->isRunning(), running); QVERIFY(!engine->hasStoredAudioDevice());
		QVERIFY(static_cast<MidiJack*>(source)->isAvailable()); QCOMPARE(t.closes.load(), 0); QCOMPARE(t.badAccess.load(), 0);
	}

	void transferJackEngineCancelledCandidateRetiresOnlyItsPorts_data()
	{ QTest::addColumn<bool>("running"); QTest::newRow("stopped") << false; QTest::newRow("running") << true; }
	void transferJackEngineCancelledCandidateRetiresOnlyItsPorts()
	{
		QFETCH(bool, running); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = installAudio(t); const auto engine = Engine::audioEngine();
		if (running) { audio->startProcessing(); } const auto source = engine->midiClient(); bool cancelled = false; FileResourceIO::Scope work([&] { return cancelled; }, false);
		QVERIFY_EXCEPTION_THROWN(StudioMidiBackendSelectionTestAccess::select(MidiJack::name(), [&] {
			auto candidate = MidiJackTestAccess::create(audio, t.api); cancelled = true; return candidate;
		}), std::runtime_error);
		QCOMPARE(engine->midiClient(), source); QCOMPARE(audio->isRunning(), running); QVERIFY(!engine->hasStoredAudioDevice());
		QVERIFY(!t.latest().midiPort()); QCOMPARE(t.closes.load(), 0); QCOMPARE(t.badAccess.load(), 0);
	}

	void transferJackCleanupPrecedesSharedOrStandaloneClientRetirement_data()
	{ QTest::addColumn<bool>("shared"); QTest::newRow("standalone") << false; QTest::newRow("shared") << true; }
	void transferJackCleanupPrecedesSharedOrStandaloneClientRetirement()
	{
		QFETCH(bool, shared); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = shared ? installAudio(t) : nullptr;
		auto midi = MidiJackTestAccess::create(audio, t.api); MidiDummy target; Processor processor; MidiPort port("transfer", midi.get(), &processor, nullptr, MidiPort::Mode::Output);
		if (audio) { audio->startProcessing(); } port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); t.latest().tick();
		if (audio) { audio->stopProcessing(); } t.latest().launchCycles();
		const auto result = MidiClientTransferTestAccess::transfer(*midi, target); t.latest().join();
		QCOMPARE(result.ports, 1u); QCOMPARE(result.retirementExceptions, 0u); QCOMPARE(port.client(), static_cast<MidiClient*>(&target));
		QVERIFY(std::any_of(t.outputHistory.begin(), t.outputHistory.end(), [](const auto& event) { return event.bytes == std::vector<unsigned char>{0xb0,120,0}; }));
		midi.reset(); QCOMPARE(port.client(), static_cast<MidiClient*>(&target)); QCOMPARE(t.closes.load(), shared ? 0 : 1);
		if (audio) { t.latest().tick(); QVERIFY(!audio->isRunning()); } QCOMPARE(t.badAccess.load(), 0);
	}

	void transferJackStalledCleanupKeepsTheOldBindingUntilRetry_data()
	{ QTest::addColumn<bool>("shared"); QTest::newRow("standalone") << false; QTest::newRow("shared") << true; }
	void transferJackStalledCleanupKeepsTheOldBindingUntilRetry()
	{
		QFETCH(bool, shared); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = shared ? installAudio(t) : nullptr;
		auto midi = MidiJackTestAccess::create(audio, t.api); MidiDummy rejected, target; Processor processor; MidiPort port("stalled", midi.get(), &processor, nullptr, MidiPort::Mode::Output);
		if (audio) { audio->startProcessing(); } port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); t.latest().tick(); if (audio) { audio->stopProcessing(); }
		QVERIFY_EXCEPTION_THROWN(MidiClientTransferTestAccess::transfer(*midi, rejected), std::runtime_error); QCOMPARE(port.client(), static_cast<MidiClient*>(midi.get()));
		QVERIFY(!MidiJackTestAccess::changingGraph(*midi)); QVERIFY(midi->isAvailable()); QCOMPARE(t.closes.load(), 0);
		t.latest().launchCycles(); MidiClientTransferTestAccess::transfer(*midi, target); t.latest().join(); QCOMPARE(port.client(), static_cast<MidiClient*>(&target)); QCOMPARE(t.badAccess.load(), 0);
	}

	void transferJackCancellationClearsTheNativeGraphReservation_data()
	{
		QTest::addColumn<bool>("shared"); QTest::addColumn<bool>("throws");
		for (const bool shared : {false, true}) { for (const bool throws : {false, true})
		{ QTest::newRow(qPrintable(QString("shared-%1-throw-%2").arg(shared).arg(throws))) << shared << throws; } }
	}
	void transferJackCancellationClearsTheNativeGraphReservation()
	{
		QFETCH(bool, shared); QFETCH(bool, throws); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto audio = shared ? installAudio(t) : nullptr;
		auto midi = MidiJackTestAccess::create(audio, t.api); MidiDummy target; Processor processor; MidiPort port("cancel", midi.get(), &processor, nullptr, MidiPort::Mode::Output);
		if (audio) { audio->startProcessing(); } port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); t.latest().tick(); if (audio) { audio->stopProcessing(); }
		QVERIFY_EXCEPTION_THROWN(MidiClientTransferTestAccess::transfer(*midi, target, [&] {
			const auto cancel = MidiJackTestAccess::changingGraph(*midi); if (cancel && throws) { throw std::runtime_error("transfer cancellation fixture"); } return cancel;
		}), std::runtime_error);
		QCOMPARE(port.client(), static_cast<MidiClient*>(midi.get())); QVERIFY(!MidiJackTestAccess::changingGraph(*midi)); QVERIFY(midi->isAvailable()); QCOMPARE(t.badAccess.load(), 0);
	}

	void busGraphOptionalRealServerReceivesPreMasterSampleAudio()
	{
		if (qgetenv("LMMS_TEST_JACK") != "1") { QSKIP("Set LMMS_TEST_JACK=1 with a running JACK server for native track port PCM verification"); }
		const detail::JackApi api;
		struct Probe { const detail::JackApi* api; jack_client_t* client = nullptr; std::array<jack_port_t*, 2> ports{}; std::atomic<unsigned int> matched{0}; std::atomic_bool invalid{false}; } sink{&api};
		const auto retire = qScopeGuard([&] {
			Engine::getSong()->stop(); Engine::audioEngine()->audioDev()->stopProcessing();
			if (sink.client) { api.deactivate(sink.client); api.close(sink.client); }
		});
		jack_status_t status{}; sink.client = api.open("lmms-studio-track-sink", JackNoStartServer, &status); QVERIFY(sink.client);
		for (unsigned int channel = 0; channel < 2; ++channel)
		{ sink.ports[channel] = api.registerPort(sink.client, channel ? "right" : "left", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0); QVERIFY(sink.ports[channel]); }
		QCOMPARE(api.setProcessCallback(sink.client, [](jack_nframes_t frames, void* raw) noexcept -> int {
			const auto sink = static_cast<Probe*>(raw); unsigned int matched = 0;
			for (unsigned int channel = 0; channel < 2; ++channel)
			{
				const auto buffer = static_cast<float*>(sink->api->portBuffer(sink->ports[channel], frames));
				if (!buffer) { sink->invalid = true; continue; }
				for (jack_nframes_t frame = 0; frame < frames; ++frame)
				{
					if (!std::isfinite(buffer[frame])) { sink->invalid = true; }
					if (std::abs(buffer[frame] - (channel ? -.125f : .25f)) < .00001f) { ++matched; }
				}
			}
			sink->matched.fetch_add(matched); return 0;
		}, &sink), 0); QCOMPARE(api.activate(sink.client), 0);
		const auto selected = StudioCapabilities::execute("audio.backend.select", {{"name", AudioJack::name()}}); QVERIFY2(selected.value("ok").toBool(), qPrintable(selected.value("error").toString()));
		const auto engine = Engine::audioEngine(); const auto audio = dynamic_cast<AudioJack*>(engine->audioDev()); QVERIFY(audio);
		const auto track = busTrack(); const auto clip = static_cast<SampleClip*>(track->createClip(0));
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(engine->outputSampleRate() * 4, SampleFrame(.25f, -.125f)), engine->outputSampleRate()));
		audio->configureBusOutput(track->audioBusHandle(), true);
		for (unsigned int channel = 0; channel < 2; ++channel) { audio->setBusRoute(track->audioBusHandle(), channel, QString::fromUtf8(api.portName(sink.ports[channel])), true); }
		Engine::getSong()->setMasterVolume(0); Engine::getSong()->playSong();
		QTRY_VERIFY_WITH_TIMEOUT(sink.matched.load() >= 1024, 3000); QVERIFY(!sink.invalid.load()); Engine::getSong()->stop();
		audio->configureBusOutput(track->audioBusHandle(), false); QVERIFY(!audio->inspectBusGraph(track->audioBusHandle()).published);
	}

	void audioGraphOptionalRealServerCarriesNativePlaybackAndCapture()
	{
		if (qgetenv("LMMS_TEST_JACK") != "1") { QSKIP("Set LMMS_TEST_JACK=1 with a running JACK server for native master graph PCM verification"); }
		const detail::JackApi api;
		struct Probe { const detail::JackApi* api; bool source; jack_client_t* client = nullptr; std::array<jack_port_t*, 2> ports{}; std::atomic<unsigned int> matched{0}; std::atomic_bool invalid{false}; };
		Probe source{&api, true}, sink{&api, false};
		const auto retire = qScopeGuard([&] {
			Engine::getSong()->stop(); Engine::audioEngine()->audioDev()->stopProcessing();
			for (const auto probe : {&source, &sink}) { if (probe->client) { api.deactivate(probe->client); api.close(probe->client); } }
		});
		const auto process = [](jack_nframes_t frames, void* raw) noexcept -> int {
			const auto probe = static_cast<Probe*>(raw);
			for (unsigned int channel = 0; channel < 2; ++channel)
			{
				const auto buffer = static_cast<float*>(probe->api->portBuffer(probe->ports[channel], frames));
				if (!buffer) { probe->invalid = true; continue; }
				if (probe->source) { std::fill_n(buffer, frames, channel == 0 ? .125f : -.25f); }
				else { unsigned int matched = 0; for (jack_nframes_t frame = 0; frame < frames; ++frame) {
					if (!std::isfinite(buffer[frame])) { probe->invalid = true; }
					if (std::abs(buffer[frame] - (channel == 0 ? .25f : -.125f)) < .00001f) { ++matched; }
				} probe->matched.fetch_add(matched); }
			}
			return 0;
		};
		for (const auto probe : {&source, &sink})
		{
			jack_status_t status{}; probe->client = api.open(probe->source ? "lmms-studio-graph-source" : "lmms-studio-graph-sink", JackNoStartServer, &status); QVERIFY(probe->client);
			for (unsigned int channel = 0; channel < 2; ++channel) { probe->ports[channel] = api.registerPort(probe->client, channel ? "right" : "left", JACK_DEFAULT_AUDIO_TYPE, probe->source ? JackPortIsOutput : JackPortIsInput, 0); QVERIFY(probe->ports[channel]); }
			QCOMPARE(api.setProcessCallback(probe->client, process, probe), 0); QCOMPARE(api.activate(probe->client), 0);
		}
		const auto selected = StudioCapabilities::execute("audio.backend.select", {{"name", AudioJack::name()}}); QVERIFY2(selected.value("ok").toBool(), qPrintable(selected.value("error").toString()));
		for (int channel = 0; channel < 2; ++channel)
		{
			for (const bool input : {false, true})
			{
				const auto name = QString::fromUtf8(api.portName((input ? source : sink).ports[channel]));
				const auto response = StudioCapabilities::execute("audio.backend.route", {{"direction", input ? "input" : "output"}, {"channel", channel}, {"port", name}, {"enabled", true}});
				QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
			}
		}
		const auto engine = Engine::audioEngine(); const auto song = Engine::getSong(); const auto device = engine->audioDev(); device->stopProcessing();
		auto track = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, song)); auto clip = static_cast<SampleClip*>(track->createClip(0));
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32768, SampleFrame(.25f, -.125f)), engine->outputSampleRate()));
		song->setMasterVolume(100); song->playSong(); device->startProcessing(); QTRY_VERIFY_WITH_TIMEOUT(sink.matched.load() > 512, 5000);
		song->stop(); device->stopProcessing(); engine->discardInputBuffers(); auto recorded = static_cast<SampleClip*>(track->createClip(0));
		QVERIFY(engine->addPlayHandle(new SampleRecordHandle(recorded, 256))); device->startProcessing();
		const auto complete = [&] {
			const auto guard = engine->requestChangesGuard();
			return !recorded->hasActiveRecording() && (recorded->hasPendingRecording() || recorded->sample().sampleSize() == 256);
		};
		QTRY_VERIFY_WITH_TIMEOUT(complete(), 5000);
		device->stopProcessing(); engine->finishSampleRecordings(); QString error; QVERIFY2(SampleRecordHandle::finalizePendingRecordings(error), qPrintable(error));
		QCOMPARE(recorded->sample().sampleSize(), std::size_t{256});
		for (std::size_t frame = 0; frame < 256; ++frame) { QCOMPARE(recorded->sample().data()[frame].left(), .125f); QCOMPARE(recorded->sample().data()[frame].right(), -.25f); }
		QVERIFY(!source.invalid.load()); QVERIFY(!sink.invalid.load());
	}

	void optionalRealServerReceivesOutputPacketsAndCleanup()
	{
		if (qgetenv("LMMS_TEST_JACK") != "1") { QSKIP("Set LMMS_TEST_JACK=1 with a running JACK server"); }
		const detail::JackApi api; auto midi = MidiJackTestAccess::create(nullptr, api); QVERIFY(midi->isAvailable());
		struct Capture
		{
			jack_port_t* input = nullptr;
			std::array<std::array<unsigned char, 3>, 16> bytes{};
			std::atomic<unsigned int> count{0};
		} capture;
		jack_status_t status{}; const auto receiver = jack_client_open("lmms-output-test", JackNoStartServer, &status); QVERIFY(receiver);
		const auto close = qScopeGuard([&] { jack_deactivate(receiver); jack_client_close(receiver); });
		capture.input = jack_port_register(receiver, "capture", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0); QVERIFY(capture.input);
		QCOMPARE(jack_set_process_callback(receiver, [](jack_nframes_t frames, void* data) -> int {
			auto& capture = *static_cast<Capture*>(data); auto buffer = jack_port_get_buffer(capture.input, frames);
			if (!buffer) { return 0; }
			for (std::uint32_t index = 0; index < jack_midi_get_event_count(buffer); ++index)
			{
				jack_midi_event_t event{}; const auto at = capture.count.load(std::memory_order_relaxed);
				if (at == capture.bytes.size()) { break; }
				if (jack_midi_event_get(&event, buffer, index) || event.size != 3) { continue; }
				std::copy_n(event.buffer, 3, capture.bytes[at].data()); capture.count.store(at + 1, std::memory_order_release);
			}
			return 0;
		}, &capture), 0);
		QCOMPARE(jack_activate(receiver), 0);
		EngineMidiScope selected(midi.get()); const auto destination = QString::fromUtf8(jack_port_name(capture.input));
		const auto connected = StudioCapabilities::execute("midi.backend.route", {{"direction", "output"}, {"port", destination}, {"enabled", true}});
		QVERIFY2(connected.value("ok").toBool(), qPrintable(connected.value("error").toString()));
		const auto graph = midi->inspectBackendGraph(false);
		QVERIFY(std::any_of(graph.endpoints.begin(), graph.endpoints.end(), [&](const auto& endpoint) { return endpoint.name == destination && endpoint.connected; }));
		Processor processor; MidiPort port("real output", midi.get(), &processor);
		midi->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, &port);
		QTRY_VERIFY_WITH_TIMEOUT(capture.count.load(std::memory_order_acquire) >= 1, 3000);
		QVERIFY(capture.bytes[0] == (std::array<unsigned char, 3>{0x90,60,100}));
		midi->cancelOutputEvents(&port); QTRY_VERIFY_WITH_TIMEOUT(capture.count.load(std::memory_order_acquire) >= 5, 3000);
		for (unsigned int index = 1; index < 5; ++index) { QCOMPARE(capture.bytes[index][0], static_cast<unsigned char>(0xb0)); QCOMPARE(capture.bytes[index][2], static_cast<unsigned char>(0)); }
		QCOMPARE(capture.bytes[4][1], static_cast<unsigned char>(120));
		QVERIFY(StudioCapabilities::execute("midi.backend.route", {{"direction", "output"}, {"port", destination}, {"enabled", false}}).value("ok").toBool());
	}
};
QTEST_GUILESS_MAIN(JackLifetimeTest)
#include "JackLifetimeTest.moc"
