/* Native track detachment, checked removal, clone receipts and recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QDomDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <stdexcept>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "SampleClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "NativeLinkedControls.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "ProjectJournal.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "NativeReplayTestAccess.h"

using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
{ return {{"id", id}, {"op", op}, {"args", args}}; }
class SoloInstrumentTrack : public InstrumentTrack
{
public:
	explicit SoloInstrumentTrack(TrackContainer* owner) : InstrumentTrack(owner) {}
	BoolModel* soloControl() { return &m_soloModel; }
};
struct Tracks { SoloInstrumentTrack* first; SampleTrack* second; AutomationTrack* automation; };
Tracks seed()
{
	auto song = Engine::getSong();
	Tracks tracks{new SoloInstrumentTrack(song), new SampleTrack(song), new AutomationTrack(song)};
	tracks.first->setMuted(true); return tracks;
}
Track* row(int index) { return Engine::getSong()->tracks().at(index); }
QJsonObject call(const QString& id, const QString& op, const QJsonObject& args, bool catalog = false)
{ if (catalog) { return {{"id", id}, {"name", "studio_execute"}, {"args", QJsonObject{{"op", op}, {"args", args}}}}; } auto name = op; name.replace('.', '_'); return {{"id", id}, {"name", name}, {"args", args}}; }
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output; for (const auto& value : calls) { const auto item = value.toObject(); const auto args = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact)); if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", args}}); } else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", args}}}}); } }
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; } return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-track-lifecycle-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }

}

class StudioTrackLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid());
		ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false);
		Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase()
	{ Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors();
		Engine::projectJournal()->clearJournal(); Engine::projectJournal()->setJournalling(false);
		ConfigManager::inst()->setValue("app", "sololegacybehavior", "0");
	}
	void cleanup()
	{
		Engine::projectJournal()->setJournalling(false); Engine::projectJournal()->clearJournal();
		Engine::getSong()->stop(); Engine::getSong()->clearProject();
	}
	void soloDetachmentAllowsContainerReadsAndPeerRetirement()
	{
		const auto tracks = seed(); tracks.first->setSolo(true); const QPointer<Track> target(tracks.first), peer(tracks.second);
		const auto dispose = qScopeGuard([&] { if (target) { delete target.data(); } });
		int observedCount = 0; const auto connection = QObject::connect(tracks.first->getMutedModel(), &BoolModel::dataChanged, this, [&] {
			observedCount = Engine::getSong()->countTracks(); if (peer) { delete peer.data(); }
		});
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Engine::getSong()->removeTrack(tracks.first); QCOMPARE(observedCount, 3); QVERIFY(!peer);
		QVERIFY(target); QVERIFY(!target->isSolo()); QVERIFY(target->isMutedByModel());
		QCOMPARE(Engine::getSong()->countTracks(), 1); QCOMPARE(row(0), static_cast<Track*>(tracks.automation));
	}
	void nestedDetachmentPublishesRemovalOnlyOnce()
	{
		const auto tracks = seed(); tracks.first->setSolo(true); const QPointer<Track> target(tracks.first);
		const auto dispose = qScopeGuard([&] { if (target) { delete target.data(); } });
		QSignalSpy removed(Engine::getSong(), &TrackContainer::trackRemoved);
		const auto connection = QObject::connect(tracks.first->getMutedModel(), &BoolModel::dataChanged, this,
			[&] { Engine::getSong()->removeTrack(target.data()); });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Engine::getSong()->removeTrack(target.data()); QCOMPARE(removed.count(), 1);
		QCOMPARE(Engine::getSong()->countTracks(), 2);
	}
	void rosterMoveDuringSoloRestorationKeepsTargetIdentity()
	{
		const auto tracks = seed(); tracks.first->setSolo(true); const QPointer<Track> target(tracks.first);
		const auto dispose = qScopeGuard([&] { if (target) { delete target.data(); } }); bool moved = false;
		const auto connection = QObject::connect(tracks.first->getMutedModel(), &BoolModel::dataChanged, this,
			[&] { moved = Engine::getSong()->moveTrack(tracks.automation, 0); });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Engine::getSong()->removeTrack(target.data()); QVERIFY(moved); QCOMPARE(Engine::getSong()->countTracks(), 2);
		QCOMPARE(row(0), static_cast<Track*>(tracks.automation)); QCOMPARE(row(1), static_cast<Track*>(tracks.second));
	}
	void ordinaryNotificationExceptionLeavesRosterIntactAndUnlocked()
	{
		const auto tracks = seed(); tracks.first->setSolo(true);
		const auto connection = QObject::connect(tracks.first->getMutedModel(), &BoolModel::dataChanged, this,
			[] { throw std::runtime_error("fixture detach notification"); });
		QVERIFY_EXCEPTION_THROWN(Engine::getSong()->removeTrack(tracks.first), std::runtime_error);
		QObject::disconnect(connection); QCOMPARE(Engine::getSong()->countTracks(), 3);
		const QPointer<Track> target(tracks.first); Engine::getSong()->removeTrack(target.data());
		QCOMPARE(Engine::getSong()->countTracks(), 2); delete target.data();
	}
	void structuredRemovalRestoresSurvivorMuteState()
	{
		const auto tracks = seed(); tracks.first->setSolo(true); const QPointer<Track> target(tracks.first);
		const auto response = StudioCapabilities::execute("track.remove", {{"track", 0}});
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		QVERIFY(!target); QCOMPARE(Engine::getSong()->countTracks(), 2); QVERIFY(!tracks.second->isMutedByModel());
	}
	void recordingAdmissionRefusesBeforeSoloOrRemoval_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("note-recording") << 0;
		QTest::newRow("sample-armed") << 1; QTest::newRow("automation-recording") << 2;
		QTest::newRow("pattern-note-recording") << 3; QTest::newRow("pattern-automation-recording") << 4;
		QTest::newRow("sparse-pattern-row") << 5; QTest::newRow("surviving-pattern-cell-recording") << 6;
	}
	void recordingAdmissionRefusesBeforeSoloOrRemoval()
	{
		QFETCH(int, failure); Track* target = nullptr; Track* recording = nullptr; SampleClip* sample = nullptr;
		AutomationClip* automation = nullptr; int capture;
		if (failure < 3)
		{
			target = Track::create(failure == 0 ? Track::Type::Instrument : failure == 1 ? Track::Type::Sample : Track::Type::Automation, Engine::getSong());
			auto clip = target->createClip(0); recording = target;
			if (failure == 1) { sample = static_cast<SampleClip*>(clip); sample->setRecord(true); }
			if (failure == 2) { automation = static_cast<AutomationClip*>(clip); automation->setRecording(true); }
		}
		else
		{
			target = new PatternTrack(Engine::getSong()); if (failure == 6) { new PatternTrack(Engine::getSong()); }
			recording = Track::create(failure == 3 || failure == 5 ? Track::Type::Instrument : Track::Type::Automation, Engine::patternStore());
			if (failure == 4 || failure == 6) { automation = static_cast<AutomationClip*>(recording->getClips().at(failure == 6 ? 1 : 0)); automation->setRecording(true); }
			if (failure == 5) { delete recording->getClips().at(0); }
		}
		if (failure == 0 || failure == 3) { QVERIFY(recording->beginNoteRecording(&capture)); }
		const auto disarm = qScopeGuard([&] {
			if (sample) { sample->setRecord(false); } if (automation) { automation->setRecording(false); }
			if (failure == 0 || failure == 3) { recording->endNoteRecording(&capture); }
		});
		target->setSolo(true); const QPointer<Track> weak(target); const int count = Engine::getSong()->countTracks();
		QVERIFY_EXCEPTION_THROWN(target->requireIdleForRemoval(), std::invalid_argument);
		const auto response = StudioCapabilities::execute("track.remove", {{"track", 0}});
		QVERIFY(!response.value("ok").toBool()); QVERIFY(weak); QVERIFY(weak->isSolo());
		QCOMPARE(Engine::getSong()->countTracks(), count);
	}
	void cloneReceiptUsesTheCurrentRowAfterNativeSoloCallbacks()
	{
		const auto tracks = seed(); tracks.first->setSolo(true); bool moved = false;
		const auto connection = QObject::connect(tracks.first->getMutedModel(), &BoolModel::dataChanged, this, [&] {
			if (Engine::getSong()->tracks().size() == 4) { moved = Engine::getSong()->moveTrack(Engine::getSong()->tracks().back(), 0); }
		});
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = StudioCapabilities::execute("track.clone", {{"track", 0}});
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(moved);
		QCOMPARE(response.value("result").toObject().value("track").toInt(), 0);
		QVERIFY(row(0)->isSolo()); QCOMPARE(row(1), static_cast<Track*>(tracks.first));
	}
	void patternOwnerRemovalRetainsSurvivingNativeCellsAndSelection()
	{
		auto first = new PatternTrack(Engine::getSong()); auto middle = new PatternTrack(Engine::getSong());
		auto last = new PatternTrack(Engine::getSong()); auto nativeRow = Track::create(Track::Type::Instrument, Engine::patternStore());
		const QPointer<Clip> left(nativeRow->getClips().at(0)), removed(nativeRow->getClips().at(1)), right(nativeRow->getClips().at(2));
		static_cast<MidiClip*>(right.data())->addNote(Note(48, 24, 64), false); Engine::patternStore()->setCurrentPattern(2);
		const QPointer<Track> owner(middle); const auto response = StudioCapabilities::execute("track.remove", {{"track", 1}});
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		QVERIFY(!owner); QVERIFY(!removed); QCOMPARE(nativeRow->numOfClips(), 2);
		QCOMPARE(nativeRow->getClips().at(0), left.data()); QCOMPARE(nativeRow->getClips().at(1), right.data());
		QCOMPARE(right->startPosition().getTicks(), TimePos::ticksPerBar());
		QCOMPARE(static_cast<MidiClip*>(right.data())->notes().at(0)->key(), 64);
		QCOMPARE(PatternTrack::findPatternTrack(0), first); QCOMPARE(PatternTrack::findPatternTrack(1), last);
		QCOMPARE(Engine::patternStore()->currentPattern(), 1);
	}
	void sessionHistoryReplayAndReopeningRestoreRemovedNativeTrack()
	{
		QTemporaryDir directory; QString error; const auto command = request("remove", "track.remove", {{"track", 0}}); QJsonObject receipt;
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); const auto tracks = seed(); tracks.first->setSolo(true);
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
			receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); VERIFY_NATIVE_REPLAY(runtime.process(command), receipt);
			QCOMPARE(Engine::getSong()->countTracks(), 2); QVERIFY(!row(0)->isMutedByModel());
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool());
			QCOMPARE(Engine::getSong()->countTracks(), 3); QVERIFY(row(0)->isSolo()); QVERIFY(row(0)->isMutedBeforeSolo()); QVERIFY(row(1)->isMutedByModel());
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
			QCOMPARE(Engine::getSong()->countTracks(), 2); QVERIFY(!row(0)->isMutedByModel());
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error));
		VERIFY_NATIVE_REPLAY(reopened.process(command), receipt); QCOMPARE(Engine::getSong()->countTracks(), 2);
	}
	void sessionRecoversPreTeardownNotificationFailures_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("solo-exception") << 0;
		QTest::newRow("source-retirement") << 1; QTest::newRow("clone-retirement") << 2;
		QTest::newRow("clone-detachment") << 3;
	}
	void sessionRecoversPreTeardownNotificationFailures()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error));
		const auto tracks = seed(); tracks.first->setSolo(true);
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		bool invoked = false;
		const auto connection = QObject::connect(tracks.first->getMutedModel(), &BoolModel::dataChanged, this, [&] {
			if (invoked) { return; } invoked = true;
			if (failure == 0) { throw std::runtime_error("fixture pre-teardown solo"); }
			if (failure == 1) { delete tracks.first; }
			if (failure == 2) { delete Engine::getSong()->tracks().back(); }
			if (failure == 3) { Engine::getSong()->removeTrack(Engine::getSong()->tracks().back()); }
		});
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto command = request("reject", failure >= 2 ? "track.clone" : "track.remove", {{"track", 0}});
		const auto response = runtime.process(command); QVERIFY(invoked); QVERIFY(!response.value("ok").toBool());
		VERIFY_NATIVE_REPLAY(runtime.process(command), response); QCOMPARE(Engine::getSong()->countTracks(), 3);
		QVERIFY(row(0)->isSolo()); QVERIFY(row(1)->isMutedByModel()); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsCloneThenRemoveThroughNativeTools_data() { protocols(); }
	void scriptedAgentsCloneThenRemoveThroughNativeTools()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error;
		StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Duplicate then remove the duplicate"}, {"steps", QJsonArray{"Clone native track", "Remove its returned row"}}, {"decisions", QJsonArray{}}}),
				call("clone", "track.clone", {{"track", 0}}, catalog), call("remove", "track.remove", {{"track", 3}}, catalog),
				call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native clone removed"}, {"evidence", QJsonArray{"clone", "remove"}}})});
		});
		QVERIFY(runtime.open(error)); const auto tracks = seed(); tracks.first->setSolo(true);
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
		QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Clone the first track, then remove the clone."}})).value("ok").toBool());
		for (int index = 0; index < 7; ++index) { runtime.advanceAgent(); }
		QVERIFY(record(directory, "clone").value("ok").toBool()); QVERIFY(record(directory, "remove").value("ok").toBool());
		QCOMPARE(Engine::getSong()->countTracks(), 3); QVERIFY(!row(0)->isSolo()); QVERIFY(row(0)->isMutedByModel());
		QVERIFY(!row(1)->isMutedByModel()); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void cancelledStructuralCallsDoNotModifyTheRoster_data()
	{ QTest::addColumn<QString>("operation"); QTest::newRow("remove") << QString("track.remove"); QTest::newRow("clone") << QString("track.clone"); }
	void cancelledStructuralCallsDoNotModifyTheRoster()
	{
		QFETCH(QString, operation); const auto tracks = seed(); tracks.first->setSolo(true);
		const auto response = StudioCapabilities::execute(operation, {{"track", 0}}, [] { return true; });
		QVERIFY(!response.value("ok").toBool()); QCOMPARE(Engine::getSong()->countTracks(), 3); QVERIFY(tracks.first->isSolo());
	}
};
QTEST_GUILESS_MAIN(StudioTrackLifecycleTest)
#include "StudioTrackLifecycleTest.moc"
