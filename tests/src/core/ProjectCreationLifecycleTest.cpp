/* Native project creation, defaults and recovery. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <stdexcept>
#include <thread>
#include "AudioEngine.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "NativeSongTestAccess.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
bool projectTemplate(const QString& path, int tempo, const QString& name)
{
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) { return false; }
	DataFile file(DataFile::Type::SongProjectTemplate); file.head().setAttribute("bpm", tempo);
	auto tracks = file.createElement("trackcontainer"), track = file.createElement("track");
	track.setAttribute("type", static_cast<int>(Track::Type::Automation)); track.setAttribute("name", name);
	track.appendChild(file.createElement("automationtrack")); tracks.appendChild(track); file.content().appendChild(tracks); return file.writeFile(path);
}
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory)
{ QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
[[noreturn]] void raise(bool nonstandard) { if (nonstandard) { throw 23; } throw std::runtime_error("fixture creation failure"); }
bool released(Song* song)
{ return !song->isLoadingProject() && !NativeSongTestAccess::isCreating(song) && !NativeSongTestAccess::permitsCreationLoad(song); }
}
class ProjectCreationLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QStringList dataPaths;
	QString factory;
	bool journalling = false;
	int serial = 0;
	static void exceptions() { QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	QString userTemplate() const { return ConfigManager::inst()->userTemplateDir() + "default.mpt"; }
	QString factoryTemplate() const { return factory + "/projects/templates/default.mpt"; }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("initial")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing();
		dataPaths = QDir::searchPaths("data"); journalling = Engine::projectJournal()->isJournalling();
	}
	void cleanupTestCase()
	{ QDir::setSearchPaths("data", dataPaths); Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		auto song = Engine::getSong(); song->stop(); song->clearProject(); song->clearErrors(); song->setLoadOnLaunch(false); Engine::projectJournal()->setJournalling(false);
		const QString stem = QString::number(++serial); factory = configuration.filePath(stem + "/factory"); QVERIFY(QDir().mkpath(factory + "/projects/templates"));
		QDir::setSearchPaths("data", QStringList{factory}); ConfigManager::inst()->setWorkingDir(configuration.filePath(stem + "/workspace"));
		QVERIFY(QDir().mkpath(ConfigManager::inst()->userTemplateDir()));
	}
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void emptyCreationPreservesJournalPolicy_data()
	{ QTest::addColumn<bool>("enabled"); QTest::newRow("disabled") << false; QTest::newRow("enabled") << true; }
	void emptyCreationPreservesJournalPolicy()
	{
		QFETCH(bool, enabled); auto song = Engine::getSong(); new AutomationTrack(song); song->setModified(); song->collectError("stale"); Engine::projectJournal()->setJournalling(enabled);
		song->createEmptyProject(); QVERIFY(song->tracks().empty()); QVERIFY(Engine::patternStore()->tracks().empty()); QVERIFY(song->projectFileName().isEmpty());
		QVERIFY(!song->isModified()); QVERIFY(!song->hasErrors()); QVERIFY(released(song)); QCOMPARE(Engine::projectJournal()->isJournalling(), enabled);
	}
	void absentDefaultsCreateActualBuiltinInstruments()
	{
		auto song = Engine::getSong(); song->createNewProject(); QVERIFY(!song->hasErrors()); QCOMPARE(song->tracks().size(), std::size_t(4)); QCOMPARE(Engine::patternStore()->tracks().size(), std::size_t(1));
		auto lead = dynamic_cast<InstrumentTrack*>(song->tracks()[0]); auto kick = dynamic_cast<InstrumentTrack*>(Engine::patternStore()->tracks()[0]);
		QVERIFY(lead && lead->instrument() && lead->instrument()->descriptor()); QVERIFY(kick && kick->instrument() && kick->instrument()->descriptor());
		QCOMPARE(QString::fromUtf8(lead->instrument()->descriptor()->name), QString("tripleoscillator")); QCOMPARE(QString::fromUtf8(kick->instrument()->descriptor()->name), QString("kicker"));
		QCOMPARE(song->tracks()[1]->type(), Track::Type::Sample); QCOMPARE(song->tracks()[2]->type(), Track::Type::Pattern); QCOMPARE(song->tracks()[3]->type(), Track::Type::Automation);
		QCOMPARE(Engine::patternStore()->numOfPatterns(), 1); QCOMPARE(song->getTempo(), DefaultTempo); QVERIFY(!song->isModified()); QVERIFY(released(song)); QVERIFY(!Engine::projectJournal()->isJournalling());
	}
	void userTemplatePrecedesFactoryDefault()
	{
		QVERIFY(projectTemplate(userTemplate(), 83, "User default")); QVERIFY(projectTemplate(factoryTemplate(), 109, "Factory default")); auto song = Engine::getSong(); song->createNewProject();
		QVERIFY(!song->hasErrors()); QCOMPARE(song->getTempo(), 83); QCOMPARE(song->tracks().size(), std::size_t(1)); QCOMPARE(song->tracks()[0]->name(), QString("User default")); QVERIFY(song->projectFileName().isEmpty()); QVERIFY(!song->isModified()); QVERIFY(released(song));
	}
	void factoryTemplateIsUsedWhenUserDefaultIsAbsent()
	{
		QVERIFY(projectTemplate(factoryTemplate(), 109, "Factory default")); auto song = Engine::getSong(); song->createNewProject(); QVERIFY(!song->hasErrors());
		QCOMPARE(song->getTempo(), 109); QCOMPARE(song->tracks().size(), std::size_t(1)); QCOMPARE(song->tracks()[0]->name(), QString("Factory default")); QVERIFY(song->projectFileName().isEmpty()); QVERIFY(released(song));
	}
	void brokenDefaultReportsFailureWithoutStartupRecursion()
	{
		QFile file(userTemplate()); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("broken"), qint64(6)); file.close(); QVERIFY(projectTemplate(factoryTemplate(), 109, "Do not substitute"));
		auto song = Engine::getSong(); auto original = new AutomationTrack(song); song->setTempo(83); song->setLoadOnLaunch(true); const auto generation = song->replacementGeneration();
		QVERIFY(!StudioCapabilities::execute("project.new", {{"summary", true}}).value("ok").toBool()); QCOMPARE(song->getTempo(), 83); QCOMPARE(song->tracks().size(), std::size_t(1)); QCOMPARE(song->tracks()[0], static_cast<Track*>(original)); QCOMPARE(song->replacementGeneration(), generation); QVERIFY(song->hasErrors()); QVERIFY(released(song));
	}
	void explicitMissingTemplatePreservesCurrentFilename()
	{
		QVERIFY(projectTemplate(userTemplate(), 83, "Original")); auto song = Engine::getSong(); QVERIFY(song->loadProject(userTemplate())); const QString path = song->projectFileName(); auto original = song->tracks()[0]; song->setLoadOnLaunch(true);
		song->createNewProjectFromTemplate(configuration.filePath("missing.mpt")); QCOMPARE(song->projectFileName(), path); QCOMPARE(song->tracks()[0], original); QVERIFY(song->hasErrors()); QVERIFY(released(song));
	}
	void templateCreationConsumesItsLoadPermitBeforeCallbacks()
	{
		QVERIFY(projectTemplate(userTemplate(), 83, "Template")); auto song = Engine::getSong(); bool invoked = false;
		const auto connection = connect(song, &Song::projectFileNameChanged, this, [&] {
			if (invoked || !NativeSongTestAccess::isCreating(song)) { return; } invoked = true; QVERIFY(!NativeSongTestAccess::permitsCreationLoad(song));
			QVERIFY(!song->loadProject(userTemplate())); QVERIFY(!song->saveProjectFile(configuration.filePath("nested.mmp"))); QVERIFY_EXCEPTION_THROWN(song->clearProject(), std::logic_error);
			QVERIFY_EXCEPTION_THROWN(song->createEmptyProject(), std::logic_error); QVERIFY_EXCEPTION_THROWN(song->createNewProject(), std::logic_error); QVERIFY_EXCEPTION_THROWN(song->createNewProjectFromTemplate(userTemplate()), std::logic_error);
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); song->createNewProjectFromTemplate(userTemplate());
		QVERIFY(invoked); QVERIFY(!song->hasErrors()); QCOMPARE(song->getTempo(), 83); QVERIFY(song->projectFileName().isEmpty()); QVERIFY(released(song));
	}
	void clearFailuresReleaseCreationAdmission_data() { exceptions(); }
	void clearFailuresReleaseCreationAdmission()
	{
		QFETCH(bool, nonstandard); auto song = Engine::getSong(); bool armed = true; Engine::projectJournal()->setJournalling(true);
		const auto connection = connect(song, &Song::scaleListChanged, this, [&](int) { if (armed) { armed = false; raise(nonstandard); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		bool caught = false; try { song->createEmptyProject(); } catch (...) { caught = true; } QVERIFY(caught); QVERIFY(!armed); QVERIFY(released(song)); QVERIFY(Engine::projectJournal()->isJournalling()); song->createEmptyProject(); QVERIFY(released(song));
	}
	void laterBuiltinConstructionCannotRetireEarlierTrack()
	{
		auto song = Engine::getSong(); QPointer<Track> first; bool invoked = false;
		const auto connection = connect(song, &TrackContainer::trackAdded, this, [&](Track* track) { if (!first && track->type() == Track::Type::Instrument) { first = track; } else if (track->type() == Track::Type::Sample && first) { invoked = true; delete first.data(); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY_EXCEPTION_THROWN(song->createNewProject(), std::runtime_error); QVERIFY(invoked); QVERIFY(!first); QVERIFY(released(song));
	}
	void templateHeaderFailuresRestoreOuterCreationFlags_data() { exceptions(); }
	void templateHeaderFailuresRestoreOuterCreationFlags()
	{
		QFETCH(bool, nonstandard); QVERIFY(projectTemplate(userTemplate(), 83, "Template")); auto song = Engine::getSong(); bool armed = true; Engine::projectJournal()->setJournalling(true);
		const auto connection = connect(song, &Song::tempoChanged, this, [&](bpm_t tempo) { if (tempo == 83 && armed) { armed = false; raise(nonstandard); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		bool caught = false; try { song->createNewProjectFromTemplate(userTemplate()); } catch (...) { caught = true; } QVERIFY(caught); QVERIFY(!armed); QVERIFY(released(song)); QVERIFY(Engine::projectJournal()->isJournalling());
		song->createNewProjectFromTemplate(userTemplate()); QVERIFY(!song->hasErrors()); QCOMPARE(song->getTempo(), 83); QVERIFY(song->projectFileName().isEmpty()); QVERIFY(released(song));
	}
	void borrowedTemplatePathIsCopiedBeforeCooperativeWork()
	{
		QVERIFY(projectTemplate(userTemplate(), 83, "Template")); QString path = userTemplate(); bool changed = false;
		{ FileResourceIO::Scope work([&] { if (!changed && NativeSongTestAccess::isCreating(Engine::getSong())) { changed = true; path = configuration.filePath("missing.mpt"); } return false; }); Engine::getSong()->createNewProjectFromTemplate(path); }
		QVERIFY(changed); QVERIFY(!Engine::getSong()->hasErrors()); QCOMPARE(Engine::getSong()->getTempo(), 83); QVERIFY(released(Engine::getSong()));
	}
	void completionNotificationsCannotChangeVisibleState_data()
	{ QTest::addColumn<bool>("dirty"); QTest::newRow("length") << false; QTest::newRow("modified") << true; }
	void completionNotificationsCannotChangeVisibleState()
	{
		QFETCH(bool, dirty); auto song = Engine::getSong(); NativeSongTestAccess::setModified(song, true); bool invoked = false;
		const auto mutate = [&] { if (invoked || song->isLoadingProject() || !NativeSongTestAccess::isCreating(song)) { return; } invoked = true; new AutomationTrack(song); };
		QMetaObject::Connection connection; if (dirty) { connection = connect(song, &Song::modified, this, mutate); } else { connection = connect(song, &Song::lengthChanged, this, [&](int) { mutate(); }); }
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY_EXCEPTION_THROWN(song->createEmptyProject(), std::runtime_error); QVERIFY(invoked); QVERIFY(released(song)); song->createEmptyProject(); QVERIFY(song->tracks().empty());
	}
	void headlessCreationDoesNotDispatchQueuedWork()
	{
		bool queued = false; QTimer::singleShot(0, this, [&] { queued = true; }); { FileResourceIO::Scope work({}, true); Engine::getSong()->createEmptyProject(); QVERIFY(!queued); }
		QCoreApplication::processEvents(); QVERIFY(queued);
	}
	void earlyCancellationPreservesExistingProject()
	{
		auto song = Engine::getSong(); auto original = new AutomationTrack(song); song->setTempo(83); const auto generation = song->replacementGeneration();
		{ FileResourceIO::Scope work([] { return true; }); QVERIFY_EXCEPTION_THROWN(song->createEmptyProject(), std::runtime_error); }
		QCOMPARE(song->tracks().size(), std::size_t(1)); QCOMPARE(song->tracks()[0], static_cast<Track*>(original)); QCOMPARE(song->getTempo(), 83); QCOMPARE(song->replacementGeneration(), generation); QVERIFY(released(song));
	}
	void creationOnAnotherThreadIsRefused()
	{
		auto song = Engine::getSong(); const auto generation = song->replacementGeneration(); bool caught = false; std::thread worker([&] { try { song->createEmptyProject(); } catch (const std::logic_error&) { caught = true; } }); worker.join();
		QVERIFY(caught); QCOMPARE(song->replacementGeneration(), generation); QVERIFY(released(song));
	}
	void runtimeRecoversAndReplaysCreationFailure_data() { exceptions(); }
	void runtimeRecoversAndReplaysCreationFailure()
	{
		QFETCH(bool, nonstandard); QTemporaryDir directory; const auto command = request("new", "project.new", {{"empty", true}, {"summary", true}}); QJsonObject rejected;
		{ StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 83}})).value("ok").toBool()); QVERIFY(runtime.process(request("track", "track.add", {{"type", "automation"}, {"name", "Keep me"}})).value("ok").toBool()); const auto before = manifest(directory); bool armed = true;
			const auto connection = connect(Engine::getSong(), &Song::scaleListChanged, this, [&](int) { if (armed) { armed = false; raise(nonstandard); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
			rejected = runtime.process(command); QVERIFY(!rejected.value("ok").toBool()); QVERIFY(!runtime.stopping()); QVERIFY(!armed); QCOMPARE(Engine::getSong()->getTempo(), 83); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks()[0]->name(), QString("Keep me")); QVERIFY(released(Engine::getSong())); QCOMPARE(manifest(directory).value("history"), before.value("history")); auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected); }
		StudioRuntime reopened(directory.path()); QString error; QVERIFY(reopened.open(error)); auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected); QCOMPARE(Engine::getSong()->getTempo(), 83);
	}
	void structuredPublicationRejectsDirtyCallbackChanges()
	{
		auto song = Engine::getSong(); bool invoked = false;
		const auto connection = connect(song, &Song::modified, this, [&] { if (!invoked && song->isModified() && !NativeSongTestAccess::isCreating(song)) { invoked = true; new AutomationTrack(song); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!StudioCapabilities::execute("project.new", {{"empty", true}, {"summary", true}}).value("ok").toBool()); QVERIFY(invoked); QVERIFY(released(song));
	}
	void modifiedObserversCannotReassertDirtyStateDuringCompletion()
	{
		auto song = Engine::getSong(); NativeSongTestAccess::setModified(song, true); bool invoked = false;
		const auto connection = connect(song, &Song::modified, this, [&] { if (!invoked && NativeSongTestAccess::isCreating(song) && !song->isModified()) { invoked = true; song->setModified(); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY_EXCEPTION_THROWN(song->createEmptyProject(), std::runtime_error); QVERIFY(invoked); QVERIFY(song->isModified()); QVERIFY(released(song)); song->createEmptyProject(); QVERIFY(!song->isModified());
	}
	void lateCreationCancellationUsesNativeRuntimeRollback()
	{
		QTemporaryDir directory; StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 83}})).value("ok").toBool());
		QVERIFY(projectTemplate(userTemplate(), 97, "Cancelled default")); const auto before = manifest(directory); bool cancelled = false;
		{ FileResourceIO::Scope work([&] { cancelled = cancelled || (NativeSongTestAccess::isCreating(Engine::getSong()) && Engine::getSong()->getTempo() == 97); return cancelled; }); QVERIFY(!runtime.process(request("cancelled", "project.new", {{"summary", true}})).value("ok").toBool()); }
		QVERIFY(cancelled); QVERIFY(!runtime.stopping()); QCOMPARE(Engine::getSong()->getTempo(), 83); QVERIFY(released(Engine::getSong())); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QVERIFY(runtime.process(request("retry", "project.new", {{"summary", true}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->getTempo(), 97);
	}
	void actualBuiltinProjectCheckpointReopensAndReplays()
	{
		QTemporaryDir directory; const auto command = request("new", "project.new", {{"summary", true}}); QJsonObject created;
		{ StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); created = runtime.process(command); QVERIFY2(created.value("ok").toBool(), qPrintable(QJsonDocument(created).toJson())); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(4)); }
		StudioRuntime reopened(directory.path()); QString error; QVERIFY(reopened.open(error)); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(4)); QCOMPARE(Engine::patternStore()->tracks().size(), std::size_t(1));
		auto lead = dynamic_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0]); auto kick = dynamic_cast<InstrumentTrack*>(Engine::patternStore()->tracks()[0]); QVERIFY(lead && lead->instrument()); QVERIFY(kick && kick->instrument());
		QCOMPARE(QString::fromUtf8(lead->instrument()->descriptor()->name), QString("tripleoscillator")); QCOMPARE(QString::fromUtf8(kick->instrument()->descriptor()->name), QString("kicker")); auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, created);
	}
};
QTEST_GUILESS_MAIN(ProjectCreationLifecycleTest)
#include "ProjectCreationLifecycleTest.moc"
