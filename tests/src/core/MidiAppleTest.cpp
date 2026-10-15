/* Native CoreMIDI framing, routing and retirement tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QEvent>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include "ConfigManager.h"
#include "Engine.h"
#include "MidiApple.h"
#include "MidiEventProcessor.h"
#include "NotePlayHandle.h"
#include "Song.h"

using namespace lmms;

namespace lmms
{
class MidiAppleTestAccess
{
public:
	std::unique_ptr<MidiApple> client{new MidiApple(false)};
	std::uintptr_t addSource(MIDIEndpointRef endpoint, const QString& name, MIDIDeviceRef device = 0)
	{
		const auto token = client->registerSource(endpoint, name, 0, device);
		const auto lock = std::lock_guard{client->m_devicesMutex}; client->m_inputDevices.insert(name, endpoint); return token;
	}
	void removeSource(MIDIEndpointRef endpoint, const QString& name)
	{
		{
			const auto lock = std::lock_guard{client->m_devicesMutex};
			const auto source = client->m_sources.value(endpoint);
			if (source) { client->retireSourceLocked(source); client->m_sources.remove(endpoint); }
			client->m_inputDevices.remove(name);
		}
		emit client->readablePortsChanged();
	}
	void deliver(const MIDIPacketList* packets, std::uintptr_t source)
	{
		lateCallback(packets, token(), source);
	}
	std::uintptr_t token() const { return client->m_callbackToken; }
	static void lateCallback(const MIDIPacketList* packets, std::uintptr_t client, std::uintptr_t source)
	{
		MidiApple::ReadCallback(packets, reinterpret_cast<void*>(client), reinterpret_cast<void*>(source));
	}
	static void notify(const MIDINotification* notification) { MidiApple::NotifyCallback(notification, nullptr); }
	static bool encode(const MidiEvent& event, MIDIPacketList& packets) { return MidiApple::createMidiPacketList(event, packets); }
	static QMap<QString, MIDIEndpointRef> names()
	{
		return MidiApple::endpointNames({{1, "Device:Port", 10}, {2, "Device:Port", 20}, {3, "Device:Port [id:10]", 30},
			{4, QString::fromUtf8("Virtual, \xce\xb1"), 40}, {5, "Other", 50}});
	}
	int sources() const { const auto lock = std::lock_guard{client->m_devicesMutex}; return client->m_sourceTokens.size(); }
	bool pendingRefresh() const { const auto lock = std::lock_guard{client->m_devicesMutex}; return client->m_updatePending; }
};
}

class AppleMidiProcessor final : public MidiEventProcessor
{
public:
	std::vector<MidiEvent> events;
	std::vector<QThread*> threads;
	void processInEvent(const MidiEvent& event, const TimePos&, f_cnt_t) override { events.push_back(event); threads.push_back(QThread::currentThread()); }
	void processOutEvent(const MidiEvent&, const TimePos&, f_cnt_t) override { }
};

class MidiAppleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	static void deliver(MidiAppleTestAccess& backend, std::uintptr_t source, const QByteArray& bytes)
	{
		MIDIPacketList packets{}; const auto packet = MIDIPacketListInit(&packets);
		if (!MIDIPacketListAdd(&packets, sizeof(packets), packet, 0, bytes.size(), reinterpret_cast<const UInt8*>(bytes.constData()))) { qFatal("Fixture packet allocation failed"); }
		backend.deliver(&packets, source);
	}

private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->clearProject(); }

	void outputEncodingUsesNativeChannelVoiceLengthsAndPitchBend_data()
	{
		QTest::addColumn<int>("type"); QTest::addColumn<int>("first"); QTest::addColumn<int>("second"); QTest::addColumn<QByteArray>("expected");
		QTest::newRow("note-on") << int(MidiNoteOn) << 60 << 100 << QByteArray::fromHex("923c64");
		QTest::newRow("note-off") << int(MidiNoteOff) << 60 << 0 << QByteArray::fromHex("823c00");
		QTest::newRow("key-pressure") << int(MidiKeyPressure) << 60 << 80 << QByteArray::fromHex("a23c50");
		QTest::newRow("controller") << int(MidiControlChange) << 74 << 99 << QByteArray::fromHex("b24a63");
		QTest::newRow("program") << int(MidiProgramChange) << 127 << 0 << QByteArray::fromHex("c27f");
		QTest::newRow("pressure") << int(MidiChannelPressure) << 80 << 0 << QByteArray::fromHex("d250");
		QTest::newRow("bend") << int(MidiPitchBend) << 16383 << 0 << QByteArray::fromHex("e27f7f");
		QTest::newRow("invalid") << int(MidiNoteOn) << 60 << 128 << QByteArray{};
	}
	void outputEncodingUsesNativeChannelVoiceLengthsAndPitchBend()
	{
		QFETCH(int, type); QFETCH(int, first); QFETCH(int, second); QFETCH(QByteArray, expected); MIDIPacketList packets{};
		const auto encoded = MidiAppleTestAccess::encode(MidiEvent(static_cast<MidiEventTypes>(type), 2, first, second), packets);
		if (expected.isEmpty()) { QVERIFY(!encoded); return; }
		QVERIFY(encoded); QCOMPARE(packets.numPackets, UInt32(1)); QCOMPARE(int(packets.packet[0].length), expected.size());
		QCOMPARE(QByteArray(reinterpret_cast<const char*>(packets.packet[0].data), packets.packet[0].length), expected);
	}

	void emptyShortAndSplitPacketsUseOwnedSourceIdentity()
	{
		MidiAppleTestAccess backend; AppleMidiProcessor processor; const auto name = QString::fromUtf8("Keyboard, \xce\xb1");
		const auto source = backend.addSource(101, name); MidiPort port("Apple", backend.client.get(), &processor); port.subscribeReadablePort(name);
		std::thread reader([&] {
			MIDIPacketList empty{}; empty.numPackets = 1; empty.packet[0].length = 0; backend.deliver(&empty, source);
			deliver(backend, source, QByteArray::fromHex("c27f")); deliver(backend, source, QByteArray::fromHex("d250"));
			deliver(backend, source, QByteArray::fromHex("923c")); deliver(backend, source, QByteArray::fromHex("64f83d65"));
		}); reader.join(); QVERIFY(processor.events.empty()); QTRY_COMPARE(processor.events.size(), std::size_t(4));
		QCOMPARE(processor.events.at(0).type(), MidiProgramChange); QCOMPARE(processor.events.at(0).param(1), int16_t(0));
		QCOMPARE(processor.events.at(1).channelPressure(), uint8_t(80)); QCOMPARE(processor.events.at(2).key(), 60); QCOMPARE(processor.events.at(3).key(), 61);
		for (std::size_t index = 0; index < processor.events.size(); ++index)
		{
			QVERIFY(processor.events[index].sourcePort() == nullptr); QCOMPARE(backend.client->sourcePortName(processor.events[index]), name);
			QCOMPARE(processor.threads[index], QThread::currentThread());
		}
	}

	void variableLengthPacketListAndSysexDoNotReadPastMessageBytes()
	{
		MidiAppleTestAccess backend; AppleMidiProcessor processor; const auto source = backend.addSource(101, "Input");
		MidiPort port("Apple", backend.client.get(), &processor); port.subscribeReadablePort("Input");
		alignas(MIDIPacketList) std::array<UInt8, 4096> storage{}; auto packets = reinterpret_cast<MIDIPacketList*>(storage.data());
		auto next = MIDIPacketListInit(packets); const QByteArray program = QByteArray::fromHex("c27f");
		next = MIDIPacketListAdd(packets, storage.size(), next, 1, program.size(), reinterpret_cast<const UInt8*>(program.constData())); QVERIFY(next);
		QByteArray sysex(1200, char(1)); sysex[0] = char(0xf0); sysex[1199] = char(0xf7);
		next = MIDIPacketListAdd(packets, storage.size(), next, 2, sysex.size(), reinterpret_cast<const UInt8*>(sysex.constData())); QVERIFY(next);
		const QByteArray bend = QByteArray::fromHex("e27f7f");
		next = MIDIPacketListAdd(packets, storage.size(), next, 3, bend.size(), reinterpret_cast<const UInt8*>(bend.constData())); QVERIFY(next);
		backend.deliver(packets, source); QTRY_COMPARE(processor.events.size(), std::size_t(2));
		QCOMPARE(processor.events.at(0).type(), MidiProgramChange); QCOMPARE(processor.events.at(1).pitchBend(), int16_t(16383));
	}

	void parserStateIsIndependentAcrossNativeSources()
	{
		MidiAppleTestAccess backend; AppleMidiProcessor processor; const auto first = backend.addSource(101, "A"), second = backend.addSource(102, "B");
		MidiPort port("Apple", backend.client.get(), &processor); port.subscribeReadablePort("A"); port.subscribeReadablePort("B");
		deliver(backend, first, QByteArray::fromHex("903c")); deliver(backend, second, QByteArray::fromHex("913d65"));
		deliver(backend, first, QByteArray::fromHex("64")); QTRY_COMPARE(processor.events.size(), std::size_t(2));
		QCOMPARE(processor.events.at(0).channel(), int8_t(1)); QCOMPARE(backend.client->sourcePortName(processor.events.at(0)), QString("B"));
		QCOMPARE(processor.events.at(1).channel(), int8_t(0)); QCOMPARE(backend.client->sourcePortName(processor.events.at(1)), QString("A"));
	}

	void endpointNamesKeepLegacyUnicodeAndAvoidDuplicateCollisions()
	{
		const auto names = MidiAppleTestAccess::names(); QCOMPARE(names.size(), 5);
		QCOMPARE(names.value("Device:Port [id:10]"), MIDIEndpointRef(3)); QCOMPARE(names.value("Device:Port [id:10] [ref:1]"), MIDIEndpointRef(1));
		QCOMPARE(names.value("Device:Port [id:20]"), MIDIEndpointRef(2)); QCOMPARE(names.value("Other"), MIDIEndpointRef(5));
		QCOMPARE(names.value(QString::fromUtf8("Virtual, \xce\xb1")), MIDIEndpointRef(4));
	}

	void retiredSourceAndClientTokensNeverReactivateCallbacks()
	{
		MidiAppleTestAccess backend; AppleMidiProcessor processor; const auto source = backend.addSource(101, "Input");
		auto port = std::make_unique<MidiPort>("Apple", backend.client.get(), &processor); port->subscribeReadablePort("Input");
		deliver(backend, source, QByteArray::fromHex("903c64")); port.reset(); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.events.empty());
		backend.removeSource(101, "Input"); const auto replacement = backend.addSource(101, "Input"); QVERIFY(replacement != source); QCOMPARE(backend.sources(), 1);
		MidiPort survivor("survivor", backend.client.get(), &processor); survivor.subscribeReadablePort("Input");
		deliver(backend, source, QByteArray::fromHex("903c64")); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.events.empty());
		const auto client = backend.token(); backend.client.reset(); MIDIPacketList packets{};
		MidiAppleTestAccess::lateCallback(&packets, client, replacement); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.events.empty());
		QVERIFY(survivor.client()->isRaw());
	}

	void callbackTrafficCanRacePortAndSourceRetirement()
	{
		MidiAppleTestAccess backend; AppleMidiProcessor processor; const auto source = backend.addSource(101, "Input"); std::atomic<bool> started{false};
		std::thread reader([&] {
			started.store(true, std::memory_order_release);
			for (int index = 0; index < 4000; ++index) { deliver(backend, source, QByteArray::fromHex("903c64")); }
		});
		while (!started.load(std::memory_order_acquire)) { std::this_thread::yield(); }
		for (int index = 0; index < 200; ++index)
		{
			auto port = std::make_unique<MidiPort>("Apple", backend.client.get(), &processor); port->subscribeReadablePort("Input"); port.reset();
		}
		backend.removeSource(101, "Input"); reader.join(); QCOMPARE(backend.sources(), 0);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.events.empty());
	}

	void ioErrorsAreObservableAndNotificationRefreshIsCoalesced()
	{
		MidiAppleTestAccess backend; AppleMidiProcessor processor; backend.addSource(101, "Input");
		MidiPort port("Apple", backend.client.get(), &processor); port.subscribeReadablePort("Input");
		MIDINotification changed{}; changed.messageID = kMIDIMsgSetupChanged; changed.messageSize = sizeof(changed);
		for (int index = 0; index < 100; ++index) { MidiAppleTestAccess::notify(&changed); }
		QVERIFY(backend.pendingRefresh());
		MIDIIOErrorNotification error{}; error.messageID = kMIDIMsgIOError; error.messageSize = sizeof(error); error.errorCode = 1234;
		MidiAppleTestAccess::notify(reinterpret_cast<const MIDINotification*>(&error)); error.errorCode = 4321;
		MidiAppleTestAccess::notify(reinterpret_cast<const MIDINotification*>(&error));
		QTRY_COMPARE(processor.events.size(), std::size_t(MidiChannelCount)); QVERIFY(!backend.pendingRefresh());
		QVERIFY(backend.client->inputDeliveryStatus(&port).failure == MidiClient::InputDeliveryFailure::BackendInput);
		QCOMPARE(backend.client->inputBackendStatus().nativeErrorCode, 1234);
	}

	void sourceRemovalFaultsItsSubscribersWhileOtherSourcesContinue()
	{
		MidiAppleTestAccess backend; AppleMidiProcessor firstProcessor, secondProcessor;
		const auto first = backend.addSource(101, "A"), second = backend.addSource(102, "B");
		MidiPort firstPort("A", backend.client.get(), &firstProcessor), secondPort("B", backend.client.get(), &secondProcessor);
		firstPort.subscribeReadablePort("A"); secondPort.subscribeReadablePort("B");
		deliver(backend, first, QByteArray::fromHex("903c64")); backend.removeSource(101, "A");
		deliver(backend, second, QByteArray::fromHex("904264"));
		QTRY_COMPARE(firstProcessor.events.size(), std::size_t(MidiChannelCount)); QTRY_COMPARE(secondProcessor.events.size(), std::size_t(1));
		for (const auto& event : firstProcessor.events) { QCOMPARE(event.controllerNumber(), uint8_t(MidiControllerAllSoundOff)); }
		QVERIFY(backend.client->inputDeliveryStatus(&firstPort).failed); QVERIFY(!backend.client->inputDeliveryStatus(&secondPort).failed);
		QCOMPARE(secondProcessor.events.front().key(), 66); QCOMPARE(firstPort.requestedReadablePorts(), QStringList{"A"});
	}

	void callbackTrafficCanRaceClientDestruction()
	{
		MidiAppleTestAccess backend; AppleMidiProcessor processor; const auto source = backend.addSource(101, "Input"), client = backend.token();
		MidiPort survivor("survivor", backend.client.get(), &processor); survivor.subscribeReadablePort("Input"); std::atomic<bool> started{false};
		std::thread reader([&] {
			MIDIPacketList packets{}; MidiAppleTestAccess::encode(MidiEvent(MidiNoteOn, 0, 60, 100), packets);
			started.store(true, std::memory_order_release);
			for (int index = 0; index < 4000; ++index) { MidiAppleTestAccess::lateCallback(&packets, client, source); }
		});
		while (!started.load(std::memory_order_acquire)) { std::this_thread::yield(); }
		backend.client.reset(); reader.join(); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QVERIFY(processor.events.empty()); QVERIFY(survivor.client()->isRaw());
	}

	void deviceIoErrorsAffectOnlyThatDevicesSubscribedInputs()
	{
		MidiAppleTestAccess backend; AppleMidiProcessor firstProcessor, secondProcessor;
		backend.addSource(101, "A", 201); const auto second = backend.addSource(102, "B", 202);
		MidiPort first("A", backend.client.get(), &firstProcessor), unaffected("B", backend.client.get(), &secondProcessor);
		first.subscribeReadablePort("A"); unaffected.subscribeReadablePort("B");
		MIDIIOErrorNotification error{}; error.messageID = kMIDIMsgIOError; error.messageSize = sizeof(error); error.driverDevice = 203; error.errorCode = 1234;
		MidiAppleTestAccess::notify(reinterpret_cast<const MIDINotification*>(&error)); QCOMPARE(backend.client->inputBackendStatus().nativeErrorCode, 0);
		error.driverDevice = 201; MidiAppleTestAccess::notify(reinterpret_cast<const MIDINotification*>(&error));
		deliver(backend, second, QByteArray::fromHex("904264")); QTRY_COMPARE(secondProcessor.events.size(), std::size_t(1));
		QTRY_COMPARE(firstProcessor.events.size(), std::size_t(MidiChannelCount));
		QVERIFY(backend.client->inputDeliveryStatus(&first).failed); QVERIFY(!backend.client->inputDeliveryStatus(&unaffected).failed);
		QCOMPARE(backend.client->inputBackendStatus().nativeErrorCode, 1234);
	}

	void nativeVirtualEndpointRoundTripAndBackendReconstruction()
	{
		auto backend = std::make_unique<MidiApple>();
		if (!backend->isAvailable()) { QSKIP("Requires an available macOS CoreMIDI service; no physical MIDI device is needed."); }
		QVERIFY(backend->inputBackendStatus().active);
		MIDIClientRef peer = 0; QVERIFY(MIDIClientCreate(CFSTR("LMMS Studio Test Peer"), nullptr, nullptr, &peer) == noErr);
		const auto cleanupPeer = qScopeGuard([&] { MIDIClientDispose(peer); }); MIDIEndpointRef source = 0;
		QVERIFY(MIDISourceCreate(peer, CFSTR("LMMS Studio Virtual, \u03b1"), &source) == noErr);
		const auto cleanupSource = qScopeGuard([&] { MIDIEndpointDispose(source); });
		const auto sourceName = QString::fromUtf8("LMMS Studio Virtual, \xce\xb1"); QTRY_VERIFY2(backend->readablePorts().contains(sourceName), qPrintable(backend->readablePorts().join(" | ")));
		AppleMidiProcessor processor; auto port = std::make_unique<MidiPort>("virtual", backend.get(), &processor); port->subscribeReadablePort(sourceName);
		MIDIPacketList packets{}; QVERIFY(MidiAppleTestAccess::encode(MidiEvent(MidiNoteOn, 2, 60, 100), packets));
		QVERIFY(MIDIReceived(source, &packets) == noErr); QTRY_COMPARE(processor.events.size(), std::size_t(1));
		QCOMPARE(processor.events.front().key(), 60); QCOMPARE(backend->sourcePortName(processor.events.front()), sourceName);
		QCOMPARE(processor.threads.front(), QThread::currentThread()); backend.reset(); QVERIFY(port->client()->isRaw());
		QTRY_COMPARE(processor.events.size(), std::size_t(1 + MidiChannelCount));
		for (int channel = 0; channel < MidiChannelCount; ++channel)
		{
			const auto& event = processor.events[channel + 1]; QCOMPARE(event.type(), MidiControlChange);
			QCOMPARE(event.controllerNumber(), uint8_t(MidiControllerAllSoundOff)); QCOMPARE(event.controllerValue(), uint8_t(0));
			QCOMPARE(event.channel(), channel); QCOMPARE(processor.threads[channel + 1], QThread::currentThread());
		}
		port.reset();
		backend = std::make_unique<MidiApple>(); QVERIFY(backend->isAvailable()); QTRY_VERIFY2(backend->readablePorts().contains(sourceName), qPrintable(backend->readablePorts().join(" | ")));
		QVERIFY(backend->inputBackendStatus().active);
		port = std::make_unique<MidiPort>("reconstructed", backend.get(), &processor); port->subscribeReadablePort(sourceName);
		QVERIFY(MIDIReceived(source, &packets) == noErr); QTRY_COMPARE(processor.events.size(), std::size_t(2 + MidiChannelCount));
		QCOMPARE(processor.events.back().type(), MidiNoteOn); QCOMPARE(processor.events.back().key(), 60);
		QCOMPARE(backend->sourcePortName(processor.events.back()), sourceName); QCOMPARE(processor.threads.back(), QThread::currentThread());
	}
};

int main(int argc, char** argv)
{
	// Match the production macOS QCoreApplication startup contract.
	const auto previous = qgetenv("QT_EVENT_DISPATCHER_CORE_FOUNDATION");
	const bool present = qEnvironmentVariableIsSet("QT_EVENT_DISPATCHER_CORE_FOUNDATION");
	qputenv("QT_EVENT_DISPATCHER_CORE_FOUNDATION", "1"); QCoreApplication application(argc, argv);
	if (present) { qputenv("QT_EVENT_DISPATCHER_CORE_FOUNDATION", previous); } else { qunsetenv("QT_EVENT_DISPATCHER_CORE_FOUNDATION"); }
	MidiAppleTest test; return QTest::qExec(&test, argc, argv);
}
#include "MidiAppleTest.moc"
