/* Native project restoration and recovery boundaries. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <stdexcept>
#include "AudioEngine.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "SerializingObject.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
class Container final : public TrackContainer
{
public:
	Container() { setType(Type::Song); }
	std::function<void()> onUpdate;
	QString nodeName() const override { return "load-fixture-container"; }
	void updateAfterTrackAdd() override { const auto callback = onUpdate; if (callback) { callback(); } }
};
class Hook final : public SerializingObjectHook
{
public:
	std::function<void()> callback;
	void saveSettings(QDomDocument&, QDomElement&) override { }
	void loadSettings(const QDomElement&) override { const auto copy = callback; if (copy) { copy(); } }
};
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory)
{ QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
QDomElement row(QDomDocument& document, int type = static_cast<int>(Track::Type::Automation))
{
	auto element = document.createElement("track"); element.setAttribute("type", type); element.setAttribute("name", "Incoming track"); element.appendChild(document.createElement("automationtrack")); return element;
}
bool project(const QString& path, int count = 1, DataFile::Type type = DataFile::Type::SongProject, bool unknown = false)
{
	DataFile file(type); file.head().setAttribute("bpm", 97); auto tracks = file.createElement("trackcontainer"); file.content().appendChild(tracks);
	for (int index = 0; index < count; ++index) { tracks.appendChild(row(file, unknown ? 99 : static_cast<int>(Track::Type::Automation))); }
	tracks.appendChild(file.createComment("preserved formatting")); tracks.appendChild(file.createElement("future-extension")); return file.writeFile(path);
}
[[noreturn]] void raise(bool nonstandard) { if (nonstandard) { throw 17; } throw std::runtime_error("fixture project load failure"); }
}
class ProjectLoadLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	static void exceptions() { QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void nativeXmlCompressedAndTemplateProjectsRestore_data()
	{ QTest::addColumn<QString>("extension"); QTest::addColumn<bool>("isTemplate"); QTest::newRow("xml") << QString("mmp") << false; QTest::newRow("compressed") << QString("mmpz") << false; QTest::newRow("template") << QString("mpt") << true; }
	void nativeXmlCompressedAndTemplateProjectsRestore()
	{
		QFETCH(QString, extension); QFETCH(bool, isTemplate); QTemporaryDir directory; const auto path = directory.filePath("incoming." + extension); QVERIFY(project(path, 2, isTemplate ? DataFile::Type::SongProjectTemplate : DataFile::Type::SongProject)); const auto enabled = Engine::projectJournal()->isJournalling();
		QVERIFY(Engine::getSong()->loadProject(path)); QCOMPARE(Engine::getSong()->getTempo(), 97); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(2)); QCOMPARE(Engine::getSong()->getLoadingTrackCount(), 2); QCOMPARE(Engine::getSong()->projectFileName(), path); QVERIFY(!Engine::getSong()->isLoadingProject()); QCOMPARE(Engine::projectJournal()->isJournalling(), enabled);
	}
	void rejectedInputsPreserveTheCurrentNativeProject_data()
	{ QTest::addColumn<bool>("missing"); QTest::newRow("missing") << true; QTest::newRow("preset") << false; }
	void rejectedInputsPreserveTheCurrentNativeProject()
	{
		QFETCH(bool, missing); auto song = Engine::getSong(); auto track = new AutomationTrack(song); song->setTempo(83); const auto original = song->projectFileName(); QTemporaryDir directory; const auto path = directory.filePath(missing ? "missing.mmp" : "preset.xpf");
		if (!missing) { DataFile file(DataFile::Type::InstrumentTrackSettings); QVERIFY(file.writeFile(path)); }
		QVERIFY(!song->loadProject(path)); QCOMPARE(song->getTempo(), 83); QCOMPARE(song->tracks().size(), std::size_t(1)); QCOMPARE(song->tracks().front(), static_cast<Track*>(track)); QCOMPARE(song->projectFileName(), original); QVERIFY(!song->isLoadingProject());
	}
	void callbacksCannotNestNativeProjectReplacement()
	{
		QTemporaryDir directory; const auto path = directory.filePath("incoming.mmp"), nested = directory.filePath("nested.mmp"); QVERIFY(project(path)); bool invoked = false; auto song = Engine::getSong();
		const auto connection = connect(song, &Song::tempoChanged, this, [&](bpm_t tempo) { if (tempo != 97 || invoked) { return; } invoked = true; QVERIFY(!song->loadProject(path)); QVERIFY(!song->saveProjectFile(nested)); QVERIFY_EXCEPTION_THROWN(song->clearProject(), std::logic_error); QVERIFY_EXCEPTION_THROWN(song->createEmptyProject(), std::logic_error); QVERIFY_EXCEPTION_THROWN(song->createNewProject(), std::logic_error); QVERIFY_EXCEPTION_THROWN(song->createNewProjectFromTemplate(path), std::logic_error); });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(song->loadProject(path)); QVERIFY(invoked); QVERIFY(!QFile::exists(nested)); QCOMPARE(song->tracks().size(), std::size_t(1));
	}
	void borrowedFilenameIsCopiedBeforeCooperativeWork()
	{
		QTemporaryDir directory; QString path = directory.filePath("incoming.mmp"); const auto original = path; QVERIFY(project(path)); bool changed = false;
		{ FileResourceIO::Scope work([&] { if (!changed && Engine::getSong()->isLoadingProject()) { changed = true; path = directory.filePath("missing.mmp"); } return false; }); QVERIFY(Engine::getSong()->loadProject(path)); }
		QVERIFY(changed); QVERIFY(path != original); QCOMPARE(Engine::getSong()->projectFileName(), original); QCOMPARE(Engine::getSong()->getTempo(), 97);
	}
	void rejectedTemplateDoesNotRenameTheExistingProject()
	{
		QTemporaryDir directory; const auto path = directory.filePath("incoming.mmp"); QVERIFY(project(path)); auto song = Engine::getSong(); QVERIFY(song->loadProject(path)); const auto track = song->tracks().front();
		song->createNewProjectFromTemplate(directory.filePath("missing.mpt")); QCOMPARE(song->projectFileName(), path); QCOMPARE(song->tracks().front(), track); QCOMPARE(song->getTempo(), 97);
	}
	void headerExceptionsRestoreFlagsAndPermitRetry_data() { exceptions(); }
	void headerExceptionsRestoreFlagsAndPermitRetry()
	{
		QFETCH(bool, nonstandard); QTemporaryDir directory; const auto path = directory.filePath("incoming.mmp"); QVERIFY(project(path)); auto song = Engine::getSong(); bool armed = true; const auto enabled = Engine::projectJournal()->isJournalling();
		const auto connection = connect(song, &Song::tempoChanged, this, [&](bpm_t tempo) { if (tempo == 97 && armed) { armed = false; raise(nonstandard); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		bool caught = false; try { song->loadProject(path); } catch (...) { caught = true; } QVERIFY(caught); QVERIFY(!armed); QVERIFY(!song->isLoadingProject()); QCOMPARE(Engine::projectJournal()->isJournalling(), enabled); QVERIFY(song->loadProject(path)); QCOMPARE(song->tracks().size(), std::size_t(1));
	}
	void loadedNotificationFailuresRestoreFlags_data() { exceptions(); }
	void loadedNotificationFailuresRestoreFlags()
	{
		QFETCH(bool, nonstandard); QTemporaryDir directory; const auto path = directory.filePath("incoming.mmp"); QVERIFY(project(path)); auto song = Engine::getSong(); bool armed = true; const auto enabled = Engine::projectJournal()->isJournalling();
		const auto connection = connect(song, &Song::projectLoaded, this, [&] { if (armed) { armed = false; raise(nonstandard); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		bool caught = false; try { song->loadProject(path); } catch (...) { caught = true; } QVERIFY(caught); QVERIFY(!song->isLoadingProject()); QCOMPARE(Engine::projectJournal()->isJournalling(), enabled); QVERIFY(song->loadProject(path));
	}
	void headlessLoadDoesNotDispatchQueuedWorkDuringReplacement()
	{
		QTemporaryDir directory; const auto path = directory.filePath("incoming.mmp"); QVERIFY(project(path)); bool queued = false; QTimer::singleShot(0, this, [&] { queued = true; });
		{ FileResourceIO::Scope work({}, true); QVERIFY(Engine::getSong()->loadProject(path)); QVERIFY(!queued); }
		QCoreApplication::processEvents(); QVERIFY(queued);
	}
	void restoredTrackOrContainerRetirementDoesNotDoubleDelete_data()
	{ QTest::addColumn<bool>("parent"); QTest::newRow("track") << false; QTest::newRow("container") << true; }
	void restoredTrackOrContainerRetirementDoesNotDoubleDelete()
	{
		QFETCH(bool, parent); auto container = new Container; const QPointer<Container> owner(container); Hook hook; int deaths = 0; bool invoked = false;
		connect(container, &TrackContainer::trackAdded, this, [&](Track* track) { track->setHook(&hook); connect(track, &QObject::destroyed, this, [&] { ++deaths; }); hook.callback = [&, track] { invoked = true; if (parent) { delete container; } else { delete track; } }; });
		DataFile file(DataFile::Type::SongProject); file.content().appendChild(row(file)); QVERIFY_EXCEPTION_THROWN(Track::create(file.content().firstChildElement("track"), container), std::runtime_error); QVERIFY(invoked); QCOMPARE(deaths, 1); if (owner) { QVERIFY(owner->tracks().empty()); delete owner.data(); }
	}
	void factoryUpdateCallbacksReleaseOnlySurvivingCandidates_data()
	{ QTest::addColumn<int>("action"); QTest::newRow("track") << 0; QTest::newRow("container") << 1; QTest::newRow("standard") << 2; QTest::newRow("nonstandard") << 3; }
	void factoryUpdateCallbacksReleaseOnlySurvivingCandidates()
	{
		QFETCH(int, action); auto container = new Container; const QPointer<Container> owner(container); int deaths = 0;
		connect(container, &TrackContainer::trackAdded, this, [&](Track* track) { connect(track, &QObject::destroyed, this, [&] { ++deaths; }); });
		container->onUpdate = [&] { if (action == 0) { delete container->tracks().front(); } if (action == 1) { delete container; } if (action >= 2) { raise(action == 3); } }; bool caught = false;
		try { Track::create(Track::Type::Automation, container); } catch (...) { caught = true; } QVERIFY(caught); QCOMPARE(deaths, 1); if (owner) { QVERIFY(owner->tracks().empty()); owner->onUpdate = {}; QVERIFY(Track::create(Track::Type::Automation, owner.data())); delete owner.data(); }
	}
	void containerRestorationRejectsNestedSaveAndLoad()
	{
		Container container; Hook hook; DataFile file(DataFile::Type::SongProject); file.content().appendChild(row(file)); bool invoked = false;
		connect(&container, &TrackContainer::trackAdded, this, [&](Track* track) { track->setHook(&hook); }); hook.callback = [&] { invoked = true; QVERIFY_EXCEPTION_THROWN(container.restoreState(file.content()), std::logic_error); DataFile save(DataFile::Type::SongProject); QVERIFY_EXCEPTION_THROWN(container.saveState(save, save.content()), std::logic_error); };
		container.restoreState(file.content()); QVERIFY(invoked); hook.callback = {}; DataFile saved(DataFile::Type::SongProject); container.saveState(saved, saved.content());
	}
	void clearCallbacksRejectReplacementAndReleaseAdmissionAfterFailure_data() { exceptions(); }
	void clearCallbacksRejectReplacementAndReleaseAdmissionAfterFailure()
	{
		QFETCH(bool, nonstandard); auto song = Engine::getSong(); QTemporaryDir directory; const auto path = directory.filePath("incoming.mmp"); QVERIFY(project(path)); bool armed = true; const auto enabled = Engine::projectJournal()->isJournalling();
		const auto connection = connect(song, &Song::scaleListChanged, this, [&](int) { if (!armed) { return; } armed = false; QVERIFY(!song->loadProject(path)); QVERIFY(!song->saveProjectFile(directory.filePath("nested.mmp"))); QVERIFY_EXCEPTION_THROWN(song->clearProject(), std::logic_error); raise(nonstandard); }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		bool caught = false; try { song->clearProject(); } catch (...) { caught = true; } QVERIFY(caught); QVERIFY(!armed); QCOMPARE(Engine::projectJournal()->isJournalling(), enabled); song->clearProject(); QVERIFY(song->loadProject(path));
	}
	void runtimeRecoversAndReplaysLoadedNotificationFailure_data() { exceptions(); }
	void runtimeRecoversAndReplaysLoadedNotificationFailure()
	{
		QFETCH(bool, nonstandard); QTemporaryDir directory; QTemporaryDir input; const auto path = input.filePath("incoming.mmp"); QVERIFY(project(path, 2)); const auto command = request("load", "project.load", {{"path", path}}); QJsonObject rejected;
		{ StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 83}})).value("ok").toBool()); QVERIFY(runtime.process(request("track", "track.add", {{"type", "automation"}, {"name", "Keep me"}})).value("ok").toBool()); const auto before = manifest(directory); bool armed = true;
			const auto connection = connect(Engine::getSong(), &Song::projectLoaded, this, [&] { if (armed) { armed = false; raise(nonstandard); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
			rejected = runtime.process(command); QVERIFY(!rejected.value("ok").toBool()); QVERIFY(!runtime.stopping()); QVERIFY(!armed); QCOMPARE(Engine::getSong()->getTempo(), 83); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks().front()->name(), QString("Keep me")); QVERIFY(!Engine::getSong()->isLoadingProject()); QCOMPARE(manifest(directory).value("history"), before.value("history")); auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected); }
		StudioRuntime reopened(directory.path()); QString error; QVERIFY(reopened.open(error)); auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected); QCOMPARE(Engine::getSong()->getTempo(), 83);
	}
	void cancelledReplacementUsesMandatoryNativeRollback()
	{
		QTemporaryDir directory; QTemporaryDir input; const auto path = input.filePath("incoming.mmp"); QVERIFY(project(path)); StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 83}})).value("ok").toBool()); const auto before = manifest(directory); bool cancelled = false;
		{ FileResourceIO::Scope work([&] { cancelled = cancelled || Engine::getSong()->getTempo() == 97; return cancelled; }); QVERIFY(!runtime.process(request("load", "project.load", {{"path", path}})).value("ok").toBool()); }
		QVERIFY(cancelled); QVERIFY(!runtime.stopping()); QCOMPARE(Engine::getSong()->getTempo(), 83); QVERIFY(!Engine::getSong()->isLoadingProject()); QCOMPARE(manifest(directory).value("history"), before.value("history")); QVERIFY(runtime.process(request("retry", "project.load", {{"path", path}})).value("ok").toBool());
	}
	void unavailableTrackTypeProducesFailureAndNativeRecovery()
	{
		QTemporaryDir directory; QTemporaryDir input; const auto path = input.filePath("incoming.mmp"); QVERIFY(project(path, 1, DataFile::Type::SongProject, true)); StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 83}})).value("ok").toBool());
		QVERIFY(!runtime.process(request("load", "project.load", {{"path", path}})).value("ok").toBool()); QVERIFY(!runtime.stopping()); QCOMPARE(Engine::getSong()->getTempo(), 83); QVERIFY(Engine::getSong()->tracks().empty());
	}
};
QTEST_GUILESS_MAIN(ProjectLoadLifecycleTest)
#include "ProjectLoadLifecycleTest.moc"
