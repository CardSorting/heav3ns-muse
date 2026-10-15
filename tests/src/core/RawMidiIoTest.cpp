/* Native ALSA/sndio raw MIDI lifecycle tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QEvent>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <deque>
#include <memory>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "MidiAlsaRaw.h"
#include "MidiDummy.h"
#include "MidiEventProcessor.h"
#include "MidiPort.h"
#include "MidiSndio.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "../../../src/core/midi/MidiIoWakeup.h"
#include "../../../src/core/midi/RawMidiApi.h"

namespace lmms
{
class RawMidiIoTestAccess
{
public:
#ifdef LMMS_HAVE_ALSA
	static std::unique_ptr<MidiClient> create(const detail::AlsaRawMidiApi& api) { return std::unique_ptr<MidiClient>(new MidiAlsaRaw(api)); }
#endif
#ifdef LMMS_HAVE_SNDIO
	static std::unique_ptr<MidiClient> create(const detail::SndioMidiApi& api) { return std::unique_ptr<MidiClient>(new MidiSndio(api)); }
#endif
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
QStringList backends()
{
	QStringList names;
#ifdef LMMS_HAVE_ALSA
	names.append("alsa");
#endif
#ifdef LMMS_HAVE_SNDIO
	names.append("sndio");
#endif
	return names;
}
void backendRows() { QTest::addColumn<QString>("backend"); for (const auto& name : backends()) { QTest::newRow(qPrintable(name)) << name; } }
void pump() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }

struct Transport
{
	static inline Transport* current = nullptr;
	struct Handle { bool live = false; } input, output;
	QString backend, startupFault;
	detail::MidiIoWakeup incoming;
	std::mutex mutex;
	std::deque<unsigned char> pending;
	std::vector<unsigned char> written;
	std::atomic_int openCalls{0}, closes{0}, reads{0}, writes{0}, polls{0}, badAccess{0}, inCall{0};
	std::atomic_int descriptorCount{3}, fillFault{0}, eventFault{0}, readFault{0}, writeFault{0}, pollFault{0};
	std::atomic_bool eof{false}, allowRead{true}, enteredRead{false}, allowOutput{true};
	std::atomic_int maximumWrite{3};
	std::atomic_int statusCalls{0}, statusFault{0}, statusInterruptions{0}, afterReadStatusFault{0};
	std::atomic<std::size_t> reportedOverruns{0}, sampledOverruns{0}, afterReadOverruns{0};
	std::atomic_bool writesOnOwner{false};
	bool nonblocking = false;
	struct Call
	{
		Transport& t;
		explicit Call(Transport& transport) : t(transport) { if (++t.inCall != 1) { ++t.badAccess; } }
		~Call() { --t.inCall; }
	};
	void check(void* handle) { if (!handle || !static_cast<Handle*>(handle)->live) { ++badAccess; } }
	void close(void* handle) { Call call(*this); check(handle); static_cast<Handle*>(handle)->live = false; ++closes; }
	int count(void* handle) { Call call(*this); check(handle); return descriptorCount.load(); }
	int descriptors(void* handle, pollfd* result, int capacity, int wanted = 0)
	{
		Call call(*this); check(handle); const int count = descriptorCount.load();
		if (count < 1 || count > capacity) { ++badAccess; return -EINVAL; }
		if (!wanted) { wanted = handle == &output ? POLLOUT : POLLIN; }
		for (int index = 0; index < count; ++index) { result[index] = {incoming.descriptor(), static_cast<short>(wanted), 0}; }
		if (fillFault) { return backend == "alsa" ? -EIO : 0; }
		return count;
	}
	int events(void* handle, pollfd* descriptors)
	{
		Call call(*this); check(handle);
		if (const auto fault = eventFault.load()) { return fault; }
		return descriptors[0].revents;
	}
	ssize_t read(void* handle, void* destination, std::size_t size)
	{
		Call call(*this); check(handle); ++reads; enteredRead = true;
		QElapsedTimer limit; limit.start();
		while (!allowRead.load() && limit.elapsed() < 2000) { std::this_thread::yield(); }
		if (!allowRead) { ++badAccess; }
		const auto fault = readFault.exchange(0);
		if (fault == 1) { return backend == "alsa" ? -EAGAIN : 0; }
		if (fault == 2) { if (backend == "sndio") { eof = true; } return -ENODEV; }
		if (fault == 3) { return static_cast<ssize_t>(size + 1); }
		if (fault == 4) { throw std::runtime_error("native raw MIDI reader fixture"); }
		if (fault == 5) { return -EINTR; }
		const auto lock = std::lock_guard{mutex};
		const auto count = std::min(size, pending.size());
		for (std::size_t index = 0; index < count; ++index) { static_cast<unsigned char*>(destination)[index] = pending.front(); pending.pop_front(); }
		if (pending.empty()) { unsigned char bytes[32]; while (::read(incoming.descriptor(), bytes, sizeof(bytes)) > 0) { } }
		if (const auto lost = afterReadOverruns.exchange(0)) { reportedOverruns = lost; }
		if (const auto fault = afterReadStatusFault.exchange(0)) { statusFault = fault; }
		return static_cast<ssize_t>(count);
	}
	ssize_t write(void* handle, const void* bytes, std::size_t size)
	{
		Call call(*this); check(handle); ++writes;
		if (QThread::currentThread() == Engine::audioEngine()->thread()) { writesOnOwner = true; }
		if (!allowOutput.load()) { return backend == "alsa" ? -EAGAIN : 0; }
		const auto fault = writeFault.load();
		if (fault == 1) { allowOutput = false; return backend == "alsa" ? -EAGAIN : 0; }
		if (fault == 2) { if (backend == "sndio") { eof = true; } return -EIO; }
		if (fault == 3) { return static_cast<ssize_t>(size + 1); }
		if (fault == 4 && writes.load() % 2 == 1) { return -EINTR; }
		if (fault == 5 && writes.load() == 2) { allowOutput = false; return backend == "alsa" ? -EAGAIN : 0; }
		if (fault == 6) { allowOutput = false; return -EINTR; }
		const auto lock = std::lock_guard{mutex}; const auto data = static_cast<const unsigned char*>(bytes);
		const auto count = std::min(size, static_cast<std::size_t>(fault == 5 ? 1 : maximumWrite.load()));
		written.insert(written.end(), data, data + count); return static_cast<ssize_t>(count);
	}
	static int poll(pollfd* descriptors, nfds_t count, int timeout)
	{
		auto& t = *current; ++t.polls;
		if (const auto error = t.pollFault.exchange(0)) { errno = error; return -1; }
		std::vector<short> wanted; wanted.reserve(count); bool writable = false;
		for (nfds_t index = 0; index < count; ++index)
		{
			wanted.push_back(descriptors[index].events); writable = writable || ((descriptors[index].events & POLLOUT) && t.allowOutput.load());
			descriptors[index].events &= ~POLLOUT;
		}
		const auto ready = ::poll(descriptors, count, writable ? 0 : timeout);
		int result = 0;
		for (nfds_t index = 0; index < count; ++index)
		{
			descriptors[index].events = wanted[index]; if ((wanted[index] & POLLOUT) && t.allowOutput.load()) { descriptors[index].revents |= POLLOUT; }
			if (descriptors[index].revents) { ++result; }
		}
		return ready < 0 ? ready : result;
	}
	void feed(std::initializer_list<unsigned char> bytes)
	{ { const auto lock = std::lock_guard{mutex}; pending.insert(pending.end(), bytes); } incoming.signal(); }
	void resumeOutput() { writeFault = 0; allowOutput = true; incoming.signal(); }
	int expectedCloses() const { return backend == "alsa" ? 2 : 1; }
#ifdef LMMS_HAVE_ALSA
	detail::AlsaRawMidiApi alsa;
#endif
#ifdef LMMS_HAVE_SNDIO
	detail::SndioMidiApi sndio;
#endif
	explicit Transport(QString name) : backend(std::move(name))
	{
		current = this; if (!incoming.open()) { throw std::runtime_error("fixture pipe failed"); }
#ifdef LMMS_HAVE_ALSA
		alsa.open = [](snd_rawmidi_t** input, snd_rawmidi_t** output, const char*, int mode) {
			auto& t = *current; ++t.openCalls; t.nonblocking = (mode & SND_RAWMIDI_NONBLOCK) != 0;
			if (t.startupFault == "open") { return -EBUSY; }
			t.input.live = t.output.live = true; *input = reinterpret_cast<snd_rawmidi_t*>(&t.input); *output = reinterpret_cast<snd_rawmidi_t*>(&t.output);
			return t.startupFault == "partial-open" ? -EIO : 0;
		};
		alsa.close = [](snd_rawmidi_t* handle) { current->close(handle); return 0; };
		alsa.descriptorCount = [](snd_rawmidi_t* handle) { return current->count(handle); };
		alsa.descriptors = [](snd_rawmidi_t* handle, pollfd* descriptors, unsigned int count) { return current->descriptors(handle, descriptors, static_cast<int>(count)); };
		alsa.events = [](snd_rawmidi_t* handle, pollfd* descriptors, unsigned int, unsigned short* events) {
			const auto result = current->events(handle, descriptors); if (result < 0) { return result; } *events = result; return 0;
		};
		alsa.read = [](snd_rawmidi_t* handle, void* bytes, std::size_t size) { return current->read(handle, bytes, size); };
		alsa.status = [](snd_rawmidi_t* handle, snd_rawmidi_status_t* status) {
			auto& t = *current; Call call(t); t.check(handle); ++t.statusCalls; if (!status || handle != reinterpret_cast<snd_rawmidi_t*>(&t.input)) { ++t.badAccess; }
			if (t.statusInterruptions > 0) { --t.statusInterruptions; return -EINTR; }
			const auto fault = t.statusFault.load(); if (fault == 2) { throw std::runtime_error("raw status fixture"); }
			if (fault && fault != 3) { return fault; }
			t.sampledOverruns = t.reportedOverruns.exchange(0); return 0;
		};
		alsa.statusXruns = [](const snd_rawmidi_status_t* status) -> std::size_t {
			auto& t = *current; Call call(t); if (!status) { ++t.badAccess; }
			if (t.statusFault == 3) { throw std::runtime_error("raw status counter fixture"); } return t.sampledOverruns.load();
		};
		alsa.write = [](snd_rawmidi_t* handle, const void* bytes, std::size_t size) { return current->write(handle, bytes, size); };
		alsa.poll = &poll;
#endif
#ifdef LMMS_HAVE_SNDIO
		sndio.open = [](const char*, unsigned int mode, int nonblocking) -> mio_hdl* {
			auto& t = *current; ++t.openCalls; t.nonblocking = nonblocking && mode == (MIO_IN | MIO_OUT);
			if (t.startupFault == "open") { return nullptr; } t.input.live = true; return reinterpret_cast<mio_hdl*>(&t.input);
		};
		sndio.close = [](mio_hdl* handle) { current->close(handle); };
		sndio.descriptorCount = [](mio_hdl* handle) { return current->count(handle); };
		sndio.descriptors = [](mio_hdl* handle, pollfd* descriptors, int events) { if (!(events & POLLIN) || (events & ~(POLLIN | POLLOUT))) { ++current->badAccess; } return current->descriptors(handle, descriptors, current->descriptorCount, events); };
		sndio.events = [](mio_hdl* handle, pollfd* descriptors) { return current->events(handle, descriptors); };
		sndio.read = [](mio_hdl* handle, void* bytes, std::size_t size) { const auto count = current->read(handle, bytes, size); return count < 0 ? std::size_t{0} : static_cast<std::size_t>(count); };
		sndio.write = [](mio_hdl* handle, const void* bytes, std::size_t size) { const auto count = current->write(handle, bytes, size); return count < 0 ? std::size_t{0} : static_cast<std::size_t>(count); };
		sndio.eof = [](mio_hdl* handle) { Call call(*current); current->check(handle); return current->eof.load() ? 1 : 0; };
		sndio.poll = &poll;
#endif
	}
	std::unique_ptr<MidiClient> create()
	{
#ifdef LMMS_HAVE_ALSA
		if (backend == "alsa") { return RawMidiIoTestAccess::create(alsa); }
#endif
#ifdef LMMS_HAVE_SNDIO
		if (backend == "sndio") { return RawMidiIoTestAccess::create(sndio); }
#endif
		throw std::runtime_error("Unknown compiled raw MIDI fixture");
	}
	QString nativeName() const
	{
#ifdef LMMS_HAVE_ALSA
		if (backend == "alsa") { return MidiAlsaRaw::name(); }
#endif
#ifdef LMMS_HAVE_SNDIO
		if (backend == "sndio") { return MidiSndio::name(); }
#endif
		return {};
	}
};
struct Processor : MidiEventProcessor
{
	std::vector<MidiEvent> events; bool wrongThread = false, cleanupRequired = false;
	void processInEvent(const MidiEvent& event, const TimePos&, f_cnt_t) override
	{ wrongThread = wrongThread || QThread::currentThread() != Engine::audioEngine()->thread(); events.push_back(event); }
	bool requiresNoteCleanup() const override { return cleanupRequired; }
	int notes() const { return static_cast<int>(std::count_if(events.begin(), events.end(), [](const auto& e) { return e.type() == MidiNoteOn; })); }
	int cleanups() const { return static_cast<int>(std::count_if(events.begin(), events.end(), [](const auto& e) { return e.type() == MidiControlChange && e.controllerNumber() == MidiControllerAllSoundOff; })); }
};
}

class RawMidiIoTest : public QObject
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

	void opensNonblockingAndJoinsIdleReaderBeforeClosingEveryHandle_data()
	{
		QTest::addColumn<QString>("backend"); QTest::addColumn<int>("count");
		for (const auto& backend : backends()) { for (const int count : {1, 3, 64}) { QTest::newRow(qPrintable(QString("%1-%2").arg(backend).arg(count))) << backend << count; } }
	}
	void opensNonblockingAndJoinsIdleReaderBeforeClosingEveryHandle()
	{
		QFETCH(QString, backend); QFETCH(int, count); Transport t(backend); t.descriptorCount = count; auto client = t.create();
		QVERIFY(t.nonblocking); QTRY_VERIFY(t.polls.load() > 0); const auto status = client->inputBackendStatus(); QVERIFY(status.observed && status.active); QCOMPARE(status.nativeErrorCode, 0);
		QCOMPARE(status.overrunObservationAvailable, backend == "alsa"); QCOMPARE(status.reportedOverrunBytes, std::uint64_t{0});
		QElapsedTimer elapsed; elapsed.start(); client.reset(); QVERIFY(elapsed.elapsed() < 1000); QCOMPARE(t.closes.load(), t.expectedCloses()); QCOMPARE(t.badAccess.load(), 0);
	}

	void failedConstructionRetiresPartialNativeResources_data()
	{
		QTest::addColumn<QString>("backend"); QTest::addColumn<QString>("fault");
		for (const auto& backend : backends())
		{
			QStringList faults{"open", "negative-count", "zero-count", "oversized-count", "fill"}; faults.append(backend == "alsa" ? "partial-open" : "eof");
			for (const auto& fault : faults) { QTest::newRow(qPrintable(backend + "-" + fault)) << backend << fault; }
		}
	}
	void failedConstructionRetiresPartialNativeResources()
	{
		QFETCH(QString, backend); QFETCH(QString, fault); Transport t(backend); t.startupFault = fault;
		if (fault == "negative-count") { t.descriptorCount = -EIO; } if (fault == "zero-count") { t.descriptorCount = 0; }
		if (fault == "oversized-count") { t.descriptorCount = 65; } if (fault == "fill") { t.fillFault = 1; } if (fault == "eof") { t.eof = true; }
		auto client = t.create(); const auto status = client->inputBackendStatus(); QVERIFY(status.observed && !status.active); QVERIFY(status.nativeErrorCode != 0);
		QCOMPARE(t.closes.load(), fault == "open" ? 0 : t.expectedCloses()); client.reset(); QCOMPARE(t.closes.load(), fault == "open" ? 0 : t.expectedCloses()); QCOMPARE(t.badAccess.load(), 0);
	}

	void fragmentedNativeInputIsDeliveredOnlyOnTheModelOwner_data() { backendRows(); }
	void fragmentedNativeInputIsDeliveredOnlyOnTheModelOwner()
	{
		QFETCH(QString, backend); Transport t(backend); auto client = t.create(); Processor processor; MidiPort port("input", client.get(), &processor, nullptr, MidiPort::Mode::Input);
		t.feed({0x92, 60}); QTRY_VERIFY(t.reads.load() > 0); pump(); QVERIFY(processor.events.empty());
		t.feed({100, 0xf8, 61, 101}); QTRY_COMPARE(processor.events.size(), std::size_t{2}); QVERIFY(!processor.wrongThread);
		QCOMPARE(processor.events[0].type(), MidiNoteOn); QCOMPARE(processor.events[0].channel(), 2); QCOMPARE(processor.events[0].param(0), 60); QCOMPARE(processor.events[1].param(0), 61);
		QVERIFY(client->inputBackendStatus().active); QCOMPARE(t.badAccess.load(), 0);
	}

	void interruptedPollsAndEmptyReadsKeepTheNativeReaderAlive_data()
	{
		QTest::addColumn<QString>("backend"); QTest::addColumn<QString>("fault");
		for (const auto& backend : backends())
		{
			QStringList faults{"empty-read", "poll-interrupt"}; if (backend == "alsa") { faults.append("read-interrupt"); }
			for (const auto& fault : faults) { QTest::newRow(qPrintable(backend + "-" + fault)) << backend << fault; }
		}
	}
	void interruptedPollsAndEmptyReadsKeepTheNativeReaderAlive()
	{
		QFETCH(QString, backend); QFETCH(QString, fault); Transport t(backend); if (fault == "poll-interrupt") { t.pollFault = EINTR; }
		auto client = t.create(); Processor processor; MidiPort port("transient", client.get(), &processor, nullptr, MidiPort::Mode::Input);
		if (fault == "empty-read") { t.readFault = 1; } if (fault == "read-interrupt") { t.readFault = 5; }
		t.feed({0x90, 60, 100}); QTRY_COMPARE(processor.events.size(), std::size_t{1}); QVERIFY(client->inputBackendStatus().active); QCOMPARE(t.badAccess.load(), 0);
	}

	void terminalNativeErrorsLatchInputAndPreventFalseRecovery_data()
	{
		QTest::addColumn<QString>("backend"); QTest::addColumn<QString>("fault");
		for (const auto& backend : backends())
		{
			QStringList faults{"read-error", "oversized-read", "read-exception", "poll-error", "hangup", "invalid-poll", "zero-descriptors", "large-descriptors", "fill-error"};
			faults.append(backend == "alsa" ? "event-error" : "eof");
			for (const auto& fault : faults) { QTest::newRow(qPrintable(backend + "-" + fault)) << backend << fault; }
		}
	}
	void terminalNativeErrorsLatchInputAndPreventFalseRecovery()
	{
		QFETCH(QString, backend); QFETCH(QString, fault); Transport t(backend); auto client = t.create();
		{
			Processor processor; MidiPort port("failed", client.get(), &processor, nullptr, MidiPort::Mode::Input); QTRY_VERIFY(t.polls.load() > 0);
			if (fault == "read-error") { t.readFault = 2; } if (fault == "oversized-read") { t.readFault = 3; } if (fault == "read-exception") { t.readFault = 4; }
			if (fault == "poll-error") { t.pollFault = EIO; } if (fault == "hangup") { t.eventFault = POLLHUP; } if (fault == "invalid-poll") { t.eventFault = POLLNVAL; }
			if (fault == "zero-descriptors") { t.descriptorCount = 0; } if (fault == "large-descriptors") { t.descriptorCount = 65; }
			if (fault == "fill-error") { t.fillFault = 1; } if (fault == "event-error") { t.eventFault = -EIO; } if (fault == "eof") { t.eof = true; }
			t.feed({}); QTRY_VERIFY(!dynamic_cast<QThread*>(client.get())->isRunning()); pump();
			const auto status = client->inputBackendStatus(); QVERIFY(status.observed && !status.active); QVERIFY(status.nativeErrorCode != 0);
			QVERIFY(client->inputDeliveryStatus(&port).failed); QString error; bool changed = false; QVERIFY(!port.resetInputDelivery(error, changed)); QVERIFY(!changed); QVERIFY(!error.isEmpty());
		}
		client.reset(); QCOMPARE(t.closes.load(), t.expectedCloses()); QCOMPARE(t.badAccess.load(), 0);
	}

	void outputBackpressureAndFatalErrorsAreExplicit_data()
	{
		QTest::addColumn<QString>("backend"); QTest::addColumn<int>("fault");
		for (const auto& backend : backends()) { for (const int fault : {1, 2, 3, 5}) { QTest::newRow(qPrintable(QString("%1-%2").arg(backend).arg(fault))) << backend << fault; } }
	}
	void outputBackpressureAndFatalErrorsAreExplicit()
	{
		QFETCH(QString, backend); QFETCH(int, fault); Transport t(backend); auto client = t.create(); Processor processor;
		MidiPort port("output", client.get(), &processor, nullptr, MidiPort::Mode::Duplex); QTRY_COMPARE(client->outputBackendStatus().pendingEvents, 0u); t.writeFault = fault; t.writes = 0;
		{ const auto lock = std::lock_guard{t.mutex}; t.written.clear(); }
		port.processOutEvent(MidiEvent(MidiControlChange, 0, 7, 90));
		if (fault == 1 || fault == 5)
		{
			QTRY_VERIFY(!t.allowOutput.load()); QCOMPARE(client->outputBackendStatus().pendingEvents, 1u); QVERIFY(client->outputBackendStatus().active);
			QVERIFY(client->inputBackendStatus().active); QVERIFY(!client->inputDeliveryStatus(&port).failed);
			{ const auto lock = std::lock_guard{t.mutex}; QCOMPARE(t.written.size(), fault == 5 ? std::size_t{1} : std::size_t{0}); }
			port.processOutEvent(MidiEvent(MidiProgramChange, 0, 4, 0)); t.resumeOutput(); QTRY_COMPARE(client->outputBackendStatus().pendingEvents, 0u);
			const auto lock = std::lock_guard{t.mutex}; QCOMPARE(t.written, (std::vector<unsigned char>{0xb0,7,90,0xc0,4}));
		}
		else
		{
			QTRY_VERIFY(!client->inputBackendStatus().active); QVERIFY(client->inputDeliveryStatus(&port).failed); QVERIFY(client->outputBackendStatus().nativeErrorCode != 0);
			QVERIFY_EXCEPTION_THROWN(port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)), std::runtime_error);
		}
		QCOMPARE(t.badAccess.load(), 0);
	}

#ifdef LMMS_HAVE_ALSA
	void alsaStartupAndIdleStatusLossCannotReportAHealthyClient_data()
	{ QTest::addColumn<bool>("startup"); QTest::newRow("startup") << true; QTest::newRow("idle-poll") << false; }
	void alsaStartupAndIdleStatusLossCannotReportAHealthyClient()
	{
		QFETCH(bool, startup); Transport t("alsa"); if (startup) { t.reportedOverruns = 5; } auto client = t.create();
		if (!startup) { QTRY_VERIFY(t.polls.load() > 0); QVERIFY(client->inputBackendStatus().active); t.reportedOverruns = 5; }
		QTRY_VERIFY(!dynamic_cast<QThread*>(client.get())->isRunning()); const auto status = client->inputBackendStatus();
		QVERIFY(!status.active); QVERIFY(status.overrunObservationAvailable); QCOMPARE(status.nativeErrorCode, -EOVERFLOW); QCOMPARE(status.reportedOverrunBytes, std::uint64_t{5});
		QCOMPARE(t.reads.load(), 0); if (startup) { QCOMPARE(t.polls.load(), 0); QCOMPARE(t.closes.load(), 2); }
		client.reset(); QCOMPARE(t.closes.load(), 2); QCOMPARE(t.badAccess.load(), 0);
	}

	void alsaInputOverrunsStopFramingAndRemainObservableThroughRecovery_data()
	{
		QTest::addColumn<bool>("duringRead"); QTest::addColumn<qulonglong>("lost");
		for (const bool duringRead : {false, true}) { for (const auto lost : {std::size_t{1}, std::size_t{17}, std::numeric_limits<std::size_t>::max()})
		{ QTest::newRow(qPrintable(QString("during-read-%1-bytes-%2").arg(duringRead).arg(static_cast<qulonglong>(lost)))) << duringRead << static_cast<qulonglong>(lost); } }
	}
	void alsaInputOverrunsStopFramingAndRemainObservableThroughRecovery()
	{
		QFETCH(bool, duringRead); QFETCH(qulonglong, lost); Transport t("alsa"); const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		StudioMidiBackendSelectionTestAccess::select(t.nativeName(), [&] { return t.create(); }); const auto source = engine->midiClient(); Processor processor; processor.cleanupRequired = true;
		MidiPort port("lost-input", source, &processor, nullptr, MidiPort::Mode::Duplex);
		port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); port.processOutEvent(MidiEvent(MidiControlChange, 0, MidiControllerSustain, 127)); QTRY_COMPARE(source->outputBackendStatus().pendingEvents, 0u);
		t.feed({0x90, 40, 100}); QTRY_COMPARE(processor.notes(), 1); const auto reads = t.reads.load();
		if (duringRead) { t.afterReadOverruns = static_cast<std::size_t>(lost); } else { t.reportedOverruns = static_cast<std::size_t>(lost); }
		t.feed({41, 101}); QTRY_VERIFY(!dynamic_cast<QThread*>(source)->isRunning()); pump();
		const auto status = source->inputBackendStatus(); QVERIFY(status.observed && !status.active); QVERIFY(status.overrunObservationAvailable);
		QCOMPARE(status.nativeErrorCode, -EOVERFLOW); QCOMPARE(status.reportedOverrunBytes, static_cast<std::uint64_t>(lost)); QCOMPARE(t.reads.load(), reads + (duringRead ? 1 : 0));
		QCOMPARE(processor.notes(), 1); QCOMPARE(processor.cleanups(), 16); QVERIFY(!processor.wrongThread); QVERIFY(source->inputDeliveryStatus(&port).failed);
		QVERIFY(source->inputDeliveryStatus(&port).requiresBackendObservation); QString error; bool changed = false; QVERIFY(!port.resetInputDelivery(error, changed)); QVERIFY(!changed);
		const auto inspected = StudioCapabilities::execute("transport.inspect", {}); QVERIFY(inspected.value("ok").toBool());
		const auto input = inspected.value("result").toObject().value("midiBackend").toObject().value("inputBackend").toObject();
		QVERIFY(input.value("overrunObservationAvailable").toBool()); QCOMPARE(input.value("reportedOverrunBytes").toString(), QString::number(lost)); QCOMPARE(input.value("nativeErrorCode").toInt(), -EOVERFLOW);
		QVERIFY(source->outputBackendStatus().abandonmentEligible); QCOMPARE(port.trackedOutputNotes(), 1u);
		QVERIFY_EXCEPTION_THROWN(engine->selectMidiBackend(MidiDummy::name()), std::runtime_error); QCOMPARE(port.trackedOutputNotes(), 1u);
		const auto replaced = engine->selectMidiBackend(MidiDummy::name(), true); QVERIFY(replaced.outputAbandonment.abandoned); QCOMPARE(replaced.outputAbandonment.backend.nativeErrorCode, -EOVERFLOW);
		QCOMPARE(replaced.outputAbandonment.trackedNotes, std::uint64_t{1}); QCOMPARE(replaced.outputAbandonment.pedalHolds, std::uint64_t{1}); QVERIFY(replaced.audioRestored);
		QVERIFY(port.resetInputDelivery(error, changed)); QVERIFY(changed); QVERIFY(!engine->midiClient()->inputBackendStatus().overrunObservationAvailable);
		QCOMPARE(t.closes.load(), 2); QCOMPARE(t.badAccess.load(), 0);
	}

	void alsaStatusFailureNeverParsesAnUnverifiedRead_data()
	{
		QTest::addColumn<bool>("duringRead"); QTest::addColumn<int>("fault");
		for (const bool duringRead : {false, true}) { for (const auto fault : {-EIO, -EINVAL, 1, 2, 3})
		{ QTest::newRow(qPrintable(QString("during-read-%1-fault-%2").arg(duringRead).arg(fault))) << duringRead << fault; } }
	}
	void alsaStatusFailureNeverParsesAnUnverifiedRead()
	{
		QFETCH(bool, duringRead); QFETCH(int, fault); Transport t("alsa"); auto client = t.create(); Processor processor; processor.cleanupRequired = true;
		MidiPort port("unknown-loss", client.get(), &processor, nullptr, MidiPort::Mode::Input); t.feed({0x90, 40, 100}); QTRY_COMPARE(processor.notes(), 1); const auto reads = t.reads.load();
		if (duringRead) { t.afterReadStatusFault = fault; } else { t.statusFault = fault; }
		t.feed({41, 101}); QTRY_VERIFY(!dynamic_cast<QThread*>(client.get())->isRunning()); pump(); const auto status = client->inputBackendStatus();
		QVERIFY(!status.active); QVERIFY(!status.overrunObservationAvailable); QCOMPARE(status.reportedOverrunBytes, std::uint64_t{0}); QCOMPARE(status.nativeErrorCode, fault < 0 ? fault : -EIO);
		QCOMPARE(t.reads.load(), reads + (duringRead ? 1 : 0)); QCOMPARE(processor.notes(), 1); QCOMPARE(processor.cleanups(), 16); QVERIFY(!processor.wrongThread);
		QVERIFY(client->inputDeliveryStatus(&port).failed); QCOMPARE(t.badAccess.load(), 0);
	}

	void alsaStatusInterruptionsAreBoundedWithoutInventingLostBytes_data()
	{ QTest::addColumn<int>("interruptions"); for (const int count : {0, 1, 3, 4}) { QTest::newRow(qPrintable(QString::number(count))) << count; } }
	void alsaStatusInterruptionsAreBoundedWithoutInventingLostBytes()
	{
		QFETCH(int, interruptions); Transport t("alsa"); t.statusInterruptions = interruptions; auto client = t.create();
		if (interruptions == 4)
		{
			QTRY_VERIFY(!dynamic_cast<QThread*>(client.get())->isRunning()); const auto status = client->inputBackendStatus();
			QCOMPARE(t.statusCalls.load(), 4); QVERIFY(!status.active); QVERIFY(!status.overrunObservationAvailable); QCOMPARE(status.nativeErrorCode, -EINTR); QCOMPARE(status.reportedOverrunBytes, std::uint64_t{0});
		}
		else
		{
			QTRY_VERIFY(t.polls.load() > 0); const auto status = client->inputBackendStatus(); QVERIFY(status.active && status.overrunObservationAvailable);
			QVERIFY(t.statusCalls.load() >= interruptions + 1); QCOMPARE(status.nativeErrorCode, 0); QCOMPARE(status.reportedOverrunBytes, std::uint64_t{0});
		}
		client.reset(); QCOMPARE(t.closes.load(), 2); QCOMPARE(t.badAccess.load(), 0);
	}

	void interruptedAlsaWritesRetainPacketsUntilNativeProgress_data()
	{ QTest::addColumn<bool>("persistent"); QTest::newRow("transient") << false; QTest::newRow("persistent") << true; }
	void interruptedAlsaWritesRetainPacketsUntilNativeProgress()
	{
		QFETCH(bool, persistent); Transport t("alsa"); auto client = t.create(); Processor processor; MidiPort port("interrupted", client.get(), &processor, nullptr, MidiPort::Mode::Output);
		QTRY_COMPARE(client->outputBackendStatus().pendingEvents, 0u); t.writeFault = persistent ? 6 : 4; t.writes = 0; { const auto lock = std::lock_guard{t.mutex}; t.written.clear(); }
		port.processOutEvent(MidiEvent(MidiControlChange, 0, 7, 99));
		if (persistent) { QTRY_VERIFY(!t.allowOutput.load()); QCOMPARE(t.writes.load(), 1); QCOMPARE(client->outputBackendStatus().pendingEvents, 1u); t.resumeOutput(); }
		QTRY_COMPARE(client->outputBackendStatus().pendingEvents, 0u);
		QVERIFY(client->inputBackendStatus().active); QCOMPARE(t.badAccess.load(), 0);
		const auto lock = std::lock_guard{t.mutex}; QCOMPARE(t.written, (std::vector<unsigned char>{0xb0,7,99}));
	}
#endif

	void independentOutputPortsNeverInterleaveTheirPackets_data() { backendRows(); }
	void independentOutputPortsNeverInterleaveTheirPackets()
	{
		QFETCH(QString, backend); Transport t(backend); auto client = t.create(); Processor processor;
		MidiPort a("a", client.get(), &processor, nullptr, MidiPort::Mode::Output), b("b", client.get(), &processor, nullptr, MidiPort::Mode::Output);
		a.setOutputChannel(0); b.setOutputChannel(0); QTRY_COMPARE(client->outputBackendStatus().pendingEvents, 0u); QTRY_COMPARE(client->outputBackendStatus().cleanupPendingChannels, 0u);
		{ const auto lock = std::lock_guard{t.mutex}; t.written.clear(); }
		std::atomic_bool failed{false}; const auto send = [&](MidiPort& port, int channel, int control) { try { for (int i = 0; i < 128; ++i) { port.processOutEvent(MidiEvent(MidiControlChange, channel, control, i)); } } catch (...) { failed = true; } };
		std::thread one([&] { send(a, 0, 7); }), two([&] { send(b, 1, 10); }); one.join(); two.join(); QVERIFY(!failed.load());
		QTRY_COMPARE(client->outputBackendStatus().pendingEvents, 0u);
		const auto lock = std::lock_guard{t.mutex}; QCOMPARE(t.written.size(), std::size_t{768});
		for (std::size_t i = 0; i < t.written.size(); i += 3) { QVERIFY(t.written[i] == 0xb0 || t.written[i] == 0xb1); QCOMPARE(t.written[i + 1], static_cast<unsigned char>(t.written[i] == 0xb0 ? 7 : 10)); QVERIFY(t.written[i + 2] < 128); }
		QCOMPARE(t.badAccess.load(), 0);
		QVERIFY(!t.writesOnOwner.load());
	}

	void cancelledQueuedNotesAreNeverSubmitted_data() { backendRows(); }
	void cancelledQueuedNotesAreNeverSubmitted()
	{
		QFETCH(QString, backend); Transport t(backend); auto client = t.create(); Processor processor; MidiPort port("cancel", client.get(), &processor, nullptr, MidiPort::Mode::Output);
		QTRY_COMPARE(client->outputBackendStatus().pendingEvents, 0u); { const auto lock = std::lock_guard{t.mutex}; t.written.clear(); }
		t.allowOutput = false; port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); port.processOutEvent(MidiEvent(MidiNoteOn, 0, 62, 100));
		QCOMPARE(client->outputBackendStatus().pendingEvents, 2u); port.releaseOutput(); QCOMPARE(client->outputBackendStatus().pendingEvents, 0u);
		t.resumeOutput(); QTRY_COMPARE(client->outputBackendStatus().cleanupPendingChannels, 0u);
		const auto lock = std::lock_guard{t.mutex}; QCOMPARE(t.written, (std::vector<unsigned char>{0xb0,120,0,0xb0,64,0,0xb0,66,0,0xb0,123,0}));
		QVERIFY(!t.writesOnOwner.load());
	}

	void retiringPartialOutputCompletesItsFramingBeforeCleanup_data() { backendRows(); }
	void retiringPartialOutputCompletesItsFramingBeforeCleanup()
	{
		QFETCH(QString, backend); Transport t(backend); auto client = t.create(); Processor processor;
		auto port = std::make_unique<MidiPort>("partial", client.get(), &processor, nullptr, MidiPort::Mode::Output);
		QTRY_COMPARE(client->outputBackendStatus().pendingEvents, 0u); { const auto lock = std::lock_guard{t.mutex}; t.written.clear(); }
		t.writeFault = 5; t.writes = 0; port->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); QTRY_VERIFY(!t.allowOutput.load());
		port->processOutEvent(MidiEvent(MidiNoteOn, 0, 62, 100)); port.reset(); QCOMPARE(client->outputBackendStatus().pendingEvents, 1u); QCOMPARE(client->outputBackendStatus().cleanupPendingChannels, 1u);
		t.resumeOutput(); QTRY_COMPARE(client->outputBackendStatus().pendingEvents, 0u); QTRY_COMPARE(client->outputBackendStatus().cleanupPendingChannels, 0u);
		const auto lock = std::lock_guard{t.mutex}; QCOMPARE(t.written, (std::vector<unsigned char>{0x90,60,100,0xb0,120,0,0xb0,64,0,0xb0,66,0,0xb0,123,0}));
		QCOMPARE(t.badAccess.load(), 0);
	}

	void engineTransferWaitsForCleanupAndCanRetryAfterRefusal_data()
	{
		QTest::addColumn<QString>("backend"); QTest::addColumn<bool>("cancel");
		for (const auto& backend : backends()) { for (const bool cancel : {false, true}) { QTest::newRow(qPrintable(QString("%1-cancel-%2").arg(backend).arg(cancel))) << backend << cancel; } }
	}
	void engineTransferWaitsForCleanupAndCanRetryAfterRefusal()
	{
		QFETCH(QString, backend); QFETCH(bool, cancel); Transport t(backend); const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		Processor processor; MidiPort port("transfer-output", engine->midiClient(), &processor, nullptr, MidiPort::Mode::Output);
		StudioMidiBackendSelectionTestAccess::select(t.nativeName(), [&] { return t.create(); }); const auto source = engine->midiClient();
		port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); QTRY_COMPARE(source->outputBackendStatus().pendingEvents, 0u); t.allowOutput = false;
		QVERIFY(source->outputBackendStatus().abandonmentSupported); QVERIFY(!source->outputBackendStatus().abandonmentEligible);
		QVERIFY_EXCEPTION_THROWN(engine->selectMidiBackend(MidiDummy::name(), true), std::runtime_error); QCOMPARE(engine->midiClient(), source); QCOMPARE(port.trackedOutputNotes(), 1u);
		{
			FileResourceIO::Scope work([&] { return cancel && source->outputBackendStatus().cleanupPendingChannels != 0; }, false);
			QVERIFY_EXCEPTION_THROWN(engine->selectMidiBackend(MidiDummy::name()), std::runtime_error);
		}
		QCOMPARE(port.client(), source); QCOMPARE(engine->midiClient(), source); QCOMPARE(t.closes.load(), 0); QVERIFY(!engine->hasStoredAudioDevice());
		QVERIFY(source->outputBackendStatus().cleanupPendingChannels != 0); t.resumeOutput(); QTRY_COMPARE(source->outputBackendStatus().cleanupPendingChannels, 0u);
		QVERIFY(engine->selectMidiBackend(MidiDummy::name()).audioRestored); QCOMPARE(port.client(), engine->midiClient()); QCOMPARE(t.closes.load(), t.expectedCloses()); QCOMPARE(t.badAccess.load(), 0);
	}

	void terminalOutputCanBeExplicitlyAbandonedAfterItsWorkerStops_data()
	{
		QTest::addColumn<QString>("backend"); QTest::addColumn<bool>("partial"); QTest::addColumn<bool>("retired");
		for (const auto& backend : backends()) { for (const bool partial : {false, true}) { for (const bool retired : {false, true})
		{ QTest::newRow(qPrintable(QString("%1-partial-%2-retired-%3").arg(backend).arg(partial).arg(retired))) << backend << partial << retired; } } }
	}
	void terminalOutputCanBeExplicitlyAbandonedAfterItsWorkerStops()
	{
		QFETCH(QString, backend); QFETCH(bool, partial); QFETCH(bool, retired); Transport t(backend); const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		StudioMidiBackendSelectionTestAccess::select(t.nativeName(), [&] { return t.create(); }); const auto source = engine->midiClient(); Processor processor;
		auto port = std::make_unique<MidiPort>("terminal-output", source, &processor, nullptr, MidiPort::Mode::Output); QTRY_COMPARE(source->outputBackendStatus().pendingEvents, 0u);
		if (partial) { t.writeFault = 5; t.writes = 0; }
		port->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100));
		if (partial)
		{
			QTRY_VERIFY(!t.allowOutput.load()); port->processOutEvent(MidiEvent(MidiNoteOn, 0, 62, 100));
			t.writeFault = 2; t.allowOutput = true; t.incoming.signal();
		}
		else
		{
			port->processOutEvent(MidiEvent(MidiControlChange, 0, MidiControllerSustain, 127)); QTRY_COMPARE(source->outputBackendStatus().pendingEvents, 0u);
			t.readFault = 2; t.feed({});
		}
		QTRY_VERIFY(!dynamic_cast<QThread*>(source)->isRunning()); QVERIFY(source->outputBackendStatus().abandonmentEligible);
		if (retired) { port.reset(); }
		const auto pending = source->outputBackendStatus(); QVERIFY(pending.cleanupPendingChannels != 0);
		QVERIFY_EXCEPTION_THROWN(engine->selectMidiBackend(MidiDummy::name()), std::runtime_error); QCOMPARE(engine->midiClient(), source); QCOMPARE(t.closes.load(), 0);
		std::vector<unsigned char> written; { const auto lock = std::lock_guard{t.mutex}; written = t.written; }
		const auto observed = StudioCapabilities::execute("transport.inspect", {}); QVERIFY(observed.value("ok").toBool());
		QVERIFY(observed.value("result").toObject().value("midiBackend").toObject().value("outputBackend").toObject().value("abandonmentEligible").toBool());
		const auto response = StudioCapabilities::execute("midi.backend.select", {{"name", MidiDummy::name()}, {"abandonFailedOutput", true}}); QVERIFY(response.value("ok").toBool());
		const auto result = response.value("result").toObject(), loss = result.value("outputAbandonment").toObject();
		QCOMPARE(result.value("previousBackend").toString(), t.nativeName());
		QVERIFY(result.value("committed").toBool()); QVERIFY(result.value("audioRestored").toBool()); QVERIFY(loss.value("abandoned").toBool()); QVERIFY(!loss.value("deviceCleanupVerified").toBool());
		QCOMPARE(loss.value("trackedNotes").toString(), QString::number(retired ? 0 : partial ? 2 : 1)); QCOMPARE(loss.value("pedalHolds").toString(), QString::number(!retired && !partial ? 1 : 0));
		QCOMPARE(loss.value("pendingEvents").toInt(), static_cast<int>(pending.pendingEvents)); QCOMPARE(loss.value("cleanupPendingChannels").toInt(), static_cast<int>(pending.cleanupPendingChannels));
		QCOMPARE(loss.value("nativeErrorCode").toInt(), pending.nativeErrorCode); QCOMPARE(t.closes.load(), t.expectedCloses()); QCOMPARE(t.badAccess.load(), 0);
		{ const auto lock = std::lock_guard{t.mutex}; QCOMPARE(t.written, written); }
		if (port) { QCOMPARE(port->client(), engine->midiClient()); QCOMPARE(port->trackedOutputNotes(), 0u); port->processOutEvent(MidiEvent(MidiNoteOn, 0, 67, 100)); QCOMPARE(port->trackedOutputNotes(), 1u); }
	}

	void retiredPortCleanupRemainsVisibleThroughTransportInspection_data() { backendRows(); }
	void retiredPortCleanupRemainsVisibleThroughTransportInspection()
	{
		QFETCH(QString, backend); Transport t(backend); const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		StudioMidiBackendSelectionTestAccess::select(t.nativeName(), [&] { return t.create(); }); Processor processor;
		{
			MidiPort port("retired-output", engine->midiClient(), &processor, nullptr, MidiPort::Mode::Output);
			port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); QTRY_COMPARE(engine->midiClient()->outputBackendStatus().pendingEvents, 0u);
			t.allowOutput = false;
		}
		const auto inspection = StudioCapabilities::execute("transport.inspect", {}); QVERIFY(inspection.value("ok").toBool());
		const auto output = inspection.value("result").toObject().value("midiBackend").toObject().value("outputBackend").toObject();
		QVERIFY(output.value("observed").toBool()); QVERIFY(!output.value("active").toBool()); QCOMPARE(output.value("queueLimit").toInt(), 1024);
		QCOMPARE(output.value("pendingEvents").toInt(), 0); QCOMPARE(output.value("cleanupPendingChannels").toInt(), 1);
		t.resumeOutput(); QTRY_COMPARE(engine->midiClient()->outputBackendStatus().cleanupPendingChannels, 0u);
		const auto recovered = StudioCapabilities::execute("transport.inspect", {}); QVERIFY(recovered.value("ok").toBool());
		QVERIFY(recovered.value("result").toObject().value("midiBackend").toObject().value("outputBackend").toObject().value("active").toBool());
		QVERIFY(engine->selectMidiBackend(MidiDummy::name()).audioRestored); QCOMPARE(t.closes.load(), t.expectedCloses());
	}

	void shutdownSubmitsReservedCleanupBeforeClosingNativeHandles_data() { backendRows(); }
	void shutdownSubmitsReservedCleanupBeforeClosingNativeHandles()
	{
		QFETCH(QString, backend); Transport t(backend); auto client = t.create(); Processor processor;
		{
			MidiPort port("shutdown", client.get(), &processor, nullptr, MidiPort::Mode::Output); QTRY_COMPARE(client->outputBackendStatus().pendingEvents, 0u);
			{ const auto lock = std::lock_guard{t.mutex}; t.written.clear(); }
			port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); QTRY_COMPARE(client->outputBackendStatus().pendingEvents, 0u);
			t.allowOutput = false;
		}
		t.resumeOutput(); client.reset(); QCOMPARE(t.closes.load(), t.expectedCloses()); QCOMPARE(t.badAccess.load(), 0);
		const auto lock = std::lock_guard{t.mutex}; QCOMPARE(t.written, (std::vector<unsigned char>{0x90,60,100,0xb0,120,0,0xb0,64,0,0xb0,66,0,0xb0,123,0}));
	}

	void shutdownWaitsForAnInFlightNativeReadBeforeClosing_data() { backendRows(); }
	void shutdownWaitsForAnInFlightNativeReadBeforeClosing()
	{
		QFETCH(QString, backend); Transport t(backend); auto client = t.create(); t.allowRead = false; t.feed({0x90, 60, 100}); QTRY_VERIFY(t.enteredRead.load());
		std::atomic_bool deleting{false}, closedEarly{false}; std::thread release([&] {
			while (!deleting.load()) { std::this_thread::yield(); } QThread::msleep(25); closedEarly = t.closes.load() != 0; t.allowRead = true;
		});
		deleting = true; client.reset(); release.join(); QVERIFY(!closedEarly.load()); QCOMPARE(t.closes.load(), t.expectedCloses()); QCOMPARE(t.badAccess.load(), 0);
	}

	void engineSelectionRetiresRawClientAndRevokesItsQueuedInput_data() { backendRows(); }
	void engineSelectionRetiresRawClientAndRevokesItsQueuedInput()
	{
		QFETCH(QString, backend); Transport t(backend); const auto destroy = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		Processor processor; MidiPort port("engine", engine->midiClient(), &processor, nullptr, MidiPort::Mode::Input);
		const auto selected = StudioMidiBackendSelectionTestAccess::select(t.nativeName(), [&] { return t.create(); }); QVERIFY(selected.audioRestored);
		QCOMPARE(port.client(), engine->midiClient()); QCOMPARE(engine->midiClientName(), t.nativeName()); QVERIFY(engine->midiClient()->inputBackendStatus().observed);
		QString error; bool changed = false; QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
		t.feed({0x90, 60, 100}); QElapsedTimer timeout; timeout.start();
		while (engine->midiClient()->inputDeliveryStatus(&port).pendingEvents == 0 && timeout.elapsed() < 2000) { std::this_thread::yield(); }
		QVERIFY(engine->midiClient()->inputDeliveryStatus(&port).pendingEvents > 0);
		const auto retired = engine->selectMidiBackend(MidiDummy::name()); QVERIFY(retired.audioRestored); QCOMPARE(t.closes.load(), t.expectedCloses()); pump(); QVERIFY(processor.events.empty());
		QCOMPARE(port.client(), engine->midiClient()); QVERIFY(engine->midiClient()->inputDeliveryStatus(&port).failed); QCOMPARE(t.badAccess.load(), 0);
	}

	void cancelledEngineCandidateClosesItsNativeResources_data() { backendRows(); }
	void cancelledEngineCandidateClosesItsNativeResources()
	{
		QFETCH(QString, backend); Transport t(backend); const auto source = Engine::audioEngine()->midiClient(); bool cancelled = false;
		FileResourceIO::Scope work([&] { return cancelled; }, false);
		QVERIFY_EXCEPTION_THROWN(StudioMidiBackendSelectionTestAccess::select(t.nativeName(), [&] { auto client = t.create(); cancelled = true; return client; }), std::runtime_error);
		QCOMPARE(Engine::audioEngine()->midiClient(), source); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice()); QCOMPARE(t.closes.load(), t.expectedCloses()); QCOMPARE(t.badAccess.load(), 0);
	}
};
QTEST_GUILESS_MAIN(RawMidiIoTest)
#include "RawMidiIoTest.moc"
