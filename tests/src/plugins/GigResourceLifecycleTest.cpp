#include "../core/NativeSongTestAccess.h"
/* Native GIG resource/settings lifetimes. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "GigPlayer.h"
#include "InstrumentTrack.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PluginFactory.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "../core/GigNativeFixture.h"
using namespace lmms;
namespace lmms
{
class GigResourceTestAccess
{
public:
	static bool mutexesAvailableFromAnotherThread(GigInstrument* native)
	{
		bool available = false; std::thread observer([&] { const bool synth = native->m_synthMutex.tryLock(); if (synth) { native->m_synthMutex.unlock(); } const bool notes = native->m_notesMutex.tryLock(); if (notes) { native->m_notesMutex.unlock(); } available = synth && notes; }); observer.join(); return available;
	}
};
}
namespace
{
class NativeTrack final : public InstrumentTrack
{
public:
	NativeTrack() : InstrumentTrack(Engine::getSong()) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new GigInstrument(this); }
};
QDomElement settings(QDomDocument& document, const QString& path)
{ auto node = document.createElement("gigplayer"); document.appendChild(node); node.setAttribute("src", path); node.setAttribute("bank", 2); node.setAttribute("patch", 7); node.setAttribute("gain", .73); return node; }
std::shared_ptr<const FileResourceSnapshot> snapshot(const Plugin::FileResource& resource) { return std::static_pointer_cast<const FileResourceSnapshot>(resource.identity); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
double energy(GigInstrument* native)
{
	const auto frames = Engine::audioEngine()->framesPerPeriod(); std::vector<SampleFrame> pcm(frames); NotePlayHandle voice(native->instrumentTrack(), 0, frames * 4, Note(TimePos(48), TimePos(), 69)); native->playNote(&voice, pcm.data()); double total = 0;
	for (int period = 0; period < 4; ++period) { native->play(pcm.data()); for (const auto& frame : pcm) { for (int channel = 0; channel < 2; ++channel) { if (!std::isfinite(frame[channel])) { throw std::runtime_error("Nonfinite native GIG output"); } total += std::abs(frame[channel]); } } } return total;
}
}
class GigPublicationFault final : public QObject
{
	Q_OBJECT
public:
	bool arm = false, faulted = false;
public slots:
	void fail() { if (arm) { arm = false; faulted = true; throw std::runtime_error("Native GIG publication fixture"); } }
};
class GigResourceLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration, banks;
	QString bank(bool split = false) const { return banks.filePath(split ? "split.gig" : "single.gig"); }
	GigInstrument* seed() { auto track = new NativeTrack; auto native = dynamic_cast<GigInstrument*>(track->loadInstrument("gigplayer")); if (!native) { throw std::runtime_error("Native GIG was not selected"); } return native; }
	Instrument* current() const { return static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument(); }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); QVERIFY(banks.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); QString error; QVERIFY2(test::gigNative::create(bank(), false, error), qPrintable(error)); QVERIFY2(test::gigNative::create(bank(true), true, error), qPrintable(error)); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); NativeSongTestAccess::setProjectFileName(Engine::getSong(), ""); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void nativeControlsProgramsAndAcceptedGroupsRoundTrip_data()
	{ QTest::addColumn<bool>("split"); QTest::newRow("single-file") << false; QTest::newRow("companion-pool") << true; }
	void nativeControlsProgramsAndAcceptedGroupsRoundTrip()
	{
		QFETCH(bool, split); auto native = seed(); QDomDocument document; native->loadSettings(settings(document, bank(split))); QVERIFY(!Engine::getSong()->hasFatalErrors()); QCOMPARE(native->getBank(), 2); QCOMPARE(native->getPatch(), 7); QCOMPARE(native->programSnapshot().size(), std::size_t(1)); QCOMPARE(native->getCurrentPatchName(), QString("Native persistent bank")); const auto accepted = snapshot(native->fileResources()[0]); QVERIFY(accepted); QCOMPARE(accepted->members().size(), split ? 2 : 1); QDomDocument saved; auto node = saved.createElement("gigplayer"); saved.appendChild(node); native->saveSettings(saved, node); QCOMPARE(node.attribute("gain").toFloat(), .73f); auto restored = seed(); restored->loadSettings(node); QCOMPARE(restored->getBank(), 2); QCOMPARE(restored->getPatch(), 7); QCOMPARE(restored->programSnapshot().size(), std::size_t(1));
	}
	void captureAndCompanionWorkHoldNeitherInstanceMutex_data()
	{ QTest::addColumn<bool>("split"); QTest::newRow("single-file") << false; QTest::newRow("companion-pool") << true; }
	void captureAndCompanionWorkHoldNeitherInstanceMutex()
	{
		QFETCH(bool, split); auto native = seed(); int checks = 0; bool available = true; QString error; { FileResourceIO::Scope work([&] { ++checks; available = GigResourceTestAccess::mutexesAvailableFromAnotherThread(native) && available; return false; }); QVERIFY(native->setFileResource("bank", bank(split), error)); } QVERIFY(checks > 3); QVERIFY(available);
	}
	void fileWorkCanReturnAfterOwnerOrTrackRetirement_data()
	{ QTest::addColumn<bool>("track"); QTest::addColumn<int>("stage"); QTest::newRow("entry-owner") << false << 1; QTest::newRow("entry-track") << true << 1; QTest::newRow("capture-owner") << false << 4; QTest::newRow("capture-track") << true << 4; }
	void fileWorkCanReturnAfterOwnerOrTrackRetirement()
	{
		QFETCH(bool, track); QFETCH(int, stage); const QPointer<GigInstrument> owner(seed()); auto native = owner.data(); const QPointer<InstrumentTrack> parent(owner->instrumentTrack()); int checks = 0; bool retired = false; QString error; { FileResourceIO::Scope work([&] { if (++checks == stage) { retired = true; if (track) { delete parent.data(); } else { delete owner.data(); } } return false; }); QVERIFY_EXCEPTION_THROWN(native->setFileResource("bank", bank(true), error), std::runtime_error); } QVERIFY(retired); QVERIFY(!owner); QVERIFY(Engine::audioEngine()->canJoinBackendCallbacks());
	}
	void allSettingsFieldsObserveReturnedLifetimes_data()
	{ QTest::addColumn<int>("field"); QTest::addColumn<bool>("saving"); for (int field = 0; field < 3; ++field) { QTest::newRow(qPrintable(QString::number(field) + "-load")) << field << false; QTest::newRow(qPrintable(QString::number(field) + "-save")) << field << true; } }
	void allSettingsFieldsObserveReturnedLifetimes()
	{
		QFETCH(int, field); QFETCH(bool, saving); const char* keys[] = {"bank", "patch", "gain"}; const QPointer<GigInstrument> owner(seed()); auto native = owner.data(); const QPointer<InstrumentTrack> track(owner->instrumentTrack()); const auto models = owner->findChildren<AutomatableModel*>(QString{}, Qt::FindDirectChildrenOnly); QCOMPARE(models.size(), 3); QDomDocument document; auto node = settings(document, bank()); bool retired = false;
		if (saving) { for (const auto key : keys) { node.removeAttribute(key); } FileResourceIO::Scope work([&] { if (!retired && node.hasAttribute(keys[field])) { retired = true; delete track.data(); } return false; }); QVERIFY_EXCEPTION_THROWN(native->saveSettings(document, node), std::runtime_error); }
		else { connect(models[field], &AutomatableModel::dataChanged, this, [&] { if (!retired) { retired = true; delete track.data(); } }); QVERIFY_EXCEPTION_THROWN(native->loadSettings(node), std::runtime_error); } QVERIFY(retired); QVERIFY(!owner); QVERIFY(!track);
	}
	void weakSignalRestorationSurvivesRetirementInsideBlockedBankLoad()
	{
		const QPointer<GigInstrument> owner(seed()); auto native = owner.data(); const QPointer<InstrumentTrack> track(owner->instrumentTrack()); bool returnedFile = false, retired = false; connect(native, &GigInstrument::fileChanged, this, [&] { returnedFile = true; }); QDomDocument document; const auto node = settings(document, bank()); { FileResourceIO::Scope work([&] { if (returnedFile && !retired) { retired = true; delete track.data(); } return false; }); QVERIFY_EXCEPTION_THROWN(native->loadSettings(node), std::runtime_error); } QVERIFY(returnedFile); QVERIFY(retired); QVERIFY(!owner);
	}
	void labelPropertyCallbacksCannotResumeOnDeadModels_data()
	{ QTest::addColumn<bool>("patch"); QTest::newRow("bank-labels") << false; QTest::newRow("patch-labels") << true; }
	void labelPropertyCallbacksCannotResumeOnDeadModels()
	{
		QFETCH(bool, patch); const QPointer<GigInstrument> owner(seed()); auto native = owner.data(); const QPointer<InstrumentTrack> track(owner->instrumentTrack()); bool retired = false; connect(native->childModel(patch ? "patch" : "bank"), &AutomatableModel::propertiesChanged, this, [&] { if (!retired) { retired = true; delete track.data(); } }); QString error; QVERIFY_EXCEPTION_THROWN(native->setFileResource("bank", bank(), error), std::runtime_error); QVERIFY(retired); QVERIFY(!owner);
	}
	void fileAndPatchNotificationsCheckReturnedOwnership_data()
	{ QTest::addColumn<int>("stage"); QTest::newRow("file-loading") << 0; QTest::newRow("patch-changed") << 1; QTest::newRow("file-changed") << 2; }
	void fileAndPatchNotificationsCheckReturnedOwnership()
	{
		QFETCH(int, stage); const QPointer<GigInstrument> owner(seed()); auto native = owner.data(); const QPointer<InstrumentTrack> track(owner->instrumentTrack()); bool retired = false; const auto retire = [&] { if (!retired) { retired = true; delete track.data(); } }; if (stage == 0) { connect(native, &GigInstrument::fileLoading, this, retire); } if (stage == 1) { connect(native, &GigInstrument::patchChanged, this, retire); } if (stage == 2) { connect(native, &GigInstrument::fileChanged, this, retire); } QString error; QVERIFY_EXCEPTION_THROWN(native->setFileResource("bank", bank(), error), std::runtime_error); QVERIFY(retired); QVERIFY(!owner);
	}
	void nestedSettingsResourcesAndProgramSelectionAreRefused()
	{
		auto native = seed(); bool nested = false; QDomDocument document; const auto node = settings(document, bank()); const auto connection = connect(native, &GigInstrument::fileLoading, this, [&] { if (nested) { return; } nested = true; QString error; QVERIFY_EXCEPTION_THROWN(native->setFileResource("bank", {}, error), std::logic_error); QVERIFY_EXCEPTION_THROWN(native->loadSettings(node), std::logic_error); QDomDocument saved; auto element = saved.createElement("gigplayer"); QVERIFY_EXCEPTION_THROWN(native->saveSettings(saved, element), std::logic_error); QVERIFY_EXCEPTION_THROWN(native->selectProgram(2, 7, error), std::logic_error); }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QString error; QVERIFY(native->setFileResource("bank", bank(), error)); QVERIFY(nested);
	}
	void cancellationAndExceptionsReleaseResourceAdmission_data()
	{ QTest::addColumn<int>("kind"); QTest::addColumn<int>("stage"); for (int kind = 0; kind < 3; ++kind) { QTest::newRow(qPrintable(QString::number(kind) + "-entry")) << kind << 1; QTest::newRow(qPrintable(QString::number(kind) + "-capture")) << kind << 4; } }
	void cancellationAndExceptionsReleaseResourceAdmission()
	{
		QFETCH(int, kind); QFETCH(int, stage); auto native = seed(); QString error; bool caught = false, loaded = false; int checks = 0; try { FileResourceIO::Scope work([&] { if (++checks < stage) { return false; } if (kind == 1) { throw std::runtime_error("Native GIG work fixture"); } if (kind == 2) { throw 137; } return true; }); loaded = native->setFileResource("bank", bank(true), error); } catch (...) { caught = true; } QVERIFY(checks >= stage); QVERIFY(caught || !loaded); if (kind != 0) { QVERIFY(caught); } QVERIFY(native->setFileResource("bank", bank(true), error));
	}
	void copiedInputsAndNativeControlRootChangesAreChecked_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("borrowed-input") << 0; QTest::newRow("control-mutation") << 1; QTest::newRow("project-path") << 2; }
	void copiedInputsAndNativeControlRootChangesAreChecked()
	{
		QFETCH(int, kind); auto native = seed(); QString path = bank(), key = "bank", error; bool changed = false; { FileResourceIO::Scope work([&] { if (!changed) { changed = true; if (kind == 0) { path = configuration.filePath("absent.gig"); key = "changed"; } if (kind == 1) { native->childModel("bank")->setValue(111); } if (kind == 2) { NativeSongTestAccess::setProjectFileName(Engine::getSong(), configuration.filePath("other.mmp")); } } return false; }); if (kind == 0) { QVERIFY(native->setFileResource(key, path, error)); } else { QVERIFY_EXCEPTION_THROWN(native->setFileResource(key, path, error), std::runtime_error); } } QVERIFY(changed); QCOMPARE(native->programSnapshot().size(), kind == 0 ? std::size_t(1) : std::size_t(0));
	}
	void corruptMissingCompanionAndClearKeepNativeReplacementSemantics()
	{
		auto native = seed(); QString error; QVERIFY(native->setFileResource("bank", bank(true), error)); const auto before = native->fileResources()[0]; QTemporaryDir source; const auto bad = source.filePath("bad.gig"), split = source.filePath("missing.gig"); QVERIFY(test::gigNative::write(bad, "invalid gig")); QVERIFY(test::gigNative::create(split, true, error)); QVERIFY(QFile::remove(source.filePath("missing.gx01"))); QVERIFY(!native->setFileResource("bank", bad, error)); QVERIFY(!native->setFileResource("bank", split, error)); QVERIFY(!native->setFileResource("bank", source.filePath("absent.gig"), error)); QVERIFY(native->fileResources()[0].identity == before.identity); QCOMPARE(native->getBank(), 2); QCOMPARE(native->getPatch(), 7); QVERIFY(native->setFileResource("bank", {}, error)); QVERIFY(native->fileResources()[0].path.isEmpty()); QVERIFY(!native->fileResources()[0].identity); QVERIFY(native->programSnapshot().empty());
	}
	void programSelectionAndNamesRemainNativeAndChecked()
	{
		auto native = seed(); QString error; QVERIFY(native->setFileResource("bank", bank(), error)); native->childModel("bank")->setValue(0); QVERIFY(native->getCurrentPatchName().isEmpty()); QVERIFY(native->selectProgram(2, 7, error)); QCOMPARE(native->getCurrentPatchName(), QString("Native persistent bank")); QVERIFY(!native->selectProgram(111, 99, error)); QCOMPARE(native->getBank(), 2); QCOMPARE(native->getPatch(), 7);
	}
	void ownedBankSettingsAndNativeAudioSurviveOriginalRemoval_data()
	{ QTest::addColumn<bool>("split"); QTest::newRow("single-file") << false; QTest::newRow("companion-pool") << true; }
	void ownedBankSettingsAndNativeAudioSurviveOriginalRemoval()
	{
		QFETCH(bool, split); QTemporaryDir source, destination; const auto path = source.filePath("bank.gig"); QString error; QVERIFY2(test::gigNative::create(path, split, error), qPrintable(error)); auto native = seed(); QVERIFY(native->setFileResource("bank", path, error)); const auto accepted = snapshot(native->fileResources()[0]); QVERIFY(accepted); DataFile document(DataFile::Type::SongProject); auto node = document.createElement("gigplayer"); document.content().appendChild(node); { DataFile::ResourceSourceScope saving(document); native->saveSettings(document, node); } for (const auto& member : accepted->members()) { QVERIFY(QFile::remove(source.filePath(member.name))); } QVERIFY(energy(native) > 0.); auto restored = seed(); { DataFile::ResourceReadScope reading(document); restored->loadSettings(node); } QVERIFY(!Engine::getSong()->hasFatalErrors()); QCOMPARE(restored->programSnapshot().size(), std::size_t(1)); QVERIFY(energy(restored) > 0.); QJsonArray assets; QVERIFY(document.copyResources(destination.filePath("assets"), &assets)); QCOMPARE(assets.size(), 1); QVERIFY(document.matchesAssetReferences(assets));
	}
	void QtEventsStayOutsideGroupCaptureAndSettings()
	{
		auto native = seed(); bool queued = false; QString error; QTimer::singleShot(0, this, [&] { queued = true; }); { FileResourceIO::Scope work({}, true); QVERIFY(native->setFileResource("bank", bank(true), error)); QVERIFY(!queued); } QCoreApplication::processEvents(); QVERIFY(queued);
	}
	void actualModuleRuntimeRecoversPublicationFailureAndRetainsOwnedGroup()
	{
		QVERIFY2(!getPluginFactory()->pluginInfo("gigplayer").isNull(), "The built native GIG module must be discoverable"); QTemporaryDir source, session; QString error; const auto path = source.filePath("bank.gig"); QVERIFY(test::gigNative::create(path, true, error)); QJsonObject rejected; const auto command = request("bank", "instrument.resource.set", {{"track", 0}, {"resource", "bank"}, {"path", path}});
		{ StudioRuntime runtime(session.path()); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool()); QVERIFY(runtime.process(request("instrument", "instrument.load", {{"track", 0}, {"instrument", "gigplayer"}})).value("ok").toBool()); auto native = current(); GigPublicationFault fault; const auto connection = connect(native, SIGNAL(fileChanged()), &fault, SLOT(fail())); QVERIFY(connection); fault.arm = true; rejected = runtime.process(command); fault.arm = false; QObject::disconnect(connection); QVERIFY(!rejected.value("ok").toBool()); QVERIFY(fault.faulted); auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected); QVERIFY(current()->fileResources()[0].path.isEmpty()); QVERIFY(runtime.process(request("retry", "instrument.resource.set", {{"track", 0}, {"resource", "bank"}, {"path", path}})).value("ok").toBool()); const auto before = manifest(session); QVERIFY(!before.isEmpty()); QVERIFY(!runtime.process(request("missing", "instrument.resource.set", {{"track", 0}, {"resource", "bank"}, {"path", source.filePath("absent.gig")}})).value("ok").toBool()); QCOMPARE(manifest(session).value("history"), before.value("history")); QVERIFY(!runtime.stopping()); }
		QVERIFY(QFile::remove(path)); QVERIFY(QFile::remove(source.filePath("bank.gx01"))); StudioRuntime reopened(session.path()); QVERIFY2(reopened.open(error), qPrintable(error)); QCOMPARE(current()->programs().size(), std::size_t(1)); const auto retained = snapshot(current()->fileResources()[0]); QVERIFY(retained); QCOMPARE(retained->members().size(), 2);
	}
};
QTEST_GUILESS_MAIN(GigResourceLifecycleTest)
#include "GigResourceLifecycleTest.moc"
