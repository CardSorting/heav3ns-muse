/* Native MIDI CC rack delivery, lifecycle and serialization.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <functional>
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
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "StudioParameterTestUtils.h"

using namespace lmms;
namespace
{
const Plugin::Descriptor fixtureDescriptor{"cc-fixture", "CC fixture", "Native CC delivery fixture", "LMMS tests",
	1, Plugin::Type::Instrument, nullptr, "", nullptr};
struct Received
{
	int controller, value, channel, time;
	f_cnt_t offset;
	MidiEvent::Source source;
};
struct Receiver
{
	std::vector<Received> events;
	int failure = -1;
	bool accepts = true;
	std::function<void(const MidiEvent&)> callback;
};
class RecordingInstrument final : public Instrument
{
	std::shared_ptr<Receiver> m_receiver;
public:
	RecordingInstrument(InstrumentTrack* track, std::shared_ptr<Receiver> receiver)
		: Instrument(track, &fixtureDescriptor), m_receiver(std::move(receiver)) {}
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
	void saveSettings(QDomDocument&, QDomElement&) override {}
	void loadSettings(const QDomElement&) override {}
	QString nodeName() const override { return "cc-fixture"; }
	bool handleMidiEvent(const MidiEvent& event, const TimePos& time, f_cnt_t offset) override
	{
		if (event.type() != MidiControlChange) { return true; }
		if (event.controllerNumber() == m_receiver->failure)
		{ m_receiver->failure = -1; throw std::runtime_error("CC delivery fixture failure"); }
		if (m_receiver->callback) { m_receiver->callback(event); }
		m_receiver->events.push_back({event.controllerNumber(), event.controllerValue(), event.channel(), time.getTicks(), offset, event.source()});
		return m_receiver->accepts;
	}
};
class RecordingTrack final : public InstrumentTrack
{
public:
	std::shared_ptr<Receiver> receiver = std::make_shared<Receiver>();
	RecordingTrack() : InstrumentTrack(Engine::getSong()) {}
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override
	{ return new RecordingInstrument(this, receiver); }
};
QDomDocument state(InstrumentTrack* track)
{
	QDomDocument document; auto root = document.createElement("fixture"); document.appendChild(root);
	track->saveState(document, root); return document;
}
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
{ return {{"id", id}, {"op", op}, {"args", args}}; }
}

class InstrumentMidiCCTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	RecordingTrack* fixture()
	{
		auto track = new RecordingTrack; track->loadInstrument("cc-fixture"); return track;
	}
	QDomElement controls(QDomDocument& document) const
	{ return document.documentElement().firstChildElement("track").firstChildElement("instrumenttrack"); }
	void restore(InstrumentTrack* track, const QDomDocument& document)
	{ track->restoreState(document.documentElement().firstChildElement("track")); }
	void clearNativePitchSetup(RecordingTrack* track)
	{
		// Full track restoration republishes pitch sensitivity independently of the CC rack.
		const auto& events = track->receiver->events; QCOMPARE(events.size(), std::size_t(3));
		const int controllers[] = {MidiControllerRegisteredParameterNumberLSB, MidiControllerRegisteredParameterNumberMSB, MidiControllerDataEntry};
		for (int index = 0; index < 3; ++index)
		{
			QCOMPARE(events[index].controller, controllers[index]); QCOMPARE(events[index].value, index == 2 ? track->midiPitchRange() : 0);
			QCOMPARE(events[index].channel, track->midiPort()->realOutputChannel()); QCOMPARE(events[index].time, 0); QCOMPARE(events[index].offset, f_cnt_t(0));
		}
		track->receiver->events.clear();
	}
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		ConfigManager::inst()->setValue("audioengine", "samplerate", "48000");
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void enablingSendsOnlyUsedControllersIncludingExplicitZero()
	{
		auto track = fixture(); QVERIFY(track->initializeMidiCC(11, 127)); QVERIFY(!track->initializeMidiCC(128, 1));
		QVERIFY(!track->initializeMidiCC(1, -1)); QVERIFY(!track->initializeMidiCC(1, 128));
		track->midiCCModel(1)->setValue(20); QVERIFY(track->activateMidiCC(64));
		QVERIFY(track->receiver->events.empty()); track->midiCCEnabledModel()->setValue(true);
		const auto& events = track->receiver->events; QCOMPARE(events.size(), std::size_t(2));
		QCOMPARE(events[0].controller, 1); QCOMPARE(events[0].value, 20);
		QCOMPARE(events[1].controller, 64); QCOMPARE(events[1].value, 0);
		QVERIFY(events[0].source == MidiEvent::Source::Internal); QVERIFY(events[1].source == MidiEvent::Source::Internal);
		track->play(0, 32, 0); QCOMPARE(events.size(), std::size_t(2));
		QVERIFY(!track->activateMidiCC(-1)); QVERIFY(!track->activateMidiCC(128));
	}

	void nativeWireValuesRoundAndSuppressEquivalentBytes_data()
	{
		QTest::addColumn<float>("value"); QTest::addColumn<int>("expected");
		QTest::newRow("below-half") << .49f << 0; QTest::newRow("half") << .5f << 1;
		QTest::newRow("middle-half") << 63.5f << 64; QTest::newRow("near-maximum") << 126.6f << 127;
		QTest::newRow("maximum") << 127.f << 127;
	}
	void nativeWireValuesRoundAndSuppressEquivalentBytes()
	{
		QFETCH(float, value); QFETCH(int, expected); auto track = fixture(); QVERIFY(track->activateMidiCC(1));
		track->midiCCModel(1)->setValue(value); track->midiCCEnabledModel()->setValue(true);
		QCOMPARE(track->receiver->events.size(), std::size_t(1)); QCOMPARE(track->receiver->events[0].value, expected);
		track->midiCCModel(1)->setValue(expected); QCOMPARE(track->receiver->events.size(), std::size_t(1));
	}

	void unchangedValuesReplayAfterNativeStateChanges_data()
	{
		QTest::addColumn<int>("change"); QTest::newRow("silence") << 0; QTest::newRow("disable-enable") << 1;
		QTest::newRow("instrument-replacement") << 2; QTest::newRow("output-channel") << 3;
	}
	void unchangedValuesReplayAfterNativeStateChanges()
	{
		QFETCH(int, change); auto track = fixture(); track->midiCCModel(64)->setValue(127);
		track->midiCCEnabledModel()->setValue(true); QCOMPARE(track->receiver->events.size(), std::size_t(1));
		QVERIFY(track->isSustainPedalPressed());
		if (change == 0) { track->silenceAllNotes(); QVERIFY(!track->isSustainPedalPressed()); }
		if (change == 1) { track->midiCCEnabledModel()->setValue(false); track->midiCCEnabledModel()->setValue(true); }
		if (change == 2) { track->loadInstrument("cc-fixture"); QVERIFY(!track->isSustainPedalPressed()); }
		if (change == 3) { track->midiPort()->setOutputChannel(5); }
		track->play(48, 32, 7); QCOMPARE(track->receiver->events.size(), std::size_t(2));
		QCOMPARE(track->receiver->events.back().value, 127); QVERIFY(track->isSustainPedalPressed());
		if (change == 3) { QCOMPARE(track->receiver->events.back().channel, 4); }
		track->play(49, 32, 9); QCOMPARE(track->receiver->events.size(), std::size_t(2));
	}

	void playbackUsesNativeTickAndFrameOffsetBeforeNotes()
	{
		auto track = fixture(); Engine::getSong()->playSong();
		track->midiCCModel(1)->setValue(30); track->midiCCEnabledModel()->setValue(true);
		QVERIFY(track->receiver->events.empty()); track->play(48, 32, 11);
		QCOMPARE(track->receiver->events.size(), std::size_t(1)); QCOMPARE(track->receiver->events[0].time, 48);
		QCOMPARE(track->receiver->events[0].offset, f_cnt_t(11));
		track->midiCCModel(1)->setValue(70); QCOMPARE(track->receiver->events.size(), std::size_t(1));
		track->play(49, 32, 23); QCOMPARE(track->receiver->events.size(), std::size_t(2));
		QCOMPARE(track->receiver->events[1].time, 49); QCOMPARE(track->receiver->events[1].offset, f_cnt_t(23));
		QVERIFY(track->receiver->events[1].source == MidiEvent::Source::Internal);
	}

	void channelModeCommandsDoNotCauseRepeatedRackFlushes()
	{
		auto track = fixture(); QVERIFY(track->activateMidiCC(120)); QVERIFY(track->activateMidiCC(123));
		track->midiCCEnabledModel()->setValue(true); QCOMPARE(track->receiver->events.size(), std::size_t(2));
		for (int tick = 0; tick < 5; ++tick) { track->play(tick, 32, 0); }
		QCOMPARE(track->receiver->events.size(), std::size_t(2));
		track->silenceAllNotes(); track->play(5, 32, 0); QCOMPARE(track->receiver->events.size(), std::size_t(4));
		track->play(6, 32, 0); QCOMPARE(track->receiver->events.size(), std::size_t(4));
	}

	void nativeRestoreSuppressesEventsAndRetainsExplicitZeroActivation()
	{
		auto track = fixture(); track->midiCCModel(1)->setValue(27); QVERIFY(track->activateMidiCC(64));
		track->midiCCEnabledModel()->setValue(true); auto document = state(track);
		QCOMPARE(controls(document).attribute("activecc"), QString("1,64")); track->receiver->events.clear();
		restore(track, document); clearNativePitchSetup(track); QVERIFY(track->receiver->events.empty());
		QVERIFY(track->midiCCEnabledModel()->value()); track->play(0, 32, 0);
		QCOMPARE(track->receiver->events.size(), std::size_t(2)); QCOMPARE(track->receiver->events[0].value, 27);
		QCOMPARE(track->receiver->events[1].controller, 64); QCOMPARE(track->receiver->events[1].value, 0);
		track->play(1, 32, 0); QCOMPARE(track->receiver->events.size(), std::size_t(2));
	}

	void rackChannelModeRestoresValuesWithoutRepeatingCommands_data()
	{
		QTest::addColumn<int>("controller"); QTest::newRow("all-sound-off") << int(MidiControllerAllSoundOff);
		QTest::newRow("all-notes-off") << int(MidiControllerAllNotesOff); QTest::newRow("omni-off") << int(MidiControllerOmniOff);
	}
	void rackChannelModeRestoresValuesWithoutRepeatingCommands()
	{
		QFETCH(int, controller); auto track = fixture(); track->midiCCModel(1)->setValue(27); track->midiCCModel(64)->setValue(127);
		QVERIFY(track->activateMidiCC(controller)); track->midiCCEnabledModel()->setValue(true);
		QCOMPARE(track->receiver->events.size(), std::size_t(3)); QCOMPARE(track->receiver->events.back().controller, controller); QVERIFY(!track->isSustainPedalPressed());
		track->play(48, 32, 11); QCOMPARE(track->receiver->events.size(), std::size_t(5)); QVERIFY(track->isSustainPedalPressed());
		QCOMPARE(track->receiver->events[3].controller, 1); QCOMPARE(track->receiver->events[3].value, 27);
		QCOMPARE(track->receiver->events[4].controller, 64); QCOMPARE(track->receiver->events[4].value, 127);
		for (int tick = 49; tick < 55; ++tick) { track->play(tick, 32, 0); }
		QCOMPARE(track->receiver->events.size(), std::size_t(5));
	}

	void malformedActiveListsDisableTheRackWithoutPartialDelivery_data()
	{
		QTest::addColumn<QString>("list"); QTest::newRow("negative") << QString("-1"); QTest::newRow("large") << QString("128");
		QTest::newRow("not-an-integer") << QString("1,x"); QTest::newRow("fractional") << QString("1.5");
		QTest::newRow("empty-member") << QString("1,,64"); QTest::newRow("overflow") << QString("999999999999999999999");
		QTest::newRow("oversized-list") << QString(513, '1');
	}
	void malformedActiveListsDisableTheRackWithoutPartialDelivery()
	{
		QFETCH(QString, list); auto track = fixture(); track->midiCCModel(1)->setValue(27); track->midiCCEnabledModel()->setValue(true);
		auto document = state(track); controls(document).setAttribute("activecc", list); track->receiver->events.clear();
		restore(track, document); clearNativePitchSetup(track); QVERIFY(!track->midiCCEnabledModel()->value()); QVERIFY(Engine::getSong()->hasErrors());
		QVERIFY(track->receiver->events.empty()); track->play(0, 32, 0); QVERIFY(track->receiver->events.empty());
	}

	void legacyProjectsRecoverNonzeroAndExplicitModelValues()
	{
		auto track = fixture(); track->midiCCModel(1)->setValue(27); track->midiCCEnabledModel()->setValue(true);
		auto document = state(track); auto node = controls(document); node.removeAttribute("activecc");
		auto controllers = node.firstChildElement("midicontrollers"); auto zero = document.createElement("cc64");
		zero.setAttribute("value", 0); zero.setAttribute("id", track->midiCCModel(64)->id()); controllers.appendChild(zero);
		track->receiver->events.clear(); restore(track, document); clearNativePitchSetup(track); QVERIFY(track->receiver->events.empty());
		track->play(0, 32, 0); QCOMPARE(track->receiver->events.size(), std::size_t(2));
		QCOMPARE(track->receiver->events[0].controller, 1); QCOMPARE(track->receiver->events[1].controller, 64);
	}

	void deliveryFailureUnlocksPlaybackAndAllowsRetry()
	{
		auto track = fixture(); track->midiCCModel(1)->setValue(27); Engine::getSong()->playSong();
		track->midiCCEnabledModel()->setValue(true); track->receiver->failure = 1;
		QVERIFY_EXCEPTION_THROWN(track->play(0, 32, 0), std::runtime_error);
		QVERIFY(track->tryLock()); track->unlock(); QVERIFY(track->receiver->events.empty());
		track->play(0, 32, 0); QCOMPARE(track->receiver->events.size(), std::size_t(1));
		track->play(1, 32, 0); QCOMPARE(track->receiver->events.size(), std::size_t(1));
	}

	void callbackEditsWaitForTheNextNativeTick()
	{
		auto track = fixture(); track->midiCCModel(1)->setValue(27); Engine::getSong()->playSong(); track->midiCCEnabledModel()->setValue(true);
		track->receiver->callback = [track](const MidiEvent& event)
		{ if (event.controllerNumber() == 1 && event.controllerValue() == 27) { track->midiCCModel(1)->setValue(50); } };
		track->play(0, 32, 0); QCOMPARE(track->receiver->events.size(), std::size_t(1)); QCOMPARE(track->receiver->events[0].value, 27);
		track->play(1, 32, 0); QCOMPARE(track->receiver->events.size(), std::size_t(2)); QCOMPARE(track->receiver->events[1].value, 50);
	}

	void acceptedInputKeepsNativeModelsAndRestoresOnlyAffectedControls_data()
	{
		QTest::addColumn<int>("value"); QTest::addColumn<int>("channel"); QTest::addColumn<bool>("reassert");
		QTest::newRow("same-byte") << 27 << 0 << false; QTest::newRow("changed-byte") << 90 << 0 << true;
		QTest::newRow("other-channel") << 27 << 1 << true;
	}
	void acceptedInputKeepsNativeModelsAndRestoresOnlyAffectedControls()
	{
		QFETCH(int, value); QFETCH(int, channel); QFETCH(bool, reassert); auto track = fixture(); track->midiPort()->setReadable(true);
		track->midiCCModel(1)->setValue(27); track->midiCCModel(64)->setValue(127); track->midiCCEnabledModel()->setValue(true);
		track->receiver->events.clear(); Engine::getSong()->playSong();
		track->midiPort()->processInEvent(MidiEvent(MidiControlChange, channel, 1, value), TimePos(17));
		QCOMPARE(track->receiver->events.size(), std::size_t(1)); QCOMPARE(track->receiver->events[0].value, value);
		QCOMPARE(track->receiver->events[0].channel, channel); QCOMPARE(track->receiver->events[0].time, 17);
		QVERIFY(track->receiver->events[0].source == MidiEvent::Source::External); QCOMPARE(track->midiCCModel(1)->value(), 27.f);
		auto saved = state(track); QCOMPARE(controls(saved).attribute("activecc"), QString("1,64"));
		track->play(48, 32, 11); QCOMPARE(track->receiver->events.size(), std::size_t(reassert ? 2 : 1));
		if (reassert)
		{
			const auto& event = track->receiver->events.back(); QCOMPARE(event.controller, 1); QCOMPARE(event.value, 27);
			QCOMPARE(event.channel, 0); QCOMPARE(event.time, 48); QCOMPARE(event.offset, f_cnt_t(11)); QVERIFY(event.source == MidiEvent::Source::Internal);
		}
		track->play(49, 32, 23); QCOMPARE(track->receiver->events.size(), std::size_t(reassert ? 2 : 1));
	}

	void filteredInputDoesNotInvalidateRackAttempts_data()
	{
		QTest::addColumn<bool>("disabled"); QTest::newRow("disabled-port") << true; QTest::newRow("filtered-channel") << false;
	}
	void filteredInputDoesNotInvalidateRackAttempts()
	{
		QFETCH(bool, disabled); auto track = fixture(); track->midiPort()->setReadable(!disabled); track->midiPort()->setInputChannel(2);
		track->midiCCModel(1)->setValue(27); track->midiCCEnabledModel()->setValue(true); track->receiver->events.clear(); Engine::getSong()->playSong();
		track->midiPort()->processInEvent(MidiEvent(MidiControlChange, 0, 1, 90), TimePos(17));
		track->play(48, 32, 11); QVERIFY(track->receiver->events.empty()); QCOMPARE(track->midiCCModel(1)->value(), 27.f);
	}

	void disabledRackLeavesLiveInputInControl()
	{
		auto track = fixture(); track->midiCCModel(1)->setValue(27); Engine::getSong()->playSong();
		track->processInEvent(MidiEvent(MidiControlChange, 0, 1, 90)); track->processInEvent(MidiEvent(MidiControlChange, 0, 64, 127));
		QCOMPARE(track->receiver->events.size(), std::size_t(2)); track->play(48, 32, 11); QCOMPARE(track->receiver->events.size(), std::size_t(2));
		QCOMPARE(track->midiCCModel(1)->value(), 27.f); QCOMPARE(track->midiCCModel(64)->value(), 0.f); QVERIFY(track->isSustainPedalPressed());
		auto saved = state(track); QCOMPARE(controls(saved).attribute("activecc"), QString("1"));
		track->midiCCEnabledModel()->setValue(true); track->play(49, 32, 23); QCOMPARE(track->receiver->events.size(), std::size_t(3));
		QCOMPARE(track->receiver->events.back().controller, 1); QCOMPARE(track->receiver->events.back().value, 27); QVERIFY(track->isSustainPedalPressed());
		track->play(50, 32, 0); QCOMPARE(track->receiver->events.size(), std::size_t(3));
	}

	void nativeOutputParticipatesInRackBookkeeping_data()
	{
		QTest::addColumn<int>("value"); QTest::newRow("same-byte") << 27; QTest::newRow("changed-byte") << 90;
	}
	void nativeOutputParticipatesInRackBookkeeping()
	{
		QFETCH(int, value); auto track = fixture(); track->midiCCModel(1)->setValue(27); track->midiCCEnabledModel()->setValue(true);
		track->receiver->events.clear(); Engine::getSong()->playSong();
		track->processOutEvent(MidiEvent(MidiControlChange, 0, 1, value, nullptr, MidiEvent::Source::Internal), TimePos(17), 5);
		QCOMPARE(track->receiver->events.size(), std::size_t(1)); QCOMPARE(track->receiver->events[0].time, 17); QCOMPARE(track->receiver->events[0].offset, f_cnt_t(5));
		track->play(48, 32, 11); QCOMPARE(track->receiver->events.size(), std::size_t(value == 27 ? 1 : 2));
		QCOMPARE(track->receiver->events.back().value, 27); QCOMPARE(track->midiCCModel(1)->value(), 27.f);
		track->play(49, 32, 23); QCOMPARE(track->receiver->events.size(), std::size_t(value == 27 ? 1 : 2));
	}

	void resetAllControllersReleasesSustainAndReassertsActiveModels_data()
	{
		QTest::addColumn<bool>("output"); QTest::newRow("input") << false; QTest::newRow("native-output") << true;
	}
	void resetAllControllersReleasesSustainAndReassertsActiveModels()
	{
		QFETCH(bool, output); auto track = fixture(); track->midiCCModel(1)->setValue(27); track->midiCCModel(64)->setValue(127);
		track->midiCCEnabledModel()->setValue(true); QVERIFY(track->isSustainPedalPressed()); track->receiver->events.clear(); Engine::getSong()->playSong();
		const MidiEvent reset(MidiControlChange, 0, MidiControllerResetAllControllers, 0);
		if (output) { track->processOutEvent(reset, TimePos(17), 5); } else { track->processInEvent(reset, TimePos(17), 5); }
		QVERIFY(!track->isSustainPedalPressed()); QCOMPARE(track->receiver->events.size(), std::size_t(1));
		track->play(48, 32, 11); QCOMPARE(track->receiver->events.size(), std::size_t(3)); QVERIFY(track->isSustainPedalPressed());
		QCOMPARE(track->receiver->events[1].controller, 1); QCOMPARE(track->receiver->events[1].value, 27);
		QCOMPARE(track->receiver->events[2].controller, 64); QCOMPARE(track->receiver->events[2].value, 127);
		QCOMPARE(track->midiCCModel(1)->value(), 27.f); QCOMPARE(track->midiCCModel(64)->value(), 127.f);
		track->play(49, 32, 23); QCOMPARE(track->receiver->events.size(), std::size_t(3));
	}

	void rackResetCommandDoesNotRepeatEveryTick()
	{
		auto track = fixture(); track->midiCCModel(1)->setValue(27); QVERIFY(track->activateMidiCC(MidiControllerResetAllControllers));
		track->midiCCEnabledModel()->setValue(true); QCOMPARE(track->receiver->events.size(), std::size_t(2));
		QCOMPARE(track->receiver->events[1].controller, MidiControllerResetAllControllers);
		track->play(48, 32, 11); QCOMPARE(track->receiver->events.size(), std::size_t(3)); QCOMPARE(track->receiver->events.back().controller, 1);
		for (int tick = 49; tick < 55; ++tick) { track->play(tick, 32, 0); }
		QCOMPARE(track->receiver->events.size(), std::size_t(3));
	}

	void resetCompletesSustainedNativeInputNotesOnce_data()
	{
		QTest::addColumn<bool>("output"); QTest::newRow("input-reset") << false; QTest::newRow("native-output-reset") << true;
	}
	void resetCompletesSustainedNativeInputNotesOnce()
	{
		QFETCH(bool, output); auto track = fixture(); std::vector<Note> released; QObject lifetime;
		connect(track, &InstrumentTrack::midiNoteOff, &lifetime, [&](const Note& note) { released.push_back(note); }, Qt::DirectConnection);
		track->processInEvent(MidiEvent(MidiControlChange, 0, MidiControllerSustain, 127)); QVERIFY(track->isSustainPedalPressed());
		track->processInEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); track->processInEvent(MidiEvent(MidiNoteOff, 0, 60, 0)); QVERIFY(released.empty());
		const f_cnt_t offset = 23; const MidiEvent reset(MidiControlChange, 0, MidiControllerResetAllControllers, 0);
		if (output) { track->processOutEvent(reset, TimePos(17), offset); } else { track->processInEvent(reset, TimePos(17), offset); }
		QVERIFY(!track->isSustainPedalPressed()); QCOMPARE(released.size(), std::size_t(1)); QCOMPARE(released[0].key(), 60);
		QCOMPARE(track->receiver->events.back().time, 17); QCOMPARE(track->receiver->events.back().offset, offset);
		track->processInEvent(reset, TimePos(18), offset); QCOMPARE(released.size(), std::size_t(1));
	}

	void failedInputDeliveryLeavesTheRackEligibleForRetry()
	{
		auto track = fixture(); track->midiCCModel(1)->setValue(27); track->midiCCEnabledModel()->setValue(true);
		track->receiver->events.clear(); Engine::getSong()->playSong(); track->receiver->failure = 1;
		QVERIFY_EXCEPTION_THROWN(track->processInEvent(MidiEvent(MidiControlChange, 0, 1, 90)), std::runtime_error);
		QVERIFY(track->receiver->events.empty()); track->play(48, 32, 11); QCOMPARE(track->receiver->events.size(), std::size_t(1));
		QCOMPARE(track->receiver->events[0].value, 27); track->play(49, 32, 23); QCOMPARE(track->receiver->events.size(), std::size_t(1));
	}

	void ignoredRackControllerDoesNotGenerateRepeatedAttempts()
	{
		auto track = fixture(); Engine::getSong()->playSong(); track->receiver->accepts = false; track->midiCCModel(1)->setValue(27);
		track->midiCCEnabledModel()->setValue(true); QTest::ignoreMessage(QtWarningMsg, "InstrumentTrack: unhandled MIDI event 176");
		track->play(48, 32, 11); QCOMPARE(track->receiver->events.size(), std::size_t(1));
		track->play(49, 32, 23); QCOMPARE(track->receiver->events.size(), std::size_t(1));
	}

	void nestedCcDeliveryCannotHideTheOuterAttempt_data()
	{
		QTest::addColumn<bool>("output"); QTest::newRow("nested-input") << false; QTest::newRow("nested-native-output") << true;
	}
	void nestedCcDeliveryCannotHideTheOuterAttempt()
	{
		QFETCH(bool, output); auto track = fixture(); Engine::getSong()->playSong(); track->midiCCModel(1)->setValue(27); track->midiCCEnabledModel()->setValue(true);
		bool entered = false;
		track->receiver->callback = [&](const MidiEvent& event) {
			if (entered || event.source() != MidiEvent::Source::Internal) { return; } entered = true;
			const MidiEvent incoming(MidiControlChange, 0, 1, 90);
			if (output) { track->processOutEvent(incoming, TimePos(17), 5); } else { track->processInEvent(incoming, TimePos(17), 5); }
		};
		const auto clearCallback = qScopeGuard([track] { track->receiver->callback = {}; });
		track->play(48, 32, 11); QVERIFY(entered); QCOMPARE(track->receiver->events.size(), std::size_t(2));
		track->receiver->callback = {}; track->play(49, 32, 23); QCOMPARE(track->receiver->events.size(), std::size_t(3));
		QCOMPARE(track->receiver->events.back().value, 27); QCOMPARE(track->receiver->events.back().offset, f_cnt_t(23));
		track->play(50, 32, 0); QCOMPARE(track->receiver->events.size(), std::size_t(3));
	}

	void callbackInvalidationCannotBecomeAnAcknowledgedRackAttempt_data()
	{
		QTest::addColumn<int>("change"); QTest::newRow("native-silence") << 0; QTest::newRow("rack-transition") << 1;
		QTest::newRow("nested-channel-mode") << 2;
	}
	void callbackInvalidationCannotBecomeAnAcknowledgedRackAttempt()
	{
		QFETCH(int, change); auto track = fixture(); Engine::getSong()->playSong(); track->midiCCModel(64)->setValue(127); track->midiCCEnabledModel()->setValue(true);
		bool entered = false;
		track->receiver->callback = [&](const MidiEvent& event) {
			if (entered || event.controllerNumber() != 64) { return; } entered = true;
			if (change == 0) { track->silenceAllNotes(); }
			if (change == 1) { track->midiCCEnabledModel()->setValue(false); track->midiCCEnabledModel()->setValue(true); }
			if (change == 2) { track->processInEvent(MidiEvent(MidiControlChange, 0, MidiControllerAllNotesOff, 0)); }
		};
		const auto clearCallback = qScopeGuard([track] { track->receiver->callback = {}; });
		track->play(48, 32, 11); QVERIFY(entered); const auto attempts = track->receiver->events.size();
		track->receiver->callback = {}; track->play(49, 32, 23); QCOMPARE(track->receiver->events.size(), attempts + 1);
		QCOMPARE(track->receiver->events.back().controller, 64); QCOMPARE(track->receiver->events.back().value, 127); QVERIFY(track->isSustainPedalPressed());
		track->play(50, 32, 0); QCOMPARE(track->receiver->events.size(), attempts + 1);
	}

	void malformedCcCannotChangeSustainOrInvalidateRackAttempts_data()
	{
		QTest::addColumn<int>("controller"); QTest::addColumn<int>("value"); QTest::addColumn<int>("channel"); QTest::addColumn<bool>("output");
		QTest::newRow("negative-controller") << -1 << 0 << 0 << false; QTest::newRow("large-controller") << 128 << 0 << 0 << true;
		QTest::newRow("negative-pedal-value") << 64 << -1 << 0 << false; QTest::newRow("large-pedal-value") << 64 << 128 << 0 << true;
		QTest::newRow("negative-channel") << 64 << 0 << -1 << false; QTest::newRow("large-channel") << 64 << 0 << 16 << true;
		QTest::newRow("controller-high-byte-alias") << 256 << 0 << 0 << false;
		QTest::newRow("controller-negative-alias") << -128 << 0 << 0 << true;
		QTest::newRow("controller-signed-maximum") << 32767 << 0 << 0 << false;
		QTest::newRow("value-high-byte-alias") << 64 << 256 << 0 << true;
		QTest::newRow("value-negative-alias") << 64 << -256 << 0 << false;
		QTest::newRow("value-signed-maximum") << 64 << 32767 << 0 << true;
	}
	void malformedCcCannotChangeSustainOrInvalidateRackAttempts()
	{
		QFETCH(int, controller); QFETCH(int, value); QFETCH(int, channel); QFETCH(bool, output); auto track = fixture();
		track->midiCCModel(64)->setValue(127); track->midiCCEnabledModel()->setValue(true); track->receiver->events.clear(); Engine::getSong()->playSong();
		const MidiEvent event(MidiControlChange, channel, controller, value);
		if (output) { track->processOutEvent(event); } else { track->processInEvent(event); }
		QVERIFY(track->isSustainPedalPressed()); track->play(48, 32, 11); QVERIFY(track->receiver->events.empty()); QCOMPARE(track->midiCCModel(64)->value(), 127.f);
	}

	void nativeParameterHistoryAndRecoveryRetainActivation()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		{
			StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0]);
			const auto parameters = test::executeWithAllParameters("instrument.inspect", {{"track", 0}}).value("result").toObject().value("parameters").toArray();
			bool found = false;
			for (const auto item : parameters)
			{
				const auto model = item.toObject(); if (model.value("id").toInt() != int(track->midiCCEnabledModel()->id())) { continue; }
				QCOMPARE(model.value("kind").toString(), QString("boolean")); found = true;
			}
			QVERIFY(found);
			QVERIFY(runtime.process(request("enable", "parameter.set", {{"parameter", int(track->midiCCEnabledModel()->id())}, {"value", 1}})).value("ok").toBool());
			const auto edit = request("sustain", "parameter.set", {{"parameter", int(track->midiCCModel(64)->id())}, {"value", 127}});
			QVERIFY(runtime.process(edit).value("ok").toBool()); auto saved = state(track); QCOMPARE(controls(saved).attribute("activecc"), QString("64"));
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool());
			track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0]); saved = state(track);
			QVERIFY(track->midiCCEnabledModel()->value()); QCOMPARE(controls(saved).attribute("activecc"), QString());
			QVERIFY(runtime.process(edit).value("replayed").toBool()); QCOMPARE(track->midiCCModel(64)->value(), 0.f);
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
		}
		StudioRuntime recovered(directory.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0]); QVERIFY(track->midiCCEnabledModel()->value());
		QCOMPARE(track->midiCCModel(64)->value(), 127.f); auto saved = state(track); QCOMPARE(controls(saved).attribute("activecc"), QString("64"));
	}
};
QTEST_GUILESS_MAIN(InstrumentMidiCCTest)
#include "InstrumentMidiCCTest.moc"
