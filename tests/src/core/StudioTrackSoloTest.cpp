/* Native track solo policy without views, restoration and session recovery.
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
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "InstrumentTrack.h"
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
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-track-solo-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
void legacyRows()
{
	QTest::addColumn<bool>("legacy");
	QTest::newRow("modern") << false; QTest::newRow("legacy") << true;
}
}

class StudioTrackSoloTest : public QObject
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
	void nativeSoloIsolatesWithoutViews_data() { legacyRows(); }
	void nativeSoloIsolatesWithoutViews()
	{
		QFETCH(bool, legacy); ConfigManager::inst()->setValue("app", "sololegacybehavior", legacy ? "1" : "0");
		const auto tracks = seed(); tracks.first->setSolo(true);
		QVERIFY(tracks.first->isSolo()); QVERIFY(!tracks.first->isMutedByModel());
		QVERIFY(tracks.second->isMutedByModel()); QCOMPARE(tracks.automation->isMutedByModel(), legacy);
		QVERIFY(tracks.first->isMutedBeforeSolo()); QVERIFY(!tracks.second->isMutedBeforeSolo());
		tracks.first->setSolo(false); QVERIFY(tracks.first->isMutedByModel());
		QVERIFY(!tracks.second->isMutedByModel()); QVERIFY(!tracks.automation->isMutedByModel());
	}
	void switchingSoloRetainsOriginalMuteCaches_data() { legacyRows(); }
	void switchingSoloRetainsOriginalMuteCaches()
	{
		QFETCH(bool, legacy); ConfigManager::inst()->setValue("app", "sololegacybehavior", legacy ? "1" : "0");
		const auto tracks = seed(); tracks.first->setSolo(true); tracks.second->setSolo(true);
		QVERIFY(!tracks.first->isSolo()); QVERIFY(tracks.second->isSolo());
		QVERIFY(tracks.first->isMutedByModel()); QVERIFY(!tracks.second->isMutedByModel());
		QVERIFY(tracks.first->isMutedBeforeSolo()); QVERIFY(!tracks.second->isMutedBeforeSolo());
		tracks.second->setSolo(false); QVERIFY(tracks.first->isMutedByModel());
		QVERIFY(!tracks.second->isMutedByModel()); QVERIFY(!tracks.automation->isMutedByModel());
	}
	void automatedModelWritesUseNativeSoloPolicy()
	{
		const auto tracks = seed(); tracks.first->soloControl()->setValue(1.f, true);
		QVERIFY(tracks.first->isSolo()); QVERIFY(tracks.second->isMutedByModel());
		tracks.first->soloControl()->setValue(0.f, true);
		QVERIFY(tracks.first->isMutedByModel()); QVERIFY(!tracks.second->isMutedByModel());
	}
	void patternRowsDoNotMuteSongTracks()
	{
		auto songTrack = new InstrumentTrack(Engine::getSong()); new PatternTrack(Engine::getSong());
		auto first = new InstrumentTrack(Engine::patternStore()); auto second = new InstrumentTrack(Engine::patternStore());
		const auto response = StudioCapabilities::execute("track.edit", {{"container", "pattern"}, {"track", 0}, {"solo", true}});
		QVERIFY(response.value("ok").toBool()); QVERIFY(first->isSolo()); QVERIFY(second->isMutedByModel());
		QVERIFY(!songTrack->isMutedByModel()); first->setSolo(false); QVERIFY(!second->isMutedByModel());
	}
	void trackLoadPreservesStoredMasksInsteadOfReplayingSolo()
	{
		const auto tracks = seed(); QDomDocument document; auto state = document.createElement("track"); document.appendChild(state);
		tracks.first->saveSettings(document, state); state.setAttribute("solo", 1); state.setAttribute("muted", 1);
		state.setAttribute("mutedBeforeSolo", 0); tracks.first->loadSettings(state);
		QVERIFY(tracks.first->isSolo()); QVERIFY(tracks.first->isMutedByModel());
		QVERIFY(!tracks.first->isMutedBeforeSolo()); QVERIFY(!tracks.second->isMutedByModel());
	}
	void linkedRestoreScopeSuppressesCommandEffectsUntilItExits()
	{
		const auto tracks = seed(); QDomDocument document; auto root = document.createElement("state"); document.appendChild(root);
		QString error;
		{
			NativeLinkedControls::RestoreScope links(document); QVERIFY(NativeLinkedControls::isRestoring());
			tracks.first->setSolo(true); QVERIFY(!tracks.second->isMutedByModel());
			QVERIFY2(links.finish(root, error), qPrintable(error));
			tracks.first->setSolo(false); QVERIFY(!tracks.second->isMutedByModel());
		}
		QVERIFY(!NativeLinkedControls::isRestoring()); tracks.first->setSolo(true); QVERIFY(tracks.second->isMutedByModel());
	}
	void portablePresetEndsSoloAndRestoresPreviouslyMutedTarget()
	{
		const auto tracks = seed(); QDomDocument document; auto preset = document.createElement("preset"); document.appendChild(preset);
		tracks.first->savePreset(document, preset); tracks.first->setSolo(true); tracks.first->loadPreset(preset);
		QVERIFY(!tracks.first->isSolo()); QVERIFY(tracks.first->isMutedByModel());
		QVERIFY(!tracks.second->isMutedByModel()); QVERIFY(!tracks.first->isMutedBeforeSolo());
	}
	void soloCloneTransfersSelectionOnceAndRetainsOriginalCaches()
	{
		const auto tracks = seed(); tracks.first->setSolo(true); auto copy = tracks.first->clone();
		QVERIFY2(copy, qPrintable(Engine::getSong()->errorSummary())); QVERIFY(copy->isSolo());
		QVERIFY(!tracks.first->isSolo()); QVERIFY(tracks.first->isMutedByModel());
		QVERIFY(copy->isMutedBeforeSolo()); copy->setSolo(false);
		QVERIFY(copy->isMutedByModel()); QVERIFY(tracks.first->isMutedByModel()); QVERIFY(!tracks.second->isMutedByModel());
	}
	void retiredPeerIsSkippedDuringNativeNotifications()
	{
		const auto tracks = seed(); const QPointer<Track> peer(tracks.second);
		const auto connection = QObject::connect(tracks.first->getMutedModel(), &BoolModel::dataChanged, this, [&] { if (peer) { delete peer.data(); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		tracks.first->setSolo(true); QVERIFY(!peer); QVERIFY(tracks.first->isSolo());
		tracks.first->setSolo(false); QVERIFY(tracks.first->isMutedByModel());
	}
	void ordinaryExceptionReleasesContainerTransitionGuard()
	{
		const auto tracks = seed();
		const auto connection = QObject::connect(tracks.second->getMutedModel(), &BoolModel::dataChanged, this,
			[] { throw std::runtime_error("fixture solo mute notification"); });
		QVERIFY_EXCEPTION_THROWN(tracks.first->setSolo(true), std::runtime_error); QObject::disconnect(connection);
		tracks.first->setSolo(false); tracks.first->setSolo(true); QVERIFY(tracks.second->isMutedByModel());
		tracks.first->setSolo(false); QVERIFY(tracks.first->isMutedByModel()); QVERIFY(!tracks.second->isMutedByModel());
	}
	void retiredSourceStopsLaterPeerWritesAndReleasesGuard()
	{
		const auto tracks = seed(); const QPointer<Track> source(tracks.first);
		QObject::connect(tracks.first->getMutedModel(), &BoolModel::dataChanged, this,
			[source] { if (source) { delete source.data(); } });
		tracks.first->setSolo(true); QVERIFY(!source); QVERIFY(!tracks.second->isMutedByModel());
		tracks.second->setSolo(true); QVERIFY(tracks.second->isSolo());
		auto peer = new InstrumentTrack(Engine::getSong()); tracks.second->setSolo(false);
		tracks.second->setSolo(true); QVERIFY(peer->isMutedByModel()); tracks.second->setSolo(false);
	}
	void structuredEditReportsNativeMuteAfterSoloAndCurrentRow()
	{
		const auto tracks = seed(); const auto response = StudioCapabilities::execute("track.edit",
			{{"track", 0}, {"muted", true}, {"solo", true}, {"name", "Solo"}, {"to", 1}});
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		const auto result = response.value("result").toObject(); QCOMPARE(result.value("track").toInt(), 1);
		QVERIFY(result.value("solo").toBool()); QVERIFY(!result.value("muted").toBool());
		QCOMPARE(row(1), static_cast<Track*>(tracks.first)); QVERIFY(tracks.second->isMutedByModel());
	}
	void sessionHistoryReplayAndReopeningPreserveSoloAndCaches()
	{
		QTemporaryDir directory; QString error; const auto command = request("solo", "track.edit", {{"track", 0}, {"solo", true}});
		QJsonObject receipt;
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed();
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
			receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); QCOMPARE(runtime.process(command), receipt);
			QVERIFY(row(0)->isSolo()); QVERIFY(row(1)->isMutedByModel());
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool());
			QVERIFY(!row(0)->isSolo()); QVERIFY(row(0)->isMutedByModel()); QVERIFY(!row(1)->isMutedByModel());
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
			QVERIFY(row(0)->isSolo()); QVERIFY(row(0)->isMutedBeforeSolo()); QVERIFY(row(1)->isMutedByModel());
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error));
		QCOMPARE(reopened.process(command), receipt); QVERIFY(row(0)->isSolo()); QVERIFY(row(1)->isMutedByModel());
		row(0)->setSolo(false); QVERIFY(row(0)->isMutedByModel()); QVERIFY(!row(1)->isMutedByModel());
	}
	void sessionRecoversTrackRetirementAndNativeExceptions_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("rename-retirement") << 0;
		QTest::newRow("move-retirement") << 1; QTest::newRow("solo-exception") << 2;
	}
	void sessionRecoversTrackRetirementAndNativeExceptions()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path());
		QVERIFY(runtime.open(error)); const auto tracks = seed(); const auto originalName = tracks.first->name();
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		bool invoked = false; QMetaObject::Connection connection;
		if (failure == 0) { connection = QObject::connect(tracks.first, &Track::nameChanged, this, [&] { invoked = true; delete tracks.first; }); }
		else if (failure == 1) { connection = QObject::connect(Engine::getSong(), &TrackContainer::trackMoved, this, [&] { invoked = true; delete tracks.first; }); }
		else { connection = QObject::connect(tracks.second->getMutedModel(), &BoolModel::dataChanged, this, [&] { invoked = true; throw std::runtime_error("fixture solo exception"); }); }
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QJsonObject args{{"track", 0}, {"solo", true}};
		if (failure == 0) { args.insert("name", "Retire"); } if (failure == 1) { args.insert("to", 1); }
		const auto command = request("reject", "track.edit", args); const auto response = runtime.process(command);
		QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QCOMPARE(runtime.process(command), response);
		QCOMPARE(row(0)->name(), originalName); QVERIFY(!row(0)->isSolo()); QVERIFY(row(0)->isMutedByModel());
		QVERIFY(!row(1)->isMutedByModel()); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsApplyNativeSoloThroughDirectAndCatalogTools_data() { protocols(); }
	void scriptedAgentsApplyNativeSoloThroughDirectAndCatalogTools()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error;
		StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Isolate the first track"}, {"steps", QJsonArray{"Select native solo"}}}),
				call("solo", "track.edit", {{"track", 0}, {"solo", true}}, catalog),
				call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native solo selected"}, {"evidence", QJsonArray{"solo"}}})});
		});
		QVERIFY(runtime.open(error)); seed();
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
		QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Solo the first track."}})).value("ok").toBool());
		for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); }
		QVERIFY(record(directory, "solo").value("ok").toBool()); QVERIFY(row(0)->isSolo());
		QVERIFY(!row(0)->isMutedByModel()); QVERIFY(row(1)->isMutedByModel());
		QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void preflightCancellationDoesNotChangeSolo()
	{
		const auto tracks = seed(); const auto response = StudioCapabilities::execute("track.edit", {{"track", 0}, {"solo", true}}, [] { return true; });
		QVERIFY(!response.value("ok").toBool()); QVERIFY(!tracks.first->isSolo()); QVERIFY(!tracks.second->isMutedByModel());
	}
	void toolDeclarationsRetainTrackEditAndProviderBudget()
	{
		for (const auto protocol : {QString("chat"), QString("responses")})
		{
			const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit);
			bool direct = false, catalog = false;
			for (const auto& value : tools)
			{
				const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject();
				direct = direct || tool.value("name") == "track_edit"; catalog = catalog || tool.value("name") == "studio_execute";
			}
			QVERIFY(direct); QVERIFY(catalog);
		}
	}

};
QTEST_GUILESS_MAIN(StudioTrackSoloTest)
#include "StudioTrackSoloTest.moc"
