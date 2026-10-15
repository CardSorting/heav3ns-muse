/* Native MIDI event boundaries, previews, releases and receiver failures.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QTemporaryDir>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "Piano.h"
#include "Song.h"

using namespace lmms;
namespace
{
const Plugin::Descriptor fixtureDescriptor{"event-fixture", "Event fixture", "Native MIDI event fixture", "LMMS tests",
	1, Plugin::Type::Instrument, nullptr, "", nullptr};
struct Delivery { MidiEventTypes type; int first, second, channel, time; f_cnt_t offset; };
struct Receiver { std::vector<Delivery> events; int failure = -1; };
class EventInstrument final : public Instrument
{
	std::shared_ptr<Receiver> m_receiver;
public:
	EventInstrument(InstrumentTrack* track, std::shared_ptr<Receiver> receiver) : Instrument(track, &fixtureDescriptor), m_receiver(std::move(receiver)) {}
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
	void saveSettings(QDomDocument&, QDomElement&) override {}
	void loadSettings(const QDomElement&) override {}
	QString nodeName() const override { return "event-fixture"; }
	bool handleMidiEvent(const MidiEvent& event, const TimePos& time, f_cnt_t offset) override
	{
		if (int(event.type()) == m_receiver->failure) { m_receiver->failure = -1; throw std::runtime_error("Native receiver fixture failure"); }
		m_receiver->events.push_back({event.type(), event.param(0), event.param(1), event.channel(), time.getTicks(), offset}); return true;
	}
};
class EventTrack final : public InstrumentTrack
{
public:
	std::shared_ptr<Receiver> receiver = std::make_shared<Receiver>();
	EventTrack() : InstrumentTrack(Engine::getSong()) {}
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override
	{ return new EventInstrument(this, receiver); }
};
MidiEvent event(int type, int first, int second, int channel)
{
	MidiEvent result(static_cast<MidiEventTypes>(type), channel, first, second);
	if (result.type() == MidiMetaEvent) { result.setMetaEvent(MidiNotePanning); }
	return result;
}
void malformedRows()
{
	QTest::addColumn<int>("type"); QTest::addColumn<int>("first"); QTest::addColumn<int>("second"); QTest::addColumn<int>("channel");
	for (const auto type : {MidiNoteOn, MidiNoteOff, MidiKeyPressure})
	{
		const auto name = QString::number(type) + '-';
		QTest::newRow(qPrintable(name + "negative-key")) << int(type) << -1 << 1 << 0;
		QTest::newRow(qPrintable(name + "large-key")) << int(type) << NumKeys << 1 << 0;
		QTest::newRow(qPrintable(name + "negative-data")) << int(type) << 60 << -1 << 0;
		QTest::newRow(qPrintable(name + "large-data")) << int(type) << 60 << 128 << 0;
		QTest::newRow(qPrintable(name + "negative-channel")) << int(type) << 60 << 1 << -2;
		QTest::newRow(qPrintable(name + "large-channel")) << int(type) << 60 << 1 << 16;
	}
	for (const auto type : {MidiProgramChange, MidiChannelPressure, MidiPitchBend})
	{
		const auto name = QString::number(type) + '-'; const int maximum = type == MidiPitchBend ? MidiMaxPitchBend : 127;
		QTest::newRow(qPrintable(name + "negative-data")) << int(type) << -1 << 0 << 0;
		QTest::newRow(qPrintable(name + "large-data")) << int(type) << maximum + 1 << 0 << 0;
		QTest::newRow(qPrintable(name + "signed-maximum")) << int(type) << 32767 << 0 << 0;
		QTest::newRow(qPrintable(name + "preview-channel-refused")) << int(type) << 1 << 0 << -1;
		QTest::newRow(qPrintable(name + "large-channel")) << int(type) << 1 << 0 << 16;
	}
	QTest::newRow("panning-negative-key") << int(MidiMetaEvent) << -1 << 0 << 0;
	QTest::newRow("panning-large-key") << int(MidiMetaEvent) << NumKeys << 0 << 0;
	QTest::newRow("panning-below-native-range") << int(MidiMetaEvent) << 60 << MidiMinPanning - 1 << 0;
	QTest::newRow("panning-above-native-range") << int(MidiMetaEvent) << 60 << MidiMaxPanning + 1 << 0;
	QTest::newRow("panning-negative-channel") << int(MidiMetaEvent) << 60 << 0 << -2;
	QTest::newRow("panning-large-channel") << int(MidiMetaEvent) << 60 << 0 << 16;
}
}

class InstrumentMidiEventTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	EventTrack* fixture() { auto track = new EventTrack; track->loadInstrument("event-fixture"); return track; }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void malformedNativeInputPreservesHeldNotesAndModels_data() { malformedRows(); }
	void malformedNativeInputPreservesHeldNotesAndModels()
	{
		QFETCH(int, type); QFETCH(int, first); QFETCH(int, second); QFETCH(int, channel); auto track = fixture();
		std::vector<Note> onsets, releases; QObject lifetime;
		connect(track, &InstrumentTrack::midiNoteOn, &lifetime, [&](const Note& note) { onsets.push_back(note); }, Qt::DirectConnection);
		connect(track, &InstrumentTrack::midiNoteOff, &lifetime, [&](const Note& note) { releases.push_back(note); }, Qt::DirectConnection);
		track->processInEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); QCOMPARE(onsets.size(), std::size_t(1));
		const auto pitch = track->pitchModel()->value(); const auto deliveries = track->receiver->events.size();
		track->processInEvent(event(type, first, second, channel), TimePos(17), 5);
		QCOMPARE(onsets.size(), std::size_t(1)); QVERIFY(releases.empty()); QCOMPARE(track->pitchModel()->value(), pitch);
		QCOMPARE(track->receiver->events.size(), deliveries); QVERIFY(!track->isSustainPedalPressed());
		track->processInEvent(MidiEvent(MidiNoteOff, 0, 60, 0)); QCOMPARE(releases.size(), std::size_t(1));
		QCOMPARE(releases[0].key(), 60); QCOMPARE(releases[0].getVolume(), onsets[0].getVolume()); QCOMPARE(releases[0].getPanning(), onsets[0].getPanning());
	}

	void malformedNativeOutputDoesNotEnterReceiverOrPiano_data() { malformedRows(); }
	void malformedNativeOutputDoesNotEnterReceiverOrPiano()
	{
		QFETCH(int, type); QFETCH(int, first); QFETCH(int, second); QFETCH(int, channel); auto track = fixture();
		track->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); QVERIFY(track->pianoModel()->isKeyPressed(60));
		const auto deliveries = track->receiver->events.size(); QSignalSpy on(track, &InstrumentTrack::newNote), off(track, &InstrumentTrack::endNote);
		track->processOutEvent(event(type, first, second, channel), TimePos(17), 5);
		QCOMPARE(track->receiver->events.size(), deliveries); QCOMPARE(on.size(), 0); QCOMPARE(off.size(), 0); QVERIFY(track->pianoModel()->isKeyPressed(60));
		track->processOutEvent(MidiEvent(MidiNoteOff, 0, 60, 0)); QCOMPARE(off.size(), 1); QVERIFY(!track->pianoModel()->isKeyPressed(60));
	}

	void validOutputEndpointsRetainNativeValuesAndTiming_data()
	{
		QTest::addColumn<int>("type"); QTest::addColumn<bool>("maximum");
		for (const auto type : {MidiNoteOn, MidiNoteOff, MidiKeyPressure, MidiControlChange, MidiProgramChange, MidiChannelPressure, MidiPitchBend})
		{
			QTest::newRow(qPrintable(QString::number(type) + "-minimum")) << int(type) << false;
			QTest::newRow(qPrintable(QString::number(type) + "-maximum")) << int(type) << true;
		}
	}
	void validOutputEndpointsRetainNativeValuesAndTiming()
	{
		QFETCH(int, type); QFETCH(bool, maximum); auto track = fixture(); track->midiPort()->setOutputChannel(0);
		const bool pair = type == MidiNoteOn || type == MidiNoteOff || type == MidiKeyPressure || type == MidiControlChange;
		const int first = maximum ? (type == MidiPitchBend ? MidiMaxPitchBend : 127) : 0, second = maximum && pair ? 127 : 0;
		track->processOutEvent(event(type, first, second, 15), TimePos(17), 5); QCOMPARE(track->receiver->events.size(), std::size_t(1));
		const auto& delivered = track->receiver->events[0];
		const auto expected = type == MidiNoteOn && second == 0 ? MidiNoteOff : static_cast<MidiEventTypes>(type);
		QCOMPARE(delivered.type, expected); QCOMPARE(delivered.first, first); QCOMPARE(delivered.second, expected == MidiNoteOff ? 0 : second);
		QCOMPARE(delivered.channel, 15); QCOMPARE(delivered.time, 17); QCOMPARE(delivered.offset, f_cnt_t(5));
	}

	void nativePreviewSentinelAndSignedPanningRemainUsable()
	{
		auto track = fixture(); std::vector<Note> releases; QObject lifetime;
		connect(track, &InstrumentTrack::midiNoteOff, &lifetime, [&](const Note& note) { releases.push_back(note); }, Qt::DirectConnection);
		track->pianoModel()->handleKeyPress(60, 100); QVERIFY(track->pianoModel()->isKeyPressed(60));
		track->processInEvent(MidiEvent(MidiKeyPressure, -1, 60, 64)); QVERIFY(!track->receiver->events.empty());
		QCOMPARE(track->receiver->events.back().type, MidiKeyPressure); QCOMPARE(track->receiver->events.back().first, 60);
		QCOMPARE(track->receiver->events.back().channel, track->midiPort()->realOutputChannel());
		track->processInEvent(event(MidiMetaEvent, 60, MidiMinPanning, -1));
		track->pianoModel()->handleKeyRelease(60); QCOMPARE(releases.size(), std::size_t(1));
		QCOMPARE(releases[0].getPanning(), panning_t(PanningLeft)); track->pianoModel()->handleKeyPress(60, 100);
		track->processInEvent(event(MidiMetaEvent, 60, MidiMaxPanning, -1)); track->pianoModel()->handleKeyRelease(60);
		QCOMPARE(releases.size(), std::size_t(2)); QCOMPARE(releases[1].key(), 60); QCOMPARE(releases[1].getPanning(), panning_t(PanningRight));
		QVERIFY(!track->pianoModel()->isKeyPressed(60));
	}

	void nativeInputExtremeKeysRemainPlayable_data()
	{
		QTest::addColumn<int>("key"); QTest::newRow("lowest-key") << 0; QTest::newRow("highest-key") << NumKeys - 1;
	}
	void nativeInputExtremeKeysRemainPlayable()
	{
		QFETCH(int, key); auto track = fixture(); std::vector<Note> onsets, releases; QObject lifetime;
		connect(track, &InstrumentTrack::midiNoteOn, &lifetime, [&](const Note& note) { onsets.push_back(note); }, Qt::DirectConnection);
		connect(track, &InstrumentTrack::midiNoteOff, &lifetime, [&](const Note& note) { releases.push_back(note); }, Qt::DirectConnection);
		track->processInEvent(MidiEvent(MidiNoteOn, 15, key, 127)); track->processInEvent(MidiEvent(MidiNoteOff, 15, key, 0));
		QCOMPARE(onsets.size(), std::size_t(1)); QCOMPARE(releases.size(), std::size_t(1)); QCOMPARE(onsets[0].key(), key); QCOMPARE(releases[0].key(), key);
	}

	void zeroVelocityOutputCompletesTheNativeRelease()
	{
		auto track = fixture(); QSignalSpy on(track, &InstrumentTrack::newNote), off(track, &InstrumentTrack::endNote);
		track->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); track->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 0), TimePos(17), 5);
		QCOMPARE(track->receiver->events.size(), std::size_t(2)); QCOMPARE(track->receiver->events[1].type, MidiNoteOff);
		QCOMPARE(on.size(), 1); QCOMPARE(off.size(), 1); QVERIFY(!track->pianoModel()->isKeyPressed(60));
	}

	void unmatchedReleasesDoNotCorruptRetriggerAccounting()
	{
		auto track = fixture();
		for (int i = 0; i < 2; ++i) { track->processOutEvent(MidiEvent(MidiNoteOff, 0, 60, 0)); }
		track->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); track->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100));
		QCOMPARE(track->receiver->events.size(), std::size_t(5)); QCOMPARE(track->receiver->events[3].type, MidiNoteOff); QCOMPARE(track->receiver->events[4].type, MidiNoteOn);
		track->processOutEvent(MidiEvent(MidiNoteOff, 0, 60, 0)); QCOMPARE(track->receiver->events.size(), std::size_t(5));
		track->processOutEvent(MidiEvent(MidiNoteOff, 0, 60, 0)); QCOMPARE(track->receiver->events.size(), std::size_t(6)); QCOMPARE(track->receiver->events.back().type, MidiNoteOff);
	}

	void receiverExceptionsLeaveNativeCleanupAndFurtherOutputUsable_data()
	{
		QTest::addColumn<int>("type"); QTest::newRow("onset") << int(MidiNoteOn); QTest::newRow("release") << int(MidiNoteOff); QTest::newRow("pressure") << int(MidiKeyPressure);
	}
	void receiverExceptionsLeaveNativeCleanupAndFurtherOutputUsable()
	{
		QFETCH(int, type); auto track = fixture(); if (type != MidiNoteOn) { track->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); }
		track->receiver->failure = type; QVERIFY_EXCEPTION_THROWN(track->processOutEvent(event(type, 60, 100, 0)), std::runtime_error);
		// Cleanup enters the same native note mutex; an unbalanced exception would
		// hang this test and be bounded by its CTest process timeout.
		track->processInEvent(MidiEvent(MidiControlChange, 0, MidiControllerAllNotesOff, 0)); track->receiver->events.clear();
		track->processOutEvent(MidiEvent(MidiNoteOn, 0, 61, 100)); track->processOutEvent(MidiEvent(MidiNoteOff, 0, 61, 0));
		QCOMPARE(track->receiver->events.size(), std::size_t(2)); QCOMPARE(track->receiver->events[0].type, MidiNoteOn); QCOMPARE(track->receiver->events[1].type, MidiNoteOff);
	}
};
QTEST_GUILESS_MAIN(InstrumentMidiEventTest)
#include "InstrumentMidiEventTest.moc"
