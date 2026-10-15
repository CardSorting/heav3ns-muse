/* Native ALSA routing and lifetime tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QEvent>
#include <QThread>
#include <QTemporaryDir>
#include <QFile>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "ConfigManager.h"
#include "AudioEngine.h"
#include "Engine.h"
#include "MidiAlsaSeq.h"
#include "MidiEventProcessor.h"
#include "MidiPort.h"
#include "MeterModel.h"
#include "NotePlayHandle.h"
#include "Song.h"

using namespace lmms;

namespace lmms
{
class MidiAlsaSeqTestAccess
{
public:
	std::unique_ptr<MidiAlsaSeq> client{new MidiAlsaSeq(false)};
	MidiAlsaSeqTestAccess() { client->m_clientID = 42; }
	void addTarget(MidiPort* port, int id = 7)
	{
		const QMutexLocker lock(&client->m_seqMutex); client->m_portIDs[port][0] = id;
	}
	void deliver(const snd_seq_event_t& event, const QString& name = QStringLiteral("55:1 Fixture:Source"))
	{
		const QMutexLocker lock(&client->m_seqMutex); client->processInputEventLocked(event, name);
	}
	void fail(int code, bool terminal)
	{
		const QMutexLocker lock(&client->m_seqMutex); client->inputFailed(code, terminal);
	}
	int targetCount() const { const QMutexLocker lock(&client->m_seqMutex); return client->m_portIDs.size(); }
	static bool hasInput(MidiAlsaSeq& client, MidiPort* port)
	{
		const QMutexLocker lock(&client.m_seqMutex); return client.m_portIDs.value(port)[0] >= 0;
	}
	static bool queueRunning(MidiAlsaSeq& client, bool running)
	{
		const QMutexLocker lock(&client.m_seqMutex);
		const int changed = running ? snd_seq_continue_queue(client.m_seqHandle, client.m_queueID, nullptr)
			: snd_seq_stop_queue(client.m_seqHandle, client.m_queueID, nullptr);
		return changed >= 0 && snd_seq_drain_output(client.m_seqHandle) >= 0;
	}
	static int queuedEvents(MidiAlsaSeq& client)
	{
		const QMutexLocker lock(&client.m_seqMutex); snd_seq_queue_status_t* status; snd_seq_queue_status_alloca(&status);
		return snd_seq_get_queue_status(client.m_seqHandle, client.m_queueID, status) < 0 ? -1 : snd_seq_queue_status_get_events(status);
	}
	static int synchronizeTempo(MidiAlsaSeq& client, bpm_t tempo)
	{
		const QMutexLocker lock(&client.m_seqMutex); return client.synchronizeQueueTempoLocked(tempo);
	}
	static void tempoChanged(MidiAlsaSeq& client, bpm_t tempo) { client.changeQueueTempo(tempo); }
	static std::pair<int, unsigned int> nativeTempo(MidiAlsaSeq& client)
	{
		const QMutexLocker lock(&client.m_seqMutex); snd_seq_queue_tempo_t* tempo; snd_seq_queue_tempo_alloca(&tempo);
		if (snd_seq_get_queue_tempo(client.m_seqHandle, client.m_queueID, tempo) < 0) { return {-1, 0}; }
		return {snd_seq_queue_tempo_get_ppq(tempo), snd_seq_queue_tempo_get_tempo(tempo)};
	}
	class OutputCapture
	{
		MidiAlsaSeq& m_client;
	public:
		struct Removal { unsigned int flags; int tag, queue; snd_seq_addr_t destination; };
		std::vector<snd_seq_event_t> attempts, pending, delivered;
		std::vector<Removal> removals;
		int removeError = 0, failSendAt = -1;
		int ppq = 96, tempoReads = 0, tempoWrites = 0, failReadAt = -1, setTempoError = 0;
		unsigned int microseconds = 500000, skew = 0x10000, skewBase = 0x10000;
		bool ignoreTempoWrite = false;
		QString corruptReadback;
		std::vector<int> tempoQueues;
		explicit OutputCapture(MidiAlsaSeq& client) : m_client(client)
		{
			// Only the injected output/clock API calls see this opaque fixture handle. This object
			// must retire before the ports; real ALSA lifecycle APIs never receive it.
			m_client.m_seqHandle = reinterpret_cast<snd_seq_t*>(this); m_client.m_queueID = 19;
			m_client.m_outputApi = {&send, &remove, &getTempo, &setTempo};
		}
		~OutputCapture()
		{
			m_client.m_seqHandle = nullptr; m_client.m_queueID = -1; m_client.m_portIDs.clear(); m_client.m_outputApi = {};
			m_client.m_queueTempo = 0; m_client.m_queueMicrosecondsPerQuarterNote = 0;
		}
		void add(MidiPort& port, int output = 7, int input = -1)
		{
			auto& ids = m_client.m_portIDs[&port]; ids[0] = input; ids[1] = output;
		}
		quint16 activeChannels(const MidiPort& port) const
		{
			return m_client.m_portIDs.value(const_cast<MidiPort*>(&port)).activeOutputChannels;
		}
	private:
		static int getTempo(snd_seq_t* handle, int queue, snd_seq_queue_tempo_t* tempo)
		{
			auto& self = *reinterpret_cast<OutputCapture*>(handle); self.tempoQueues.push_back(queue);
			if (++self.tempoReads == self.failReadAt) { return -EIO; }
			const bool corrupt = self.tempoWrites > 0;
			snd_seq_queue_tempo_set_tempo(tempo, self.microseconds + (corrupt && self.corruptReadback == "tempo" ? 1 : 0));
			snd_seq_queue_tempo_set_ppq(tempo, corrupt && self.corruptReadback == "ppq" ? 16 : self.ppq);
			snd_seq_queue_tempo_set_skew(tempo, corrupt && self.corruptReadback == "skew" ? 0x8000 : self.skew);
			snd_seq_queue_tempo_set_skew_base(tempo, corrupt && self.corruptReadback == "zero-skew-base" ? 0 : self.skewBase);
			return 0;
		}
		static int setTempo(snd_seq_t* handle, int queue, snd_seq_queue_tempo_t* tempo)
		{
			auto& self = *reinterpret_cast<OutputCapture*>(handle); ++self.tempoWrites; self.tempoQueues.push_back(queue);
			if (!self.ignoreTempoWrite)
			{
				self.ppq = snd_seq_queue_tempo_get_ppq(tempo); self.microseconds = snd_seq_queue_tempo_get_tempo(tempo);
				self.skew = snd_seq_queue_tempo_get_skew(tempo); self.skewBase = snd_seq_queue_tempo_get_skew_base(tempo);
			}
			return self.setTempoError;
		}
		static int send(snd_seq_t* handle, snd_seq_event_t* event)
		{
			auto& self = *reinterpret_cast<OutputCapture*>(handle); self.attempts.push_back(*event);
			(event->queue == SND_SEQ_QUEUE_DIRECT ? self.delivered : self.pending).push_back(*event);
			return int(self.attempts.size()) == self.failSendAt ? -EIO : int(sizeof(*event));
		}
		static int remove(snd_seq_t* handle, snd_seq_remove_events_t* condition)
		{
			auto& self = *reinterpret_cast<OutputCapture*>(handle);
			const Removal removal{snd_seq_remove_events_get_condition(condition), snd_seq_remove_events_get_tag(condition),
				snd_seq_remove_events_get_queue(condition), *snd_seq_remove_events_get_dest(condition)};
			self.removals.push_back(removal); if (self.removeError) { return self.removeError; }
			std::erase_if(self.pending, [&](const auto& event) {
				return event.tag == removal.tag && event.queue == removal.queue
					&& event.dest.client == removal.destination.client && event.dest.port == removal.destination.port;
			});
			return 0;
		}
	};
};
}

class AlsaMidiProcessor final : public MidiEventProcessor
{
public:
	std::vector<MidiEvent> events;
	std::vector<TimePos> times;
	std::vector<QThread*> threads;
	void processInEvent(const MidiEvent& event, const TimePos& time, f_cnt_t) override
	{
		events.push_back(event); times.push_back(time); threads.push_back(QThread::currentThread());
	}
	void processOutEvent(const MidiEvent&, const TimePos&, f_cnt_t) override { }
};

class MidiAlsaSeqTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	static snd_seq_event_t note()
	{
		snd_seq_event_t event{}; snd_seq_ev_clear(&event); snd_seq_ev_set_noteon(&event, 2, 60, 100);
		event.source = {55, 1}; event.dest = {42, 7}; event.time.tick = 37; return event;
	}

private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->clearProject(); }

	void channelVoiceDeliveryOwnsItsSourceAndUsesThePortThread_data()
	{
		QTest::addColumn<int>("nativeType"); QTest::addColumn<int>("type"); QTest::addColumn<int>("first"); QTest::addColumn<int>("second");
		QTest::newRow("note-on") << int(SND_SEQ_EVENT_NOTEON) << int(MidiNoteOn) << 60 << 100;
		QTest::newRow("note-off") << int(SND_SEQ_EVENT_NOTEOFF) << int(MidiNoteOff) << 60 << 0;
		QTest::newRow("key-pressure") << int(SND_SEQ_EVENT_KEYPRESS) << int(MidiKeyPressure) << 60 << 80;
		QTest::newRow("controller") << int(SND_SEQ_EVENT_CONTROLLER) << int(MidiControlChange) << 74 << 99;
		QTest::newRow("program") << int(SND_SEQ_EVENT_PGMCHANGE) << int(MidiProgramChange) << 127 << 0;
		QTest::newRow("channel-pressure") << int(SND_SEQ_EVENT_CHANPRESS) << int(MidiChannelPressure) << 80 << 0;
		QTest::newRow("pitch-bend") << int(SND_SEQ_EVENT_PITCHBEND) << int(MidiPitchBend) << 16383 << 0;
	}
	void channelVoiceDeliveryOwnsItsSourceAndUsesThePortThread()
	{
		QFETCH(int, nativeType); QFETCH(int, type); QFETCH(int, first); QFETCH(int, second);
		MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor; MidiPort port("ALSA", backend.client.get(), &processor);
		port.setReadable(true); backend.addTarget(&port); const QString name = QString::fromUtf8("55:1 Fixture:Source, \xce\xb1");
		std::thread reader([&] {
			auto event = note(); event.type = nativeType;
			if (nativeType == SND_SEQ_EVENT_NOTEON || nativeType == SND_SEQ_EVENT_NOTEOFF || nativeType == SND_SEQ_EVENT_KEYPRESS)
			{
				event.data.note.note = first; event.data.note.velocity = second;
			}
			else
			{
				event.data.control.channel = 2;
				event.data.control.param = nativeType == SND_SEQ_EVENT_CONTROLLER ? first : 17;
				event.data.control.value = nativeType == SND_SEQ_EVENT_CONTROLLER ? second : nativeType == SND_SEQ_EVENT_PITCHBEND ? first - 8192 : first;
			}
			backend.deliver(event, name); event = {}; // Native callback storage expires before delivery.
		}); reader.join(); QVERIFY(processor.events.empty()); QTRY_COMPARE(processor.events.size(), std::size_t(1));
		const auto& event = processor.events.front(); QCOMPARE(int(event.type()), type); QCOMPARE(event.param(0), int16_t(first)); QCOMPARE(event.param(1), int16_t(second));
		QVERIFY(event.sourcePort() == nullptr); QCOMPARE(backend.client->sourcePortName(event), name); QCOMPARE(processor.threads.front(), QThread::currentThread());
		QCOMPARE(int(processor.times.front()), nativeType == SND_SEQ_EVENT_NOTEON || nativeType == SND_SEQ_EVENT_NOTEOFF ? 37 : 0);
	}

	void malformedOrForeignEventsNeverReachTheNativeProcessor_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto fault : {"client", "port", "channel", "key", "velocity", "controller", "control-value", "program", "pressure", "bend-low", "bend-high", "sysex"})
		{
			QTest::newRow(fault) << QString::fromLatin1(fault);
		}
	}
	void malformedOrForeignEventsNeverReachTheNativeProcessor()
	{
		QFETCH(QString, fault); MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor; MidiPort port("ALSA", backend.client.get(), &processor);
		port.setReadable(true); backend.addTarget(&port); auto event = note(); const char borrowed[]{1, 2, 3};
		if (fault == "client") { event.dest.client = 43; }
		else if (fault == "port") { event.dest.port = 8; }
		else if (fault == "channel") { event.data.note.channel = 16; }
		else if (fault == "key") { event.data.note.note = 128; }
		else if (fault == "velocity") { event.data.note.velocity = 128; }
		else if (fault == "sysex") { event.type = SND_SEQ_EVENT_SYSEX; event.data.ext.ptr = const_cast<char*>(borrowed); event.data.ext.len = sizeof(borrowed); }
		else
		{
			event.data.control.channel = 2; event.data.control.param = 74; event.data.control.value = 99;
			if (fault == "controller" || fault == "control-value")
			{
				event.type = SND_SEQ_EVENT_CONTROLLER;
				if (fault == "controller") { event.data.control.param = 128; } else { event.data.control.value = 65536; }
			}
			else if (fault == "program") { event.type = SND_SEQ_EVENT_PGMCHANGE; event.data.control.value = 65536; }
			else if (fault == "pressure") { event.type = SND_SEQ_EVENT_CHANPRESS; event.data.control.value = 128; }
			else { event.type = SND_SEQ_EVENT_PITCHBEND; event.data.control.value = fault == "bend-low" ? -8193 : 8192; }
		}
		backend.deliver(event); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.events.empty());
		QCOMPARE(backend.client->inputDeliveryStatus(&port).pendingEvents, 0u); QVERIFY(!backend.client->inputDeliveryStatus(&port).failed);
	}

	void portAndClientRetirementCancelAlreadyQueuedInput()
	{
		MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor; auto port = std::make_unique<MidiPort>("ALSA", backend.client.get(), &processor);
		port->setReadable(true); backend.addTarget(port.get()); backend.deliver(note()); port.reset();
		QCOMPARE(backend.targetCount(), 0); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.events.empty());
		MidiPort survivor("survivor", backend.client.get(), &processor); survivor.setReadable(true); backend.addTarget(&survivor); backend.deliver(note());
		backend.client.reset(); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.events.empty()); QVERIFY(survivor.client()->isRaw());
	}

	void readerCanRaceNativePortMapConstructionAndDeletion()
	{
		MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor; std::atomic<bool> started{false};
		std::thread reader([&] {
			started.store(true, std::memory_order_release);
			for (int n = 0; n < 4000; ++n) { backend.deliver(note()); }
		});
		while (!started.load(std::memory_order_acquire)) { std::this_thread::yield(); }
		for (int n = 0; n < 200; ++n)
		{
			auto port = std::make_unique<MidiPort>("racing", backend.client.get(), &processor); port->setReadable(true); backend.addTarget(port.get()); port.reset();
		}
		reader.join(); QCOMPARE(backend.targetCount(), 0); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.events.empty());
		MidiPort fresh("fresh", backend.client.get(), &processor); fresh.setReadable(true); backend.addTarget(&fresh); backend.deliver(note());
		QTRY_COMPARE(processor.events.size(), std::size_t(1));
	}

	void inputLossFaultsExistingPortsAndPreservesTheFirstNativeError()
	{
		MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor; MidiPort port("loss", backend.client.get(), &processor);
		port.setReadable(true); backend.addTarget(&port); backend.deliver(note()); backend.fail(-ENOSPC, false);
		const auto delivery = backend.client->inputDeliveryStatus(&port); QVERIFY(delivery.failed); QCOMPARE(delivery.pendingEvents, 0u);
		QVERIFY(delivery.failure == MidiClient::InputDeliveryFailure::BackendInput); QCOMPARE(delivery.droppedEvents, std::uint64_t(1));
		QTRY_COMPARE(processor.events.size(), std::size_t(MidiChannelCount));
		for (const auto& event : processor.events) { QCOMPARE(event.controllerNumber(), uint8_t(MidiControllerAllSoundOff)); }
		backend.fail(-ENODEV, true); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		const auto state = backend.client->inputBackendStatus(); QVERIFY(state.observed); QVERIFY(!state.active); QCOMPARE(state.nativeErrorCode, -ENOSPC);
		QVERIFY(backend.client->readablePorts().isEmpty());
	}

	void noteTimestampModesAndBoundsAreExplicit_data()
	{
		QTest::addColumn<QString>("mode"); QTest::newRow("tick") << QString("tick");
		QTest::newRow("real") << QString("real"); QTest::newRow("overflow") << QString("overflow");
	}
	void noteTimestampModesAndBoundsAreExplicit()
	{
		QFETCH(QString, mode); MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor; MidiPort port("time", backend.client.get(), &processor);
		port.setReadable(true); backend.addTarget(&port); auto event = note();
		if (mode == "real") { event.flags = SND_SEQ_TIME_STAMP_REAL; event.time.time.tv_sec = 100; event.time.time.tv_nsec = 123456; }
		if (mode == "overflow") { event.time.tick = static_cast<unsigned int>(std::numeric_limits<int>::max()) + 1u; }
		backend.deliver(event); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		if (mode == "overflow") { QVERIFY(processor.events.empty()); }
		else { QCOMPARE(processor.events.size(), std::size_t(1)); QCOMPARE(int(processor.times.front()), mode == "real" ? 0 : 37); }
	}

	void outputUsesPortTagsAndDirectOrQueuedDelivery_data()
	{
		QTest::addColumn<int>("delay"); QTest::newRow("immediate") << 0; QTest::newRow("future") << 37;
		QTest::newRow("maximum-delay") << std::numeric_limits<int>::max();
	}
	void outputUsesPortTagsAndDirectOrQueuedDelivery()
	{
		QFETCH(int, delay); MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor;
		MidiPort port("output", backend.client.get(), &processor); port.setWritable(true);
		MidiAlsaSeqTestAccess::OutputCapture capture(*backend.client); capture.add(port, 17, 7);
		backend.client->processOutEvent(MidiEvent(MidiPitchBend, 2, 16383), TimePos(delay), &port);
		QCOMPARE(capture.attempts.size(), std::size_t(1)); const auto& event = capture.attempts.front();
		QCOMPARE(int(event.source.port), 17); QCOMPARE(int(event.tag), 17);
		QCOMPARE(int(event.dest.client), int(SND_SEQ_ADDRESS_SUBSCRIBERS)); QCOMPARE(int(event.dest.port), int(SND_SEQ_ADDRESS_UNKNOWN));
		QCOMPARE(int(event.type), int(SND_SEQ_EVENT_PITCHBEND)); QCOMPARE(int(event.data.control.channel), 2); QCOMPARE(event.data.control.value, 8191);
		QCOMPARE(int(event.queue), delay ? 19 : int(SND_SEQ_QUEUE_DIRECT));
		if (delay) { QCOMPARE(event.time.tick, static_cast<unsigned int>(delay)); QVERIFY(event.flags & SND_SEQ_TIME_MODE_REL); }
		port.releaseOutput(); QVERIFY(capture.pending.empty()); QVERIFY(!capture.activeChannels(port));
	}

	void queuedReleasesRetainCleanupStateAndCancellationIsScoped()
	{
		MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor;
		MidiPort first("first", backend.client.get(), &processor), second("second", backend.client.get(), &processor);
		first.setWritable(true); second.setWritable(true); first.setOutputChannel(0); second.setOutputChannel(0);
		MidiAlsaSeqTestAccess::OutputCapture capture(*backend.client); capture.add(first, 7); capture.add(second, 8);
		first.processOutEvent(MidiEvent(MidiNoteOn, 2, 60, 100), TimePos(10));
		first.processOutEvent(MidiEvent(MidiNoteOff, 2, 60, 0), TimePos(20));
		first.processOutEvent(MidiEvent(MidiControlChange, 2, MidiControllerSustain, 127), TimePos(5));
		first.processOutEvent(MidiEvent(MidiControlChange, 2, MidiControllerSustain, 0), TimePos(25));
		first.processOutEvent(MidiEvent(MidiProgramChange, 5, 9), TimePos(30));
		second.processOutEvent(MidiEvent(MidiNoteOn, 3, 61, 100), TimePos(10));
		QCOMPARE(first.trackedOutputNotes(), 0u); QCOMPARE(capture.activeChannels(first), quint16(1u << 2));
		first.setOutputSource(MidiPort::OutputSource::Instrument);
		QCOMPARE(capture.pending.size(), std::size_t(1)); QCOMPARE(int(capture.pending.front().tag), 8);
		QCOMPARE(capture.removals.size(), std::size_t(1)); const auto& removal = capture.removals.front();
		QCOMPARE(removal.flags, unsigned(SND_SEQ_REMOVE_OUTPUT | SND_SEQ_REMOVE_TAG_MATCH | SND_SEQ_REMOVE_DEST));
		QCOMPARE(removal.tag, 7); QCOMPARE(removal.queue, 19);
		QCOMPARE(int(removal.destination.client), int(SND_SEQ_ADDRESS_SUBSCRIBERS));
		QCOMPARE(int(removal.destination.port), int(SND_SEQ_ADDRESS_UNKNOWN));
		QCOMPARE(capture.delivered.size(), std::size_t(4));
		const int controllers[]{MidiControllerSustain, MidiControllerSostenuto, MidiControllerAllNotesOff, MidiControllerAllSoundOff};
		for (int i = 0; i < 4; ++i)
		{
			const auto& event = capture.delivered[i]; QCOMPARE(int(event.queue), int(SND_SEQ_QUEUE_DIRECT));
			QCOMPARE(int(event.source.port), 7); QCOMPARE(int(event.tag), 7); QCOMPARE(int(event.type), int(SND_SEQ_EVENT_CONTROLLER));
			QCOMPARE(int(event.data.control.channel), 2); QCOMPARE(event.data.control.param, unsigned(controllers[i])); QCOMPARE(event.data.control.value, 0);
		}
		QCOMPARE(capture.activeChannels(first), quint16(0)); QCOMPARE(capture.activeChannels(second), quint16(1u << 3));
		first.processOutEvent(MidiEvent(MidiNoteOn, 2, 62, 100), TimePos(1), MidiPort::OutputSource::Instrument);
		QCOMPARE(capture.pending.size(), std::size_t(2)); first.releaseOutput(); second.releaseOutput(); QVERIFY(capture.pending.empty());
	}

	void nativeCancellationFailuresPreserveCleanupAndSourceForRetry_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("remove") << -1;
		QTest::newRow("sustain-release") << 0; QTest::newRow("sostenuto-release") << 1;
		QTest::newRow("all-notes-off") << 2; QTest::newRow("all-sound-off") << 3;
	}
	void nativeCancellationFailuresPreserveCleanupAndSourceForRetry()
	{
		QFETCH(int, failure); MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor;
		MidiPort port("retry", backend.client.get(), &processor); port.setWritable(true); port.setOutputChannel(0);
		MidiAlsaSeqTestAccess::OutputCapture capture(*backend.client); capture.add(port);
		port.processOutEvent(MidiEvent(MidiNoteOn, 2, 60, 100), TimePos(10)); port.processOutEvent(MidiEvent(MidiNoteOff, 2, 60, 0), TimePos(20));
		if (failure < 0) { capture.removeError = -EIO; } else { capture.failSendAt = int(capture.attempts.size()) + failure + 1; }
		QVERIFY_EXCEPTION_THROWN(port.setOutputSource(MidiPort::OutputSource::Instrument), std::runtime_error);
		QVERIFY(port.outputSource() == MidiPort::OutputSource::Track); QCOMPARE(capture.activeChannels(port), quint16(1u << 2));
		QCOMPARE(backend.client->inputBackendStatus().nativeErrorCode, -EIO);
		if (failure < 0) { QCOMPARE(capture.pending.size(), std::size_t(2)); QVERIFY(capture.delivered.empty()); }
		capture.removeError = 0; capture.failSendAt = -1; port.setOutputSource(MidiPort::OutputSource::Instrument);
		QVERIFY(capture.pending.empty()); QVERIFY(!capture.activeChannels(port)); QCOMPARE(port.trackedOutputNotes(), 0u);
		QCOMPARE(int(capture.delivered.back().data.control.param), int(MidiControllerAllSoundOff));
	}

	void failedNativeSubmissionRemainsReleasable()
	{
		MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor; MidiPort port("send", backend.client.get(), &processor); port.setWritable(true);
		MidiAlsaSeqTestAccess::OutputCapture capture(*backend.client); capture.add(port); capture.failSendAt = 1;
		QVERIFY_EXCEPTION_THROWN(port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), TimePos(10)), std::runtime_error);
		QCOMPARE(port.trackedOutputNotes(), 1u); QCOMPARE(capture.activeChannels(port), quint16(1));
		capture.failSendAt = -1; port.releaseOutput(); QVERIFY(capture.pending.empty()); QVERIFY(!capture.activeChannels(port));
		QCOMPARE(port.trackedOutputNotes(), 0u); QCOMPARE(int(capture.delivered.back().type), int(SND_SEQ_EVENT_NOTEOFF));
	}

	void outputRejectsInvalidDelayOrNativeRoute_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto value : {"negative-delay", "missing-output", "input-only", "tag-overflow", "missing-handle"})
		{
			QTest::newRow(value) << QString::fromLatin1(value);
		}
	}
	void outputRejectsInvalidDelayOrNativeRoute()
	{
		QFETCH(QString, fault); MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor; MidiPort port("invalid", backend.client.get(), &processor);
		const MidiEvent event(MidiNoteOn, 0, 60, 100);
		if (fault == "missing-handle")
		{
			QVERIFY_EXCEPTION_THROWN(backend.client->processOutEvent(event, {}, &port), std::runtime_error); return;
		}
		MidiAlsaSeqTestAccess::OutputCapture capture(*backend.client);
		if (fault != "missing-output") { capture.add(port, fault == "input-only" ? -1 : fault == "tag-overflow" ? 256 : 7, 9); }
		QVERIFY_EXCEPTION_THROWN(backend.client->processOutEvent(event, TimePos(fault == "negative-delay" ? -1 : 10), &port), std::exception);
		QVERIFY(capture.attempts.empty());
	}

	void musicalQueueClockMatchesNativeFramesAcrossTempoAndMeter_data()
	{
		QTest::addColumn<int>("bpm"); QTest::addColumn<int>("rate"); QTest::addColumn<int>("numerator"); QTest::addColumn<int>("denominator");
		QTest::newRow("one-second-quarter") << 60 << 48000 << 4 << 4;
		QTest::newRow("half-second-quarter") << 120 << 96000 << 7 << 8;
		QTest::newRow("minimum-tempo") << 10 << 22050 << 3 << 4;
		QTest::newRow("maximum-tempo") << 999 << 192000 << 5 << 8;
		QTest::newRow("nonintegral-microseconds") << 140 << 44100 << 4 << 4;
		QTest::newRow("compound-meter") << 75 << 48000 << 6 << 8;
	}
	void musicalQueueClockMatchesNativeFramesAcrossTempoAndMeter()
	{
		QFETCH(int, bpm); QFETCH(int, rate); QFETCH(int, numerator); QFETCH(int, denominator);
		auto song = Engine::getSong(); song->setTempo(bpm); song->getTimeSigModel().setNumerator(numerator); song->getTimeSigModel().setDenominator(denominator);
		MidiAlsaSeqTestAccess backend; MidiAlsaSeqTestAccess::OutputCapture capture(*backend.client);
		capture.skew = 0x8000; // An inherited half-speed skew must not change LMMS's musical clock.
		QCOMPARE(MidiAlsaSeqTestAccess::synchronizeTempo(*backend.client, song->getTempo()), 0);
		const auto status = backend.client->outputClockStatus(); QVERIFY(status.observed); QVERIFY(status.configurationVerified);
		QCOMPARE(status.ticksPerQuarterNote, 48); QCOMPARE(capture.ppq, 48); QCOMPARE(capture.skew, capture.skewBase);
		QCOMPARE(status.microsecondsPerQuarterNote, 60000000u / unsigned(bpm)); QCOMPARE(status.nativeErrorCode, 0);
		QCOMPARE(capture.tempoQueues, std::vector<int>({19, 19, 19})); QCOMPARE(capture.tempoReads, 2); QCOMPARE(capture.tempoWrites, 1);
		const double expectedQuarterSeconds = 60.0 / bpm;
		const double nativeQuarterSeconds = double(status.microsecondsPerQuarterNote) / 1000000.0;
		QVERIFY(std::abs(nativeQuarterSeconds - expectedQuarterSeconds) < 0.000001);
		const auto frames = TimePos(48).frames(Engine::framesPerTick(rate));
		QVERIFY(std::abs(double(frames) - rate * expectedQuarterSeconds) <= 1.0);
		QVERIFY(std::abs(double(Engine::framesPerTick()) * 48 - Engine::audioEngine()->outputSampleRate() * expectedQuarterSeconds) < 1.0);
	}

	void failedClockSynchronizationBlocksScheduledOutputAndRetries_data()
	{
		QTest::addColumn<QString>("failure");
		for (const auto value : {"initial-read", "set", "readback", "tempo", "ppq", "skew", "zero-skew-base", "ignored-write"})
		{
			QTest::newRow(value) << QString::fromLatin1(value);
		}
	}
	void failedClockSynchronizationBlocksScheduledOutputAndRetries()
	{
		QFETCH(QString, failure); MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor;
		MidiPort port("clock", backend.client.get(), &processor); port.setWritable(true);
		MidiAlsaSeqTestAccess::OutputCapture capture(*backend.client); capture.add(port);
		if (failure == "initial-read") { capture.failReadAt = 1; }
		else if (failure == "readback") { capture.failReadAt = 2; }
		else if (failure == "set") { capture.setTempoError = -EBUSY; }
		else if (failure == "ignored-write") { capture.ignoreTempoWrite = true; }
		else { capture.corruptReadback = failure; }
		QVERIFY(MidiAlsaSeqTestAccess::synchronizeTempo(*backend.client, Engine::getSong()->getTempo()) < 0);
		const auto failed = backend.client->outputClockStatus(); QVERIFY(failed.observed); QVERIFY(!failed.configurationVerified); QVERIFY(failed.nativeErrorCode < 0);
		// Immediate release remains possible even with an unusable musical queue.
		backend.client->processOutEvent(MidiEvent(MidiControlChange, 0, MidiControllerAllSoundOff, 0), {}, &port);
		QCOMPARE(capture.delivered.size(), std::size_t(1)); capture.setTempoError = -EIO;
		QVERIFY_EXCEPTION_THROWN(backend.client->processOutEvent(MidiEvent(MidiProgramChange, 0, 7), TimePos(1), &port), std::runtime_error);
		QCOMPARE(capture.attempts.size(), std::size_t(1)); QVERIFY(capture.pending.empty());
		capture.failReadAt = -1; capture.setTempoError = 0; capture.ignoreTempoWrite = false; capture.corruptReadback.clear();
		backend.client->processOutEvent(MidiEvent(MidiProgramChange, 0, 7), TimePos(1), &port);
		QCOMPARE(capture.pending.size(), std::size_t(1)); QVERIFY(backend.client->outputClockStatus().configurationVerified);
		QCOMPARE(backend.client->outputClockStatus().nativeErrorCode, 0); port.releaseOutput();
	}

	void tempoChangesSynchronizeBeforeSchedulingWithoutRepeatedIoctls()
	{
		MidiAlsaSeqTestAccess backend; AlsaMidiProcessor processor; MidiPort port("tempo", backend.client.get(), &processor); port.setWritable(true);
		MidiAlsaSeqTestAccess::OutputCapture capture(*backend.client); capture.add(port);
		auto song = Engine::getSong(); song->setTempo(120); const MidiEvent event(MidiProgramChange, 0, 7);
		backend.client->processOutEvent(event, TimePos(12), &port); QCOMPARE(capture.tempoWrites, 1); QCOMPARE(capture.microseconds, 500000u);
		for (int i = 0; i < 16; ++i) { backend.client->processOutEvent(event, TimePos(12), &port); }
		QCOMPARE(capture.tempoReads, 2); QCOMPARE(capture.tempoWrites, 1);
		song->setTempo(60); // Fixture has no signal connection: the send path must also detect a changed tempo.
		backend.client->processOutEvent(event, TimePos(12), &port); QCOMPARE(capture.tempoWrites, 2); QCOMPARE(capture.microseconds, 1000000u);
		song->setTempo(150); MidiAlsaSeqTestAccess::tempoChanged(*backend.client, 150);
		QCOMPARE(capture.microseconds, 400000u); QCOMPARE(capture.tempoWrites, 3); QCOMPARE(capture.attempts.size(), std::size_t(18));
		backend.client->processOutEvent(event, TimePos(12), &port); QCOMPARE(capture.tempoWrites, 3);
		MidiAlsaSeqTestAccess::tempoChanged(*backend.client, 0); QVERIFY(!backend.client->outputClockStatus().configurationVerified);
		QCOMPARE(backend.client->outputClockStatus().nativeErrorCode, -EINVAL);
		backend.client->processOutEvent(event, TimePos(12), &port); QCOMPARE(capture.tempoWrites, 4); QVERIFY(backend.client->outputClockStatus().configurationVerified);
		port.releaseOutput(); QVERIFY(capture.pending.empty());
	}

	void realKernelClockTracksNativeTempoBeforeScheduledDelivery_data()
	{
		QTest::addColumn<int>("bpm"); QTest::newRow("slow") << 60; QTest::newRow("fast") << 240;
	}
	void realKernelClockTracksNativeTempoBeforeScheduledDelivery()
	{
		QFETCH(int, bpm); if (!QFile::exists("/dev/snd/seq")) { QSKIP("Requires the native Linux ALSA sequencer; no physical MIDI device is needed."); }
		snd_seq_t* handle = nullptr; const int opened = snd_seq_open(&handle, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
		std::unique_ptr<snd_seq_t, decltype(&snd_seq_close)> peer(handle, &snd_seq_close); QVERIFY2(opened >= 0, snd_strerror(opened));
		QVERIFY(snd_seq_set_client_name(peer.get(), "Studio Clock Peer") >= 0);
		const int sink = snd_seq_create_simple_port(peer.get(), "Clock Sink", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE, SND_SEQ_PORT_TYPE_APPLICATION);
		QVERIFY(sink >= 0); auto song = Engine::getSong(); song->setTempo(120);
		auto backend = std::make_unique<MidiAlsaSeq>(); AlsaMidiProcessor processor; QTRY_VERIFY(backend->inputBackendStatus().active);
		QCOMPARE(MidiAlsaSeqTestAccess::nativeTempo(*backend), std::make_pair(48, 500000u));
		song->setTempo(bpm); // The actual direct Qt connection must update the running kernel queue before returning.
		QCOMPARE(MidiAlsaSeqTestAccess::nativeTempo(*backend), std::make_pair(48, 60000000u / unsigned(bpm)));
		song->getTimeSigModel().setNumerator(7); song->getTimeSigModel().setDenominator(8);
		QCOMPARE(MidiAlsaSeqTestAccess::nativeTempo(*backend), std::make_pair(48, 60000000u / unsigned(bpm)));
		MidiPort port("clock", backend.get(), &processor);
		port.subscribeWritablePort(QString("%1:%2 Studio Clock Peer:Clock Sink").arg(snd_seq_client_id(peer.get())).arg(sink));
		QElapsedTimer elapsed; elapsed.start(); port.processOutEvent(MidiEvent(MidiProgramChange, 0, 9), TimePos(12));
		bool arrived = false;
		QTRY_VERIFY(([&] {
			for (int n = 0; n < 64; ++n)
			{
				snd_seq_event_t* event = nullptr; const int result = snd_seq_event_input(peer.get(), &event);
				if (result == -EAGAIN) { break; }
				if (result < 0) { return false; }
				if (event->type == SND_SEQ_EVENT_PGMCHANGE && event->data.control.value == 9) { arrived = true; }
			}
			return arrived;
		})());
		// A coarse lower bound catches accidental immediate dispatch; the queue
		// readback above proves the scale without imposing an OS jitter deadline.
		QVERIFY(elapsed.elapsed() >= (15000 / bpm) / 2);
		QCOMPARE(MidiAlsaSeqTestAccess::queuedEvents(*backend), 0); QVERIFY(backend->outputClockStatus().configurationVerified);
		QCOMPARE(backend->outputClockStatus().nativeErrorCode, 0);
	}

	void realKernelSequencerInputAndShutdown()
	{
		if (!QFile::exists("/dev/snd/seq")) { QSKIP("Requires the native Linux ALSA sequencer; no physical MIDI device is needed."); }
		snd_seq_t* handle = nullptr;
		const auto opened = snd_seq_open(&handle, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
		std::unique_ptr<snd_seq_t, decltype(&snd_seq_close)> peer(handle, &snd_seq_close);
		QVERIFY2(opened >= 0, snd_strerror(opened)); QVERIFY(snd_seq_set_client_name(peer.get(), "Studio Test Peer") >= 0);
		const auto peerPort = snd_seq_create_simple_port(peer.get(), "Input Source", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, SND_SEQ_PORT_TYPE_APPLICATION);
		QVERIFY(peerPort >= 0); const auto peerClient = snd_seq_client_id(peer.get()); QVERIFY(peerClient >= 0);
		const auto sourceName = QString("%1:%2 Studio Test Peer:Input Source").arg(peerClient).arg(peerPort);
		auto backend = std::make_unique<MidiAlsaSeq>(); AlsaMidiProcessor processor;
		QTRY_VERIFY(backend->inputBackendStatus().active); QCOMPARE(backend->inputBackendStatus().nativeErrorCode, 0);
		auto port = std::make_unique<MidiPort>("kernel", backend.get(), &processor); port->subscribeReadablePort(sourceName);
		port->subscribeReadablePort(sourceName); // Endpoint refresh must not turn an existing connection into an error.
		QVERIFY(MidiAlsaSeqTestAccess::hasInput(*backend, port.get()));
		QCOMPARE(backend->inputBackendStatus().nativeErrorCode, 0);
		auto event = note(); snd_seq_ev_set_source(&event, peerPort); event.source.client = peerClient;
		snd_seq_ev_set_subs(&event); snd_seq_ev_set_direct(&event);
		QVERIFY(snd_seq_event_output_direct(peer.get(), &event) >= 0); QTRY_COMPARE(processor.events.size(), std::size_t(1));
		QCOMPARE(processor.events.front().key(), 60); QCOMPARE(backend->sourcePortName(processor.events.front()), sourceName);
		QCOMPARE(processor.threads.front(), QThread::currentThread());
		port->subscribeReadablePort(sourceName, false); port->subscribeReadablePort(sourceName, false);
		QCOMPARE(backend->inputBackendStatus().nativeErrorCode, 0);
		QElapsedTimer elapsed; elapsed.start(); backend.reset(); QVERIFY(elapsed.elapsed() < 5000);
		QVERIFY(port->client()->isRaw()); // The surviving native port was invalidated after the reader joined.
	}

	void realKernelCancellationReleasesImmediatelyAndPreservesOtherPorts_data()
	{
		QTest::addColumn<QString>("retirement");
		for (const auto value : {"release", "source", "disable", "unsubscribe", "port-destruction", "recycle", "client-destruction"})
		{
			QTest::newRow(value) << QString::fromLatin1(value);
		}
	}
	void realKernelCancellationReleasesImmediatelyAndPreservesOtherPorts()
	{
		QFETCH(QString, retirement);
		if (!QFile::exists("/dev/snd/seq")) { QSKIP("Requires the native Linux ALSA sequencer; no physical MIDI device is needed."); }
		snd_seq_t* handle = nullptr; const int opened = snd_seq_open(&handle, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
		std::unique_ptr<snd_seq_t, decltype(&snd_seq_close)> peer(handle, &snd_seq_close); QVERIFY2(opened >= 0, snd_strerror(opened));
		QVERIFY(snd_seq_set_client_name(peer.get(), "Studio Output Peer") >= 0);
		const int peerPort = snd_seq_create_simple_port(peer.get(), "Sink", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE, SND_SEQ_PORT_TYPE_APPLICATION);
		QVERIFY(peerPort >= 0); const int peerClient = snd_seq_client_id(peer.get()); QVERIFY(peerClient >= 0);
		const QString destination = QString("%1:%2 Studio Output Peer:Sink").arg(peerClient).arg(peerPort);
		auto backend = std::make_unique<MidiAlsaSeq>(); AlsaMidiProcessor processor; QTRY_VERIFY(backend->inputBackendStatus().active);
		auto first = std::make_unique<MidiPort>("first", backend.get(), &processor);
		MidiPort second("second", backend.get(), &processor);
		first->subscribeWritablePort(destination); second.subscribeWritablePort(destination); first->setOutputChannel(0); second.setOutputChannel(0);
		QCOMPARE(backend->inputBackendStatus().nativeErrorCode, 0); QVERIFY(MidiAlsaSeqTestAccess::queueRunning(*backend, false));
		std::vector<snd_seq_event_t> received;
		const auto drain = [&] {
			for (int n = 0; n < 64; ++n)
			{
				snd_seq_event_t* event = nullptr; const int result = snd_seq_event_input(peer.get(), &event);
				if (result == -EAGAIN) { break; }
				if (result < 0) { throw std::runtime_error(snd_strerror(result)); }
				received.push_back(*event);
			}
		};
		first->processOutEvent(MidiEvent(MidiNoteOn, 2, 60, 100));
		QTRY_VERIFY(([&] { drain(); return !received.empty(); })()); received.clear();
		first->processOutEvent(MidiEvent(MidiNoteOff, 2, 60, 0), TimePos(2));
		first->processOutEvent(MidiEvent(MidiNoteOn, 2, 62, 100), TimePos(1));
		first->processOutEvent(MidiEvent(MidiNoteOff, 2, 62, 0), TimePos(2));
		first->processOutEvent(MidiEvent(MidiProgramChange, 2, 8), TimePos(3));
		second.processOutEvent(MidiEvent(MidiProgramChange, 3, 9), TimePos(1));
		QCOMPARE(first->trackedOutputNotes(), 0u); QCOMPARE(MidiAlsaSeqTestAccess::queuedEvents(*backend), 5);
		if (retirement == "source") { first->setOutputSource(MidiPort::OutputSource::Instrument); }
		else if (retirement == "disable") { first->setWritable(false); }
		else if (retirement == "unsubscribe") { first->subscribeWritablePort(destination, false); }
		else if (retirement == "port-destruction" || retirement == "recycle") { first.reset(); }
		else if (retirement == "client-destruction") { backend.reset(); }
		else { first->releaseOutput(); }
		QTRY_VERIFY(([&] {
			drain(); return std::any_of(received.begin(), received.end(), [](const auto& event) {
				return event.type == SND_SEQ_EVENT_CONTROLLER && event.data.control.channel == 2 && event.data.control.param == MidiControllerAllSoundOff;
			});
		})()); // Cleanup bypasses the stopped queue, including client-first teardown.
		if (!backend) { QVERIFY(first->client()->isRaw()); QVERIFY(second.client()->isRaw()); return; }
		QCOMPARE(MidiAlsaSeqTestAccess::queuedEvents(*backend), 1);
		if (retirement == "recycle")
		{
			first = std::make_unique<MidiPort>("replacement", backend.get(), &processor); first->subscribeWritablePort(destination); first->setOutputChannel(0);
			first->processOutEvent(MidiEvent(MidiProgramChange, 4, 10), TimePos(1)); QCOMPARE(MidiAlsaSeqTestAccess::queuedEvents(*backend), 2);
		}
		QVERIFY(MidiAlsaSeqTestAccess::queueRunning(*backend, true));
		QTRY_VERIFY(([&] {
			drain(); return std::any_of(received.begin(), received.end(), [](const auto& event) {
				return event.type == SND_SEQ_EVENT_PGMCHANGE && event.data.control.channel == 3 && event.data.control.value == 9;
			});
		})());
		if (retirement == "recycle")
		{
			QTRY_VERIFY(([&] {
				drain(); return std::any_of(received.begin(), received.end(), [](const auto& event) {
					return event.type == SND_SEQ_EVENT_PGMCHANGE && event.data.control.channel == 4 && event.data.control.value == 10;
				});
			})());
		}
		QTRY_COMPARE(MidiAlsaSeqTestAccess::queuedEvents(*backend), 0); drain();
		QVERIFY(std::none_of(received.begin(), received.end(), [](const auto& event) {
			return event.type == SND_SEQ_EVENT_NOTEON || event.type == SND_SEQ_EVENT_NOTEOFF
				|| (event.type == SND_SEQ_EVENT_PGMCHANGE && event.data.control.channel == 2);
		}));
		QCOMPARE(backend->inputBackendStatus().nativeErrorCode, 0);
	}

	void failedNativeStartupReportsAnErrorAndCanBeDestroyed()
	{
		const auto config = ConfigManager::inst(); const auto previous = config->value("Midialsaseq", "device", QString{});
		const auto restore = qScopeGuard([&] {
			if (previous.isNull()) { config->deleteValue("Midialsaseq", "device"); } else { config->setValue("Midialsaseq", "device", previous); }
		});
		config->setValue("Midialsaseq", "device", "__lmms_missing_sequencer_" + m_configuration.path());
		auto backend = std::make_unique<MidiAlsaSeq>();
		const auto state = backend->inputBackendStatus(); QVERIFY(state.observed); QVERIFY(!state.active); QVERIFY(state.nativeErrorCode < 0);
		QVERIFY(!backend->isRunning()); QVERIFY(backend->readablePorts().isEmpty()); backend.reset();
	}
};

QTEST_GUILESS_MAIN(MidiAlsaSeqTest)
#include "MidiAlsaSeqTest.moc"
