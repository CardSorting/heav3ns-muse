/* Native sampler settings, resources and returned callbacks. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <stdexcept>
#include <thread>
#include "AudioEngine.h"
#include "AudioFileProcessor.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "FileResourceSnapshot.h"
#include "InstrumentTrack.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "PathUtil.h"
#include "PluginFactory.h"
#include "SerializingObject.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioAssetStore.h"
#include "StudioRuntime.h"
#include "../core/NativeSongTestAccess.h"
using namespace lmms;
namespace
{
QByteArray little(quint32 value, int count) { QByteArray result; for (int index = 0; index < count; ++index) { result.append(static_cast<char>(value >> (8 * index))); } return result; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
bool wave(const QString& path, int frames)
{ QByteArray pcm(frames * 2, '\0'); pcm[1] = 0x20; return write(path, QByteArray("RIFF") + little(36 + pcm.size(), 4) + "WAVEfmt " + little(16, 4) + little(1, 2) + little(1, 2) + little(44100, 4) + little(88200, 4) + little(2, 2) + little(16, 2) + "data" + little(pcm.size(), 4) + pcm); }
class SamplerTrack final : public InstrumentTrack
{
public:
	SamplerTrack() : InstrumentTrack(Engine::getSong()) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new AudioFileProcessor(this); }
};
const Plugin::Descriptor resourceDescriptor{"resource-publication-fixture", "Resource publication fixture", "Native resource fault fixture", "LMMS tests", 1, Plugin::Type::Instrument, nullptr, "", nullptr};
class ResourceInstrument final : public Instrument
{
public:
	explicit ResourceInstrument(InstrumentTrack* track) : Instrument(track, &resourceDescriptor), selector(std::make_unique<FloatModel>(0, 0, 1, .01f, this, "Selector")) { }
	std::unique_ptr<FloatModel> selector;
	std::shared_ptr<const void> backing = std::make_shared<const char>(0);
	QString path;
	bool pending = false;
	std::function<void(ResourceInstrument*)> onLoad;
	std::function<void()> onInspect;
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
	void saveSettings(QDomDocument&, QDomElement&) override { }
	void loadSettings(const QDomElement&) override { }
	QString nodeName() const override { return "resource-publication-fixture"; }
	std::vector<FileResource> fileResources() const override
	{ const std::vector<FileResource> result{{"sample", "Sample", path, {"wav"}, selector.get(), .5f, true, backing, pending}}; const auto callback = onInspect; if (callback) { callback(); } return result; }
	bool setFileResource(const QString&, const QString& requested, QString& error) override
	{ const QString copy(requested); const auto callback = onLoad; const QPointer<ResourceInstrument> owner(this); if (callback) { callback(this); } if (!owner) { return true; } path = copy; backing = std::make_shared<const char>(0); error.clear(); return true; }
};
class ResourceTrack final : public InstrumentTrack
{
public:
	ResourceTrack() : InstrumentTrack(Engine::getSong()) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new ResourceInstrument(this); }
};
class Hook final : public SerializingObjectHook
{
public:
	std::function<void()> callback;
	void saveSettings(QDomDocument&, QDomElement&) override { const auto copy = callback; if (copy) { copy(); } }
	void loadSettings(const QDomElement&) override { }
};
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
[[noreturn]] void raise(bool nonstandard) { if (nonstandard) { throw 53; } throw std::runtime_error("fixture sampler callback failure"); }
}
class AudioFileProcessorLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir directory;
	QString first() const { return directory.filePath("first.wav"); }
	QString second() const { return directory.filePath("second.wav"); }
	AudioFileProcessor* seed()
	{ auto track = new SamplerTrack; auto native = dynamic_cast<AudioFileProcessor*>(track->loadInstrument("audiofileprocessor")); if (!native) { throw std::runtime_error("Native AFP fixture was not selected"); } return native; }
	ResourceInstrument* resourceSeed()
	{ auto track = new ResourceTrack; return static_cast<ResourceInstrument*>(track->loadInstrument("resource-publication-fixture")); }
	QJsonObject resource(const QString& path, bool select = false) const { return { {"track", 0}, {"resource", "sample"}, {"path", path}, {"select", select} }; }
private slots:
	void initTestCase()
	{
		QVERIFY(directory.isValid()); ConfigManager::inst()->setWorkingDir(directory.filePath("workspace")); ConfigManager::inst()->loadConfigFile(directory.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); QVERIFY(wave(first(), 64)); QVERIFY(wave(second(), 128));
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void fileEmbeddedAndMissingSettingsRoundtrip_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("file") << 0; QTest::newRow("embedded") << 1; QTest::newRow("missing") << 2; }
	void fileEmbeddedAndMissingSettingsRoundtrip()
	{
		QFETCH(int, kind); auto sampler = seed(); QString error; QVERIFY(sampler->setFileResource("sample", first(), error)); sampler->endPointModel().setValue(.75f); sampler->startPointModel().setValue(.25f); sampler->loopPointModel().setValue(.5f); sampler->ampModel().setValue(175); sampler->reverseModel().setValue(true);
		QDomDocument document; auto root = document.createElement("sampler"); document.appendChild(root); sampler->saveSettings(document, root);
		if (kind == 1) { root.removeAttribute("src"); root.setAttribute("sampledata", sampler->sample().toBase64()); root.setAttribute("sampleRate", sampler->sample().sampleRate()); }
		if (kind == 2) { root.setAttribute("src", directory.filePath("missing.wav")); }
		auto restored = seed(); restored->loadSettings(root); QCOMPARE(restored->ampModel().value(), 175.f); QCOMPARE(restored->reverseModel().value(), true); QCOMPARE(restored->startPointModel().value(), .25f); QCOMPARE(restored->endPointModel().value(), .75f);
		if (kind == 2) { QVERIFY(restored->sample().buffer()->empty()); QCOMPARE(restored->fileResources()[0].path, directory.filePath("missing.wav")); }
		else { QCOMPARE(restored->sample().sampleSize(), std::size_t(64)); QCOMPARE(restored->sample().startFrame(), 16); QCOMPARE(restored->sample().endFrame(), 48); QCOMPARE(restored->sample().sampleRate(), 44100); }
		QDomDocument saved; auto output = saved.createElement("sampler"); saved.appendChild(output); restored->saveSettings(saved, output); if (kind == 1) { QVERIFY(!output.attribute("sampledata").isEmpty()); } if (kind == 2) { QCOMPARE(output.attribute("src"), directory.filePath("missing.wav")); QVERIFY(!output.hasAttribute("sampledata")); }
	}
	void legacyLoopAndInterpolationDefaultsRemainNative()
	{
		auto sampler = seed(); QDomDocument document; auto root = document.createElement("sampler"); document.appendChild(root); root.setAttribute("src", first()); root.setAttribute("sframe", .25); root.setAttribute("eframe", .75); sampler->loadSettings(root); QCOMPARE(sampler->loopPointModel().value(), sampler->startPointModel().value()); QCOMPARE(sampler->interpolationModel().value(), 1);
	}
	void borrowedFilenameIsCopiedBeforeCooperativeDecoding()
	{
		auto sampler = seed(); QString path = first(), error; bool changed = false;
		{ FileResourceIO::Scope work([&] { if (!changed) { changed = true; path = second(); } return false; }); QVERIFY(sampler->setFileResource("sample", path, error)); }
		QVERIFY(changed); QCOMPARE(sampler->sample().sampleSize(), std::size_t(64)); QCOMPARE(PathUtil::toAbsolute(sampler->sample().sampleFile()), first());
	}
	void decodeRetirementDoesNotResumeOnDeadOwners_data()
	{ QTest::addColumn<bool>("track"); QTest::newRow("instrument") << false; QTest::newRow("track") << true; }
	void decodeRetirementDoesNotResumeOnDeadOwners()
	{
		QFETCH(bool, track); const QPointer<AudioFileProcessor> sampler(seed()); const QPointer<InstrumentTrack> owner(sampler->instrumentTrack()); QString error; bool retired = false;
		{ FileResourceIO::Scope work([&] { if (!retired) { retired = true; if (track) { delete owner.data(); } else { delete sampler.data(); } } return false; }); auto active = sampler.data(); QVERIFY(!active->setFileResource("sample", first(), error)); }
		QVERIFY(retired); QVERIFY(!sampler); QVERIFY(!error.isEmpty()); if (!track) { QVERIFY(owner); QVERIFY(!owner->instrument()); }
	}
	void finalSampleNotificationCannotRetireTheReceiver()
	{
		const QPointer<AudioFileProcessor> sampler(seed()); bool retired = false; QString error;
		connect(sampler.data(), &AudioFileProcessor::sampleUpdated, this, [&] { if (!retired) { retired = true; delete sampler.data(); } }); auto active = sampler.data(); QVERIFY(!active->setFileResource("sample", first(), error)); QVERIFY(retired); QVERIFY(!sampler);
	}
	void fileCallbackExceptionsReleaseAdmission_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void fileCallbackExceptionsReleaseAdmission()
	{
		QFETCH(bool, nonstandard); auto sampler = seed(); bool armed = true; QString error; const auto connection = connect(sampler, &AudioFileProcessor::sampleUpdated, this, [&] { if (armed) { armed = false; raise(nonstandard); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!sampler->setFileResource("sample", first(), error)); QVERIFY(!armed); QVERIFY(!error.isEmpty()); QVERIFY(sampler->setFileResource("sample", second(), error)); QCOMPARE(sampler->sample().sampleSize(), std::size_t(128));
	}
	void settingsAndFileWorkCannotReenterFromNotifications()
	{
		auto sampler = seed(); bool invoked = false, fileRefused = false, saveRefused = false, loadRefused = false; QDomDocument document; auto root = document.createElement("sampler"); document.appendChild(root); QString error;
		const auto connection = connect(sampler, &AudioFileProcessor::sampleUpdated, this, [&] { if (invoked) { return; } invoked = true; QString nested; fileRefused = !sampler->setFileResource("sample", second(), nested); try { sampler->saveSettings(document, root); } catch (const std::logic_error&) { saveRefused = true; } try { sampler->loadSettings(root); } catch (const std::logic_error&) { loadRefused = true; } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(sampler->setFileResource("sample", first(), error)); QVERIFY(invoked && fileRefused && saveRefused && loadRefused); sampler->saveSettings(document, root); QVERIFY(sampler->setFileResource("sample", second(), error));
	}
	void settingsModelNotificationRetirementStopsLaterFields()
	{
		const QPointer<AudioFileProcessor> sampler(seed()); QDomDocument document; auto root = document.createElement("sampler"); document.appendChild(root); root.setAttribute("looped", 1); bool retired = false;
		connect(&sampler->loopModel(), &IntModel::dataChanged, this, [&] { retired = true; delete sampler.data(); }); bool caught = false; auto active = sampler.data(); try { active->loadSettings(root); } catch (const std::exception&) { caught = true; } QVERIFY(caught); QVERIFY(retired); QVERIFY(!sampler);
	}
	void serializationRejectsChangesToAnotherSavedControl()
	{
		auto sampler = seed(); auto midi = new MidiController(nullptr); sampler->reverseModel().setControllerConnection(new ControllerConnection(midi)); Hook hook; bool invoked = false; hook.callback = [&] { if (!invoked) { invoked = true; sampler->ampModel().setValue(31); } }; midi->setHook(&hook); QDomDocument document; auto root = document.createElement("sampler"); document.appendChild(root);
		QVERIFY_EXCEPTION_THROWN(sampler->saveSettings(document, root), std::runtime_error); QVERIFY(invoked); midi->setHook(nullptr); sampler->saveSettings(document, root);
	}
	void embeddedSerializationOwnsTheBufferAfterReceiverRetirement()
	{
		const QPointer<AudioFileProcessor> sampler(seed()); QDomDocument input; auto settings = input.createElement("sampler"); input.appendChild(settings); QString error; const auto buffer = SampleBuffer::fromFile(first()); settings.setAttribute("sampledata", buffer->toBase64()); settings.setAttribute("sampleRate", buffer->sampleRate()); sampler->loadSettings(settings); QDomDocument document; auto root = document.createElement("sampler"); document.appendChild(root); int checks = 0; bool caught = false;
		{ FileResourceIO::Scope work([&] { if (++checks == 3) { delete sampler.data(); } return false; }); auto active = sampler.data(); try { active->saveSettings(document, root); } catch (const std::exception&) { caught = true; } }
		QVERIFY(caught); QVERIFY(!sampler);
	}
	void invalidEmbeddedSettingsRetainTheUnresolvedReference()
	{
		auto sampler = seed(); QString error; const auto missing = directory.filePath("missing.wav"); QVERIFY(!sampler->setFileResource("sample", missing, error)); QDomDocument document; auto root = document.createElement("sampler"); document.appendChild(root); root.setAttribute("sampledata", "invalid-base64"); root.setAttribute("sampleRate", 44100); sampler->loadSettings(root); QCOMPARE(sampler->fileResources()[0].path, missing);
	}
	void pointCorrectionAndResourceIdentityUseNativeState()
	{
		auto sampler = seed(); QString error; QVERIFY(sampler->setFileResource("sample", first(), error)); const auto old = sampler->fileResources()[0].identity; sampler->endPointModel().setValue(.25f); sampler->startPointModel().setValue(.75f); QVERIFY(sampler->startPointModel().value() <= sampler->endPointModel().value()); QVERIFY(sampler->loopPointModel().value() >= sampler->startPointModel().value());
		QVERIFY(sampler->setFileResource("sample", second(), error)); QVERIFY(old); QVERIFY(old != sampler->fileResources()[0].identity); QCOMPARE(static_cast<const void*>(sampler->sample().buffer().get()), sampler->fileResources()[0].identity.get());
	}
	void settingsRefuseForeignThreadsAndSuppressQueuedWork()
	{
		auto sampler = seed(); QString error; bool accepted = true; std::thread foreign([&] { accepted = sampler->setFileResource("sample", first(), error); }); foreign.join(); QVERIFY(!accepted); QVERIFY(!error.isEmpty()); bool queued = false; QTimer::singleShot(0, this, [&] { queued = true; }); { FileResourceIO::Scope work({}, true); QVERIFY(sampler->setFileResource("sample", first(), error)); QVERIFY(!queued); } QCoreApplication::processEvents(); QVERIFY(queued);
	}
	void resourceGetterAndLoaderRetirementCannotPublishSuccess_data()
	{ QTest::addColumn<int>("phase"); QTest::newRow("getter") << 1; QTest::newRow("loader") << 0; QTest::newRow("result-inspection") << 3; }
	void resourceGetterAndLoaderRetirementCannotPublishSuccess()
	{
		QFETCH(int, phase); const QPointer<ResourceInstrument> sampler(resourceSeed()); int inspections = 0; bool retired = false; if (phase) { sampler->onInspect = [&] { if (++inspections == phase) { retired = true; delete sampler.data(); } }; } else { sampler->onLoad = [&](ResourceInstrument* active) { retired = true; delete active; }; }
		QVERIFY(!StudioCapabilities::execute("instrument.resource.set", resource(first())).value("ok").toBool()); QVERIFY(retired); QVERIFY(!sampler);
	}
	void nativeStructuredResourceLoadAndClearPublishFinalInventory()
	{
		auto sampler = seed(); NativeSongTestAccess::setModified(Engine::getSong(), false); const auto loaded = StudioCapabilities::execute("instrument.resource.set", resource(first())); QVERIFY2(loaded.value("ok").toBool(), QJsonDocument(loaded).toJson().constData()); QVERIFY(Engine::getSong()->isModified()); QCOMPARE(sampler->sample().sampleSize(), std::size_t(64));
		const auto inventory = loaded.value("result").toObject().value("resources").toArray(); QCOMPARE(inventory.size(), 1); QCOMPARE(PathUtil::toAbsolute(inventory[0].toObject().value("path").toString()), first()); QVERIFY(!inventory[0].toObject().contains("identity"));
		const auto cleared = StudioCapabilities::execute("instrument.resource.set", resource(QString{})); QVERIFY(cleared.value("ok").toBool()); QVERIFY(sampler->sample().buffer()->empty()); QVERIFY(cleared.value("result").toObject().value("resources").toArray()[0].toObject().value("path").toString().isEmpty());
	}
	void resourceSelectorRetirementIsRefusedBeforeActivation()
	{
		auto sampler = resourceSeed(); bool retired = false; sampler->onLoad = [&](ResourceInstrument* active) { retired = true; active->selector.reset(); }; QVERIFY(!StudioCapabilities::execute("instrument.resource.set", resource(first(), true)).value("ok").toBool()); QVERIFY(retired);
	}
	void backingChangesAtTheSamePathAreRefusedAfterDirtyNotification()
	{
		auto sampler = resourceSeed(); NativeSongTestAccess::setModified(Engine::getSong(), false); bool changed = false; const auto connection = connect(Engine::getSong(), &Song::modified, this, [&] { if (!changed && Engine::getSong()->isModified()) { changed = true; sampler->backing = std::make_shared<const char>(0); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!StudioCapabilities::execute("instrument.resource.set", resource(first())).value("ok").toBool()); QVERIFY(changed); QCOMPARE(sampler->path, first());
	}
	void actualModuleResourcePublicationFailureRecoversAndReplays_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void actualModuleResourcePublicationFailureRecoversAndReplays()
	{
		QFETCH(bool, nonstandard); if (getPluginFactory()->pluginInfo("audiofileprocessor").isNull()) { QSKIP("Requires the built native AFP module"); } QTemporaryDir session; const auto command = request("resource", "instrument.resource.set", resource(first())); QJsonObject rejected;
		{ StudioRuntime runtime(session.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool()); QVERIFY(runtime.process(request("plugin", "instrument.load", {{"track", 0}, {"instrument", "audiofileprocessor"}})).value("ok").toBool()); auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0]); bool armed = true, prepared = false;
			const auto sampleConnection = connect(track->instrument(), &Model::dataChanged, this, [&] { prepared = true; NativeSongTestAccess::setModified(Engine::getSong(), false); }); const auto connection = connect(Engine::getSong(), &Song::modified, this, [&] { if (armed && prepared && Engine::getSong()->isModified()) { armed = false; raise(nonstandard); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(sampleConnection); QObject::disconnect(connection); });
			rejected = runtime.process(command); QVERIFY(prepared && !armed); QVERIFY(!rejected.value("ok").toBool()); QVERIFY(!runtime.stopping()); auto restored = static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0]); QVERIFY(restored->instrument()); QVERIFY(restored->instrument()->fileResources()[0].path.isEmpty()); auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected); }
		StudioRuntime reopened(session.path()); QString error; QVERIFY(reopened.open(error)); auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected);
	}
	void nativeDiagnosticsRetainUnresolvedWithoutHidingFatalErrors()
	{
		auto song = Engine::getSong(); const auto before = song->errorGeneration(); song->collectUnresolvedResource(first()); QVERIFY(song->hasErrors()); QVERIFY(!song->hasFatalErrors()); QVERIFY(song->errorGeneration() != before); const auto diagnostic = song->errorSummary().trimmed();
		song->collectError(diagnostic); QVERIFY(song->hasFatalErrors()); song->collectUnresolvedResource(second()); QVERIFY(song->hasFatalErrors()); song->clearErrors(); QVERIFY(!song->hasFatalErrors()); QVERIFY(!song->hasErrors()); song->collectUnresolvedResource(QString{}); QVERIFY(song->hasFatalErrors());
	}
	void pendingSettingsStaySilentWhenAFileAppears_data()
	{ QTest::addColumn<bool>("present"); QTest::newRow("absent") << false; QTest::newRow("appeared") << true; }
	void pendingSettingsStaySilentWhenAFileAppears()
	{
		QFETCH(bool, present); QTemporaryDir source; const auto path = source.filePath("later.wav"); auto sampler = seed(); QString error; QVERIFY(!sampler->setFileResource("sample", path, error)); QDomDocument document; auto root = document.createElement("audiofileprocessor"); document.appendChild(root); sampler->saveSettings(document, root); QCOMPARE(root.attribute("unresolved"), QString("1")); if (present) { QVERIFY(wave(path, 64)); }
		QVERIFY(sampler->setFileResource("sample", first(), error)); sampler->loadSettings(root); QVERIFY(sampler->sample().buffer()->empty()); QVERIFY(sampler->fileResources()[0].unresolved); QCOMPARE(sampler->fileResources()[0].path, path); QVERIFY(Engine::getSong()->hasErrors()); QVERIFY(!Engine::getSong()->hasFatalErrors());
		QVERIFY(sampler->setFileResource("sample", first(), error)); sampler->saveSettings(document, root); QVERIFY(!root.hasAttribute("unresolved")); QVERIFY(!sampler->fileResources()[0].unresolved); QCOMPARE(sampler->sample().sampleSize(), std::size_t(64));
	}
	void malformedPendingSettingsAreRefused_data()
	{ QTest::addColumn<bool>("embedded"); QTest::newRow("empty-path") << false; QTest::newRow("conflicting-embedded") << true; }
	void malformedPendingSettingsAreRefused()
	{
		QFETCH(bool, embedded); auto sampler = seed(); QString error; QVERIFY(sampler->setFileResource("sample", first(), error)); const auto buffer = sampler->sample().buffer(); QDomDocument document; auto root = document.createElement("audiofileprocessor"); document.appendChild(root); root.setAttribute("unresolved", 1); if (embedded) { root.setAttribute("src", second()); root.setAttribute("sampledata", sampler->sample().toBase64()); }
		QVERIFY_EXCEPTION_THROWN(sampler->loadSettings(root), std::invalid_argument); QCOMPARE(sampler->sample().buffer().get(), buffer.get()); QVERIFY(!sampler->fileResources()[0].unresolved); QVERIFY(sampler->setFileResource("sample", second(), error));
	}
	void pendingCheckpointMetadataExcludesFilesAndRejectsPortableBundle_data()
	{ QTest::addColumn<bool>("present"); QTest::newRow("absent") << false; QTest::newRow("appeared") << true; }
	void pendingCheckpointMetadataExcludesFilesAndRejectsPortableBundle()
	{
		QFETCH(bool, present); QTemporaryDir source; const auto path = source.filePath("later.wav"); if (present) { QVERIFY(wave(path, 64)); } DataFile document(DataFile::Type::SongProject); auto pending = document.createElement("audiofileprocessor"); pending.setAttribute("src", path); pending.setAttribute("unresolved", 1); document.content().appendChild(pending); auto decoded = document.createElement("audiofileprocessor"); decoded.setAttribute("src", first()); document.content().appendChild(decoded);
		const auto before = document.toString(); QVERIFY(!document.copyResources(source.filePath("resources"))); QCOMPARE(document.toString(), before); QJsonArray assets; QVERIFY(document.copyResources(source.filePath("assets"), &assets)); QCOMPARE(pending.attribute("src"), path); QCOMPARE(assets.size(), 1); QVERIFY(decoded.attribute("src").startsWith("local:assets/")); QVERIFY(document.matchesAssetReferences(assets)); QString error; QVERIFY(StudioAssetStore::verify(source.path(), assets, error)); QVERIFY(!QDir(source.filePath("assets")).entryList(QDir::Files).isEmpty());
	}
	void pendingCheckpointManifestRejectsUnmarkedOrConflictingReferences_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("removed-marker") << 0; QTest::newRow("empty-path") << 1; QTest::newRow("embedded") << 2; QTest::newRow("different-native-model") << 3; QTest::newRow("ordinary-missing") << 4; QTest::newRow("relative-path") << 5; }
	void pendingCheckpointManifestRejectsUnmarkedOrConflictingReferences()
	{
		QFETCH(int, change); QTemporaryDir source; DataFile document(DataFile::Type::SongProject); auto pending = document.createElement("audiofileprocessor"); pending.setAttribute("src", source.filePath("missing.wav")); pending.setAttribute("unresolved", 1); document.content().appendChild(pending); QVERIFY(document.matchesAssetReferences({}));
		if (change == 0) { pending.removeAttribute("unresolved"); } else if (change == 1) { pending.setAttribute("src", ""); } else if (change == 2) { pending.setAttribute("sampledata", "AAAA"); } else if (change == 3) { pending.setTagName("patman"); } else if (change == 4) { auto ordinary = document.createElement("audiofileprocessor"); ordinary.setAttribute("src", source.filePath("another.wav")); document.content().appendChild(ordinary); } else { pending.setAttribute("src", "relative.wav"); }
		QVERIFY(!document.matchesAssetReferences({}));
	}
	void pendingAcceptedResourceBindingCannotBeDiscarded()
	{
		QTemporaryDir source; QString error; const auto snapshot = FileResourceSnapshot::capture(first(), error); QVERIFY(snapshot); DataFile document(DataFile::Type::SongProject); auto pending = document.createElement("audiofileprocessor"); pending.setAttribute("src", first()); pending.setAttribute("unresolved", 1); document.content().appendChild(pending); { DataFile::ResourceSourceScope scope(document); DataFile::registerResourceSnapshot(document, pending, "src", snapshot); }
		const auto before = document.toString(); QJsonArray assets; QVERIFY(!document.copyResources(source.filePath("assets"), &assets)); QVERIFY(assets.isEmpty()); QCOMPARE(document.toString(), before);
	}
	void publishedPendingStateChangesAreRefused()
	{
		auto sampler = resourceSeed(); NativeSongTestAccess::setModified(Engine::getSong(), false); bool changed = false; const auto connection = connect(Engine::getSong(), &Song::modified, this, [&] { if (!changed && Engine::getSong()->isModified()) { changed = true; sampler->pending = true; } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!StudioCapabilities::execute("instrument.resource.set", resource(first())).value("ok").toBool()); QVERIFY(changed);
	}
	void pendingSettingsCannotDiscardAcceptedCloneResources_data()
	{ QTest::addColumn<bool>("retainSample"); QTest::addColumn<bool>("retainedPath"); QTest::newRow("snapshot") << false << false; QTest::newRow("snapshot-and-sample") << true << false; QTest::newRow("declared-retained-path") << false << true; }
	void pendingSettingsCannotDiscardAcceptedCloneResources()
	{
		QFETCH(bool, retainSample); QFETCH(bool, retainedPath); auto sampler = seed(); QString error; QVERIFY(sampler->setFileResource("sample", first(), error)); const auto buffer = sampler->sample().buffer(); const auto snapshot = FileResourceSnapshot::capture(first(), error); QVERIFY(snapshot); DataFile document(DataFile::Type::InstrumentTrackSettings); auto pending = document.createElement("audiofileprocessor"); pending.setAttribute("src", retainedPath ? snapshot->path() : first()); pending.setAttribute("unresolved", 1); document.content().appendChild(pending);
		{ DataFile::ResourceSourceScope scope(document, true, retainSample); DataFile::registerResourceSnapshot(document, pending, "src", snapshot, retainSample ? buffer : nullptr); }
		{ DataFile::ResourceReadScope reading(document); QVERIFY_EXCEPTION_THROWN(sampler->loadSettings(pending), std::invalid_argument); }
		QCOMPARE(sampler->sample().buffer().get(), buffer.get()); QVERIFY(!sampler->fileResources()[0].unresolved); QVERIFY(sampler->setFileResource("sample", second(), error));
	}
	void actualModuleMissingProjectCanRelinkUndoRecoverAndReopen()
	{
		if (getPluginFactory()->pluginInfo("audiofileprocessor").isNull()) { QSKIP("Requires the built native AFP module"); } QTemporaryDir source, session; const auto path = source.filePath("later.wav"), legacy = source.filePath("legacy.mmp"), broken = source.filePath("broken.mmp"); auto sampler = seed(); QString error; QVERIFY(!sampler->setFileResource("sample", path, error)); QVERIFY(Engine::getSong()->saveProjectFile(legacy)); DataFile document(legacy); auto pending = document.elementsByTagName("audiofileprocessor").at(0).toElement(); QVERIFY(!pending.isNull()); pending.removeAttribute("unresolved"); QVERIFY(document.writeFile(legacy));
		DataFile bad(legacy); auto invalid = bad.elementsByTagName("audiofileprocessor").at(0).toElement(); invalid.setAttribute("src", ""); invalid.setAttribute("sampledata", "invalid-base64"); invalid.setAttribute("sampleRate", 44100); QVERIFY(bad.writeFile(broken));
		const auto current = [] { return static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument()->fileResources()[0]; }; const auto frames = [&] { return std::static_pointer_cast<const SampleBuffer>(current().identity)->size(); }; const auto rejectedCommand = request("broken", "project.load", {{"path", broken}, {"summary", true}}); QJsonObject rejected;
		{
			StudioRuntime runtime(session.path()); QVERIFY(runtime.open(error)); const auto loaded = runtime.process(request("legacy", "project.load", {{"path", legacy}, {"summary", true}})); QVERIFY2(loaded.value("ok").toBool(), QJsonDocument(loaded).toJson().constData()); QVERIFY(current().unresolved); QCOMPARE(current().path, path); QCOMPARE(frames(), std::size_t(0)); QVERIFY(Engine::getSong()->hasErrors()); QVERIFY(!Engine::getSong()->hasFatalErrors());
			QFile manifest(session.filePath("session.json")); QVERIFY(manifest.open(QIODevice::ReadOnly)); const auto state = QJsonDocument::fromJson(manifest.readAll()).object(); const auto checkpoint = state.value("history").toArray().last().toObject(); QVERIFY(checkpoint.value("assets").toArray().isEmpty()); DataFile saved(session.filePath(checkpoint.value("path").toString())); QVERIFY(saved.matchesAssetReferences({})); QCOMPARE(saved.elementsByTagName("audiofileprocessor").at(0).toElement().attribute("unresolved"), QString("1")); manifest.close();
			QVERIFY(wave(path, 64)); QVERIFY(runtime.process(request("tempo", "song.set", {{"tempo", 81}})).value("ok").toBool()); QVERIFY(current().unresolved); QCOMPARE(frames(), std::size_t(0)); const auto repaired = runtime.process(request("repair", "instrument.resource.set", resource(path))); QVERIFY(repaired.value("ok").toBool()); QVERIFY(!current().unresolved); QCOMPARE(frames(), std::size_t(64)); QVERIFY(!repaired.value("result").toObject().value("resources").toArray()[0].toObject().value("unresolved").toBool());
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QVERIFY(current().unresolved); QCOMPARE(frames(), std::size_t(0)); rejected = runtime.process(rejectedCommand); QVERIFY(!rejected.value("ok").toBool()); QVERIFY(!runtime.stopping()); QVERIFY(current().unresolved); QCOMPARE(frames(), std::size_t(0)); QVERIFY(!Engine::getSong()->hasFatalErrors()); auto replay = runtime.process(rejectedCommand); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected);
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QVERIFY(!current().unresolved); QCOMPARE(frames(), std::size_t(64)); QVERIFY(runtime.process(request("undo-again", "session.undo", {{"summary", true}})).value("ok").toBool()); QVERIFY(current().unresolved);
		}
		{ StudioRuntime reopened(session.path()); QVERIFY(reopened.open(error)); QVERIFY(current().unresolved); QCOMPARE(frames(), std::size_t(0)); auto replay = reopened.process(rejectedCommand); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected); QVERIFY(reopened.process(request("repair-after-reopen", "instrument.resource.set", resource(path))).value("ok").toBool()); QVERIFY(QFile::remove(path)); }
		StudioRuntime finalOpen(session.path()); QVERIFY(finalOpen.open(error)); QVERIFY(!current().unresolved); QCOMPARE(frames(), std::size_t(64));
	}
};
QTEST_GUILESS_MAIN(AudioFileProcessorLifecycleTest)
#include "AudioFileProcessorLifecycleTest.moc"
