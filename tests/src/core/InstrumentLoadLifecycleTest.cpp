/* Native instrument replacement and checked studio receipts. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "NotePlayHandle.h"
#include "PluginFactory.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "NativeSongTestAccess.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
const Plugin::Descriptor fixtureDescriptor{"instrument-load-fixture", "Instrument load fixture", "Native replacement fixture", "LMMS tests", 1, Plugin::Type::Instrument, nullptr, "", nullptr};
class LoadInstrument final : public Instrument
{
public:
	LoadInstrument(InstrumentTrack* track, const Plugin::Descriptor* desc) : Instrument(track, desc) { }
	std::function<void()> onRetire;
	~LoadInstrument() override { const auto callback = onRetire; if (callback) { callback(); } }
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
	void saveSettings(QDomDocument&, QDomElement&) override { }
	void loadSettings(const QDomElement&) override { }
	QString nodeName() const override { return "instrument-load-fixture"; }
};
class LoadTrack final : public InstrumentTrack
{
public:
	explicit LoadTrack(TrackContainer* owner) : InstrumentTrack(owner) { }
	std::function<void(LoadInstrument*)> onCreate;
	Instrument* supplied = nullptr;
	bool returnNull = false;
	int factories = 0;
	std::vector<QString> requests;
protected:
	Instrument* createInstrument(const QString& name, const Plugin::Descriptor::SubPluginFeatures::Key* key, bool) override
	{
		++factories; requests.push_back(name); if (supplied) { return supplied; } if (returnNull) { return nullptr; }
		const auto callback = onCreate; const QPointer<LoadInstrument> candidate(new LoadInstrument(this, key && key->desc ? key->desc : &fixtureDescriptor)); bool released = false;
		const auto cleanup = qScopeGuard([&] { if (!released) { delete candidate.data(); } });
		if (callback) { callback(candidate.data()); } released = true; return candidate.data();
	}
};
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
}
class InstrumentLoadLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	LoadTrack* seed()
	{ auto track = new LoadTrack(Engine::getSong()); track->loadInstrument("initial"); track->volumeModel()->setValue(42); return track; }
	QJsonObject args(int index = 0) const { return {{"track", index}, {"instrument", "tripleoscillator"}}; }
	void requireBuiltin() { QVERIFY(!getPluginFactory()->pluginInfo("tripleoscillator").isNull()); }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false);
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void replacementDetachesBeforeRetirementAndPreservesComposition()
	{
		auto track = seed(); const QPointer<Instrument> previous(track->instrument()); auto old = static_cast<LoadInstrument*>(previous.data());
		auto clip = static_cast<MidiClip*>(track->createClip(0)); const auto volume = track->volumeModel(); const auto id = volume->id(); track->midiPort()->setInputChannel(5); bool retired = false; int notifications = 0;
		old->onRetire = [&] { retired = true; QVERIFY(!track->instrument()); QVERIFY(!track->loadInstrument("nested")); };
		track->onCreate = [&](LoadInstrument*) { QVERIFY(retired); QVERIFY(!previous); QVERIFY(!track->instrument()); };
		connect(track, &InstrumentTrack::instrumentChanged, this, [&] { ++notifications; QVERIFY(track->isReplacingInstrument()); QVERIFY(!previous); QVERIFY(track->instrument()); QCOMPARE(track->volumeModel()->storedValue(), 42.f); });
		auto native = track->loadInstrument("replacement"); QVERIFY(native); QCOMPARE(track->instrument(), native); QVERIFY(!previous); QCOMPARE(notifications, 1); QVERIFY(!track->isReplacingInstrument());
		QCOMPARE(track->getClip(0), static_cast<Clip*>(clip)); QCOMPARE(track->volumeModel(), volume); QCOMPARE(volume->id(), id); QCOMPARE(track->midiPort()->inputChannel(), 5); track->onCreate = {};
	}
	void nestedLoadsAreRefusedAcrossReplacementPhases_data()
	{ QTest::addColumn<int>("phase"); QTest::newRow("retirement") << 0; QTest::newRow("factory") << 1; QTest::newRow("name") << 2; QTest::newRow("instrument") << 3; }
	void nestedLoadsAreRefusedAcrossReplacementPhases()
	{
		QFETCH(int, phase); auto track = seed(); track->setName("Before"); bool invoked = false;
		const auto callback = [&] { if (invoked) { return; } invoked = true; QVERIFY(!track->loadInstrument("nested")); };
		if (phase == 0) { static_cast<LoadInstrument*>(track->instrument())->onRetire = callback; }
		if (phase == 1) { track->onCreate = [&](LoadInstrument*) { callback(); }; }
		QMetaObject::Connection connection;
		if (phase == 2) { connection = connect(track, &Track::nameChanged, this, callback); }
		if (phase == 3) { connection = connect(track, &InstrumentTrack::instrumentChanged, this, callback); }
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(track->loadInstrument("replacement")); QVERIFY(invoked); QCOMPARE(track->factories, 2); track->onCreate = {};
	}
	void oldInstrumentRetirementCanDeleteTheTrackWithoutRelockingIt()
	{
		auto track = seed(); const QPointer<InstrumentTrack> owner(track); const QPointer<Instrument> previous(track->instrument()); bool retired = false;
		static_cast<LoadInstrument*>(track->instrument())->onRetire = [&] { retired = true; delete track; };
		QVERIFY(!track->loadInstrument("replacement")); QVERIFY(retired); QVERIFY(!owner); QVERIFY(!previous); QVERIFY(Engine::getSong()->tracks().empty());
	}
	void returnedFactoryCanRetireItsOwnerAndCleanAnUnadoptedCandidate()
	{
		auto track = seed(); const QPointer<InstrumentTrack> owner(track); QPointer<Instrument> candidate; int deaths = 0;
		track->onCreate = [&](LoadInstrument* native) { candidate = native; connect(native, &QObject::destroyed, this, [&] { ++deaths; }); delete track; };
		QVERIFY(!track->loadInstrument("replacement")); QVERIFY(!owner); QVERIFY(!candidate); QCOMPARE(deaths, 1); QVERIFY(Engine::getSong()->tracks().empty());
	}
	void failedFactoriesReleaseAdmissionAndLeaveNoSelectedInstrument_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("standard") << 0; QTest::newRow("nonstandard") << 1; QTest::newRow("null") << 2; }
	void failedFactoriesReleaseAdmissionAndLeaveNoSelectedInstrument()
	{
		QFETCH(int, failure); auto track = seed(); const QPointer<Instrument> previous(track->instrument()); QPointer<Instrument> candidate;
		track->returnNull = failure == 2; track->onCreate = [&](LoadInstrument* native) { candidate = native; if (failure == 0) { throw std::runtime_error("fixture instrument factory"); } throw 7; };
		bool caught = false; Instrument* result = nullptr; try { result = track->loadInstrument("replacement"); } catch (...) { caught = true; }
		QCOMPARE(caught, failure != 2); QVERIFY(!result); QVERIFY(!previous); QVERIFY(!candidate); QVERIFY(!track->instrument()); QVERIFY(!track->isReplacingInstrument());
		track->returnNull = false; track->onCreate = {}; QVERIFY(track->loadInstrument("retry"));
	}
	void aForeignFactoriesInstrumentIsNotOwnedByTheRequestingTrack()
	{
		auto owner = seed(); auto other = seed(); const QPointer<Instrument> foreign(other->instrument()); owner->supplied = foreign.data();
		QVERIFY(!owner->loadInstrument("foreign")); QVERIFY(!owner->instrument()); QVERIFY(foreign); QCOMPARE(other->instrument(), foreign.data()); owner->supplied = nullptr;
	}
	void independentInstrumentDeletionClearsTheNativeAccessor()
	{
		auto track = seed(); const QPointer<Instrument> previous(track->instrument()); delete previous.data(); QVERIFY(!previous); QVERIFY(!track->instrument());
		QVERIFY(track->loadInstrument("replacement")); delete track; QVERIFY(Engine::getSong()->tracks().empty());
	}
	void notificationRetirementCannotReturnADeadNativeInstrument_data()
	{
		QTest::addColumn<bool>("name"); QTest::addColumn<bool>("owner"); QTest::newRow("name-target") << true << false; QTest::newRow("name-owner") << true << true;
		QTest::newRow("instrument-target") << false << false; QTest::newRow("instrument-owner") << false << true;
	}
	void notificationRetirementCannotReturnADeadNativeInstrument()
	{
		QFETCH(bool, name); QFETCH(bool, owner); auto track = seed(); const QPointer<InstrumentTrack> weak(track); track->setName("Before"); bool invoked = false;
		const auto callback = [&] { if (invoked) { return; } invoked = true; if (owner) { delete track; } else { delete track->instrument(); } };
		const auto connection = name ? connect(track, &Track::nameChanged, this, callback) : connect(track, &InstrumentTrack::instrumentChanged, this, callback);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(!track->loadInstrument("replacement")); QVERIFY(invoked);
		if (weak) { QVERIFY(!weak->instrument()); QVERIFY(!weak->isReplacingInstrument()); }
	}
	void notificationExceptionsRetainPublishedOwnershipAndReleaseAdmission_data()
	{ QTest::addColumn<bool>("standard"); QTest::newRow("standard") << true; QTest::newRow("nonstandard") << false; }
	void notificationExceptionsRetainPublishedOwnershipAndReleaseAdmission()
	{
		QFETCH(bool, standard); auto track = seed(); const QPointer<Instrument> previous(track->instrument()); bool invoked = false;
		const auto connection = connect(track, &InstrumentTrack::instrumentChanged, this, [&] { if (invoked) { return; } invoked = true; if (standard) { throw std::runtime_error("fixture instrument notification"); } throw 7; });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool caught = false; try { track->loadInstrument("replacement"); } catch (...) { caught = true; }
		QVERIFY(caught); QVERIFY(invoked); QVERIFY(!previous); QVERIFY(track->instrument()); QVERIFY(!track->isReplacingInstrument()); QObject::disconnect(connection); QVERIFY(track->loadInstrument("retry"));
	}
	void cancellationBeforeMutationOrAfterConstructionKeepsClearOwnership_data()
	{ QTest::addColumn<bool>("prepared"); QTest::newRow("before") << false; QTest::newRow("prepared") << true; }
	void cancellationBeforeMutationOrAfterConstructionKeepsClearOwnership()
	{
		QFETCH(bool, prepared); auto track = seed(); const QPointer<Instrument> previous(track->instrument()); QPointer<Instrument> candidate;
		track->onCreate = [&](LoadInstrument* native) { candidate = native; }; FileResourceIO::Scope work([&] { return !prepared || track->factories > 1; }, false);
		QVERIFY_EXCEPTION_THROWN(track->loadInstrument("replacement"), std::runtime_error); QCOMPARE(bool(previous), !prepared); QVERIFY(!candidate); QCOMPARE(track->instrument(), previous.data()); QVERIFY(!track->isReplacingInstrument()); track->onCreate = {};
	}
	void namesAreCopiedBeforeRetirementCallbacksChangeTheirSource()
	{
		auto track = seed(); track->setName("Requested"); static_cast<LoadInstrument*>(track->instrument())->onRetire = [&] { track->setName("Changed source"); };
		QVERIFY(track->loadInstrument(track->name())); QCOMPARE(track->requests.back(), QString("Requested"));
	}
	void settingsRestoreCannotOverlapNativeReplacement_data()
	{ QTest::addColumn<bool>("preset"); QTest::newRow("settings") << false; QTest::newRow("preset") << true; }
	void settingsRestoreCannotOverlapNativeReplacement()
	{
		QFETCH(bool, preset); auto track = seed(); bool refused = false; QDomDocument document; auto element = document.createElement("instrumenttrack");
		track->onCreate = [&](LoadInstrument*) { try { if (preset) { track->replaceInstrument(DataFile(DataFile::Type::InstrumentTrackSettings)); } else { track->loadTrackSpecificSettings(element); } } catch (const std::logic_error&) { refused = true; } };
		QVERIFY(track->loadInstrument("replacement")); QVERIFY(refused); track->onCreate = {};
	}
	void songModificationDuringRenameCanRetireTheTrack()
	{
		auto target = seed(); target->setName("Before"); const QPointer<InstrumentTrack> weak(target); QPointer<Instrument> candidate; bool invoked = false; NativeSongTestAccess::setModified(Engine::getSong(), false);
		target->onCreate = [&](LoadInstrument* native) { candidate = native; };
		const auto connection = connect(Engine::getSong(), &Song::modified, this, [&] { if (!invoked) { invoked = true; delete target; } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(!target->loadInstrument("replacement")); QVERIFY(invoked); QVERIFY(!weak); QVERIFY(!candidate);
	}
	void structuredReceiptsUseTheCurrentSurvivingTrackIndex()
	{
		requireBuiltin(); auto earlier = new InstrumentTrack(Engine::getSong()); auto target = seed(); const QPointer<InstrumentTrack> weak(target);
		connect(target, &InstrumentTrack::instrumentChanged, this, [&] { delete earlier; earlier = nullptr; });
		const auto response = StudioCapabilities::execute("instrument.load", args(1)); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		QVERIFY(weak); QCOMPARE(response.value("result").toObject().value("track").toInt(), 0); QCOMPARE(Engine::getSong()->tracks().front(), static_cast<Track*>(target));
	}
	void structuredCancellationDoesNotConstructOrRetireAnInstrument()
	{
		auto target = seed(); const QPointer<Instrument> previous(target->instrument()); const auto response = StudioCapabilities::execute("instrument.load", args(), [] { return true; });
		QVERIFY(!response.value("ok").toBool()); QVERIFY(previous); QCOMPARE(target->instrument(), previous.data()); QCOMPARE(target->factories, 1);
	}
	void selectedNoteRecordingRefusesReplacementBeforeFactoryWork()
	{
		auto target = seed(); const QPointer<Instrument> previous(target->instrument()); int capture = 0; QVERIFY(target->beginNoteRecording(&capture));
		const auto release = qScopeGuard([&] { target->endNoteRecording(&capture); }); const auto response = StudioCapabilities::execute("instrument.load", args());
		QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("recording")); QVERIFY(previous); QCOMPARE(target->instrument(), previous.data()); QCOMPARE(target->factories, 1);
	}
	void projectNotificationCannotRetireTheReportedInstrument_data()
	{ QTest::addColumn<bool>("owner"); QTest::newRow("instrument") << false; QTest::newRow("track") << true; }
	void projectNotificationCannotRetireTheReportedInstrument()
	{
		QFETCH(bool, owner); requireBuiltin(); auto target = seed(); const QPointer<InstrumentTrack> weak(target); bool invoked = false;
		connect(target, &InstrumentTrack::instrumentChanged, this, [&] { NativeSongTestAccess::setModified(Engine::getSong(), false); });
		const auto connection = connect(Engine::getSong(), &Song::modified, this, [&] { if (invoked || !weak || weak->isReplacingInstrument()) { return; } invoked = true; if (owner) { delete target; } else { delete target->instrument(); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto response = StudioCapabilities::execute("instrument.load", args());
		QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); if (weak) { QVERIFY(!weak->instrument()); }
	}
	void runtimeRecoversAndCachesInstrumentNotificationFailure_data()
	{ QTest::addColumn<bool>("standard"); QTest::newRow("standard") << true; QTest::newRow("nonstandard") << false; }
	void runtimeRecoversAndCachesInstrumentNotificationFailure()
	{
		QFETCH(bool, standard); requireBuiltin(); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error));
		QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool()); QVERIFY(runtime.process(request("seed", "instrument.load", args())).value("ok").toBool());
		auto target = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().front()); bool invoked = false;
		const auto connection = connect(target, &InstrumentTrack::instrumentChanged, this, [&] { if (invoked) { return; } invoked = true; if (standard) { throw std::runtime_error("fixture instrument runtime"); } throw 7; });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("replace", "instrument.load", args()); const auto receipt = runtime.process(command);
		QVERIFY(invoked); QVERIFY(!receipt.value("ok").toBool()); auto current = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().front()); QVERIFY(current->instrument()); QCOMPARE(QString::fromUtf8(current->instrument()->descriptor()->name), QString("tripleoscillator"));
		auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); QVERIFY(!runtime.stopping());
	}
};
QTEST_GUILESS_MAIN(InstrumentLoadLifecycleTest)
#include "InstrumentLoadLifecycleTest.moc"
