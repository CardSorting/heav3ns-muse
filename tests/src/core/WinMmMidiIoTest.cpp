/* Native WinMM MIDI lifecycle tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QEvent>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <vector>
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "MidiDummy.h"
#include "MidiEventProcessor.h"
#include "MidiPort.h"
#include "MidiWinMM.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "../../../src/core/midi/WinMmMidiApi.h"

namespace lmms
{
class WinMmMidiIoTestAccess
{
public:
	static std::unique_ptr<MidiWinMM> create(const detail::WinMmMidiApi& api)
	{ return std::unique_ptr<MidiWinMM>(new MidiWinMM(api, true)); }
	static void flush(MidiWinMM& client, const std::function<bool()>& cancelled = {})
	{ client.finishPortTransferOutput(cancelled); }
	static void close(MidiWinMM& client) { client.closeDevices(); }
	static void refresh(MidiWinMM& client) { client.updateDeviceList(); }
	static MidiWinMM* construct(void* storage, const detail::WinMmMidiApi& api) { return new (storage) MidiWinMM(api, true); }
};
class StudioMidiBackendSelectionTestAccess
{
public:
	static auto select(const std::function<std::unique_ptr<MidiClient>()>& factory)
	{ return Engine::audioEngine()->selectMidiBackend(MidiWinMM::name(), factory); }
};
}
using namespace lmms;
namespace
{
void pump() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
QByteArray settings(MidiPort& port)
{
	QDomDocument document; auto element = document.createElement("midiport"); document.appendChild(element);
	port.saveSettings(document, element); return document.toByteArray();
}
struct Processor : MidiEventProcessor
{
	std::vector<MidiEvent> events;
	bool wrongThread = false;
	void processInEvent(const MidiEvent& event, const TimePos&, f_cnt_t) override
	{ wrongThread = wrongThread || QThread::currentThread() != Engine::audioEngine()->thread(); events.push_back(event); }
	void processOutEvent(const MidiEvent&, const TimePos&, f_cnt_t) override { }
	bool requiresNoteCleanup() const override { return false; }
};
struct Transport
{
	using Callback = void (CALLBACK *)(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
	static inline Transport* current = nullptr;
	struct Handle
	{
		bool live = false;
		int opens = 0, resets = 0, closes = 0;
		MMRESULT result = MMSYSERR_NOERROR;
		MMRESULT resetResult = MMSYSERR_NOERROR, closeResult = MMSYSERR_NOERROR;
		bool throwReset = false, throwClose = false;
		bool throwSend = false;
		DWORD failMessage = 0;
		bool throwMessage = false;
		std::size_t messagesAtClose = 0;
		Callback callback = nullptr;
		DWORD_PTR token = 0;
		std::vector<DWORD> messages;
	};
	std::array<Handle, 2> inputs, outputs;
	UINT inputDevices = 1, outputDevices = 2;
	QString startupFault;
	int faultDevice = 0, badAccess = 0;
	bool loopback = false, callbacksDuringRetirement = false;
	std::function<void()> onOutputMessage;
	std::function<void()> onNativeReset, onNativeClose;
	detail::WinMmMidiApi api;

	Transport()
	{
		current = this;
		api.inputCount = &inCount; api.inputCapabilities = &inCapabilities; api.inputOpen = &inOpen;
		api.inputStart = &inStart; api.inputReset = &inReset; api.inputClose = &inClose;
		api.outputCount = &outCount; api.outputCapabilities = &outCapabilities; api.outputOpen = &outOpen;
		api.outputShortMessage = &outShortMessage; api.outputReset = &outReset; api.outputClose = &outClose;
	}
	~Transport()
	{
		// Keep fixture call targets alive through cleanup even when a Qt
		// assertion returns early. Each test inspects retained state first.
		onOutputMessage = onNativeReset = onNativeClose = {}; loopback = callbacksDuringRetirement = false;
		for (auto* group : {&inputs, &outputs}) { for (auto& handle : *group) {
			handle.result = handle.resetResult = handle.closeResult = MMSYSERR_NOERROR;
			handle.throwReset = handle.throwClose = handle.throwSend = handle.throwMessage = false; handle.failMessage = 0;
		} }
		inputDevices = outputDevices = 0;
		const auto state = MidiWinMM::retiredHandleStatus();
		if (state.pendingInputHandles || state.pendingOutputHandles) { auto cleanup = WinMmMidiIoTestAccess::create(api); }
		current = nullptr;
	}
	auto create() { return WinMmMidiIoTestAccess::create(api); }
	bool fault(const char* name, UINT index) const { return startupFault == name && index == static_cast<UINT>(faultDevice); }
	Handle* input(HMIDIIN handle)
	{
		for (auto& item : inputs) { if (handle == reinterpret_cast<HMIDIIN>(&item) && item.live) { return &item; } }
		++badAccess; return nullptr;
	}
	Handle* output(HMIDIOUT handle)
	{
		for (auto& item : outputs) { if (handle == reinterpret_cast<HMIDIOUT>(&item) && item.live) { return &item; } }
		++badAccess; return nullptr;
	}
	void feed(DWORD message = 0x00643c90, UINT kind = MIM_DATA)
	{
		auto& input = inputs[0];
		if (input.callback) { input.callback(reinterpret_cast<HMIDIIN>(&input), kind, input.token, message, 0); }
	}
	static UINT WINAPI inCount() { return current->inputDevices; }
	static UINT WINAPI outCount() { return current->outputDevices; }
	static MMRESULT WINAPI inCapabilities(UINT_PTR index, LPMIDIINCAPSW caps, UINT size)
	{
		auto& t = *current;
		if (index >= t.inputDevices || size != sizeof(*caps)) { ++t.badAccess; return MMSYSERR_BADDEVICEID; }
		if (t.fault("input-capabilities", static_cast<UINT>(index))) { return MMSYSERR_ERROR; }
		const auto name = QString("Input %1").arg(index).toStdWString(); std::copy_n(name.c_str(), name.size() + 1, caps->szPname);
		return MMSYSERR_NOERROR;
	}
	static MMRESULT WINAPI outCapabilities(UINT_PTR index, LPMIDIOUTCAPSW caps, UINT size)
	{
		auto& t = *current;
		if (index >= t.outputDevices || size != sizeof(*caps)) { ++t.badAccess; return MMSYSERR_BADDEVICEID; }
		if (t.fault("output-capabilities", static_cast<UINT>(index))) { return MMSYSERR_ERROR; }
		const auto name = QString("Output %1").arg(index).toStdWString(); std::copy_n(name.c_str(), name.size() + 1, caps->szPname);
		return MMSYSERR_NOERROR;
	}
	static MMRESULT WINAPI inOpen(LPHMIDIIN handle, UINT index, DWORD_PTR callback, DWORD_PTR token, DWORD flags)
	{
		auto& t = *current;
		if (index >= t.inputDevices || flags != (CALLBACK_FUNCTION | MIDI_IO_STATUS) || !callback || !token)
		{ ++t.badAccess; return MMSYSERR_INVALPARAM; }
		if (t.fault("input-open", index)) { return MMSYSERR_ALLOCATED; }
		if (t.fault("input-null", index)) { *handle = nullptr; return MMSYSERR_NOERROR; }
		auto& input = t.inputs[index]; if (input.live) { ++t.badAccess; return MMSYSERR_ALLOCATED; }
		++input.opens; input.live = true;
		input.callback = reinterpret_cast<Callback>(callback); input.token = token;
		*handle = reinterpret_cast<HMIDIIN>(&input);
		if (t.fault("input-open-throws", index)) { throw std::runtime_error("native input open fixture"); }
		return MMSYSERR_NOERROR;
	}
	static MMRESULT WINAPI outOpen(LPHMIDIOUT handle, UINT index, DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
	{
		auto& t = *current;
		if (index >= t.outputDevices || callback || instance || flags != CALLBACK_NULL) { ++t.badAccess; return MMSYSERR_INVALPARAM; }
		if (t.fault("output-open", index)) { return MMSYSERR_ALLOCATED; }
		if (t.fault("output-null", index)) { *handle = nullptr; return MMSYSERR_NOERROR; }
		auto& output = t.outputs[index]; if (output.live) { ++t.badAccess; return MMSYSERR_ALLOCATED; }
		++output.opens; output.live = true;
		*handle = reinterpret_cast<HMIDIOUT>(&output);
		if (t.fault("output-open-throws", index)) { throw std::runtime_error("native output open fixture"); }
		return MMSYSERR_NOERROR;
	}
	static MMRESULT WINAPI inStart(HMIDIIN handle)
	{
		auto& t = *current; const auto input = t.input(handle); if (!input) { return MMSYSERR_INVALHANDLE; }
		return t.fault("input-start", static_cast<UINT>(input - t.inputs.data())) ? MMSYSERR_ERROR : MMSYSERR_NOERROR;
	}
	static MMRESULT WINAPI inReset(HMIDIIN handle)
	{
		auto& t = *current; const auto input = t.input(handle); if (!input) { return MMSYSERR_INVALHANDLE; }
		++input->resets; if (t.onNativeReset) { t.onNativeReset(); } if (t.callbacksDuringRetirement) { t.feed(); }
		if (input->throwReset) { throw std::runtime_error("native input reset fixture"); } return input->resetResult;
	}
	static MMRESULT WINAPI inClose(HMIDIIN handle)
	{
		auto& t = *current; const auto input = t.input(handle); if (!input) { return MMSYSERR_INVALHANDLE; }
		++input->closes; if (t.onNativeClose) { t.onNativeClose(); }
		if (input->throwClose) { throw std::runtime_error("native input close fixture"); }
		if (input->closeResult == MMSYSERR_NOERROR || input->closeResult == MMSYSERR_INVALHANDLE) { input->live = false; }
		if (t.callbacksDuringRetirement) { t.feed(0, MIM_CLOSE); } return input->closeResult;
	}
	static MMRESULT WINAPI outShortMessage(HMIDIOUT handle, DWORD message)
	{
		auto& t = *current; const auto output = t.output(handle); if (!output) { return MMSYSERR_INVALHANDLE; }
		output->messages.push_back(message); if (t.onOutputMessage) { t.onOutputMessage(); } if (t.loopback) { t.feed(message); }
		if (output->failMessage && output->failMessage == message)
		{
			if (output->throwMessage) { throw std::runtime_error("native WinMM cleanup fixture"); }
			return MIDIERR_NOTREADY;
		}
		if (output->throwSend) { throw std::runtime_error("native WinMM send fixture"); } return output->result;
	}
	static MMRESULT WINAPI outClose(HMIDIOUT handle)
	{
		auto& t = *current; const auto output = t.output(handle); if (!output) { return MMSYSERR_INVALHANDLE; }
		++output->closes; output->messagesAtClose = output->messages.size(); if (t.onNativeClose) { t.onNativeClose(); }
		if (output->throwClose) { throw std::runtime_error("native output close fixture"); }
		if (output->closeResult == MMSYSERR_NOERROR || output->closeResult == MMSYSERR_INVALHANDLE) { output->live = false; }
		return output->closeResult;
	}
	static MMRESULT WINAPI outReset(HMIDIOUT handle)
	{
		auto& t = *current; const auto output = t.output(handle); if (!output) { return MMSYSERR_INVALHANDLE; }
		++output->resets; if (t.onNativeReset) { t.onNativeReset(); } if (t.loopback) { t.feed(); }
		if (output->throwReset) { throw std::runtime_error("native output reset fixture"); } return output->resetResult;
	}
};
std::vector<DWORD> cleanupMessages(int channel)
{
	return {DWORD(0x000078b0 | channel), DWORD(0x000040b0 | channel), DWORD(0x000042b0 | channel), DWORD(0x00007bb0 | channel)};
}
void retireNote(Transport& transport, MidiClient* client, int channel = 0)
{
	Processor processor; MidiPort port("retired-note", client, &processor, nullptr, MidiPort::Mode::Output);
	port.setOutputChannel(0); port.subscribeWritablePort("Output 0");
	port.processOutEvent(MidiEvent(MidiNoteOn, channel, 60, 100)); transport.outputs[0].result = MIDIERR_NOTREADY;
	QTest::ignoreMessage(QtWarningMsg, "MIDI output cleanup failed while retiring a port");
}
}

class WinMmMidiIoTest : public QObject
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
	}
	void cleanup() { Engine::destroy(); }

	void encodesChannelMessages_data()
	{
		QTest::addColumn<int>("type"); QTest::addColumn<int>("first"); QTest::addColumn<int>("second"); QTest::addColumn<quint32>("packed");
		QTest::newRow("note-on") << int(MidiNoteOn) << 60 << 100 << quint32(0x00643c95);
		QTest::newRow("note-off") << int(MidiNoteOff) << 60 << 17 << quint32(0x00113c85);
		QTest::newRow("key-pressure") << int(MidiKeyPressure) << 60 << 73 << quint32(0x00493ca5);
		QTest::newRow("control") << int(MidiControlChange) << 74 << 99 << quint32(0x00634ab5);
		QTest::newRow("program") << int(MidiProgramChange) << 37 << 0 << quint32(0x000025c5);
		QTest::newRow("channel-pressure") << int(MidiChannelPressure) << 81 << 0 << quint32(0x000051d5);
		QTest::newRow("pitch") << int(MidiPitchBend) << 0x2345 << 0 << quint32(0x004645e5);
	}
	void encodesChannelMessages()
	{
		QFETCH(int, type); QFETCH(int, first); QFETCH(int, second); QFETCH(quint32, packed);
		Transport t; auto client = t.create(); Processor processor; MidiPort port("output", client.get(), &processor, nullptr, MidiPort::Mode::Output);
		port.subscribeWritablePort("Output 1"); t.outputs[1].messages.clear();
		client->processOutEvent(MidiEvent(static_cast<MidiEventTypes>(type), 5, first, second), {}, &port);
		QCOMPARE(t.outputs[1].messages.size(), std::size_t{1}); QCOMPARE(quint32(t.outputs[1].messages.front()), packed);
		QVERIFY(t.outputs[0].messages.empty()); QCOMPARE(t.badAccess, 0);
	}

	void rejectsInvalidPackets_data()
	{
		QTest::addColumn<int>("type"); QTest::addColumn<int>("channel"); QTest::addColumn<int>("first"); QTest::addColumn<int>("second");
		QTest::newRow("negative-channel") << int(MidiNoteOn) << -1 << 60 << 100;
		QTest::newRow("large-channel") << int(MidiNoteOn) << 16 << 60 << 100;
		QTest::newRow("negative-key") << int(MidiNoteOn) << 0 << -1 << 100;
		QTest::newRow("large-key") << int(MidiNoteOn) << 0 << 128 << 100;
		QTest::newRow("large-velocity") << int(MidiNoteOn) << 0 << 60 << 128;
		QTest::newRow("large-program") << int(MidiProgramChange) << 0 << 128 << 0;
		QTest::newRow("large-pitch") << int(MidiPitchBend) << 0 << 16384 << 0;
		QTest::newRow("system-exclusive") << int(MidiSysEx) << 0 << 0 << 0;
	}
	void rejectsInvalidPackets()
	{
		QFETCH(int, type); QFETCH(int, channel); QFETCH(int, first); QFETCH(int, second);
		Transport t; auto client = t.create(); Processor processor; MidiPort port("output", client.get(), &processor, nullptr, MidiPort::Mode::Output);
		port.subscribeWritablePort("Output 0"); t.outputs[0].messages.clear();
		client->processOutEvent(MidiEvent(static_cast<MidiEventTypes>(type), channel, first, second), {}, &port);
		QVERIFY(t.outputs[0].messages.empty()); QVERIFY(t.outputs[1].messages.empty()); QVERIFY(client->outputBackendStatus().active);
	}

	void sendsOnlyToSubscribedPorts()
	{
		Transport t; auto client = t.create(); Processor processor;
		MidiPort a("a", client.get(), &processor, nullptr, MidiPort::Mode::Output), b("b", client.get(), &processor, nullptr, MidiPort::Mode::Output);
		a.subscribeWritablePort("Output 0"); b.subscribeWritablePort("Output 1"); a.subscribeWritablePort("missing output");
		for (auto& output : t.outputs) { output.messages.clear(); }
		client->processOutEvent(MidiEvent(MidiProgramChange, 0, 37), {}, &a);
		QCOMPARE(t.outputs[0].messages.size(), std::size_t{1}); QVERIFY(t.outputs[1].messages.empty());
		a.subscribeWritablePort("Output 0", false); client->processOutEvent(MidiEvent(MidiProgramChange, 0, 38), {}, &a);
		QCOMPARE(t.outputs[0].messages.size(), std::size_t{1}); QVERIFY(t.outputs[1].messages.empty());
		client->processOutEvent(MidiEvent(MidiProgramChange, 0, 39), {}, &b); QCOMPARE(t.outputs[1].messages.size(), std::size_t{1});
	}

	void fanoutFailureRetainsNotesUntilRetry_data()
	{
		QTest::addColumn<QString>("failure");
		for (const auto& failure : {"first", "second", "both", "exception"}) { QTest::newRow(failure) << QString(failure); }
	}
	void fanoutFailureRetainsNotesUntilRetry()
	{
		QFETCH(QString, failure); Transport t; auto client = t.create(); Processor processor;
		MidiPort port("output", client.get(), &processor, nullptr, MidiPort::Mode::Output); port.setOutputChannel(0);
		port.subscribeWritablePort("Output 0"); port.subscribeWritablePort("Output 1");
		port.processOutEvent(MidiEvent(MidiNoteOn, 3, 60, 100)); QCOMPARE(port.trackedOutputNotes(), 1u);
		for (auto& output : t.outputs) { output.messages.clear(); }
		if (failure == "first" || failure == "both") { t.outputs[0].result = MIDIERR_NOTREADY; }
		if (failure == "second" || failure == "both") { t.outputs[1].result = MIDIERR_NOTREADY; }
		if (failure == "exception") { t.outputs[0].throwSend = true; }
		QVERIFY_EXCEPTION_THROWN(port.processOutEvent(MidiEvent(MidiNoteOff, 3, 60, 0)), std::runtime_error);
		QCOMPARE(port.trackedOutputNotes(), 1u);
		for (const auto& output : t.outputs) { QCOMPARE(output.messages.size(), std::size_t{1}); QCOMPARE(output.messages.front(), DWORD(0x00003c83)); }
		const int expected = failure == "exception" ? MMSYSERR_ERROR : MIDIERR_NOTREADY;
		auto status = client->outputBackendStatus(); QVERIFY(status.observed && !status.active); QCOMPARE(status.nativeErrorCode, expected);
		QCOMPARE(status.lastNativeErrorCode, expected); QCOMPARE(status.pendingEvents, 0u); QCOMPARE(status.queueLimit, 0u);
		QVERIFY(!status.abandonmentSupported && !status.abandonmentEligible); QCOMPARE(client->inputBackendStatus().nativeErrorCode, 0);
		for (auto& output : t.outputs) { output.result = MMSYSERR_NOERROR; output.throwSend = false; }
		port.releaseOutput(); QCOMPARE(port.trackedOutputNotes(), 0u);
		for (const auto& output : t.outputs) { QCOMPARE(output.messages.size(), std::size_t{2}); QCOMPARE(output.messages.back(), DWORD(0x00003c83)); }
		status = client->outputBackendStatus(); QVERIFY(status.active); QCOMPARE(status.nativeErrorCode, 0); QCOMPARE(status.lastNativeErrorCode, expected);
		port.releaseOutput(); for (const auto& output : t.outputs) { QCOMPARE(output.messages.size(), std::size_t{2}); }
	}

	void failedPedalReleaseRemainsRetryable_data()
	{
		QTest::addColumn<int>("controller"); QTest::newRow("sustain") << int(MidiControllerSustain); QTest::newRow("sostenuto") << int(MidiControllerSostenuto);
	}
	void failedPedalReleaseRemainsRetryable()
	{
		QFETCH(int, controller); Transport t; auto client = t.create(); Processor processor;
		MidiPort port("pedal", client.get(), &processor, nullptr, MidiPort::Mode::Output); port.subscribeWritablePort("Output 0");
		port.processOutEvent(MidiEvent(MidiControlChange, 0, controller, 127)); t.outputs[0].messages.clear(); t.outputs[0].result = MMSYSERR_INVALHANDLE;
		QVERIFY_EXCEPTION_THROWN(port.releaseOutput(), std::runtime_error); QCOMPARE(t.outputs[0].messages.size(), std::size_t{1});
		t.outputs[0].result = MMSYSERR_NOERROR; port.releaseOutput(); QCOMPARE(t.outputs[0].messages.size(), std::size_t{2});
		QCOMPARE(t.outputs[0].messages.front(), DWORD(0xb0 | (controller << 8))); QCOMPARE(t.outputs[0].messages.back(), t.outputs[0].messages.front());
		port.releaseOutput(); QCOMPARE(t.outputs[0].messages.size(), std::size_t{2}); QVERIFY(client->outputBackendStatus().active);
	}

	void errorOnAnotherDestinationSurvivesSuccess()
	{
		Transport t; auto client = t.create(); Processor processor;
		MidiPort a("a", client.get(), &processor, nullptr, MidiPort::Mode::Output), b("b", client.get(), &processor, nullptr, MidiPort::Mode::Output);
		a.subscribeWritablePort("Output 0"); b.subscribeWritablePort("Output 1"); t.outputs[0].result = MIDIERR_NOTREADY;
		QVERIFY_EXCEPTION_THROWN(a.processOutEvent(MidiEvent(MidiProgramChange, 0, 37)), std::runtime_error);
		b.processOutEvent(MidiEvent(MidiProgramChange, 0, 38)); QVERIFY(!client->outputBackendStatus().active); QCOMPARE(client->outputBackendStatus().nativeErrorCode, int(MIDIERR_NOTREADY));
		t.outputs[1].result = MMSYSERR_INVALHANDLE; QVERIFY_EXCEPTION_THROWN(b.processOutEvent(MidiEvent(MidiProgramChange, 0, 39)), std::runtime_error);
		QCOMPARE(client->outputBackendStatus().lastNativeErrorCode, int(MMSYSERR_INVALHANDLE));
		t.outputs[0].result = MMSYSERR_NOERROR; a.processOutEvent(MidiEvent(MidiProgramChange, 0, 40)); QVERIFY(!client->outputBackendStatus().active);
		QCOMPARE(client->outputBackendStatus().nativeErrorCode, int(MMSYSERR_INVALHANDLE));
		t.outputs[1].result = MMSYSERR_NOERROR; b.processOutEvent(MidiEvent(MidiProgramChange, 0, 41)); QVERIFY(client->outputBackendStatus().active);
		QCOMPARE(client->outputBackendStatus().lastNativeErrorCode, int(MMSYSERR_INVALHANDLE));
	}

	void outputFaultDoesNotInvalidateInput_data()
	{
		QTest::addColumn<QString>("failure");
		for (const auto& failure : {"send", "output-capabilities", "output-open", "output-null", "no-outputs"}) { QTest::newRow(failure) << QString(failure); }
	}
	void outputFaultDoesNotInvalidateInput()
	{
		QFETCH(QString, failure); Transport t; t.outputDevices = failure == "no-outputs" ? 0 : 1; t.startupFault = failure;
		auto client = t.create(); Processor processor; MidiPort port("duplex", client.get(), &processor, nullptr, MidiPort::Mode::Duplex);
		port.subscribeReadablePort("Input 0"); port.subscribeWritablePort("Output 0");
		if (failure == "send") { t.outputs[0].result = MIDIERR_NOTREADY; QVERIFY_EXCEPTION_THROWN(port.processOutEvent(MidiEvent(MidiProgramChange, 0, 37)), std::runtime_error); }
		auto input = client->inputBackendStatus(); QVERIFY(input.observed && input.active); QCOMPARE(input.nativeErrorCode, 0); QVERIFY(client->isAvailable());
		const auto output = client->outputBackendStatus(); QVERIFY(output.observed && !output.active);
		QCOMPARE(output.nativeErrorCode, failure == "no-outputs" ? 0 : failure == "send" ? int(MIDIERR_NOTREADY) : failure == "output-open" ? int(MMSYSERR_ALLOCATED) : failure == "output-null" ? int(MMSYSERR_INVALHANDLE) : int(MMSYSERR_ERROR));
		t.feed(); QCOMPARE(client->inputDeliveryStatus(&port).pendingEvents, 1u); pump(); QCOMPARE(processor.events.size(), std::size_t{1});
		QVERIFY(!processor.wrongThread); QCOMPARE(processor.events.front().sourcePortName(), QString("Input 0")); QVERIFY(!client->inputDeliveryStatus(&port).failed);
		t.outputs[0].result = MMSYSERR_NOERROR;
	}

	void loopbackInputIsQueuedWhileSending()
	{
		Transport t; auto client = t.create(); Processor processor; MidiPort port("loopback", client.get(), &processor, nullptr, MidiPort::Mode::Duplex);
		port.subscribeReadablePort("Input 0"); port.subscribeWritablePort("Output 0"); t.loopback = true;
		port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); QVERIFY(processor.events.empty()); QCOMPARE(client->inputDeliveryStatus(&port).pendingEvents, 1u);
		pump(); QCOMPARE(processor.events.size(), std::size_t{1}); QCOMPARE(processor.events.front().type(), MidiNoteOn); QVERIFY(!processor.wrongThread);
		t.loopback = false; port.releaseOutput(); QCOMPARE(t.badAccess, 0);
	}

	void constructionKeepsOnlyOwnedHandles_data()
	{
		QTest::addColumn<QString>("failure");
		for (const auto& failure : {"none", "input-only", "output-only", "empty", "input-capabilities", "input-open", "input-null", "input-start", "output-capabilities", "output-open", "output-null"})
		{ QTest::newRow(failure) << QString(failure); }
	}
	void constructionKeepsOnlyOwnedHandles()
	{
		QFETCH(QString, failure); Transport t; t.startupFault = failure;
		t.inputDevices = failure == "output-only" || failure == "empty" ? 0 : 1;
		t.outputDevices = failure == "input-only" || failure == "empty" ? 0 : 1;
		auto client = t.create();
		const bool inputFault = failure.startsWith("input-") && failure != "input-only";
		const bool outputFault = failure.startsWith("output-") && failure != "output-only";
		const bool inputActive = t.inputDevices && !inputFault, outputActive = t.outputDevices && !outputFault;
		QCOMPARE(client->inputBackendStatus().active, inputActive); QCOMPARE(client->outputBackendStatus().active, outputActive);
		QCOMPARE(client->isAvailable(), inputActive || outputActive); QCOMPARE(client->readablePorts().size(), inputActive ? 1 : 0); QCOMPARE(client->writablePorts().size(), outputActive ? 1 : 0);
		QCOMPARE(client->inputBackendStatus().nativeErrorCode != 0, inputFault); QCOMPARE(client->outputBackendStatus().lastNativeErrorCode != 0, outputFault);
		client.reset(); QCOMPARE(t.inputs[0].closes, t.inputs[0].opens); QCOMPARE(t.outputs[0].closes, t.outputs[0].opens);
		QCOMPARE(t.inputs[0].resets, inputActive ? 1 : 0); QVERIFY(!t.inputs[0].live && !t.outputs[0].live); QCOMPARE(t.badAccess, 0);
	}

	void engineAcceptsOutputOnlyCandidateAndReportsIndependentHealth()
	{
		Transport t; t.inputDevices = 0; t.startupFault = "output-open"; t.faultDevice = 1;
		const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine(); Processor processor;
		MidiPort port("engine-output", engine->midiClient(), &processor, nullptr, MidiPort::Mode::Output);
		port.setOutputChannel(0); port.setFixedOutputVelocity(91); port.subscribeWritablePort("Output 0"); port.subscribeWritablePort("missing output"); const auto before = settings(port);
		const auto selected = StudioMidiBackendSelectionTestAccess::select([&] { return t.create(); }); QVERIFY(selected.audioRestored);
		QCOMPARE(port.client(), engine->midiClient()); QCOMPARE(engine->midiClientName(), MidiWinMM::name()); QCOMPARE(settings(port), before);
		const auto inspection = StudioCapabilities::execute("transport.inspect", {}); QVERIFY(inspection.value("ok").toBool());
		const auto backend = inspection.value("result").toObject().value("midiBackend").toObject();
		QVERIFY(!backend.value("inputBackend").toObject().value("active").toBool());
		const auto output = backend.value("outputBackend").toObject(); QVERIFY(output.value("observed").toBool() && output.value("active").toBool());
		QCOMPARE(output.value("nativeErrorCode").toInt(), 0); QCOMPARE(output.value("lastNativeErrorCode").toInt(), int(MMSYSERR_ALLOCATED));
		QCOMPARE(output.value("queueLimit").toInt(), 0); QVERIFY(!output.value("abandonmentSupported").toBool());
		t.outputs[0].messages.clear(); port.processOutEvent(MidiEvent(MidiNoteOn, 5, 60, 100));
		QCOMPARE(t.outputs[0].messages.size(), std::size_t{1}); QCOMPARE(t.outputs[0].messages.front(), DWORD(0x005b3c95));
		QVERIFY(engine->selectMidiBackend(MidiDummy::name()).audioRestored); QCOMPARE(port.trackedOutputNotes(), 0u); QCOMPARE(port.client(), engine->midiClient());
		QCOMPARE(settings(port), before); QCOMPARE(t.outputs[0].closes, 1); QCOMPARE(t.outputs[1].closes, 0); QCOMPARE(t.badAccess, 0);
	}

	void engineRejectsUnusableCandidate_data()
	{
		QTest::addColumn<QString>("failure"); for (const auto& failure : {"empty", "output-open", "input-start"}) { QTest::newRow(failure) << QString(failure); }
	}
	void engineRejectsUnusableCandidate()
	{
		QFETCH(QString, failure); Transport t; t.startupFault = failure; t.inputDevices = failure == "input-start" ? 1 : 0; t.outputDevices = failure == "output-open" ? 1 : 0;
		const auto engine = Engine::audioEngine(); const auto previous = engine->midiClient(); Processor processor;
		MidiPort port("unchanged", previous, &processor, nullptr, MidiPort::Mode::Output); const auto before = settings(port);
		QVERIFY_EXCEPTION_THROWN(StudioMidiBackendSelectionTestAccess::select([&] { return t.create(); }), std::runtime_error);
		QCOMPARE(engine->midiClient(), previous); QCOMPARE(port.client(), previous); QCOMPARE(settings(port), before); QVERIFY(!engine->hasStoredAudioDevice());
		QCOMPARE(t.inputs[0].closes, t.inputs[0].opens); QCOMPARE(t.outputs[0].closes, t.outputs[0].opens); QCOMPARE(t.badAccess, 0);
	}

	void failedCleanupPreventsEngineReplacement_data()
	{
		QTest::addColumn<bool>("pedal"); QTest::newRow("note") << false; QTest::newRow("pedal") << true;
	}
	void failedCleanupPreventsEngineReplacement()
	{
		QFETCH(bool, pedal); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine(); Processor processor;
		StudioMidiBackendSelectionTestAccess::select([&] { return t.create(); }); const auto source = engine->midiClient();
		MidiPort port("cleanup", source, &processor, nullptr, MidiPort::Mode::Output); port.subscribeWritablePort("Output 0");
		port.processOutEvent(pedal ? MidiEvent(MidiControlChange, 0, MidiControllerSustain, 127) : MidiEvent(MidiNoteOn, 0, 60, 100));
		t.outputs[0].messages.clear(); t.outputs[0].result = MIDIERR_NOTREADY;
		QVERIFY_EXCEPTION_THROWN(engine->selectMidiBackend(MidiDummy::name()), std::runtime_error);
		QCOMPARE(engine->midiClient(), source); QCOMPARE(port.client(), source); QCOMPARE(port.trackedOutputNotes(), pedal ? 0u : 1u);
		QCOMPARE(t.outputs[0].closes, 0); QCOMPARE(t.outputs[0].messages.size(), std::size_t{1}); QVERIFY(!engine->hasStoredAudioDevice());
		QVERIFY_EXCEPTION_THROWN(engine->selectMidiBackend(MidiDummy::name(), true), std::runtime_error); QCOMPARE(engine->midiClient(), source);
		QCOMPARE(t.outputs[0].messages.size(), std::size_t{1}); t.outputs[0].result = MMSYSERR_NOERROR;
		QVERIFY(engine->selectMidiBackend(MidiDummy::name()).audioRestored); QCOMPARE(port.client(), engine->midiClient()); QCOMPARE(port.trackedOutputNotes(), 0u);
		const DWORD release = pedal ? DWORD(0x000040b0) : DWORD(0x00003c80);
		std::vector<DWORD> expected{release, release}; const auto channelCleanup = cleanupMessages(0); expected.insert(expected.end(), channelCleanup.begin(), channelCleanup.end());
		QCOMPARE(t.outputs[0].messages, expected);
		QCOMPARE(t.inputs[0].closes, 1); QCOMPARE(t.outputs[0].closes, 1); QCOMPARE(t.outputs[1].closes, 1); QCOMPARE(t.badAccess, 0);
	}

	void callbacksAreRevokedBeforeResetAndClose()
	{
		Transport t; t.callbacksDuringRetirement = true; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine(); Processor processor;
		MidiPort port("input", engine->midiClient(), &processor, nullptr, MidiPort::Mode::Input); port.subscribeReadablePort("Input 0");
		StudioMidiBackendSelectionTestAccess::select([&] { return t.create(); }); QString error; bool changed = false;
		QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed); t.feed(); QCOMPARE(engine->midiClient()->inputDeliveryStatus(&port).pendingEvents, 1u);
		QVERIFY(engine->selectMidiBackend(MidiDummy::name()).audioRestored); t.feed(); pump(); QVERIFY(processor.events.empty());
		QCOMPARE(t.inputs[0].resets, 1); QCOMPARE(t.inputs[0].closes, 1); QCOMPARE(t.badAccess, 0); QVERIFY(port.client()->inputDeliveryStatus(&port).failed);
	}

	void retiredCleanupRemainsVisibleAndPrecedesNewMusic_data()
	{
		QTest::addColumn<QString>("activation");
		for (const auto activation : {"note", "sustain", "sostenuto", "failed-note-on"}) { QTest::newRow(activation) << QString(activation); }
	}
	void retiredCleanupRemainsVisibleAndPrecedesNewMusic()
	{
		QFETCH(QString, activation); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		StudioMidiBackendSelectionTestAccess::select([&] { return t.create(); }); Processor processor;
		{
			MidiPort old("retired", engine->midiClient(), &processor, nullptr, MidiPort::Mode::Output); old.subscribeWritablePort("Output 0");
			if (activation == "failed-note-on")
			{
				t.outputs[0].result = MIDIERR_NOTREADY;
				QVERIFY_EXCEPTION_THROWN(old.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)), std::runtime_error);
			}
			else if (activation == "note") { old.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); }
			else { old.processOutEvent(MidiEvent(MidiControlChange, 0, activation == "sustain" ? MidiControllerSustain : MidiControllerSostenuto, 127)); }
			t.outputs[0].result = MIDIERR_NOTREADY; QTest::ignoreMessage(QtWarningMsg, "MIDI output cleanup failed while retiring a port");
		}
		auto status = engine->midiClient()->outputBackendStatus(); QVERIFY(!status.active); QCOMPARE(status.cleanupPendingChannels, 1u);
		QCOMPARE(status.queueLimit, 0u); QCOMPARE(status.pendingEvents, 0u);
		const auto inspection = StudioCapabilities::execute("transport.inspect", {}); QVERIFY(inspection.value("ok").toBool());
		QCOMPARE(inspection.value("result").toObject().value("midiBackend").toObject().value("outputBackend").toObject().value("cleanupPendingChannels").toInt(), 1);
		MidiPort other("other", engine->midiClient(), &processor, nullptr, MidiPort::Mode::Output); other.subscribeWritablePort("Output 1");
		other.processOutEvent(MidiEvent(MidiProgramChange, 0, 37)); QVERIFY(!engine->midiClient()->outputBackendStatus().active);
		QCOMPARE(engine->midiClient()->outputBackendStatus().cleanupPendingChannels, 1u);
		t.outputs[0].result = MMSYSERR_NOERROR; t.outputs[0].messages.clear();
		MidiPort next("next", engine->midiClient(), &processor, nullptr, MidiPort::Mode::Output); next.subscribeWritablePort("Output 0");
		next.processOutEvent(MidiEvent(MidiProgramChange, 0, 38)); auto expected = cleanupMessages(0); expected.push_back(0x000026c0);
		QCOMPARE(t.outputs[0].messages, expected); status = engine->midiClient()->outputBackendStatus();
		QVERIFY(status.active); QCOMPARE(status.cleanupPendingChannels, 0u); QCOMPARE(status.nativeErrorCode, 0); QCOMPARE(status.lastNativeErrorCode, int(MIDIERR_NOTREADY));
		QCOMPARE(t.badAccess, 0);
	}

	void retirementRetainsEveryUnreleasedChannel_data()
	{
		QTest::addColumn<bool>("pedal"); QTest::newRow("note-release") << false; QTest::newRow("pedal-release") << true;
	}
	void retirementRetainsEveryUnreleasedChannel()
	{
		QFETCH(bool, pedal); Transport t; auto client = t.create(); Processor processor;
		{
			MidiPort old("many-channels", client.get(), &processor, nullptr, MidiPort::Mode::Output); old.setOutputChannel(0); old.subscribeWritablePort("Output 0");
			for (const int channel : {0, 2, 10}) { old.processOutEvent(MidiEvent(MidiNoteOn, channel, 60, 100)); }
			old.processOutEvent(MidiEvent(MidiControlChange, 15, MidiControllerSostenuto, 127));
			if (pedal) { old.processOutEvent(MidiEvent(MidiControlChange, 2, MidiControllerSustain, 127)); }
			t.outputs[0].messages.clear(); t.outputs[0].failMessage = pedal ? 0x000040b2 : 0x00003c82;
			QTest::ignoreMessage(QtWarningMsg, "MIDI output cleanup failed while retiring a port");
		}
		QCOMPARE(t.outputs[0].messages.size(), std::size_t{2}); QCOMPARE(t.outputs[0].messages.front(), DWORD(0x00003c80));
		QCOMPARE(client->outputBackendStatus().cleanupPendingChannels, 3u); t.outputs[0].failMessage = 0; t.outputs[0].messages.clear();
		WinMmMidiIoTestAccess::flush(*client); std::vector<DWORD> expected;
		// Final transfer cleanup also covers a previously released but used channel.
		for (const int channel : {0, 2, 10, 15}) { const auto messages = cleanupMessages(channel); expected.insert(expected.end(), messages.begin(), messages.end()); }
		QCOMPARE(t.outputs[0].messages, expected); QCOMPARE(client->outputBackendStatus().cleanupPendingChannels, 0u); QVERIFY(client->outputBackendStatus().active);
		QVERIFY(t.outputs[1].messages.empty()); QCOMPARE(t.badAccess, 0);
	}

	void cleanupFailureCannotBeHiddenByLaterControllerSuccess_data()
	{
		QTest::addColumn<int>("index"); QTest::addColumn<bool>("throwing");
		for (int index = 0; index < 4; ++index)
		{
			for (const bool throwing : {false, true}) { QTest::newRow(qPrintable(QString("controller-%1-throw-%2").arg(index).arg(throwing))) << index << throwing; }
		}
	}
	void cleanupFailureCannotBeHiddenByLaterControllerSuccess()
	{
		QFETCH(int, index); QFETCH(bool, throwing); Transport t; auto client = t.create(); retireNote(t, client.get(), 3);
		t.outputs[0].result = MMSYSERR_NOERROR; t.outputs[0].messages.clear(); const auto cleanup = cleanupMessages(3);
		t.outputs[0].failMessage = cleanup[index]; t.outputs[0].throwMessage = throwing;
		Processor processor; MidiPort next("next", client.get(), &processor, nullptr, MidiPort::Mode::Output); next.subscribeWritablePort("Output 0");
		QVERIFY_EXCEPTION_THROWN(next.processOutEvent(MidiEvent(MidiProgramChange, 0, 37)), std::runtime_error);
		QCOMPARE(t.outputs[0].messages, cleanup); auto status = client->outputBackendStatus(); QVERIFY(!status.active); QCOMPARE(status.cleanupPendingChannels, 1u);
		QCOMPARE(status.nativeErrorCode, throwing ? int(MMSYSERR_ERROR) : int(MIDIERR_NOTREADY)); QCOMPARE(status.lastNativeErrorCode, status.nativeErrorCode);
		t.outputs[0].failMessage = 0; t.outputs[0].messages.clear(); next.processOutEvent(MidiEvent(MidiProgramChange, 0, 38));
		auto expected = cleanup; expected.push_back(0x000026c0); QCOMPARE(t.outputs[0].messages, expected);
		QVERIFY(client->outputBackendStatus().active); QCOMPARE(client->outputBackendStatus().cleanupPendingChannels, 0u); QCOMPARE(t.badAccess, 0);
	}

	void cancelledCleanupRetainsIncompleteChannels_data()
	{
		QTest::addColumn<int>("after"); for (const int after : {0, 1, 3, 4, 5, 7}) { QTest::newRow(qPrintable(QString::number(after))) << after; }
	}
	void cancelledCleanupRetainsIncompleteChannels()
	{
		QFETCH(int, after); Transport t; auto client = t.create(); Processor processor;
		{
			MidiPort old("cancelled", client.get(), &processor, nullptr, MidiPort::Mode::Output); old.setOutputChannel(0); old.subscribeWritablePort("Output 0");
			old.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); old.processOutEvent(MidiEvent(MidiNoteOn, 7, 64, 100));
			t.outputs[0].result = MIDIERR_NOTREADY; QTest::ignoreMessage(QtWarningMsg, "MIDI output cleanup failed while retiring a port");
		}
		t.outputs[0].result = MMSYSERR_NOERROR; t.outputs[0].messages.clear();
		QVERIFY_EXCEPTION_THROWN(WinMmMidiIoTestAccess::flush(*client, [&] { return t.outputs[0].messages.size() >= static_cast<std::size_t>(after); }), std::runtime_error);
		QCOMPARE(t.outputs[0].messages.size(), static_cast<std::size_t>(after)); QVERIFY(!client->outputBackendStatus().active);
		QCOMPARE(client->outputBackendStatus().cleanupPendingChannels, after >= 4 ? 1u : 2u); t.outputs[0].messages.clear();
		WinMmMidiIoTestAccess::flush(*client); std::vector<DWORD> expected;
		if (after < 4) { expected = cleanupMessages(0); } const auto last = cleanupMessages(7); expected.insert(expected.end(), last.begin(), last.end());
		QCOMPARE(t.outputs[0].messages, expected); QVERIFY(client->outputBackendStatus().active); QCOMPARE(client->outputBackendStatus().cleanupPendingChannels, 0u);
	}

	void backendReplacementRetriesCleanupAfterLastPortRetires()
	{
		Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		StudioMidiBackendSelectionTestAccess::select([&] { return t.create(); }); const auto source = engine->midiClient(); retireNote(t, source);
		t.outputs[0].messages.clear(); const auto refused = StudioCapabilities::execute("midi.backend.select", {{"name", MidiDummy::name()}}); QVERIFY(!refused.value("ok").toBool());
		QCOMPARE(engine->midiClient(), source); QCOMPARE(source->outputBackendStatus().cleanupPendingChannels, 1u); QCOMPARE(t.outputs[0].messages, cleanupMessages(0));
		QCOMPARE(t.outputs[0].closes, 0); QVERIFY(!engine->hasStoredAudioDevice());
		const auto abandon = StudioCapabilities::execute("midi.backend.select", {{"name", MidiDummy::name()}, {"abandonFailedOutput", true}}); QVERIFY(!abandon.value("ok").toBool()); QCOMPARE(engine->midiClient(), source);
		t.outputs[0].result = MMSYSERR_NOERROR; t.outputs[0].messages.clear();
		const auto retried = StudioCapabilities::execute("midi.backend.select", {{"name", MidiDummy::name()}}); QVERIFY(retried.value("ok").toBool());
		QVERIFY(retried.value("result").toObject().value("committed").toBool()); QVERIFY(retried.value("result").toObject().value("audioRestored").toBool());
		QCOMPARE(t.outputs[0].messages, cleanupMessages(0)); QCOMPARE(t.outputs[0].closes, 1); QCOMPARE(t.outputs[1].closes, 1); QCOMPARE(t.inputs[0].closes, 1);
		QCOMPARE(engine->midiClientName(), MidiDummy::name()); QCOMPARE(t.badAccess, 0);
	}

	void shutdownAttemptsEveryRetiredEndpointBeforeClose_data()
	{
		QTest::addColumn<QString>("failure"); for (const auto failure : {"none", "first-output", "exception"}) { QTest::newRow(failure) << QString(failure); }
	}
	void shutdownAttemptsEveryRetiredEndpointBeforeClose()
	{
		QFETCH(QString, failure); Transport t; auto client = t.create(); Processor processor;
		{
			MidiPort old("fanout", client.get(), &processor, nullptr, MidiPort::Mode::Output); old.setOutputChannel(0);
			old.subscribeWritablePort("Output 0"); old.subscribeWritablePort("Output 1"); old.processOutEvent(MidiEvent(MidiNoteOn, 3, 60, 100));
			for (auto& output : t.outputs) { output.result = MIDIERR_NOTREADY; } QTest::ignoreMessage(QtWarningMsg, "MIDI output cleanup failed while retiring a port");
		}
		QCOMPARE(client->outputBackendStatus().cleanupPendingChannels, 1u);
		for (auto& output : t.outputs) { output.result = MMSYSERR_NOERROR; output.messages.clear(); }
		if (failure == "first-output") { t.outputs[0].result = MIDIERR_NOTREADY; } if (failure == "exception") { t.outputs[0].throwSend = true; }
		if (failure != "none") { QTest::ignoreMessage(QtWarningMsg, "Retired WinMM output cleanup failed before native close"); }
		client.reset(); for (const auto& output : t.outputs)
		{ QCOMPARE(output.messages, cleanupMessages(3)); QCOMPARE(output.messagesAtClose, std::size_t{4}); QCOMPARE(output.closes, 1); QVERIFY(!output.live); }
		QCOMPARE(t.inputs[0].closes, 1); QCOMPARE(t.badAccess, 0);
	}

	void reusedPortAddressDoesNotDiscardRetiredCleanup()
	{
		Transport t; auto client = t.create(); Processor processor; std::optional<MidiPort> port;
		port.emplace("old", client.get(), &processor, nullptr, MidiPort::Mode::Output); const auto previous = &*port;
		port->subscribeWritablePort("Output 0"); port->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); t.outputs[0].result = MIDIERR_NOTREADY;
		QTest::ignoreMessage(QtWarningMsg, "MIDI output cleanup failed while retiring a port"); port.reset();
		QCOMPARE(client->outputBackendStatus().cleanupPendingChannels, 1u); t.outputs[0].result = MMSYSERR_NOERROR; t.outputs[0].messages.clear();
		port.emplace("new", client.get(), &processor, nullptr, MidiPort::Mode::Output); QCOMPARE(&*port, previous); port->subscribeWritablePort("Output 0");
		port->processOutEvent(MidiEvent(MidiNoteOn, 0, 64, 100)); auto expected = cleanupMessages(0); expected.push_back(0x00644090);
		QCOMPARE(t.outputs[0].messages, expected); QCOMPARE(port->trackedOutputNotes(), 1u); QCOMPARE(client->outputBackendStatus().cleanupPendingChannels, 0u);
		port->releaseOutput(); QCOMPARE(port->trackedOutputNotes(), 0u); QCOMPARE(t.outputs[0].messages.back(), DWORD(0x00004080)); QCOMPARE(t.badAccess, 0);
	}

	void retiredCleanupAllowsLoopbackDeliveryOnOwner()
	{
		Transport t; auto client = t.create(); retireNote(t, client.get()); t.outputs[0].result = MMSYSERR_NOERROR;
		Processor processor; MidiPort input("loopback", client.get(), &processor, nullptr, MidiPort::Mode::Input); input.subscribeReadablePort("Input 0");
		t.loopback = true; WinMmMidiIoTestAccess::flush(*client); QVERIFY(processor.events.empty()); QCOMPARE(client->inputDeliveryStatus(&input).pendingEvents, 4u);
		pump(); QCOMPARE(processor.events.size(), std::size_t{4}); QVERIFY(!processor.wrongThread); QCOMPARE(client->inputBackendStatus().nativeErrorCode, 0);
		for (const auto& event : processor.events) { QCOMPARE(event.type(), MidiControlChange); QCOMPARE(event.sourcePortName(), QString("Input 0")); }
		QVERIFY(client->outputBackendStatus().active); QCOMPARE(client->outputBackendStatus().cleanupPendingChannels, 0u); QCOMPARE(t.badAccess, 0);
	}

	void clientFirstShutdownCleansPossibleNativeActivations_data()
	{
		QTest::addColumn<QString>("activation"); QTest::addColumn<QString>("failure");
		for (const auto activation : {"note", "sustain", "sostenuto", "mixed"})
		{
			for (const auto failure : {"none", "native", "exception"})
			{ QTest::newRow(qPrintable(QString("%1-%2").arg(activation, failure))) << QString(activation) << QString(failure); }
		}
	}
	void clientFirstShutdownCleansPossibleNativeActivations()
	{
		QFETCH(QString, activation); QFETCH(QString, failure); Transport t; auto client = t.create(); const auto previous = client.get(); Processor processor;
		MidiPort survivor("survivor", client.get(), &processor, nullptr, MidiPort::Mode::Output); survivor.setOutputChannel(0); survivor.subscribeWritablePort("Output 0");
		std::vector<MidiEvent> events;
		if (activation == "note" || activation == "mixed") { events.emplace_back(MidiNoteOn, 0, 60, 100); }
		if (activation == "sustain" || activation == "sostenuto")
		{ events.emplace_back(MidiControlChange, 0, activation == "sustain" ? MidiControllerSustain : MidiControllerSostenuto, 127); }
		if (activation == "mixed")
		{
			events.emplace_back(MidiNoteOn, 0, 60, 90); events.emplace_back(MidiNoteOn, 9, 64, 100);
			events.emplace_back(MidiControlChange, 15, MidiControllerSustain, 127); events.emplace_back(MidiControlChange, 15, MidiControllerSostenuto, 127);
		}
		t.outputs[0].result = failure == "native" ? MIDIERR_NOTREADY : MMSYSERR_NOERROR; t.outputs[0].throwSend = failure == "exception";
		for (const auto& event : events)
		{
			if (failure == "none") { survivor.processOutEvent(event); }
			else { QVERIFY_EXCEPTION_THROWN(survivor.processOutEvent(event), std::runtime_error); }
		}
		const auto before = settings(survivor); t.outputs[0].result = MMSYSERR_NOERROR; t.outputs[0].throwSend = false; t.outputs[0].messages.clear();
		client.reset(); auto expected = cleanupMessages(0);
		if (activation == "mixed") { for (const int channel : {9, 15}) { const auto messages = cleanupMessages(channel); expected.insert(expected.end(), messages.begin(), messages.end()); } }
		QCOMPARE(t.outputs[0].messages, expected); QCOMPARE(t.outputs[0].messagesAtClose, expected.size()); QCOMPARE(t.outputs[0].closes, 1);
		QVERIFY(t.outputs[1].messages.empty()); QCOMPARE(t.outputs[1].closes, 1); QCOMPARE(t.inputs[0].resets, 1); QCOMPARE(t.inputs[0].closes, 1);
		QVERIFY(survivor.client() != previous); QVERIFY(survivor.client()->isRaw()); QCOMPARE(survivor.trackedOutputNotes(), 0u); QCOMPARE(settings(survivor), before);
		survivor.releaseOutput(); QCOMPARE(t.outputs[0].messages, expected); QCOMPARE(t.badAccess, 0);
	}

	void nonActivatingMessagesDoNotExpandShutdownCleanup_data()
	{
		QTest::addColumn<int>("type"); QTest::addColumn<int>("first"); QTest::addColumn<int>("second");
		QTest::newRow("note-off") << int(MidiNoteOff) << 60 << 0;
		QTest::newRow("zero-note-on") << int(MidiNoteOn) << 60 << 0;
		QTest::newRow("program") << int(MidiProgramChange) << 37 << 0;
		QTest::newRow("pitch") << int(MidiPitchBend) << 8192 << 0;
		QTest::newRow("key-pressure") << int(MidiKeyPressure) << 60 << 100;
		QTest::newRow("channel-pressure") << int(MidiChannelPressure) << 100 << 0;
		QTest::newRow("sustain-off") << int(MidiControlChange) << int(MidiControllerSustain) << 0;
		QTest::newRow("sostenuto-off") << int(MidiControlChange) << int(MidiControllerSostenuto) << 0;
		QTest::newRow("other-controller") << int(MidiControlChange) << 74 << 127;
	}
	void nonActivatingMessagesDoNotExpandShutdownCleanup()
	{
		QFETCH(int, type); QFETCH(int, first); QFETCH(int, second); Transport t; auto client = t.create(); Processor processor;
		MidiPort survivor("control", client.get(), &processor, nullptr, MidiPort::Mode::Output); survivor.setOutputChannel(0); survivor.subscribeWritablePort("Output 0");
		survivor.processOutEvent(MidiEvent(static_cast<MidiEventTypes>(type), 15, first, second)); QCOMPARE(t.outputs[0].messages.size(), std::size_t{1});
		t.outputs[0].messages.clear(); client.reset(); QVERIFY(t.outputs[0].messages.empty()); QCOMPARE(t.outputs[0].messagesAtClose, std::size_t{0});
		QCOMPARE(t.outputs[0].closes, 1); QVERIFY(t.outputs[1].messages.empty()); QCOMPARE(t.badAccess, 0);
	}

	void shutdownUsesActualOutputChannelsAndDestinations()
	{
		Transport t; auto client = t.create(); Processor processor;
		MidiPort a("a", client.get(), &processor, nullptr, MidiPort::Mode::Output), b("b", client.get(), &processor, nullptr, MidiPort::Mode::Output);
		a.setOutputChannel(6); a.setFixedOutputNote(54); a.subscribeWritablePort("Output 0"); a.subscribeWritablePort("missing output");
		b.setOutputChannel(16); b.subscribeWritablePort("Output 1");
		a.processOutEvent(MidiEvent(MidiNoteOn, 2, 60, 100)); b.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100));
		QVERIFY(t.outputs[0].messages.empty()); QVERIFY(t.outputs[1].messages.empty());
		a.processOutEvent(MidiEvent(MidiNoteOn, 5, 60, 100)); b.processOutEvent(MidiEvent(MidiControlChange, 15, MidiControllerSustain, 127));
		QCOMPARE(t.outputs[0].messages, (std::vector<DWORD>{0x00643695})); QCOMPARE(t.outputs[1].messages.size(), std::size_t{1});
		for (auto& output : t.outputs) { output.messages.clear(); } client.reset();
		QCOMPARE(t.outputs[0].messages, cleanupMessages(5)); QCOMPARE(t.outputs[1].messages, cleanupMessages(15));
		QCOMPARE(a.trackedOutputNotes(), 0u); QCOMPARE(b.trackedOutputNotes(), 0u); QCOMPARE(t.badAccess, 0);
	}

	void closingIsObservableBeforeNativeCallsAndRevokesLoopback()
	{
		Transport t; auto client = t.create(); const auto source = client.get(); Processor processor;
		MidiPort survivor("duplex", source, &processor, nullptr, MidiPort::Mode::Duplex); survivor.subscribeReadablePort("Input 0"); survivor.subscribeWritablePort("Output 0");
		survivor.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); t.feed(); QCOMPARE(source->inputDeliveryStatus(&survivor).pendingEvents, 1u);
		int inspected = 0; bool unavailable = true, inputStopped = true, outputStopped = true, noQueuedInput = true;
		t.onOutputMessage = [&] {
			++inspected; unavailable = unavailable && !source->isAvailable(); inputStopped = inputStopped && !source->inputBackendStatus().active;
			outputStopped = outputStopped && !source->outputBackendStatus().active;
			noQueuedInput = noQueuedInput && source->inputDeliveryStatus(&survivor).pendingEvents == 0;
		};
		t.loopback = true; t.callbacksDuringRetirement = true; t.outputs[0].messages.clear(); client.reset(); t.onOutputMessage = {};
		pump(); QVERIFY(processor.events.empty()); QCOMPARE(inspected, 4); QVERIFY(unavailable && inputStopped && outputStopped && noQueuedInput);
		QCOMPARE(t.outputs[0].messages, cleanupMessages(0)); QCOMPARE(t.outputs[0].messagesAtClose, std::size_t{4}); QCOMPARE(t.badAccess, 0);
	}

	void shutdownContinuesAfterLiveActivationCleanupFailure_data()
	{
		QTest::addColumn<QString>("failure"); for (const auto failure : {"first", "both", "exception"}) { QTest::newRow(failure) << QString(failure); }
	}
	void shutdownContinuesAfterLiveActivationCleanupFailure()
	{
		QFETCH(QString, failure); Transport t; auto client = t.create(); Processor processor;
		MidiPort survivor("fanout", client.get(), &processor, nullptr, MidiPort::Mode::Output); survivor.subscribeWritablePort("Output 0"); survivor.subscribeWritablePort("Output 1");
		survivor.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); for (auto& output : t.outputs) { output.messages.clear(); }
		if (failure == "first" || failure == "both") { t.outputs[0].result = MIDIERR_NOTREADY; }
		if (failure == "both") { t.outputs[1].result = MIDIERR_NOTREADY; } if (failure == "exception") { t.outputs[0].throwSend = true; }
		QTest::ignoreMessage(QtWarningMsg, "Retired WinMM output cleanup failed before native close"); client.reset();
		for (const auto& output : t.outputs) { QCOMPARE(output.messages, cleanupMessages(0)); QCOMPARE(output.messagesAtClose, std::size_t{4}); QCOMPARE(output.closes, 1); }
		QCOMPARE(survivor.trackedOutputNotes(), 0u); QCOMPARE(t.inputs[0].closes, 1); QCOMPARE(t.badAccess, 0);
	}

	void completedChannelCleanupDoesNotReplayDuringShutdown_data()
	{
		QTest::addColumn<bool>("reactivated"); QTest::newRow("no-new-activation") << false; QTest::newRow("new-activation") << true;
	}
	void completedChannelCleanupDoesNotReplayDuringShutdown()
	{
		QFETCH(bool, reactivated); Transport t; auto client = t.create(); retireNote(t, client.get()); t.outputs[0].result = MMSYSERR_NOERROR; Processor processor;
		MidiPort survivor("new", client.get(), &processor, nullptr, MidiPort::Mode::Output); survivor.setOutputChannel(0); survivor.subscribeWritablePort("Output 0");
		survivor.processOutEvent(MidiEvent(MidiProgramChange, 0, 37)); QCOMPARE(client->outputBackendStatus().cleanupPendingChannels, 0u);
		if (reactivated) { survivor.processOutEvent(MidiEvent(MidiNoteOn, 7, 64, 100)); }
		t.outputs[0].messages.clear(); client.reset();
		QCOMPARE(t.outputs[0].messages, reactivated ? cleanupMessages(7) : std::vector<DWORD>{}); QCOMPARE(t.outputs[0].closes, 1); QCOMPARE(t.badAccess, 0);
	}

	void backendReplacementFlushesUsedChannelsBeforeCommit_data()
	{
		QTest::addColumn<bool>("alreadyReleased"); QTest::newRow("still-held") << false; QTest::newRow("already-released") << true;
	}
	void backendReplacementFlushesUsedChannelsBeforeCommit()
	{
		QFETCH(bool, alreadyReleased); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine(); Processor processor;
		StudioMidiBackendSelectionTestAccess::select([&] { return t.create(); }); const auto source = engine->midiClient();
		MidiPort port("live", source, &processor, nullptr, MidiPort::Mode::Output); port.setOutputChannel(0); port.subscribeWritablePort("Output 0");
		port.processOutEvent(MidiEvent(MidiNoteOn, 5, 60, 100)); if (alreadyReleased) { port.releaseOutput(); } const auto before = settings(port);
		t.outputs[0].messages.clear(); t.outputs[0].failMessage = 0x000078b5;
		const auto refused = StudioCapabilities::execute("midi.backend.select", {{"name", MidiDummy::name()}}); QVERIFY(!refused.value("ok").toBool());
		QCOMPARE(engine->midiClient(), source); QCOMPARE(port.client(), source); QCOMPARE(settings(port), before); QCOMPARE(port.trackedOutputNotes(), 0u);
		QCOMPARE(source->outputBackendStatus().cleanupPendingChannels, 1u); QCOMPARE(t.outputs[0].closes, 0); QVERIFY(!engine->hasStoredAudioDevice());
		auto expected = cleanupMessages(5); if (!alreadyReleased) { expected.insert(expected.begin(), 0x00003c85); } QCOMPARE(t.outputs[0].messages, expected);
		t.outputs[0].failMessage = 0; t.outputs[0].messages.clear(); const auto retried = StudioCapabilities::execute("midi.backend.select", {{"name", MidiDummy::name()}});
		QVERIFY(retried.value("ok").toBool()); QVERIFY(retried.value("result").toObject().value("committed").toBool()); QCOMPARE(port.client(), engine->midiClient());
		QCOMPARE(t.outputs[0].messages, cleanupMessages(5)); QCOMPARE(t.outputs[0].closes, 1); QCOMPARE(t.outputs[0].messagesAtClose, std::size_t{4});
		QCOMPARE(settings(port), before); QCOMPARE(t.badAccess, 0);
	}

	void closedClientRefusesOutputUntilNativeReopen()
	{
		Transport t; auto client = t.create(); Processor processor;
		MidiPort survivor("refresh", client.get(), &processor, nullptr, MidiPort::Mode::Duplex); survivor.subscribeReadablePort("Input 0"); survivor.subscribeWritablePort("Output 0");
		survivor.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); t.feed(); QCOMPARE(client->inputDeliveryStatus(&survivor).pendingEvents, 1u);
		const auto before = settings(survivor); const auto oldToken = t.inputs[0].token; t.outputs[0].messages.clear(); WinMmMidiIoTestAccess::close(*client);
		QVERIFY(!client->isAvailable()); QVERIFY(!client->inputBackendStatus().active); QVERIFY(!client->outputBackendStatus().active);
		QCOMPARE(client->inputDeliveryStatus(&survivor).pendingEvents, 0u); QVERIFY(client->inputDeliveryStatus(&survivor).failed);
		QVERIFY_EXCEPTION_THROWN(survivor.processOutEvent(MidiEvent(MidiProgramChange, 0, 37)), std::runtime_error);
		QCOMPARE(t.outputs[0].messages, cleanupMessages(0)); t.feed(); pump(); QVERIFY(processor.events.empty());
		WinMmMidiIoTestAccess::refresh(*client); QVERIFY(client->isAvailable()); QVERIFY(client->inputBackendStatus().active); QVERIFY(client->outputBackendStatus().active);
		QVERIFY(t.inputs[0].token != oldToken); QCOMPARE(t.outputs[0].opens, 2); QCOMPARE(t.outputs[0].closes, 1); QCOMPARE(settings(survivor), before);
		QString error; bool changed = false; QVERIFY2(survivor.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
		t.feed(); pump(); QCOMPARE(processor.events.size(), std::size_t{1}); QCOMPARE(processor.events.front().type(), MidiNoteOn);
		t.outputs[0].messages.clear(); survivor.processOutEvent(MidiEvent(MidiProgramChange, 0, 38)); QCOMPARE(t.outputs[0].messages, (std::vector<DWORD>{0x000026c0}));
		client.reset(); QCOMPARE(t.outputs[0].messagesAtClose, std::size_t{1}); QCOMPARE(t.outputs[0].closes, 2); QCOMPARE(t.badAccess, 0);
	}

	void failedNativeCloseRetainsOnlyUnresolvedHandles_data()
	{
		QTest::addColumn<QString>("direction"); QTest::addColumn<QString>("failure");
		for (const auto direction : {"input", "output", "both"})
		{
			for (const auto failure : {"still-playing", "no-memory", "exception"})
			{ QTest::newRow(qPrintable(QString("%1-%2").arg(direction, failure))) << QString(direction) << QString(failure); }
		}
	}
	void failedNativeCloseRetainsOnlyUnresolvedHandles()
	{
		QFETCH(QString, direction); QFETCH(QString, failure); Transport t; t.inputDevices = 2; auto client = t.create(); Processor processor;
		MidiPort survivor("native-close", client.get(), &processor, nullptr, MidiPort::Mode::Input); survivor.subscribeReadablePort("Input 0");
		const bool input = direction != "output", output = direction != "input";
		const auto code = failure == "still-playing" ? MIDIERR_STILLPLAYING : failure == "no-memory" ? MMSYSERR_NOMEM : MMSYSERR_ERROR;
		for (auto* handle : {input ? &t.inputs[0] : nullptr, output ? &t.outputs[0] : nullptr})
		{ if (handle) { handle->closeResult = code; handle->throwClose = failure == "exception"; } }
		t.callbacksDuringRetirement = true; const auto before = settings(survivor); client.reset(); pump();
		auto state = MidiWinMM::retiredHandleStatus(); QVERIFY(state.observationAvailable && !state.retrying);
		QCOMPARE(state.pendingInputHandles, input ? 1u : 0u); QCOMPARE(state.pendingOutputHandles, output ? 1u : 0u);
		QCOMPARE(state.nativeErrorCode, int(code)); QCOMPARE(state.lastNativeErrorCode, int(code));
		QCOMPARE(t.inputs[0].live, input); QCOMPARE(t.outputs[0].live, output); QVERIFY(!t.inputs[1].live && !t.outputs[1].live);
		QVERIFY(processor.events.empty()); QCOMPARE(settings(survivor), before); t.feed(); pump(); QVERIFY(processor.events.empty());
		for (auto* handle : {&t.inputs[0], &t.outputs[0]}) { handle->closeResult = MMSYSERR_NOERROR; handle->throwClose = false; }
		t.inputDevices = t.outputDevices = 0; auto retry = t.create();
		state = MidiWinMM::retiredHandleStatus(); QCOMPARE(state.pendingInputHandles, 0u); QCOMPARE(state.pendingOutputHandles, 0u);
		QCOMPARE(state.nativeErrorCode, 0); QCOMPARE(state.lastNativeErrorCode, int(code));
		QCOMPARE(t.inputs[0].closes, input ? 2 : 1); QCOMPARE(t.outputs[0].closes, output ? 2 : 1);
		QCOMPARE(t.inputs[1].closes, 1); QCOMPARE(t.outputs[1].closes, 1);
		QCOMPARE(t.outputs[0].resets, output && failure == "still-playing" ? 1 : 0); QCOMPARE(t.badAccess, 0);
	}

	void resetFailuresStillAttemptCloseAndKeepOtherHandlesIndependent_data()
	{
		QTest::addColumn<bool>("input"); QTest::addColumn<bool>("exception");
		QTest::newRow("input-error") << true << false; QTest::newRow("input-exception") << true << true;
		QTest::newRow("output-error") << false << false; QTest::newRow("output-exception") << false << true;
	}
	void resetFailuresStillAttemptCloseAndKeepOtherHandlesIndependent()
	{
		QFETCH(bool, input); QFETCH(bool, exception); Transport t; auto client = t.create();
		auto& target = input ? t.inputs[0] : t.outputs[0];
		if (!input) { target.closeResult = MIDIERR_STILLPLAYING; client.reset(); QCOMPARE(MidiWinMM::retiredHandleStatus().pendingOutputHandles, 1u); }
		target.resetResult = MMSYSERR_INVALHANDLE; target.throwReset = exception; target.closeResult = MMSYSERR_NOERROR;
		if (input) { client.reset(); }
		else { t.inputDevices = t.outputDevices = 0; client = t.create(); }
		const auto state = MidiWinMM::retiredHandleStatus(); QCOMPARE(state.pendingInputHandles, 0u); QCOMPARE(state.pendingOutputHandles, 0u);
		QCOMPARE(state.lastNativeErrorCode, exception ? int(MMSYSERR_ERROR) : int(MMSYSERR_INVALHANDLE));
		QCOMPARE(target.resets, 1); QCOMPARE(target.closes, input ? 1 : 2); QVERIFY(!target.live);
		QCOMPARE(t.outputs[1].closes, 1); QCOMPARE(t.badAccess, 0);
	}

	void retainedChannelCleanupSurvivesCloseFailureAndKeepsPartialProgress()
	{
		Transport t; auto client = t.create(); Processor processor;
		MidiPort port("retained-native", client.get(), &processor, nullptr, MidiPort::Mode::Output);
		port.setOutputChannel(0); port.subscribeWritablePort("Output 0");
		for (const int channel : {3, 7}) { port.processOutEvent(MidiEvent(MidiNoteOn, channel, 60, 100)); }
		t.outputs[0].result = MIDIERR_NOTREADY; t.outputs[0].closeResult = MMSYSERR_NOMEM; t.outputs[0].messages.clear();
		QTest::ignoreMessage(QtWarningMsg, "Retired WinMM output cleanup failed before native close"); client.reset();
		QCOMPARE(MidiWinMM::retiredHandleStatus().cleanupPendingChannels, 2u); QCOMPARE(t.outputs[0].messages.size(), std::size_t{8});
		t.outputs[0].result = MMSYSERR_NOERROR; t.outputs[0].failMessage = cleanupMessages(7).at(2); t.outputs[0].messages.clear();
		t.inputDevices = t.outputDevices = 0;
		{ auto retry = t.create(); QVERIFY(!retry->isAvailable()); }
		QCOMPARE(MidiWinMM::retiredHandleStatus().cleanupPendingChannels, 1u); QCOMPARE(t.outputs[0].messages.size(), std::size_t{8});
		QCOMPARE(t.outputs[0].closes, 2); t.outputs[0].messages.clear(); t.outputs[0].failMessage = 0; t.outputs[0].closeResult = MMSYSERR_NOERROR;
		{ auto retry = t.create(); }
		QCOMPARE(t.outputs[0].messages, cleanupMessages(7)); QCOMPARE(t.outputs[0].closes, 3); QCOMPARE(t.outputs[1].closes, 1);
		QCOMPARE(MidiWinMM::retiredHandleStatus().pendingOutputHandles, 0u); QCOMPARE(t.badAccess, 0);
	}

	void retirementCopiesTheApiAndCannotAliasAReusedClientAddress_data()
	{
		QTest::addColumn<bool>("input"); QTest::newRow("input") << true; QTest::newRow("output") << false;
	}
	void retirementCopiesTheApiAndCannotAliasAReusedClientAddress()
	{
		QFETCH(bool, input); Transport t; alignas(MidiWinMM) unsigned char storage[sizeof(MidiWinMM)]; MidiWinMM* slot = nullptr;
		const auto release = qScopeGuard([&] { if (slot) { slot->~MidiWinMM(); } });
		auto& target = input ? t.inputs[0] : t.outputs[0];
		{
			const auto calls = t.api; slot = WinMmMidiIoTestAccess::construct(storage, calls); target.closeResult = MMSYSERR_NOMEM;
			slot->~MidiWinMM(); slot = nullptr;
		}
		QVERIFY(target.live); QCOMPARE(target.closes, 1);
		slot = WinMmMidiIoTestAccess::construct(storage, t.api); QVERIFY(!slot->isAvailable()); QCOMPARE(target.closes, 2);
		QCOMPARE(target.opens, 1); slot->~MidiWinMM(); slot = nullptr;
		QCOMPARE(target.closes, 2); // Reused QObject storage is a different native owner generation.
		target.closeResult = MMSYSERR_NOERROR; t.inputDevices = t.outputDevices = 0; auto retry = t.create();
		QCOMPARE(target.closes, 3); QVERIFY(!target.live); QCOMPARE(t.badAccess, 0);
	}

	void confirmedInvalidHandlesAreNotRetried_data()
	{
		QTest::addColumn<bool>("input"); QTest::newRow("input") << true; QTest::newRow("output") << false;
	}
	void confirmedInvalidHandlesAreNotRetried()
	{
		QFETCH(bool, input); Transport t; auto client = t.create(); auto& target = input ? t.inputs[0] : t.outputs[0];
		target.closeResult = MMSYSERR_INVALHANDLE; client.reset(); QVERIFY(!target.live);
		const auto state = MidiWinMM::retiredHandleStatus(); QCOMPARE(state.pendingInputHandles, 0u); QCOMPARE(state.pendingOutputHandles, 0u);
		QCOMPARE(state.lastNativeErrorCode, int(MMSYSERR_INVALHANDLE)); t.inputDevices = t.outputDevices = 0;
		{ auto retry = t.create(); } QCOMPARE(target.closes, 1); QCOMPARE(t.badAccess, 0);
	}

	void partialConstructionRetainsNativeOwnershipBeforePublication_data()
	{
		QTest::addColumn<QString>("failure");
		for (const auto failure : {"input-start", "input-open-throws", "output-open-throws"}) { QTest::newRow(failure) << QString(failure); }
	}
	void partialConstructionRetainsNativeOwnershipBeforePublication()
	{
		QFETCH(QString, failure); Transport t; const bool input = failure.startsWith("input-");
		auto& target = input ? t.inputs[0] : t.outputs[0]; target.closeResult = MMSYSERR_NOMEM; t.startupFault = failure;
		auto client = t.create(); QVERIFY(!client->isAvailable()); QVERIFY(target.live); QCOMPARE(target.opens, 1);
		auto state = MidiWinMM::retiredHandleStatus(); QCOMPARE(state.pendingInputHandles, input ? 1u : 0u); QCOMPARE(state.pendingOutputHandles, input ? 0u : 1u);
		client.reset(); QVERIFY(target.live); t.startupFault.clear(); target.closeResult = MMSYSERR_NOERROR;
		client = t.create(); QVERIFY(client->isAvailable()); QCOMPARE(target.opens, 2);
		state = MidiWinMM::retiredHandleStatus(); QCOMPARE(state.pendingInputHandles, 0u); QCOMPARE(state.pendingOutputHandles, 0u); QCOMPARE(t.badAccess, 0);
	}

	void nativeRetirementPermitsInspectionAndRefusesReentrantReopen()
	{
		Transport t; auto client = t.create(); Processor processor;
		MidiPort survivor("retirement-loopback", client.get(), &processor, nullptr, MidiPort::Mode::Input); survivor.subscribeReadablePort("Input 0");
		t.callbacksDuringRetirement = true; bool observed = false, safe = true; int calls = 0;
		t.onNativeClose = [&] {
			++calls; const auto state = MidiClient::nativeRetirementStatus(); observed = observed || state.retrying;
			const auto inspected = StudioCapabilities::execute("transport.inspect", {}); safe = safe && inspected.value("ok").toBool();
			auto nested = t.create(); safe = safe && !nested->isAvailable();
		};
		client.reset(); pump(); QVERIFY(observed && safe); QCOMPARE(calls, 3); QVERIFY(processor.events.empty());
		for (const auto& output : t.outputs) { QCOMPARE(output.opens, 1); QCOMPARE(output.closes, 1); }
		QCOMPARE(t.inputs[0].opens, 1); QCOMPARE(t.inputs[0].closes, 1); QCOMPARE(t.badAccess, 0);
	}

	void backendSwitchReportsRetiredHandlesAndFailedRetryKeepsTheSelection_data()
	{
		QTest::addColumn<bool>("input"); QTest::newRow("input") << true; QTest::newRow("output") << false;
	}
	void backendSwitchReportsRetiredHandlesAndFailedRetryKeepsTheSelection()
	{
		QFETCH(bool, input); Transport t; const auto destroy = qScopeGuard([] { Engine::destroy(); });
		const auto engine = Engine::audioEngine(); Processor processor; MidiPort port("engine-retirement", engine->midiClient(), &processor);
		port.setReadable(true); port.setInputChannel(3); port.subscribeReadablePort("Input 0"); const auto before = settings(port);
		StudioMidiBackendSelectionTestAccess::select([&] { return t.create(); }); auto& target = input ? t.inputs[0] : t.outputs[0]; target.closeResult = MMSYSERR_NOMEM;
		const auto switched = StudioCapabilities::execute("midi.backend.select", {{"name", MidiDummy::name()}}); QVERIFY(switched.value("ok").toBool());
		const auto selected = engine->midiClient(); QVERIFY(dynamic_cast<MidiDummy*>(selected)); QCOMPARE(settings(port), before);
		const auto inspect = [] { return StudioCapabilities::execute("transport.inspect", {}).value("result").toObject().value("midiBackend").toObject().value("nativeRetirement").toObject(); };
		const auto pending = inspect(); QVERIFY(pending.value("observationAvailable").toBool());
		QCOMPARE(pending.value("pendingInputHandles").toInt(), input ? 1 : 0); QCOMPARE(pending.value("pendingOutputHandles").toInt(), input ? 0 : 1);
		QCOMPARE(pending.value("retryOnBackendSelection").toString(), MidiWinMM::name()); const auto closes = target.closes;
		QCOMPARE(inspect(), pending); QCOMPARE(target.closes, closes); // Inspection never calls native close.
		QVERIFY_EXCEPTION_THROWN(StudioMidiBackendSelectionTestAccess::select([&] { return t.create(); }), std::runtime_error);
		QCOMPARE(engine->midiClient(), selected); QCOMPARE(port.client(), selected); QCOMPARE(settings(port), before); QCOMPARE(target.opens, 1);
		target.closeResult = MMSYSERR_NOERROR; StudioMidiBackendSelectionTestAccess::select([&] { return t.create(); });
		QCOMPARE(target.opens, 2); QVERIFY(dynamic_cast<MidiWinMM*>(engine->midiClient())); QCOMPARE(port.client(), engine->midiClient()); QCOMPARE(settings(port), before);
		const auto recovered = inspect(); QCOMPARE(recovered.value("pendingInputHandles").toInt(), 0); QCOMPARE(recovered.value("pendingOutputHandles").toInt(), 0);
		QCOMPARE(recovered.value("lastNativeErrorCode").toInt(), int(MMSYSERR_NOMEM)); QCOMPARE(t.badAccess, 0);
	}
};
QTEST_GUILESS_MAIN(WinMmMidiIoTest)
#include "WinMmMidiIoTest.moc"
