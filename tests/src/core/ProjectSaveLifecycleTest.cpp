/* Native whole-project save continuations. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>
#include <functional>
#include <algorithm>
#include <stdexcept>
#include "AudioEngine.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "LfoController.h"
#include "Mixer.h"
#include "NativeTrackSaveSnapshot.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "ProjectJournal.h"
#include "SerializingObject.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "Timeline.h"
using namespace lmms;
namespace
{
const Plugin::Descriptor fixtureDescriptor{"project-save-fixture", "Project save fixture", "Native save fixture", "LMMS tests", 1, Plugin::Type::Instrument, nullptr, "", nullptr};
class SavingInstrument final : public Instrument
{
public:
	explicit SavingInstrument(InstrumentTrack* track) : Instrument(track, &fixtureDescriptor) { }
	std::function<void()> onSave;
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
	QString nodeName() const override { return "project-save-fixture"; }
	void saveSettings(QDomDocument&, QDomElement&) override { const auto callback = onSave; if (callback) { callback(); } }
	void loadSettings(const QDomElement&) override { }
};
class SavingTrack final : public InstrumentTrack
{
public:
	explicit SavingTrack(TrackContainer* owner) : InstrumentTrack(owner) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new SavingInstrument(this); }
};
class Container final : public TrackContainer
{
public:
	Container() { setType(Type::Song); }
	QString nodeName() const override { return "save-fixture-container"; }
};
class Hook final : public SerializingObjectHook
{
public:
	std::function<void()> callback;
	void saveSettings(QDomDocument&, QDomElement&) override { const auto copy = callback; if (copy) { copy(); } }
	void loadSettings(const QDomElement&) override { }
};
bool write(const QString& path, const QByteArray& bytes)
{ QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
}
class ProjectSaveLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	SavingTrack* seed(TrackContainer* owner = nullptr) { auto track = new SavingTrack(owner ? owner : Engine::getSong()); track->loadInstrument("project-save-fixture"); return track; }
	SavingInstrument* plugin(SavingTrack* track) const { return static_cast<SavingInstrument*>(track->instrument()); }
	QJsonObject save(const QString& path, const std::function<bool()>& cancelled = {}) const { return StudioCapabilities::execute("project.save", {{"path", path}}, cancelled); }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); ConfigManager::inst()->setValue("app", "disablebackup", "0"); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void emptyProjectKeepsItsCanonicalHiddenTrack_data()
	{ QTest::addColumn<QString>("extension"); QTest::newRow("xml") << QString("mmp"); QTest::newRow("compressed") << QString("mmpz"); }
	void emptyProjectKeepsItsCanonicalHiddenTrack()
	{
		QFETCH(QString, extension); QTemporaryDir directory; auto song = Engine::getSong(); const auto hidden = song->globalAutomationTrack(); QVERIFY(std::find(song->tracks().begin(), song->tracks().end(), hidden) == song->tracks().end());
		const auto path = directory.filePath("saved." + extension); const auto result = save(path); QVERIFY2(result.value("ok").toBool(), qPrintable(song->errorSummary())); QVERIFY(result.value("result").toObject().value("committed").toBool());
		DataFile file(path); QCOMPARE(file.type(), DataFile::Type::SongProject); bool found = false;
		for (auto node = file.content().firstChildElement("track"); !node.isNull(); node = node.nextSiblingElement("track")) { if (node.attribute("type").toInt() == static_cast<int>(Track::Type::HiddenAutomation)) { found = true; } }
		QVERIFY(found); QVERIFY(!song->isSavingProject());
	}
	void unrelatedHiddenTrackDoesNotAcquireCanonicalAdmission()
	{
		auto song = Engine::getSong(); AutomationTrack impostor(song, true); const auto guard = Engine::audioEngine()->requestChangesGuard();
		QVERIFY_EXCEPTION_THROWN(NativeTrackSaveSnapshot(&impostor, true), std::runtime_error); NativeTrackSaveSnapshot canonical(song->globalAutomationTrack(), true); canonical.validate();
	}
	void laterSerializerCannotChangeEarlierProjectSource_data()
	{
		QTest::addColumn<int>("action"); QTest::newRow("earlier-value") << 0; QTest::newRow("earlier-name") << 1; QTest::newRow("tempo") << 2; QTest::newRow("mixer-value") << 3; QTest::newRow("mixer-name") << 4; QTest::newRow("timeline") << 5; QTest::newRow("pattern-roster") << 6;
	}
	void laterSerializerCannotChangeEarlierProjectSource()
	{
		QFETCH(int, action); auto earlier = seed(); auto later = seed(); QTemporaryDir directory; const auto path = directory.filePath("saved.mmp"); QVERIFY(write(path, "old")); bool invoked = false;
		plugin(later)->onSave = [&] { invoked = true; if (action == 0) { earlier->volumeModel()->setValue(34); } if (action == 1) { earlier->setName("changed"); } if (action == 2) { Engine::getSong()->setTempo(179); } if (action == 3) { Engine::mixer()->mixerChannel(0)->m_volumeModel.setValue(.4f); } if (action == 4) { Engine::mixer()->mixerChannel(0)->m_name = "changed"; } if (action == 5) { Engine::getSong()->getTimeline(Song::PlayMode::Song).setLoopEnabled(true); } if (action == 6) { new AutomationTrack(Engine::patternStore()); } };
		const auto response = save(path); plugin(later)->onSave = {}; QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QCOMPARE(read(path), QByteArray("old")); QVERIFY(!Engine::getSong()->isSavingProject());
	}
	void trackRemovalDuringSerializationReturnsWithoutRosterDeadlock_data()
	{ QTest::addColumn<bool>("self"); QTest::newRow("earlier") << false; QTest::newRow("self") << true; }
	void trackRemovalDuringSerializationReturnsWithoutRosterDeadlock()
	{
		QFETCH(bool, self); auto earlier = seed(); auto later = seed(); const QPointer<SavingTrack> owner(later); QTemporaryDir directory; const auto path = directory.filePath("saved.mmp"); QVERIFY(write(path, "old")); bool invoked = false;
		plugin(later)->onSave = [&] { invoked = true; if (self) { delete later; } else { delete earlier; } };
		QVERIFY(!save(path).value("ok").toBool()); if (owner) { plugin(later)->onSave = {}; } QVERIFY(invoked); QCOMPARE(read(path), QByteArray("old")); QVERIFY(!Engine::getSong()->isSavingProject());
	}
	void nestedProjectSaveLoadAndClearAreRefused()
	{
		auto track = seed(); QTemporaryDir directory; const auto nested = directory.filePath("nested.mmp"); bool invoked = false;
		plugin(track)->onSave = [&] { invoked = true; QVERIFY(!Engine::getSong()->saveProjectFile(nested)); QVERIFY(!Engine::getSong()->loadProject(nested)); QVERIFY_EXCEPTION_THROWN(Engine::getSong()->clearProject(), std::logic_error); };
		const auto response = save(directory.filePath("saved.mmp")); plugin(track)->onSave = {}; QVERIFY(invoked); QVERIFY(response.value("ok").toBool()); QVERIFY(!QFile::exists(nested));
	}
	void serializerFailuresReleaseAdmissionForRetry_data()
	{ QTest::addColumn<bool>("standard"); QTest::newRow("standard") << true; QTest::newRow("nonstandard") << false; }
	void serializerFailuresReleaseAdmissionForRetry()
	{
		QFETCH(bool, standard); auto track = seed(); plugin(track)->onSave = [&] { if (standard) { throw std::runtime_error("fixture project serializer"); } throw 7; }; QTemporaryDir directory; const auto path = directory.filePath("saved.mmp"); QVERIFY(write(path, "old"));
		QVERIFY(!save(path).value("ok").toBool()); QCOMPARE(read(path), QByteArray("old")); QVERIFY(!Engine::getSong()->isSavingProject()); plugin(track)->onSave = {}; QVERIFY(save(path).value("ok").toBool());
	}
	void finalWorkCallbackCannotCommitChangedOrCancelledSource_data()
	{ QTest::addColumn<int>("action"); QTest::newRow("value") << 0; QTest::newRow("track") << 1; QTest::newRow("mixer") << 2; QTest::newRow("cancel") << 3; QTest::newRow("exception") << 4; }
	void finalWorkCallbackCannotCommitChangedOrCancelledSource()
	{
		QFETCH(int, action); auto track = seed(); QTemporaryDir directory; const auto path = directory.filePath("saved.mmp"); QVERIFY(write(path, "old")); bool invoked = false;
		const auto response = save(path, [&] { if (!invoked && QFile::exists(path + ".bak")) { invoked = true; if (action == 0) { track->volumeModel()->setValue(37); } if (action == 1) { delete track; } if (action == 2) { Engine::mixer()->mixerChannel(0)->m_name = "late change"; } if (action == 4) { throw 7; } } return invoked && action == 3; });
		QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QCOMPARE(read(path), QByteArray("old")); QCOMPARE(read(path + ".bak"), QByteArray("old")); QVERIFY(!Engine::getSong()->isSavingProject());
	}
	void containerOwnerRetirementDoesNotTouchDeadSaveFlags()
	{
		auto container = new Container; auto track = seed(container); const QPointer<TrackContainer> owner(container); plugin(track)->onSave = [&] { delete container; }; DataFile file(DataFile::Type::SongProject);
		QVERIFY_EXCEPTION_THROWN(container->saveState(file, file.content()), std::runtime_error); QVERIFY(!owner);
	}
	void laterMixerHookCannotPublishEarlierTrackMutation()
	{
		auto track = seed(); Hook hook; bool invoked = false; hook.callback = [&] { invoked = true; track->volumeModel()->setValue(27); }; auto mixer = Engine::mixer(); mixer->setHook(&hook); QTemporaryDir directory; const auto path = directory.filePath("saved.mmp"); QVERIFY(write(path, "old"));
		const auto response = save(path); mixer->setHook(nullptr); QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QCOMPARE(read(path), QByteArray("old")); QVERIFY(save(path).value("ok").toBool());
	}
	void mixerSerializationRefusesTopologyAndRecoversFromHookFailure()
	{
		auto mixer = Engine::mixer(); auto chain = &mixer->mixerChannel(0)->m_fxChain; Hook hook; bool invoked = false; hook.callback = [&] { invoked = true; const auto count = mixer->numChannels(); QVERIFY_EXCEPTION_THROWN(mixer->createChannel(), std::logic_error); QCOMPARE(mixer->numChannels(), count); throw std::runtime_error("fixture mixer chain"); }; chain->setHook(&hook);
		QTemporaryDir directory; QVERIFY(!save(directory.filePath("saved.mmp")).value("ok").toBool()); chain->setHook(nullptr); QVERIFY(invoked); const auto count = mixer->numChannels(); mixer->createChannel(); QCOMPARE(mixer->numChannels(), count + 1); QVERIFY(save(directory.filePath("retry.mmp")).value("ok").toBool());
	}
	void controllerSerializersCannotRetireRackEntries_data()
	{ QTest::addColumn<bool>("self"); QTest::newRow("earlier") << false; QTest::newRow("self") << true; }
	void controllerSerializersCannotRetireRackEntries()
	{
		QFETCH(bool, self); auto song = Engine::getSong(); auto earlier = new LfoController(song); auto later = new LfoController(song); song->addController(earlier); song->addController(later); const QPointer<Controller> owner(later); Hook hook; bool invoked = false;
		hook.callback = [&] { invoked = true; song->removeController(self ? later : earlier); }; later->setHook(&hook); QTemporaryDir directory; const auto path = directory.filePath("saved.mmp"); QVERIFY(write(path, "old"));
		QVERIFY(!save(path).value("ok").toBool()); if (owner) { owner->setHook(nullptr); } QVERIFY(invoked); QCOMPARE(read(path), QByteArray("old")); QVERIFY(!song->isSavingProject());
	}
	void laterControllerCannotRenameAnEarlierSerializedController()
	{
		auto song = Engine::getSong(); auto earlier = new LfoController(song); auto later = new LfoController(song); song->addController(earlier); song->addController(later); Hook hook; bool invoked = false;
		hook.callback = [&] { invoked = true; earlier->setName("changed after serialization"); }; later->setHook(&hook); QTemporaryDir directory; const auto path = directory.filePath("saved.mmp"); QVERIFY(write(path, "old"));
		const auto response = save(path); later->setHook(nullptr); QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QCOMPARE(read(path), QByteArray("old"));
	}
	void builtinProjectCheckpointRetainsNativePatternAndHiddenState()
	{
		QTemporaryDir directory; QString error; StudioRuntime runtime(directory.filePath("session")); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool()); QVERIFY(runtime.process(request("instrument", "instrument.load", {{"track", 0}, {"instrument", "tripleoscillator"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("pattern", "track.add", {{"type", "pattern"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("pattern-instrument", "track.add", {{"container", "pattern"}, {"type", "instrument"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("pattern-plugin", "instrument.load", {{"container", "pattern"}, {"track", 0}, {"instrument", "tripleoscillator"}})).value("ok").toBool());
		auto instrument = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().front());
		QVERIFY(runtime.process(request("hidden", "automation.global.point", {{"parameter", static_cast<int>(instrument->pitchModel()->id())}, {"position", 0}, {"value", 100}})).value("ok").toBool());
		const auto path = directory.filePath("saved.mmp"); QVERIFY(runtime.process(request("save", "project.save", {{"path", path}})).value("ok").toBool()); DataFile file(path); QCOMPARE(file.type(), DataFile::Type::SongProject); QVERIFY(!file.content().firstChildElement("trackcontainer").isNull());
		const auto hiddenCount = Engine::getSong()->globalAutomationTrack()->numOfClips(); QJsonArray assets; const auto checkpoint = directory.filePath("checkpoint.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(checkpoint, false, true, &assets)); QVERIFY(Engine::getSong()->loadProject(checkpoint));
		QCOMPARE(Engine::patternStore()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->globalAutomationTrack()->numOfClips(), hiddenCount);
		auto restored = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().front()); const auto global = StudioCapabilities::execute("automation.global.inspect", {{"parameter", static_cast<int>(restored->pitchModel()->id())}, {"position", 0}}); QVERIFY(global.value("ok").toBool()); QCOMPARE(global.value("result").toObject().value("value").toDouble(), 100.0);
	}
	void runtimeReplayDoesNotRewriteACommittedProject()
	{
		QTemporaryDir directory; QString error; QJsonObject command, receipt; const auto path = directory.filePath("saved.mmp");
		{ StudioRuntime runtime(directory.filePath("session")); QVERIFY(runtime.open(error)); command = request("save", "project.save", {{"path", path}}); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); QVERIFY(receipt.value("result").toObject().value("committed").toBool()); QVERIFY(write(path, "external change")); auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); }
		StudioRuntime reopened(directory.filePath("session")); QVERIFY(reopened.open(error)); auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); QCOMPARE(read(path), QByteArray("external change"));
	}
};
QTEST_GUILESS_MAIN(ProjectSaveLifecycleTest)
#include "ProjectSaveLifecycleTest.moc"
