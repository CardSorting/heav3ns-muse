/* Native import dispatch ownership and publication. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <stdexcept>
#include <thread>
#include <utility>
#include "AudioEngine.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "ImportFilter.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "PluginFactory.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "NativeSongTestAccess.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
namespace lmms
{
class NativeImportTestAccess
{
public:
	static bool import(const QString& path, TrackContainer* target, int pattern, QStringList filters,
		std::function<Plugin*(const QString&, void*)> instantiate)
	{ return ImportFilter::importWithFilters(path, target, pattern, std::move(filters), std::move(instantiate)); }
};
}
using namespace lmms;
namespace
{
const Plugin::Descriptor fixtureDescriptor{"import-lifecycle-fixture", "Import lifecycle fixture", "Native dispatch fault fixture", "LMMS tests", 1, Plugin::Type::ImportFilter, nullptr, "", nullptr};
class Filter final : public ImportFilter
{
public:
	Filter() : ImportFilter("unused", &fixtureDescriptor) { }
	std::function<bool(Filter*, TrackContainer*, int)> onImport;
	std::function<void()> onDestroy;
	~Filter() override { const auto callback = onDestroy; if (callback) { callback(); } }
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
protected:
	bool tryImport(TrackContainer* target) override { return tryImport(target, -1); }
	bool tryImport(TrackContainer* target, int pattern) override { const auto callback = onImport; return callback ? callback(this, target, pattern) : false; }
};
class Container final : public TrackContainer
{
public:
	Container() { setType(Type::Song); }
	QString nodeName() const override { return "import-fixture-container"; }
};
[[noreturn]] void raise(bool nonstandard) { if (nonstandard) { throw 29; } throw std::runtime_error("fixture import failure"); }
bool refused(const std::function<void()>& callback)
{ try { callback(); } catch (const std::logic_error&) { return true; } return false; }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory)
{ QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
bool midi(const QString& path)
{
	// Standard SMF, one quarter-note C4; the real native MIDI importer decodes it.
	const auto bytes = QByteArray::fromHex("4d546864000000060000000100304d54726b0000000c00903c4030803c0000ff2f00");
	QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}
class ImportFilterLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	using Factory = std::function<Plugin*(const QString&, void*)>;
	bool run(Factory factory, TrackContainer* target = nullptr, int pattern = -1, const QString& path = "input.mid", QStringList filters = {"first", "second"})
	{ return NativeImportTestAccess::import(path, target ? target : Engine::getSong(), pattern, std::move(filters), std::move(factory)); }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); journalling = Engine::projectJournal()->isJournalling();
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); Engine::projectJournal()->setJournalling(false); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void acceptedFilterRestoresJournalPolicyAndStopsFallback_data()
	{ QTest::addColumn<bool>("enabled"); QTest::newRow("disabled") << false; QTest::newRow("enabled") << true; }
	void acceptedFilterRestoresJournalPolicyAndStopsFallback()
	{
		QFETCH(bool, enabled); Engine::projectJournal()->setJournalling(enabled); int factories = 0, deaths = 0; bool guarded = false;
		QVERIFY(run([&](const QString&, void*) { ++factories; auto filter = new Filter; filter->onDestroy = [&] { ++deaths; }; filter->onImport = [&](Filter*, TrackContainer* target, int pattern) { guarded = NativeSongTestAccess::isImporting(Engine::getSong()) && !Engine::projectJournal()->isJournalling() && pattern == -1; new AutomationTrack(target); return true; }; return filter; }));
		QVERIFY(guarded); QCOMPARE(factories, 1); QCOMPARE(deaths, 1); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong())); QCOMPARE(Engine::projectJournal()->isJournalling(), enabled);
	}
	void untouchedDeclineContinuesWithIndependentUtf8Paths()
	{
		const QString path = QString::fromUtf8("\xe9\x9f\xb3\xe6\xa5\xbd.mid"); QStringList seen; int factories = 0, deaths = 0;
		QVERIFY(run([&](const QString&, void* data) { seen.append(QString::fromUtf8(static_cast<char*>(data))); static_cast<char*>(data)[0] = 'x'; const bool accepted = ++factories == 2; auto filter = new Filter; filter->onDestroy = [&] { ++deaths; }; filter->onImport = [accepted](Filter*, TrackContainer*, int) { return accepted; }; return filter; }, nullptr, -1, path));
		QCOMPARE(seen, (QStringList{path, path})); QCOMPARE(factories, 2); QCOMPARE(deaths, 2);
	}
	void missingAndNonFilterCandidatesHaveExplicitOutcomes_data()
	{ QTest::addColumn<bool>("missing"); QTest::newRow("null-result") << true; QTest::newRow("native-dummy-then-filter") << false; }
	void missingAndNonFilterCandidatesHaveExplicitOutcomes()
	{
		QFETCH(bool, missing); int factories = 0; const bool accepted = run([&](const QString&, void*) -> Plugin* { if (++factories == 1) { return missing ? nullptr : Plugin::instantiate("missing-import-lifecycle-fixture", nullptr, nullptr); } auto filter = new Filter; filter->onImport = [](Filter*, TrackContainer*, int) { return true; }; return filter; });
		QCOMPARE(accepted, !missing); QCOMPARE(factories, missing ? 1 : 2); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong()));
	}
	void declinedVisibleChangesPreventAnotherFilter_data()
	{ QTest::addColumn<int>("kind"); QTest::addColumn<bool>("factory"); QTest::newRow("filter-native-track") << 0 << false; QTest::newRow("filter-native-tempo") << 1 << false; QTest::newRow("filter-custom-container-track") << 2 << false; QTest::newRow("factory-native-track") << 0 << true; QTest::newRow("factory-native-tempo") << 1 << true; QTest::newRow("factory-custom-container-track") << 2 << true; }
	void declinedVisibleChangesPreventAnotherFilter()
	{
		QFETCH(int, kind); QFETCH(bool, factory); Container container; int factories = 0; auto song = Engine::getSong(); song->setTempo(83); auto target = kind == 2 ? static_cast<TrackContainer*>(&container) : song;
		const auto mutate = [&] { if (kind == 1) { song->setTempo(97); } else { new AutomationTrack(target); } };
		QVERIFY(!run([&](const QString&, void*) { ++factories; auto filter = new Filter; filter->onImport = [&](Filter*, TrackContainer*, int) { if (!factory) { mutate(); } return false; }; if (factory) { mutate(); } return filter; }, target));
		QCOMPARE(factories, 1); QVERIFY(song->hasErrors()); QVERIFY(!NativeSongTestAccess::isImporting(song));
	}
	void factoryAndImportExceptionsReleaseAdmission_data()
	{ QTest::addColumn<bool>("factory"); QTest::addColumn<bool>("nonstandard"); QTest::newRow("factory-standard") << true << false; QTest::newRow("factory-nonstandard") << true << true; QTest::newRow("filter-standard") << false << false; QTest::newRow("filter-nonstandard") << false << true; }
	void factoryAndImportExceptionsReleaseAdmission()
	{
		QFETCH(bool, factory); QFETCH(bool, nonstandard); Engine::projectJournal()->setJournalling(true); int deaths = 0;
		QVERIFY(!run([&](const QString&, void*) -> Plugin* { if (factory) { raise(nonstandard); } auto filter = new Filter; filter->onDestroy = [&] { ++deaths; }; filter->onImport = [nonstandard](Filter*, TrackContainer*, int) -> bool { raise(nonstandard); }; return filter; }));
		QCOMPARE(deaths, factory ? 0 : 1); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong())); QVERIFY(Engine::projectJournal()->isJournalling());
		QVERIFY(run([](const QString&, void*) { auto filter = new Filter; filter->onImport = [](Filter*, TrackContainer*, int) { return true; }; return filter; }));
	}
	void candidateRetirementAndAdoptionDoNotDoubleDelete_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("self-retirement") << 0; QTest::newRow("adopted-during-import") << 1; QTest::newRow("parented-factory-result") << 2; }
	void candidateRetirementAndAdoptionDoNotDoubleDelete()
	{
		QFETCH(int, kind); QObject adopter; QPointer<Filter> candidate; int deaths = 0, factories = 0;
		QVERIFY(!run([&](const QString&, void*) { ++factories; auto filter = new Filter; candidate = filter; connect(filter, &QObject::destroyed, this, [&] { ++deaths; }); if (kind == 2) { filter->setParent(&adopter); } filter->onImport = [&](Filter* self, TrackContainer*, int) { if (kind == 0) { delete self; } else { self->setParent(&adopter); } return true; }; return filter; }));
		QCOMPARE(factories, 1); QCOMPARE(deaths, kind == 0 ? 1 : 0); if (candidate) { QCOMPARE(candidate->parent(), &adopter); delete candidate.data(); } QCOMPARE(deaths, 1); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong()));
	}
	void destinationRetirementIsCheckedAtEveryReturnedStage_data()
	{ QTest::addColumn<int>("stage"); QTest::newRow("factory") << 0; QTest::newRow("import") << 1; QTest::newRow("destructor") << 2; }
	void destinationRetirementIsCheckedAtEveryReturnedStage()
	{
		QFETCH(int, stage); auto target = new Container; const QPointer<Container> owner(target); int factories = 0, deaths = 0;
		QVERIFY(!run([&](const QString&, void*) { ++factories; auto filter = new Filter; filter->onDestroy = [&] { ++deaths; if (stage == 2 && owner) { delete owner.data(); } }; filter->onImport = [&](Filter*, TrackContainer*, int) { if (stage == 1) { delete owner.data(); } return true; }; if (stage == 0) { delete owner.data(); } return filter; }, target));
		QVERIFY(!owner); QCOMPARE(factories, 1); QCOMPARE(deaths, 1); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong()));
	}
	void existingSongAndPatternRowsCannotBeRetired_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern-row") << true; }
	void existingSongAndPatternRowsCannotBeRetired()
	{
		QFETCH(bool, pattern); const QPointer<Track> prior(new AutomationTrack(pattern ? static_cast<TrackContainer*>(Engine::patternStore()) : Engine::getSong())); int factories = 0;
		QVERIFY(!run([&](const QString&, void*) { ++factories; auto filter = new Filter; filter->onImport = [&](Filter*, TrackContainer*, int) { delete prior.data(); return true; }; return filter; }));
		QVERIFY(!prior); QCOMPARE(factories, 1); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong()));
	}
	void originalRosterReorderingIsRefused()
	{
		auto song = Engine::getSong(); auto first = new AutomationTrack(song); new AutomationTrack(song); bool moved = false;
		QVERIFY(!run([&](const QString&, void*) { auto filter = new Filter; filter->onImport = [&](Filter*, TrackContainer*, int) { moved = song->moveTrack(first, 1); return true; }; return filter; })); QVERIFY(moved); QVERIFY(!NativeSongTestAccess::isImporting(song));
	}
	void callbacksCannotNestProjectReplacementOrImport()
	{
		auto song = Engine::getSong(); bool invoked = false;
		QVERIFY(run([&](const QString&, void*) { auto filter = new Filter; filter->onImport = [&](Filter*, TrackContainer*, int) {
			invoked = true; const bool nested = run([](const QString&, void*) -> Plugin* { return nullptr; }); if (nested || !NativeSongTestAccess::isImporting(song)) { throw std::runtime_error("nested import admission failed"); }
			if (song->loadProject("missing.mmp") || song->saveProjectFile(configuration.filePath("nested.mmp"))) { throw std::runtime_error("replacement admitted during import"); }
			if (!refused([&] { song->clearProject(); }) || !refused([&] { song->createEmptyProject(); }) || !refused([&] { song->createNewProject(); }) || !refused([&] { song->createNewProjectFromTemplate("missing.mpt"); })) { throw std::runtime_error("creation or clear admitted during import"); } return true;
		}; return filter; })); QVERIFY(invoked); QVERIFY(!NativeSongTestAccess::isImporting(song));
	}
	void borrowedInputPathIsCopiedBeforeCooperativeWork()
	{
		QString path = "original.mid", received; bool changed = false;
		{ FileResourceIO::Scope work([&] { if (!changed && NativeSongTestAccess::isImporting(Engine::getSong())) { changed = true; path = "changed.mid"; } return false; }); QVERIFY(run([&](const QString&, void* data) { received = QString::fromUtf8(static_cast<char*>(data)); auto filter = new Filter; filter->onImport = [](Filter*, TrackContainer*, int) { return true; }; return filter; }, nullptr, -1, path)); }
		QVERIFY(changed); QCOMPARE(received, QString("original.mid"));
	}
	void cancellationBeforeAndAfterFilterRestoresJournal_data()
	{ QTest::addColumn<bool>("late"); QTest::newRow("before-factory") << false; QTest::newRow("after-filter") << true; }
	void cancellationBeforeAndAfterFilterRestoresJournal()
	{
		QFETCH(bool, late); bool imported = false; int factories = 0; Engine::projectJournal()->setJournalling(true);
		{ FileResourceIO::Scope work([&] { return !late || imported; }); QVERIFY(!run([&](const QString&, void*) { ++factories; auto filter = new Filter; filter->onImport = [&](Filter*, TrackContainer*, int) { imported = true; return true; }; return filter; })); }
		QCOMPARE(factories, late ? 1 : 0); QCOMPARE(imported, late); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong())); QVERIFY(Engine::projectJournal()->isJournalling());
	}
	void cancellationCallbackExceptionsReleaseAdmission_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void cancellationCallbackExceptionsReleaseAdmission()
	{
		QFETCH(bool, nonstandard); Engine::projectJournal()->setJournalling(true); bool invoked = false;
		{ FileResourceIO::Scope work([&]() -> bool { invoked = true; raise(nonstandard); }); QVERIFY(!run([](const QString&, void*) -> Plugin* { return nullptr; })); }
		QVERIFY(invoked); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong())); QVERIFY(Engine::projectJournal()->isJournalling());
	}
	void headlessDispatchDoesNotRunQueuedWork()
	{
		bool queued = false; QTimer::singleShot(0, this, [&] { queued = true; });
		{ FileResourceIO::Scope work({}, true); QVERIFY(run([](const QString&, void*) { auto filter = new Filter; filter->onImport = [](Filter*, TrackContainer*, int) { return true; }; return filter; })); QVERIFY(!queued); }
		QCoreApplication::processEvents(); QVERIFY(queued);
	}
	void foreignThreadImportIsRefusedBeforeFactories()
	{
		int factories = 0; bool accepted = true; std::thread worker([&] { accepted = run([&](const QString&, void*) -> Plugin* { ++factories; return nullptr; }); }); worker.join(); QVERIFY(!accepted); QCOMPARE(factories, 0); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong()));
	}
	void mixerContextReplacementCannotMasqueradeAsSameMaster_data()
	{ QTest::addColumn<int>("stage"); QTest::newRow("factory") << 0; QTest::newRow("filter") << 1; QTest::newRow("destructor") << 2; }
	void mixerContextReplacementCannotMasqueradeAsSameMaster()
	{
		QFETCH(int, stage); int factories = 0;
		QVERIFY(!run([&](const QString&, void*) { ++factories; auto filter = new Filter; filter->onDestroy = [stage] { if (stage == 2) { Engine::mixer()->clear(); } }; filter->onImport = [stage](Filter*, TrackContainer*, int) { if (stage == 1) { Engine::mixer()->clear(); } return true; }; if (stage == 0) { Engine::mixer()->clear(); } return filter; }));
		QCOMPARE(factories, 1); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong()));
	}
	void filterDestructionCannotChangePublishedVisibleState_data()
	{ QTest::addColumn<bool>("accepted"); QTest::newRow("accepted") << true; QTest::newRow("declined") << false; }
	void filterDestructionCannotChangePublishedVisibleState()
	{
		QFETCH(bool, accepted); int factories = 0; Engine::getSong()->setTempo(83);
		QVERIFY(!run([&](const QString&, void*) { ++factories; auto filter = new Filter; filter->onImport = [accepted](Filter*, TrackContainer*, int) { return accepted; }; filter->onDestroy = [] { Engine::getSong()->setTempo(97); }; return filter; }));
		QCOMPARE(factories, 1); QCOMPARE(Engine::getSong()->getTempo(), 97); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong()));
	}
	void currentPatternChoiceIsPinnedBeforeCallbacks()
	{
		Track::create(Track::Type::Pattern, Engine::getSong()); Track::create(Track::Type::Pattern, Engine::getSong()); auto store = Engine::patternStore(); store->setCurrentPattern(0); int received = -1; bool changed = false;
		{ FileResourceIO::Scope work([&] { if (!changed && NativeSongTestAccess::isImporting(Engine::getSong())) { changed = true; store->setCurrentPattern(1); } return false; }); QVERIFY(run([&](const QString&, void*) { auto filter = new Filter; filter->onImport = [&](Filter*, TrackContainer*, int pattern) { received = pattern; return true; }; return filter; }, store)); }
		QVERIFY(changed); QCOMPARE(received, 0); QCOMPARE(store->currentPattern(), 1);
	}
	void emptyPatternDestinationAdoptsItsNewNativeColumn()
	{
		QVERIFY(run([](const QString&, void*) { auto filter = new Filter; filter->onImport = [](Filter*, TrackContainer* target, int pattern) { if (pattern != 0) { return false; } Track::create(Track::Type::Pattern, Engine::getSong()); new AutomationTrack(target); return true; }; return filter; }, Engine::patternStore()));
		QCOMPARE(Engine::patternStore()->numOfPatterns(), 1); QCOMPARE(Engine::patternStore()->tracks().size(), std::size_t(1)); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong()));
	}
	void selectedColumnReplacementIsRefusedAtReturnedStages_data()
	{ QTest::addColumn<int>("stage"); QTest::newRow("factory") << 0; QTest::newRow("filter") << 1; QTest::newRow("destructor") << 2; }
	void selectedColumnReplacementIsRefusedAtReturnedStages()
	{
		QFETCH(int, stage); Track::create(Track::Type::Pattern, Engine::getSong()); Track::create(Track::Type::Pattern, Engine::getSong()); auto store = Engine::patternStore(); const QPointer<PatternTrack> selected(PatternTrack::findPatternTrack(1)); int factories = 0;
		const auto replace = [&] { delete selected.data(); Track::create(Track::Type::Pattern, Engine::getSong()); };
		QVERIFY(!run([&](const QString&, void*) { ++factories; auto filter = new Filter; filter->onImport = [&](Filter*, TrackContainer*, int) { if (stage == 1) { replace(); } return true; }; filter->onDestroy = [&] { if (stage == 2) { replace(); } }; if (stage == 0) { replace(); } return filter; }, store, 1));
		QVERIFY(!selected); QCOMPARE(factories, 1); QCOMPARE(store->numOfPatterns(), 2); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong()));
	}
	void actualMidiPublicationFailureRecoversAndReplays_data()
	{ QTest::addColumn<int>("action"); QTest::newRow("standard") << 0; QTest::newRow("nonstandard") << 1; QTest::newRow("visible-tempo-edit") << 2; QTest::newRow("project-replacement") << 3; }
	void actualMidiPublicationFailureRecoversAndReplays()
	{
		QFETCH(int, action);
		if (getPluginFactory()->pluginInfo("midiimport").isNull()) { QSKIP("Requires the built native MIDI import plugin"); }
		QTemporaryDir directory, input; const auto path = input.filePath("incoming.mid"); QVERIFY(midi(path)); const auto command = request("import", "project.import", {{"path", path}, {"summary", true}}); QJsonObject rejected;
		{ StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 83}})).value("ok").toBool()); QVERIFY(runtime.process(request("track", "track.add", {{"type", "automation"}, {"name", "Keep me"}})).value("ok").toBool()); const auto before = manifest(directory); bool armed = true, prepared = false;
			const auto connection = connect(Engine::getSong(), &Song::modified, this, [&] { if (armed && prepared && !NativeSongTestAccess::isImporting(Engine::getSong()) && Engine::getSong()->isModified()) { armed = false; if (action < 2) { raise(action == 1); } else if (action == 2) { Engine::getSong()->setTempo(97); } else { Engine::getSong()->createEmptyProject(); } } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
			{ FileResourceIO::Scope work([&] { if (NativeSongTestAccess::isImporting(Engine::getSong()) && Engine::getSong()->tracks().size() > 1) { prepared = true; NativeSongTestAccess::setModified(Engine::getSong(), false); } return false; }); rejected = runtime.process(command); }
			QVERIFY(prepared); QVERIFY(!armed); QVERIFY(!rejected.value("ok").toBool()); QVERIFY(!runtime.stopping()); QCOMPARE(Engine::getSong()->getTempo(), 83); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks()[0]->name(), QString("Keep me")); QVERIFY(!NativeSongTestAccess::isImporting(Engine::getSong())); QCOMPARE(manifest(directory).value("history"), before.value("history")); auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected); }
		StudioRuntime reopened(directory.path()); QString error; QVERIFY(reopened.open(error)); auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected); QCOMPARE(Engine::getSong()->getTempo(), 83);
	}
};
QTEST_GUILESS_MAIN(ImportFilterLifecycleTest)
#include "ImportFilterLifecycleTest.moc"
