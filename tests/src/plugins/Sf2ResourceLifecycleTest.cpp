#include "../core/NativeSongTestAccess.h"
/* Native SoundFont file/settings callback lifetimes. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <stdexcept>
#include <thread>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "PluginFactory.h"
#include "Sf2Player.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace lmms
{
class Sf2ResourceTestAccess
{
public:
	static bool mutexesAvailableFromAnotherThread(Sf2Instrument* native)
	{
		bool load = false, synth = false; std::thread observer([&] { load = native->m_loadMutex.tryLock(); if (load) { native->m_loadMutex.unlock(); } synth = native->m_synthMutex.tryLock(); if (synth) { native->m_synthMutex.unlock(); } }); observer.join(); return load && synth;
	}
};
}
namespace
{
class Sf2Track final : public InstrumentTrack
{
public:
	Sf2Track() : InstrumentTrack(Engine::getSong()) { }
private:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new Sf2Instrument(this); }
};
QDomElement settings(QDomDocument& document, const QString& path)
{
	auto node = document.createElement("sf2player"); document.appendChild(node); node.setAttribute("src", path); node.setAttribute("bank", 1024); node.setAttribute("patch", 7); node.setAttribute("gain", .73); node.setAttribute("reverbOn", 1); node.setAttribute("reverbRoomSize", .37); node.setAttribute("reverbDamping", .43); node.setAttribute("reverbWidth", .61); node.setAttribute("reverbLevel", .47); node.setAttribute("chorusOn", 1); node.setAttribute("chorusNum", 5); node.setAttribute("chorusLevel", 3.3); node.setAttribute("chorusSpeed", 1.1); node.setAttribute("chorusDepth", 17); return node;
}
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
}
class PublicationFault final : public QObject
{
	Q_OBJECT
public:
	bool arm = false, faulted = false;
public slots:
	void fail() { if (arm) { arm = false; faulted = true; throw std::runtime_error("Native SoundFont publication fixture"); } }
};

class Sf2ResourceLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QString bank() const { return QString::fromUtf8(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2"; }
	Sf2Instrument* seed() { auto track = new Sf2Track; auto native = dynamic_cast<Sf2Instrument*>(track->loadInstrument("sf2player")); if (!native) { throw std::runtime_error("Native SoundFont fixture was not selected"); } return native; }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setSF2File(""); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); QVERIFY(QFile::exists(bank())); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); NativeSongTestAccess::setProjectFileName(Engine::getSong(), ""); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void nativeControlsProgramsAndAcceptedSnapshotRoundTrip()
	{
		auto native = seed(); QDomDocument document; native->loadSettings(settings(document, bank())); QVERIFY(!Engine::getSong()->hasFatalErrors()); QCOMPARE(native->programSnapshot().size(), std::size_t(2)); QCOMPARE(native->getBank(), 1024); QCOMPARE(native->getPatch(), 7); const auto resource = native->fileResources()[0]; QVERIFY(resource.identity); QDomDocument saved; auto node = saved.createElement("sf2player"); saved.appendChild(node); native->saveSettings(saved, node); QCOMPARE(node.attribute("bank").toInt(), 1024); QCOMPARE(node.attribute("patch").toInt(), 7); QCOMPARE(node.attribute("gain").toFloat(), .73f); auto restored = seed(); restored->loadSettings(node); QCOMPARE(restored->getBank(), 1024); QCOMPARE(restored->getPatch(), 7); QCOMPARE(restored->programSnapshot().size(), std::size_t(2));
	}
	void fileWorkNeverHoldsEitherInstanceMutex_data()
	{ QTest::addColumn<bool>("reload"); QTest::newRow("bank-capture") << false; QTest::newRow("snapshot-verification") << true; }
	void fileWorkNeverHoldsEitherInstanceMutex()
	{
		QFETCH(bool, reload); auto native = seed(); QString error; if (reload) { QVERIFY(native->setFileResource("soundfont", bank(), error)); } int checks = 0; bool available = true; { FileResourceIO::Scope work([&] { ++checks; available = Sf2ResourceTestAccess::mutexesAvailableFromAnotherThread(native) && available; return false; }); if (reload) { native->reloadSynth(); } else { QVERIFY(native->setFileResource("soundfont", bank(), error)); } } QVERIFY(checks > 2); QVERIFY(available);
	}
	void fileWorkCanReturnAfterOwnerOrTrackRetirement_data()
	{ QTest::addColumn<bool>("reload"); QTest::addColumn<bool>("track"); QTest::newRow("capture-owner") << false << false; QTest::newRow("capture-track") << false << true; QTest::newRow("verify-owner") << true << false; QTest::newRow("verify-track") << true << true; }
	void fileWorkCanReturnAfterOwnerOrTrackRetirement()
	{
		QFETCH(bool, reload); QFETCH(bool, track); const QPointer<Sf2Instrument> owner(seed()); const QPointer<InstrumentTrack> parent(owner->instrumentTrack()); auto active = owner.data(); QString error; if (reload) { QVERIFY(active->setFileResource("soundfont", bank(), error)); } bool retired = false; { FileResourceIO::Scope work([&] { if (!retired) { retired = true; if (track) { delete parent.data(); } else { delete owner.data(); } } return false; }); if (reload) { QVERIFY_EXCEPTION_THROWN(active->reloadSynth(), std::runtime_error); } else { QVERIFY_EXCEPTION_THROWN(active->setFileResource("soundfont", bank(), error), std::runtime_error); } } QVERIFY(retired); QVERIFY(!owner); QVERIFY(Engine::audioEngine()->canJoinBackendCallbacks());
	}
	void allSettingsFieldsObserveReturnedLifetimes_data()
	{ QTest::addColumn<int>("field"); QTest::addColumn<bool>("saving"); for (int field = 0; field < 13; ++field) { QTest::newRow(qPrintable(QString::number(field) + "-load")) << field << false; QTest::newRow(qPrintable(QString::number(field) + "-save")) << field << true; } }
	void allSettingsFieldsObserveReturnedLifetimes()
	{
		QFETCH(int, field); QFETCH(bool, saving); const char* keys[] = {"bank", "patch", "gain", "reverbOn", "reverbRoomSize", "reverbDamping", "reverbWidth", "reverbLevel", "chorusOn", "chorusNum", "chorusLevel", "chorusSpeed", "chorusDepth"}; const QPointer<Sf2Instrument> owner(seed()); auto active = owner.data(); const QPointer<InstrumentTrack> track(owner->instrumentTrack()); const auto models = owner->findChildren<AutomatableModel*>(QString{}, Qt::FindDirectChildrenOnly); QCOMPARE(models.size(), 13); QDomDocument document; auto node = settings(document, bank()); bool retired = false;
		if (saving) { for (const auto key : keys) { node.removeAttribute(key); } FileResourceIO::Scope work([&] { if (!retired && node.hasAttribute(keys[field])) { retired = true; delete track.data(); } return false; }); QVERIFY_EXCEPTION_THROWN(active->saveSettings(document, node), std::runtime_error); }
		else { connect(models[field], &AutomatableModel::dataChanged, this, [&] { if (!retired) { retired = true; delete track.data(); } }); QVERIFY_EXCEPTION_THROWN(active->loadSettings(node), std::runtime_error); } QVERIFY(retired); QVERIFY(!owner); QVERIFY(!track);
	}
	void weakSignalRestorationSurvivesRetirementInsideBlockedBankLoad()
	{
		const QPointer<Sf2Instrument> owner(seed()); auto active = owner.data(); const QPointer<InstrumentTrack> track(owner->instrumentTrack()); bool returnedFile = false, retired = false; connect(active, &Sf2Instrument::fileChanged, this, [&] { returnedFile = true; }); QDomDocument document; const auto node = settings(document, bank()); { FileResourceIO::Scope work([&] { if (returnedFile && !retired) { retired = true; delete track.data(); } return false; }); QVERIFY_EXCEPTION_THROWN(active->loadSettings(node), std::runtime_error); } QVERIFY(returnedFile); QVERIFY(retired); QVERIFY(!owner);
	}
	void labelPropertyCallbacksCannotResumeOnDeadModels_data()
	{ QTest::addColumn<bool>("patch"); QTest::newRow("bank-labels") << false; QTest::newRow("patch-labels") << true; }
	void labelPropertyCallbacksCannotResumeOnDeadModels()
	{
		QFETCH(bool, patch); const QPointer<Sf2Instrument> owner(seed()); auto active = owner.data(); const QPointer<InstrumentTrack> track(owner->instrumentTrack()); bool retired = false; connect(active->childModel(patch ? "patch" : "bank"), &AutomatableModel::propertiesChanged, this, [&] { if (!retired) { retired = true; delete track.data(); } }); QString error; QVERIFY_EXCEPTION_THROWN(active->setFileResource("soundfont", bank(), error), std::runtime_error); QVERIFY(retired); QVERIFY(!owner);
	}
	void fileAndPatchNotificationsCheckReturnedOwnership_data()
	{ QTest::addColumn<int>("signal"); QTest::newRow("fileLoading") << 0; QTest::newRow("fileChanged") << 1; QTest::newRow("patchChanged") << 2; }
	void fileAndPatchNotificationsCheckReturnedOwnership()
	{
		QFETCH(int, signal); const QPointer<Sf2Instrument> owner(seed()); auto active = owner.data(); const QPointer<InstrumentTrack> track(owner->instrumentTrack()); bool retired = false; const auto retire = [&] { if (!retired) { retired = true; delete track.data(); } }; if (signal == 0) { connect(active, &Sf2Instrument::fileLoading, this, retire); } if (signal == 1) { connect(active, &Sf2Instrument::fileChanged, this, retire); } if (signal == 2) { connect(active, &Sf2Instrument::patchChanged, this, retire); } QString error; QVERIFY_EXCEPTION_THROWN(active->setFileResource("soundfont", bank(), error), std::runtime_error); QVERIFY(retired); QVERIFY(!owner);
	}
	void nestedResourcesSettingsAndReloadAreRefused()
	{
		auto native = seed(); bool nested = false; QDomDocument document; const auto node = settings(document, bank()); const auto connection = connect(native, &Sf2Instrument::fileLoading, this, [&] { if (nested) { return; } nested = true; QString error; QVERIFY_EXCEPTION_THROWN(native->setFileResource("soundfont", {}, error), std::logic_error); QVERIFY_EXCEPTION_THROWN(native->loadSettings(node), std::logic_error); QDomDocument saved; auto element = saved.createElement("sf2player"); QVERIFY_EXCEPTION_THROWN(native->saveSettings(saved, element), std::logic_error); QVERIFY_EXCEPTION_THROWN(native->reloadSynth(), std::logic_error); }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QString error; QVERIFY(native->setFileResource("soundfont", bank(), error)); QVERIFY(nested);
	}
	void cancellationAndExceptionsReleaseResourceAdmission_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("cancel") << 0; QTest::newRow("standard") << 1; QTest::newRow("nonstandard") << 2; }
	void cancellationAndExceptionsReleaseResourceAdmission()
	{
		QFETCH(int, kind); auto native = seed(); QString error; bool caught = false; try { FileResourceIO::Scope work([&] { if (kind == 1) { throw std::runtime_error("Native SoundFont work fixture"); } if (kind == 2) { throw 131; } return true; }); native->setFileResource("soundfont", bank(), error); } catch (...) { caught = true; } QVERIFY(caught); QVERIFY(native->setFileResource("soundfont", bank(), error));
	}
	void copiedInputsAndNativeControlRootChangesAreChecked_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("borrowed-input") << 0; QTest::newRow("control-mutation") << 1; QTest::newRow("project-path") << 2; }
	void copiedInputsAndNativeControlRootChangesAreChecked()
	{
		QFETCH(int, kind); auto native = seed(); QString path = bank(), key = "soundfont", error; bool changed = false; { FileResourceIO::Scope work([&] { if (!changed) { changed = true; if (kind == 0) { path = configuration.filePath("absent.sf2"); key = "changed"; } if (kind == 1) { native->childModel("bank")->setValue(99); } if (kind == 2) { NativeSongTestAccess::setProjectFileName(Engine::getSong(), configuration.filePath("other.mmp")); } } return false; }); if (kind == 0) { QVERIFY(native->setFileResource(key, path, error)); } else { QVERIFY_EXCEPTION_THROWN(native->setFileResource(key, path, error), std::runtime_error); } } QVERIFY(changed); QCOMPARE(native->programSnapshot().size(), kind == 0 ? std::size_t(2) : std::size_t(0));
	}
	void missingCorruptAndEmptyFilesKeepNativeReplacementSemantics()
	{
		auto native = seed(); QString error; QVERIFY(native->setFileResource("soundfont", bank(), error)); QVERIFY(native->selectProgram(1024, 7, error)); const auto before = native->fileResources()[0]; QTemporaryDir source; const auto bad = source.filePath("bad.sf2"); { QFile file(bad); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("invalid soundfont"), qint64(17)); } QVERIFY(!native->setFileResource("soundfont", bad, error)); QVERIFY(native->fileResources()[0].identity == before.identity); QVERIFY(!native->setFileResource("soundfont", source.filePath("absent.sf2"), error)); QCOMPARE(native->getBank(), 1024); QCOMPARE(native->getPatch(), 7); QVERIFY(native->setFileResource("soundfont", {}, error)); QVERIFY(native->fileResources()[0].path.isEmpty()); QVERIFY(!native->fileResources()[0].identity); QVERIFY(native->programSnapshot().empty());
	}
	void programSelectionAndPreviewRemainNativeAndChecked()
	{
		auto native = seed(); QString error; QVERIFY(native->setFileResource("soundfont", bank(), error)); QVERIFY(native->selectProgram(1024, 7, error)); QCOMPARE(native->getCurrentPatchName(), QString("Native octave")); QVERIFY(!native->selectProgram(111, 99, error)); QCOMPARE(native->getBank(), 1024); Sf2Instrument::ProgramPreviewToken token; QVERIFY(native->previewProgram(0, 0, error, token)); QCOMPARE(native->getBank(), 1024); QVERIFY(native->restoreProgramPreview(token, error)); QVERIFY(!native->restoreProgramPreview(token, error)); QVERIFY(native->previewProgram(0, 0, error)); QVERIFY(native->selectProgram(0, 0, error)); QCOMPARE(native->getBank(), 0); QCOMPARE(native->getPatch(), 0);
	}
	void ownedBankSettingsRestoreAfterOriginalRemoval()
	{
		QTemporaryDir source, destination; const auto path = source.filePath("bank.sf2"); QVERIFY(QFile::copy(bank(), path)); auto native = seed(); QString error; QVERIFY(native->setFileResource("soundfont", path, error)); DataFile document(DataFile::Type::SongProject); auto node = document.createElement("sf2player"); document.content().appendChild(node); { DataFile::ResourceSourceScope saving(document); native->saveSettings(document, node); } QVERIFY(QFile::remove(path)); auto restored = seed(); { DataFile::ResourceReadScope reading(document); restored->loadSettings(node); } QCOMPARE(restored->programSnapshot().size(), std::size_t(2)); QJsonArray assets; QVERIFY(document.copyResources(destination.filePath("assets"), &assets)); QCOMPARE(assets.size(), 1); QVERIFY(document.matchesAssetReferences(assets)); restored->reloadSynth(); QCOMPARE(restored->programSnapshot().size(), std::size_t(2));
	}
	void QtEventsStayOutsideBankCaptureAndSettings()
	{
		auto native = seed(); bool queued = false; QString error; QTimer::singleShot(0, this, [&] { queued = true; }); { FileResourceIO::Scope work({}, true); QVERIFY(native->setFileResource("soundfont", bank(), error)); QVERIFY(!queued); } QCoreApplication::processEvents(); QVERIFY(queued);
	}
	void actualModuleRuntimeRecoversPublicationFailureAndRetainsOwnedBank()
	{
		QVERIFY2(!getPluginFactory()->pluginInfo("sf2player").isNull(), "The built native SoundFont module must be discoverable"); QTemporaryDir source, session; const auto path = source.filePath("bank.sf2"); QVERIFY(QFile::copy(bank(), path)); QString error; QJsonObject rejected; const auto command = request("bank", "instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", path}});
		{ StudioRuntime runtime(session.path()); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool()); QVERIFY(runtime.process(request("instrument", "instrument.load", {{"track", 0}, {"instrument", "sf2player"}})).value("ok").toBool()); auto native = static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument(); PublicationFault fault; const auto connection = connect(native, SIGNAL(fileChanged()), &fault, SLOT(fail())); QVERIFY(connection); fault.arm = true; rejected = runtime.process(command); fault.arm = false; QObject::disconnect(connection); QVERIFY(!rejected.value("ok").toBool()); QVERIFY(fault.faulted); auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected); const auto before = manifest(session); QVERIFY(!before.isEmpty()); QVERIFY(runtime.process(request("retry", "instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", path}})).value("ok").toBool()); QVERIFY(!runtime.stopping()); }
		QVERIFY(QFile::remove(path)); StudioRuntime reopened(session.path()); QVERIFY2(reopened.open(error), qPrintable(error)); QCOMPARE(static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument()->programs().size(), std::size_t(2));
	}
};
QTEST_GUILESS_MAIN(Sf2ResourceLifecycleTest)
#include "Sf2ResourceLifecycleTest.moc"
