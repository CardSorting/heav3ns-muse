/* Native automation interruption and playback roster ownership.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <memory>
#include <functional>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "Song.h"

using namespace lmms;
namespace lmms
{
class SongAutomationOwnershipTestAccess
{
public:
	static bool tick(TimePos time = 0)
	{
		const auto changes = Engine::audioEngine()->requestChangesGuard();
		std::vector<QPointer<Track>> tracks;
		if (Engine::getSong()->playMode() == Song::PlayMode::Pattern)
		{ tracks.emplace_back(PatternTrack::findPatternTrack(Engine::patternStore()->currentPattern())); }
		else { for (auto track : Engine::getSong()->tracks()) { tracks.emplace_back(track); } }
		return Engine::getSong()->processAutomations(tracks, time, 32);
	}
};
}
namespace
{
void control(AutomatableModel* model)
{
	auto controller = new MidiController(nullptr); controller->midiPort()->setInputController(74);
	controller->processInEvent(MidiEvent(MidiControlChange, 0, 74, 63), {});
	Controller::triggerFrameCounter(); controller->currentValue(0); Controller::triggerFrameCounter(); controller->currentValue(0);
	model->setControllerConnection(new ControllerConnection(controller));
}
AutomationClip* curve(AutomatableModel* model)
{
	auto track = new AutomationTrack(Engine::getSong()); auto clip = static_cast<AutomationClip*>(track->createClip(0));
	clip->addObject(model); clip->putValue(0, 25, false); clip->setAutoResize(false); clip->changeLength(192); return clip;
}
void interruptions()
{
	QTest::addColumn<int>("action"); QTest::newRow("stop") << 0; QTest::newRow("restart") << 1;
	QTest::newRow("pause") << 2; QTest::newRow("seek") << 3; QTest::newRow("clear-project") << 4;
}
void interrupt(int action)
{
	auto song = Engine::getSong();
	if (action == 0) { song->stop(); }
	if (action == 1) { song->stop(); song->playSong(); }
	if (action == 2) { song->togglePause(); }
	if (action == 3) { song->setPlayPos(48, Song::PlayMode::Song); }
	if (action == 4) { song->clearProject(); }
}
class ObservedInstrument : public InstrumentTrack
{
public:
	explicit ObservedInstrument(int& calls) : InstrumentTrack(Engine::getSong()), count(calls) {}
	int& count;
	std::function<void()> action;
	bool play(const TimePos&, f_cnt_t, f_cnt_t, int) override
	{ ++count; if (action) { action(); } return false; }
};
}
class SongAutomationInterruptionTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		ConfigManager::inst()->setValue("audioengine", "samplerate", "48000");
		ConfigManager::inst()->setValue("audioengine", "framesperaudiobuffer", "256");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void ownershipCallbackAbandonsOldAutomationSnapshot_data() { interruptions(); }
	void ownershipCallbackAbandonsOldAutomationSnapshot()
	{
		QFETCH(int, action); FloatModel model(42, 0, 100, 1, nullptr); control(&model); curve(&model);
		bool invoked = false; const auto connection = connect(&model, &AutomatableModel::dataChanged, this, [&] {
			if (!invoked && !model.useControllerValue()) { invoked = true; interrupt(action); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Engine::getSong()->playSong(); QVERIFY(!SongAutomationOwnershipTestAccess::tick()); QVERIFY(invoked);
		QCOMPARE(model.storedValue(), 42.f);
		if (action == 0 || action == 1 || action == 4) { QVERIFY(model.useControllerValue()); }
		if (action == 3) { QCOMPARE(Engine::getSong()->getPlayPos().getTicks(), 48); }
		if (action == 4) { QCOMPARE(Engine::getSong()->countTracks(), 0); }
	}
	void valueCallbackDoesNotWriteLaterTargets_data() { interruptions(); }
	void valueCallbackDoesNotWriteLaterTargets()
	{
		QFETCH(int, action); auto first = std::make_unique<FloatModel>(42, 0, 100, 1, nullptr), later = std::make_unique<FloatModel>(42, 0, 100, 1, nullptr);
		AutomatedValueMap order; order.insert(first.get(), 0); order.insert(later.get(), 0); if (order.firstKey() != first.get()) { first.swap(later); }
		control(first.get()); control(later.get()); auto native = curve(first.get()); QVERIFY(native->addObject(later.get()));
		bool invoked = false; const auto connection = connect(first.get(), &AutomatableModel::dataChanged, this, [&] {
			if (!invoked && first->storedValue() == 25.f) { invoked = true; interrupt(action); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Engine::getSong()->playSong(); QVERIFY(!SongAutomationOwnershipTestAccess::tick()); QVERIFY(invoked);
		QCOMPARE(first->storedValue(), 25.f); QCOMPARE(later->storedValue(), 42.f);
	}
	void priorOwnershipReleaseCannotApplyNewTargetsAfterInterruption_data() { interruptions(); }
	void priorOwnershipReleaseCannotApplyNewTargetsAfterInterruption()
	{
		QFETCH(int, action); FloatModel prior(42, 0, 100, 1, nullptr), next(42, 0, 100, 1, nullptr);
		control(&prior); control(&next); auto oldCurve = curve(&prior); Engine::getSong()->playSong();
		QVERIFY(SongAutomationOwnershipTestAccess::tick()); delete oldCurve; curve(&next);
		bool invoked = false; const auto connection = connect(&prior, &AutomatableModel::dataChanged, this, [&] {
			if (!invoked && prior.useControllerValue()) { invoked = true; interrupt(action); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!SongAutomationOwnershipTestAccess::tick(1)); QVERIFY(invoked); QCOMPARE(next.storedValue(), 42.f);
	}
	void recordingOwnershipInterruptionDoesNotCaptureOldTick_data() { interruptions(); }
	void recordingOwnershipInterruptionDoesNotCaptureOldTick()
	{
		QFETCH(int, action); FloatModel model(42, 0, 100, 1, nullptr); control(&model);
		const QPointer<AutomationClip> native(curve(&model)); native->reserveRecordingTake(16, 0); native->setRecording(true);
		const auto disarm = qScopeGuard([&] { if (native) { native->setRecording(false); native->discardRecordingTake(); } });
		bool invoked = false; const auto connection = connect(&model, &AutomatableModel::dataChanged, this, [&] {
			if (!invoked && model.useControllerValue()) { invoked = true; interrupt(action); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Engine::getSong()->playSong(); QVERIFY(!SongAutomationOwnershipTestAccess::tick()); QVERIFY(invoked);
		if (native) { QCOMPARE(native->recordingSamples(), std::uint64_t(0)); QCOMPARE(native->valueAt(0), 25.f); }
		else { QCOMPARE(action, 4); }
	}
	void interruptedBufferDoesNotPlaySavedTracksOrAdvanceFrames_data() { interruptions(); }
	void interruptedBufferDoesNotPlaySavedTracksOrAdvanceFrames()
	{
		QFETCH(int, action); int played = 0; new ObservedInstrument(played); FloatModel model(42, 0, 100, 1, nullptr);
		control(&model); curve(&model); bool invoked = false;
		const auto connection = connect(&model, &AutomatableModel::dataChanged, this, [&] {
			if (!invoked && !model.useControllerValue()) { invoked = true; interrupt(action); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Engine::getSong()->playSong();
		{ const auto changes = Engine::audioEngine()->requestChangesGuard(); Engine::getSong()->processNextBuffer(); }
		QVERIFY(invoked); QCOMPARE(played, 0); QCOMPARE(Engine::getSong()->playbackFrames(), f_cnt_t(0));
	}
	void playbackSkipsTracksRetiredByAutomation_data()
	{ QTest::addColumn<bool>("detach"); QTest::newRow("delete") << false; QTest::newRow("detach") << true; }
	void playbackSkipsTracksRetiredByAutomation()
	{
		QFETCH(bool, detach); int retiredCalls = 0, liveCalls = 0; const QPointer<Track> retired(new ObservedInstrument(retiredCalls));
		new ObservedInstrument(liveCalls); const auto dispose = qScopeGuard([&] { if (retired) { delete retired.data(); } });
		FloatModel model(42, 0, 100, 1, nullptr); control(&model); curve(&model); bool invoked = false;
		const auto connection = connect(&model, &AutomatableModel::dataChanged, this, [&] {
			if (!invoked && !model.useControllerValue())
			{ invoked = true; if (detach) { Engine::getSong()->removeTrack(retired.data()); } else { delete retired.data(); } }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Engine::getSong()->playSong();
		{ const auto changes = Engine::audioEngine()->requestChangesGuard(); Engine::getSong()->processNextBuffer(); }
		QVERIFY(invoked); QCOMPARE(retiredCalls, 0); QCOMPARE(liveCalls, 1); QCOMPARE(model.storedValue(), 25.f);
	}
	void trackPlaybackCanRetireALaterPeer_data()
	{ QTest::addColumn<bool>("detach"); QTest::newRow("delete") << false; QTest::newRow("detach") << true; }
	void trackPlaybackCanRetireALaterPeer()
	{
		QFETCH(bool, detach); int firstCalls = 0, retiredCalls = 0, liveCalls = 0;
		auto first = new ObservedInstrument(firstCalls); const QPointer<Track> retired(new ObservedInstrument(retiredCalls)); new ObservedInstrument(liveCalls);
		const auto dispose = qScopeGuard([&] { if (retired) { delete retired.data(); } });
		first->action = [&] { if (detach) { Engine::getSong()->removeTrack(retired.data()); } else { delete retired.data(); } };
		const auto clearAction = qScopeGuard([&] { first->action = {}; }); Engine::getSong()->playSong();
		{ const auto changes = Engine::audioEngine()->requestChangesGuard(); Engine::getSong()->processNextBuffer(); }
		QCOMPARE(firstCalls, 1); QCOMPARE(retiredCalls, 0); QCOMPARE(liveCalls, 1);
	}
	void trackPlaybackTransportChangesStopRemainingTracks_data()
	{
		QTest::addColumn<int>("action"); QTest::newRow("stop") << 0; QTest::newRow("restart") << 1;
		QTest::newRow("pause") << 2; QTest::newRow("seek") << 3;
	}
	void trackPlaybackTransportChangesStopRemainingTracks()
	{
		QFETCH(int, action); int firstCalls = 0, laterCalls = 0; const QPointer<ObservedInstrument> first(new ObservedInstrument(firstCalls));
		new ObservedInstrument(laterCalls); first->action = [&] { interrupt(action); };
		const auto clearAction = qScopeGuard([&] { if (first) { first->action = {}; } }); Engine::getSong()->playSong();
		{ const auto changes = Engine::audioEngine()->requestChangesGuard(); Engine::getSong()->processNextBuffer(); }
		QCOMPARE(firstCalls, 1); QCOMPARE(laterCalls, 0); QCOMPARE(Engine::getSong()->playbackFrames(), f_cnt_t(0));
	}
	void restartedTransportAcceptsFreshAutomationOnly()
	{
		FloatModel model(42, 0, 100, 1, nullptr); control(&model); auto native = curve(&model); bool invoked = false;
		const auto connection = connect(&model, &AutomatableModel::dataChanged, this, [&] {
			if (!invoked && !model.useControllerValue())
			{ invoked = true; Engine::getSong()->stop(); native->putValue(0, 85, false); Engine::getSong()->playSong(); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); Engine::getSong()->playSong();
		QVERIFY(!SongAutomationOwnershipTestAccess::tick()); QCOMPARE(model.storedValue(), 42.f);
		QVERIFY(SongAutomationOwnershipTestAccess::tick()); QCOMPARE(model.storedValue(), 85.f);
	}
	void timelineCallbackDoesNotOverwriteInterruptedPosition_data() { interruptions(); }
	void timelineCallbackDoesNotOverwriteInterruptedPosition()
	{
		QFETCH(int, action); int played = 0; new ObservedInstrument(played); Engine::getSong()->playSong();
		auto& timeline = Engine::getSong()->getTimeline(Song::PlayMode::Song); timeline.setFrameOffset(Engine::framesPerTick() * 1.5f);
		bool invoked = false; const auto connection = connect(&timeline, &Timeline::positionChanged, this, [&] {
			if (!invoked) { invoked = true; interrupt(action); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		{ const auto changes = Engine::audioEngine()->requestChangesGuard(); Engine::getSong()->processNextBuffer(); }
		QVERIFY(invoked); QCOMPARE(played, 0); QCOMPARE(Engine::getSong()->playbackFrames(), f_cnt_t(0));
		if (action != 2) { QCOMPARE(timeline.frameOffset(), 0.f); }
	}
	void nativeTempoAutomationKeepsTheCurrentPassValid()
	{
		FloatModel model(42, 0, 100, 1, nullptr); control(&model); curve(&model);
		const auto connection = connect(&model, &AutomatableModel::dataChanged, this, [&] {
			if (!model.useControllerValue()) { Engine::getSong()->setTempo(156); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); Engine::getSong()->playSong();
		QVERIFY(SongAutomationOwnershipTestAccess::tick()); QCOMPARE(model.storedValue(), 25.f); QCOMPARE(Engine::getSong()->getTempo(), bpm_t(156));
	}
	void patternSelectionChangesAbandonItsSavedValues_data()
	{ QTest::addColumn<bool>("retire"); QTest::newRow("select-other") << false; QTest::newRow("retire-owner") << true; }
	void patternSelectionChangesAbandonItsSavedValues()
	{
		QFETCH(bool, retire); const QPointer<PatternTrack> first(new PatternTrack(Engine::getSong())); new PatternTrack(Engine::getSong());
		FloatModel model(42, 0, 100, 1, nullptr); control(&model);
		auto row = Track::create(Track::Type::Automation, Engine::patternStore()); auto native = static_cast<AutomationClip*>(row->getClips().at(0));
		QVERIFY(native->addObject(&model)); native->putValue(0, 25, false); Engine::patternStore()->setCurrentPattern(0);
		bool invoked = false; const auto connection = connect(&model, &AutomatableModel::dataChanged, this, [&] {
			if (!invoked && !model.useControllerValue())
			{ invoked = true; if (retire) { delete first.data(); } else { Engine::patternStore()->setCurrentPattern(1); } }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); Engine::getSong()->playPattern();
		QVERIFY(!SongAutomationOwnershipTestAccess::tick()); QVERIFY(invoked); QCOMPARE(model.storedValue(), 42.f);
	}
};
QTEST_GUILESS_MAIN(SongAutomationInterruptionTest)
#include "SongAutomationInterruptionTest.moc"
