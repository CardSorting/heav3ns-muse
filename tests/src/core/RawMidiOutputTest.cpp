/* Native byte-stream packet ownership tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <stdexcept>
#include <thread>
#include <vector>
#include "MidiEventToByteSeq.h"
#include "../../../src/core/midi/RawMidiOutput.h"

using namespace lmms;
using lmms::detail::RawMidiOutput;
namespace
{
const MidiPort* source(const int& identity) { return reinterpret_cast<const MidiPort*>(&identity); }
std::vector<std::uint8_t> encode(const MidiEvent& event)
{
	std::array<std::uint8_t, 3> bytes{}; const auto count = writeToByteSeq(event, bytes.data(), bytes.size());
	return {bytes.begin(), bytes.begin() + count};
}
struct Wire
{
	std::vector<std::uint8_t> bytes;
	int maximum = 3, remainingCalls = 100000, error = 0;
	int write(const std::uint8_t* data, std::size_t size)
	{
		if (error) { return error; } if (remainingCalls-- <= 0) { return 0; }
		const auto count = std::min(size, static_cast<std::size_t>(maximum)); bytes.insert(bytes.end(), data, data + count); return static_cast<int>(count);
	}
	int service(RawMidiOutput& output) { return output.service([&](const auto* bytes, auto size) { return write(bytes, size); }); }
	void drain(RawMidiOutput& output) { for (int i = 0; i < 100 && output.wantsWrite(); ++i) { if (service(output) < 0) { break; } } }
};
}
class RawMidiOutputTest : public QObject
{
	Q_OBJECT
private slots:
	void completeChannelMessagesUseOneOrderedNativeStream_data()
	{
		QTest::addColumn<int>("type"); QTest::addColumn<int>("first"); QTest::addColumn<int>("second");
		QTest::newRow("note-on") << int(MidiNoteOn) << 60 << 90; QTest::newRow("note-off") << int(MidiNoteOff) << 60 << 0;
		QTest::newRow("key-pressure") << int(MidiKeyPressure) << 60 << 80; QTest::newRow("control") << int(MidiControlChange) << 7 << 100;
		QTest::newRow("program") << int(MidiProgramChange) << 12 << 0; QTest::newRow("channel-pressure") << int(MidiChannelPressure) << 70 << 0;
		QTest::newRow("pitch-bend") << int(MidiPitchBend) << 12000 << 0;
	}
	void completeChannelMessagesUseOneOrderedNativeStream()
	{
		QFETCH(int, type); QFETCH(int, first); QFETCH(int, second); int identity = 1; RawMidiOutput output; output.add(source(identity)); Wire wire;
		const MidiEvent event(static_cast<MidiEventTypes>(type), 3, first, second); output.enqueue(event, source(identity)); QCOMPARE(output.status(true).pendingEvents, 1u);
		wire.drain(output); QVERIFY(output.drained()); QCOMPARE(wire.bytes, encode(event)); QVERIFY(output.status(true).active);
	}

	void partialPacketsKeepTheirSuffixAheadOfOtherSources_data()
	{ QTest::addColumn<int>("prefix"); QTest::newRow("one-byte") << 1; QTest::newRow("two-bytes") << 2; }
	void partialPacketsKeepTheirSuffixAheadOfOtherSources()
	{
		QFETCH(int, prefix); int a = 1, b = 2; RawMidiOutput output; output.add(source(a)); output.add(source(b)); Wire wire;
		output.enqueue(MidiEvent(MidiNoteOn, 0, 60, 100), source(a)); output.enqueue(MidiEvent(MidiControlChange, 1, 7, 90), source(b));
		wire.maximum = prefix; wire.remainingCalls = 1; QCOMPARE(wire.service(output), 1); QCOMPARE(wire.bytes.size(), static_cast<std::size_t>(prefix)); QCOMPARE(output.status(true).pendingEvents, 2u);
		wire.maximum = 3; wire.remainingCalls = 100; wire.drain(output);
		QCOMPARE(wire.bytes, (std::vector<std::uint8_t>{0x90, 60, 100, 0xb1, 7, 90})); QVERIFY(output.drained());
	}

	void transientWritesNeverLoseOrRepeatBytes_data()
	{ QTest::addColumn<int>("error"); QTest::newRow("would-block") << -EAGAIN; QTest::newRow("interrupted") << -EINTR; QTest::newRow("zero") << 0; }
	void transientWritesNeverLoseOrRepeatBytes()
	{
		QFETCH(int, error); int identity = 1; RawMidiOutput output; output.add(source(identity)); Wire wire;
		output.enqueue(MidiEvent(MidiProgramChange, 0, 12, 0), source(identity)); wire.remainingCalls = 0; wire.error = error;
		QCOMPARE(wire.service(output), 1); QCOMPARE(output.status(true).pendingEvents, 1u); QVERIFY(wire.bytes.empty()); QVERIFY(output.status(true).active);
		wire.error = 0; wire.remainingCalls = 100; wire.drain(output); QCOMPARE(wire.bytes, (std::vector<std::uint8_t>{0xc0, 12}));
	}

	void eachServiceCycleBoundsNativeWrites()
	{
		int identity = 1; RawMidiOutput output; output.add(source(identity)); Wire wire; wire.maximum = 1;
		for (int i = 0; i < 22; ++i) { output.enqueue(MidiEvent(MidiControlChange, 0, 7, i), source(identity)); }
		wire.service(output); QCOMPARE(wire.bytes.size(), std::size_t{64}); QCOMPARE(output.status(true).pendingEvents, 1u);
		wire.drain(output); QCOMPARE(wire.bytes.size(), std::size_t{66}); QVERIFY(output.drained());
	}

	void cancellingUnsubmittedMusicDoesNotReplayItOrDiscardOtherSources()
	{
		int a = 1, b = 2; RawMidiOutput output; output.add(source(a)); output.add(source(b)); Wire wire;
		output.enqueue(MidiEvent(MidiNoteOn, 0, 60, 100), source(a)); output.enqueue(MidiEvent(MidiNoteOn, 1, 64, 100), source(b));
		output.cancel(source(a)); wire.drain(output); QCOMPARE(wire.bytes, (std::vector<std::uint8_t>{0x91, 64, 100}));
		QCOMPARE(output.status(true).cleanupPendingChannels, 0u);
	}

	void partialRetirementFinishesFramingThenCleansBeforeReusedIdentity_data()
	{ QTest::addColumn<int>("prefix"); QTest::newRow("one-byte") << 1; QTest::newRow("two-bytes") << 2; }
	void partialRetirementFinishesFramingThenCleansBeforeReusedIdentity()
	{
		QFETCH(int, prefix); int identity = 1; RawMidiOutput output; output.add(source(identity)); Wire wire; wire.maximum = prefix; wire.remainingCalls = 1;
		output.enqueue(MidiEvent(MidiNoteOn, 0, 60, 100), source(identity)); wire.service(output); output.remove(source(identity)); output.add(source(identity));
		output.enqueue(MidiEvent(MidiNoteOn, 1, 64, 90), source(identity)); QCOMPARE(output.status(true).cleanupPendingChannels, 1u);
		wire.maximum = 3; wire.remainingCalls = 100; wire.drain(output);
		QCOMPARE(wire.bytes, (std::vector<std::uint8_t>{0x90, 60, 100, 0xb0, 120, 0, 0xb0, 64, 0, 0xb0, 66, 0, 0xb0, 123, 0, 0x91, 64, 90}));
		output.cancel(source(identity)); QCOMPARE(output.status(true).cleanupPendingChannels, 1u); wire.bytes.clear(); wire.drain(output);
		QCOMPARE(wire.bytes.front(), static_cast<std::uint8_t>(0xb1)); QCOMPARE(wire.bytes.size(), std::size_t{12});
	}

	void reservedCleanupSurvivesPartialNativeWritesAndManyLocalReleases()
	{
		int identity = 1; RawMidiOutput output; output.add(source(identity)); Wire wire;
		output.enqueue(MidiEvent(MidiNoteOn, 2, 60, 100), source(identity)); wire.drain(output); output.cancel(source(identity));
		for (unsigned int i = 0; i < RawMidiOutput::QueueLimit * 2; ++i) { output.enqueue(MidiEvent(MidiNoteOff, 2, i % 128, 0), source(identity)); }
		QCOMPARE(output.status(true).pendingEvents, 0u); QCOMPARE(output.status(true).cleanupPendingChannels, 1u);
		wire.bytes.clear(); wire.maximum = 1; wire.remainingCalls = 1; wire.service(output); QCOMPARE(output.status(true).cleanupPendingChannels, 1u); QVERIFY(!output.drained());
		wire.remainingCalls = 100; wire.drain(output); QCOMPARE(wire.bytes.size(), std::size_t{12}); QVERIFY(output.drained()); QVERIFY(output.status(true).active);
	}

	void cleanupPrecedesAnUnsubmittedPacketAlreadyChosenForWriting()
	{
		int a = 1, b = 2; RawMidiOutput output; output.add(source(a)); output.add(source(b)); Wire wire;
		output.enqueue(MidiEvent(MidiNoteOn, 0, 60, 100), source(a)); wire.drain(output); wire.bytes.clear(); wire.remainingCalls = 0;
		output.enqueue(MidiEvent(MidiNoteOn, 1, 64, 100), source(b)); wire.service(output); output.cancel(source(a)); wire.remainingCalls = 100; wire.drain(output);
		QCOMPARE(wire.bytes.size(), std::size_t{15}); QCOMPARE(wire.bytes.front(), static_cast<std::uint8_t>(0xb0)); QCOMPARE(wire.bytes[12], static_cast<std::uint8_t>(0x91));
	}

	void overflowDiscardsMusicButPreservesReservedCleanup_data()
	{ QTest::addColumn<int>("activeChannels"); QTest::newRow("not-submitted") << 0; QTest::newRow("one-channel") << 1; QTest::newRow("all-channels") << 16; }
	void overflowDiscardsMusicButPreservesReservedCleanup()
	{
		QFETCH(int, activeChannels); int identity = 1; RawMidiOutput output; output.add(source(identity)); Wire wire;
		for (int channel = 0; channel < activeChannels; ++channel) { output.enqueue(MidiEvent(MidiNoteOn, channel, 60, 100), source(identity)); } wire.drain(output); wire.bytes.clear();
		for (unsigned int i = 0; i < RawMidiOutput::QueueLimit; ++i) { output.enqueue(MidiEvent(MidiControlChange, 0, 7, 90), source(identity)); }
		QVERIFY_EXCEPTION_THROWN(output.enqueue(MidiEvent(MidiNoteOn, 0, 62, 100), source(identity)), std::runtime_error);
		QCOMPARE(output.status(true).pendingEvents, 0u); QCOMPARE(output.status(true).cleanupPendingChannels, static_cast<unsigned int>(activeChannels)); QCOMPARE(output.status(true).nativeErrorCode, -ENOSPC);
		wire.drain(output); QCOMPARE(wire.bytes.size(), static_cast<std::size_t>(activeChannels * 12)); QVERIFY(output.drained()); QVERIFY(output.status(true).active); QCOMPARE(output.status(true).lastNativeErrorCode, -ENOSPC);
		output.enqueue(MidiEvent(MidiNoteOn, 0, 62, 100), source(identity)); wire.drain(output); QVERIFY(output.drained());
	}

	void nativeFailureRetainsUncertainCleanupAndRejectsNewMusic_data()
	{ QTest::addColumn<QString>("fault"); QTest::newRow("error") << QString("error"); QTest::newRow("oversized") << QString("oversized"); QTest::newRow("exception") << QString("exception"); }
	void nativeFailureRetainsUncertainCleanupAndRejectsNewMusic()
	{
		QFETCH(QString, fault); int identity = 1; RawMidiOutput output; output.add(source(identity)); output.enqueue(MidiEvent(MidiNoteOn, 4, 60, 100), source(identity));
		const auto error = output.service([&](const std::uint8_t*, std::size_t size) -> int { if (fault == "exception") { throw std::runtime_error("writer fixture"); } return fault == "oversized" ? static_cast<int>(size + 1) : -EIO; });
		QVERIFY(error < 0); QVERIFY(!output.status(true).active); QCOMPARE(output.status(true).cleanupPendingChannels, 1u); QVERIFY(!output.drained());
		QVERIFY_EXCEPTION_THROWN(output.enqueue(MidiEvent(MidiNoteOff, 4, 60, 0), source(identity)), std::runtime_error);
	}

	void retiredAndDuplicateSourcesCannotQueueMusic()
	{
		int identity = 1; RawMidiOutput output; output.add(source(identity)); QVERIFY_EXCEPTION_THROWN(output.add(source(identity)), std::logic_error);
		output.enqueue(MidiEvent(MidiNoteOn, 0, 60, 100), source(identity)); output.remove(source(identity)); QVERIFY(output.drained());
		QVERIFY_EXCEPTION_THROWN(output.enqueue(MidiEvent(MidiNoteOn, 0, 60, 100), source(identity)), std::runtime_error);
		output.add(source(identity)); output.enqueue(MidiEvent(MidiNoteOn, 0, 62, 100), source(identity)); Wire wire; wire.drain(output); QCOMPARE(wire.bytes, (std::vector<std::uint8_t>{0x90, 62, 100}));
	}

	void concurrentProducersPublishWholePackets()
	{
		int a = 1, b = 2; RawMidiOutput output; output.add(source(a)); output.add(source(b)); std::atomic_bool failed{false};
		const auto send = [&](const MidiPort* port, int channel) { try { for (int i = 0; i < 500; ++i) { output.enqueue(MidiEvent(MidiControlChange, channel, 7, i % 128), port); } } catch (...) { failed = true; } };
		std::thread one([&] { send(source(a), 0); }), two([&] { send(source(b), 1); }); one.join(); two.join(); QVERIFY(!failed.load());
		Wire wire; wire.drain(output); QCOMPARE(wire.bytes.size(), std::size_t{3000});
		for (std::size_t i = 0; i < wire.bytes.size(); i += 3) { QVERIFY(wire.bytes[i] == 0xb0 || wire.bytes[i] == 0xb1); QCOMPARE(wire.bytes[i + 1], static_cast<std::uint8_t>(7)); QVERIFY(wire.bytes[i + 2] < 128); }
	}
};
QTEST_GUILESS_MAIN(RawMidiOutputTest)
#include "RawMidiOutputTest.moc"
