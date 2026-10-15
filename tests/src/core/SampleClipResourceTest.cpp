/* Native missing sample-clip state, repair and history. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NativeClipTransfer.h"
#include "NativeSongTestAccess.h"
#include "NotePlayHandle.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
QByteArray little(quint32 value, int count) { QByteArray out; for (int i = 0; i < count; ++i) { out.append(static_cast<char>(value >> (8 * i))); } return out; }
bool wave(const QString& path, int frames)
{ QByteArray pcm(frames * 2, '\0'); pcm[1] = 0x20; const auto bytes = QByteArray("RIFF") + little(36 + pcm.size(), 4) + "WAVEfmt " + little(16, 4) + little(1, 2) + little(1, 2) + little(44100, 4) + little(88200, 4) + little(2, 2) + little(16, 2) + "data" + little(pcm.size(), 4) + pcm; QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
QDomDocument plan(const QString& path, bool marked = true, bool automatic = false)
{ QDomDocument doc; auto root = doc.createElement("sampleclip"); doc.appendChild(root); root.setAttribute("src", path); if (marked) { root.setAttribute("unresolved", 1); } root.setAttribute("name", "Missing arrangement"); root.setAttribute("pos", 192); root.setAttribute("len", 311); root.setAttribute("off", -13); root.setAttribute("autoresize", automatic ? 1 : 0); root.setAttribute("sample_start", 17); root.setAttribute("sample_end", 88); root.setAttribute("sample_rate", 44100); root.setAttribute("sample_gain", -.75); root.setAttribute("reversed", "true"); root.setAttribute("muted", 1); return doc; }
SampleClip* seed() { return static_cast<SampleClip*>(Track::create(Track::Type::Sample, Engine::getSong())->createClip(0)); }
QJsonObject args(const QString& path) { return {{"track", 0}, {"clip", 0}, {"path", path}}; }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& arguments = {}) { return {{"id", id}, {"op", op}, {"args", arguments}}; }
}
class SampleClipResourceTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QString audio() const { return configuration.filePath("audio.wav"); }
	void pending(SampleClip* clip, const QString& path)
	{ QVERIFY(clip); QVERIFY(clip->sampleUnresolved()); QCOMPARE(clip->sampleFile(), path); QCOMPARE(clip->sample().sampleSize(), std::size_t(0)); QVERIFY(!clip->playbackSample(0)); QCOMPARE(clip->startPosition().getTicks(), 192); QCOMPARE(clip->length().getTicks(), 311); QCOMPARE(clip->startTimeOffset().getTicks(), -13); QCOMPARE(clip->sample().amplification(), -.75f); QVERIFY(clip->sample().reversed()); QCOMPARE(clip->sourceSampleRate(), 44100); QVERIFY(clip->sourceSampleWindow()); QCOMPARE(clip->sourceSampleWindow()->first, 17); QCOMPARE(clip->sourceSampleWindow()->second, 88); }
private slots:
	void initTestCase()
	{ QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); QVERIFY(wave(audio(), 128)); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void missingSourcesRetainNativePlacementAndMetadata_data()
	{ QTest::addColumn<bool>("marked"); QTest::addColumn<bool>("present"); QTest::newRow("legacy-absent") << false << false; QTest::newRow("marked-absent") << true << false; QTest::newRow("marked-appeared") << true << true; }
	void missingSourcesRetainNativePlacementAndMetadata()
	{ QFETCH(bool, marked); QFETCH(bool, present); QTemporaryDir source; const auto path = source.filePath("later.wav"); if (present) { QVERIFY(wave(path, 128)); } auto clip = seed(); const auto doc = plan(path, marked); clip->loadSettings(doc.documentElement()); pending(clip, path); QVERIFY(!clip->hasSampleFileLoaded(path)); QVERIFY(Engine::getSong()->hasErrors()); QVERIFY(!Engine::getSong()->hasFatalErrors()); const auto length = clip->length(); clip->setAutoResize(true); clip->updateLength(); QCOMPARE(clip->length(), length); QCOMPARE(clip->startTimeOffset().getTicks(), -13); }
	void checkedRepairPreservesArrangementAndPlayback_data()
	{ QTest::addColumn<bool>("structured"); QTest::addColumn<bool>("automatic"); QTest::newRow("native-manual") << false << false; QTest::newRow("native-automatic") << false << true; QTest::newRow("studio-manual") << true << false; QTest::newRow("studio-automatic") << true << true; }
	void checkedRepairPreservesArrangementAndPlayback()
	{
		QFETCH(bool, structured); QFETCH(bool, automatic); QTemporaryDir source; auto clip = seed(); const auto doc = plan(source.filePath("missing.wav"), true, automatic); clip->loadSettings(doc.documentElement()); QString error;
		if (structured) { const auto result = StudioCapabilities::execute("sample.load", args(audio())); QVERIFY2(result.value("ok").toBool(), QJsonDocument(result).toJson().constData()); QVERIFY(!result.value("result").toObject().value("unresolved").toBool()); } else { QVERIFY2(clip->loadSampleFile(audio(), error, true), qPrintable(error)); }
		QVERIFY(!clip->sampleUnresolved()); QCOMPARE(clip->sample().sampleSize(), std::size_t(128)); QCOMPARE(clip->sample().startFrame(), 17); QCOMPARE(clip->sample().endFrame(), 88); QCOMPARE(clip->sample().amplification(), -.75f); QVERIFY(clip->sample().reversed()); QCOMPARE(clip->startPosition().getTicks(), 192); QCOMPARE(clip->length().getTicks(), 311); QCOMPARE(clip->startTimeOffset().getTicks(), -13); QCOMPARE(clip->getAutoResize(), automatic); QCOMPARE(clip->name(), QString("Missing arrangement")); QVERIFY(clip->isMuted());
	}
	void shorterRepairIsRefusedUntilExplicitReplacement_data()
	{ QTest::addColumn<bool>("structured"); QTest::newRow("native") << false; QTest::newRow("studio") << true; }
	void shorterRepairIsRefusedUntilExplicitReplacement()
	{
		QFETCH(bool, structured); QTemporaryDir source; const auto missing = source.filePath("missing.wav"), shortFile = source.filePath("short.wav"); QVERIFY(wave(shortFile, 32)); auto clip = seed(); const auto doc = plan(missing); clip->loadSettings(doc.documentElement()); QString error;
		if (structured) { QVERIFY(!StudioCapabilities::execute("sample.load", args(shortFile)).value("ok").toBool()); } else { QVERIFY(!clip->loadSampleFile(shortFile, error, true)); } pending(clip, missing);
		if (structured) { auto options = args(shortFile); options.insert("preserveSettings", false); QVERIFY(StudioCapabilities::execute("sample.load", options).value("ok").toBool()); } else { QVERIFY(clip->loadSampleFile(shortFile, error, false)); }
		QVERIFY(!clip->sampleUnresolved()); QCOMPARE(clip->sample().sampleSize(), std::size_t(32)); QCOMPARE(clip->sample().startFrame(), 0); QCOMPARE(clip->sample().endFrame(), 32); QCOMPARE(clip->sample().amplification(), 1.f); QVERIFY(!clip->sample().reversed()); QCOMPARE(clip->startTimeOffset().getTicks(), 0);
	}
	void nativeProjectAndCheckpointPreserveMissingState_data()
	{ QTest::addColumn<bool>("checkpoint"); QTest::newRow("plain-project") << false; QTest::newRow("checkpoint") << true; }
	void nativeProjectAndCheckpointPreserveMissingState()
	{
		QFETCH(bool, checkpoint); QTemporaryDir source; const auto path = source.filePath("later.wav"), project = source.filePath("missing.mmp"); auto clip = seed(); const auto doc = plan(path); clip->loadSettings(doc.documentElement()); QJsonArray assets; QVERIFY(Engine::getSong()->saveProjectFile(project, false, checkpoint, checkpoint ? &assets : nullptr)); QVERIFY(assets.isEmpty()); DataFile saved(project); QVERIFY(saved.matchesAssetReferences({})); QVERIFY(!saved.copyResources(source.filePath("resources"))); QVERIFY(wave(path, 128)); QVERIFY(Engine::getSong()->loadProject(project)); pending(static_cast<SampleClip*>(Engine::getSong()->tracks()[0]->getClip(0)), path); QVERIFY(!Engine::getSong()->hasFatalErrors());
	}
	void nativeCopyEntriesRetainPendingSources_data()
	{ QTest::addColumn<int>("entry"); QTest::newRow("clone") << 0; QTest::newRow("cross-track") << 1; QTest::newRow("serialized-selection") << 2; QTest::newRow("track-clone") << 3; }
	void nativeCopyEntriesRetainPendingSources()
	{
		QFETCH(int, entry); QTemporaryDir source; const auto path = source.filePath("later.wav"); auto clip = seed(); const auto doc = plan(path); clip->loadSettings(doc.documentElement()); QVERIFY(wave(path, 128)); SampleClip* copied = nullptr;
		if (entry == 0) { copied = clip->clone(); } else if (entry == 1) { copied = static_cast<SampleClip*>(clip->copyTo(Track::create(Track::Type::Sample, Engine::getSong()))); } else if (entry == 2) { DataFile transfer(DataFile::Type::DragNDropData); QString error; QVERIFY(NativeClipTransfer::save({clip}, clip, transfer, error)); QList<Clip*> out; QVERIFY2(NativeClipTransfer::load(transfer, {{Track::create(Track::Type::Sample, Engine::getSong()), 192}}, out, error), qPrintable(error)); QCOMPARE(out.size(), 1); copied = static_cast<SampleClip*>(out[0]); } else { auto track = clip->getTrack()->clone(); QVERIFY(track); copied = static_cast<SampleClip*>(track->getClip(0)); }
		pending(copied, path); QString error; QVERIFY(copied->loadSampleFile(path, error, true)); pending(clip, path);
	}
	void retainedBindingsCannotBecomeUnresolved_data()
	{ QTest::addColumn<bool>("retainedPath"); QTest::newRow("declared-source") << false; QTest::newRow("retained-source") << true; }
	void retainedBindingsCannotBecomeUnresolved()
	{
		QFETCH(bool, retainedPath); auto clip = seed(); QString error; const auto snapshot = FileResourceSnapshot::capture(audio(), error); QVERIFY(snapshot); DataFile doc(DataFile::Type::SongProject); auto element = doc.createElement("sampleclip"); doc.content().appendChild(element); element.setAttribute("src", retainedPath ? snapshot->path() : audio()); element.setAttribute("unresolved", 1); { DataFile::ResourceSourceScope saving(doc); DataFile::registerResourceSnapshot(doc, element, "src", snapshot); } { DataFile::ResourceReadScope reading(doc); QVERIFY_EXCEPTION_THROWN(clip->loadSettings(element), std::invalid_argument); } QTemporaryDir destination; QJsonArray assets; QVERIFY(!doc.copyResources(destination.filePath("assets"), &assets)); QVERIFY(assets.isEmpty());
	}
	void malformedPendingStateNeverBecomesACompleteRestore_data()
	{ QTest::addColumn<int>("change"); for (int index = 0; index < 6; ++index) { QTest::newRow(qPrintable(QString::number(index))) << index; } }
	void malformedPendingStateNeverBecomesACompleteRestore()
	{
		QFETCH(int, change); QTemporaryDir source; auto clip = seed(); auto doc = plan(source.filePath("missing.wav")); auto element = doc.documentElement(); if (change == 0) { element.setAttribute("src", ""); } else if (change == 1) { element.setAttribute("data", "AAAA"); } else if (change == 2) { element.setAttribute("sample_rate", 0); } else if (change == 3) { element.setAttribute("sample_start", -1); } else if (change == 4) { element.setAttribute("sample_end", 4); } else { element.setAttribute("sample_end", "2147483648"); }
		bool rejected = false; try { clip->loadSettings(element); } catch (const std::exception&) { rejected = true; } QVERIFY(rejected || Engine::getSong()->hasFatalErrors()); QString error; QVERIFY(clip->loadSampleFile(audio(), error));
	}
	void returnedDecoderCannotResumeOnRetiredOwners_data()
	{ QTest::addColumn<bool>("track"); QTest::newRow("clip") << false; QTest::newRow("track") << true; }
	void returnedDecoderCannotResumeOnRetiredOwners()
	{
		QFETCH(bool, track); const QPointer<SampleClip> clip(seed()); const QPointer<Track> owner(clip->getTrack()); bool retired = false; QString error; auto active = clip.data(); { FileResourceIO::Scope work([&] { if (!retired) { retired = true; if (track) { delete owner.data(); } else { delete clip.data(); } } return false; }); QVERIFY(!active->loadSampleFile(audio(), error)); } QVERIFY(retired); QVERIFY(!clip); QVERIFY(!error.isEmpty());
	}
	void dirtyNotificationCannotChangeAcceptedPlaybackState()
	{
		auto clip = seed(); NativeSongTestAccess::setModified(Engine::getSong(), false); bool changed = false; const auto connection = connect(Engine::getSong(), &Song::modified, this, [&] { if (!changed && Engine::getSong()->isModified()) { changed = true; clip->sample().setAmplification(.25f); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QString error; QVERIFY(!clip->loadSampleFile(audio(), error)); QVERIFY(changed); QVERIFY(!error.isEmpty());
	}
	void borrowedFilenameIsCopiedBeforeCooperativeWork()
	{
		auto clip = seed(); QTemporaryDir source; const auto shorter = source.filePath("short.wav"); QVERIFY(wave(shorter, 32)); QString path(audio()), error; bool changed = false; { FileResourceIO::Scope work([&] { if (!changed) { changed = true; path = shorter; } return false; }); QVERIFY(clip->loadSampleFile(path, error)); } QVERIFY(changed); QCOMPARE(clip->sample().sampleSize(), std::size_t(128));
	}
	void embeddedSerializationRetainsItsBufferAfterClipRetirement()
	{
		const QPointer<SampleClip> clip(seed()); clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(64, SampleFrame(.25f)), 44100)); QDomDocument doc; auto element = doc.createElement("sampleclip"); doc.appendChild(element); int checks = 0; bool rejected = false; auto active = clip.data(); { FileResourceIO::Scope work([&] { if (++checks == 3) { delete clip.data(); } return false; }); try { active->saveSettings(doc, element); } catch (const std::exception&) { rejected = true; } } QVERIFY(rejected); QVERIFY(!clip);
	}
	void resourceAndSettingsWorkCannotReenterFromSignals()
	{
		auto clip = seed(); bool called = false, fileRefused = false, saveRefused = false; const auto connection = connect(clip, &SampleClip::sampleChanged, this, [&] { if (called) { return; } called = true; QString error; fileRefused = !clip->loadSampleFile(audio(), error); QDomDocument doc; auto element = doc.createElement("sampleclip"); doc.appendChild(element); try { clip->saveSettings(doc, element); } catch (const std::logic_error&) { saveRefused = true; } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QString error; QVERIFY(clip->loadSampleFile(audio(), error)); QVERIFY(called && fileRefused && saveRefused); QVERIFY(clip->loadSampleFile(audio(), error));
	}
	void runtimeRepairsUndoAndRecoversPendingSampleProjects()
	{
		QTemporaryDir source, session; const auto path = source.filePath("later.wav"), legacy = source.filePath("legacy.mmp"), broken = source.filePath("broken.mmp"); auto clip = seed(); const auto initial = plan(path); clip->loadSettings(initial.documentElement()); QVERIFY(Engine::getSong()->saveProjectFile(legacy)); DataFile doc(legacy); doc.elementsByTagName("sampleclip").at(0).toElement().removeAttribute("unresolved"); QVERIFY(doc.writeFile(legacy)); DataFile bad(legacy); auto invalid = bad.elementsByTagName("sampleclip").at(0).toElement(); invalid.setAttribute("src", ""); invalid.setAttribute("data", "invalid-base64"); QVERIFY(bad.writeFile(broken)); const auto current = [] { return static_cast<SampleClip*>(Engine::getSong()->tracks()[0]->getClip(0)); }; const auto badCommand = request("bad", "project.load", {{"path", broken}, {"summary", true}}); QJsonObject rejected; QString error;
		{ StudioRuntime runtime(session.path()); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("legacy", "project.load", {{"path", legacy}, {"summary", true}})).value("ok").toBool()); pending(current(), path); QVERIFY(wave(path, 128)); QVERIFY(runtime.process(request("repair", "sample.load", args(path))).value("ok").toBool()); QVERIFY(!current()->sampleUnresolved()); QCOMPARE(current()->sample().startFrame(), 17); QCOMPARE(current()->length().getTicks(), 311); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); pending(current(), path); rejected = runtime.process(badCommand); QVERIFY(!rejected.value("ok").toBool()); QVERIFY(!runtime.stopping()); pending(current(), path); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QVERIFY(!current()->sampleUnresolved()); QVERIFY(runtime.process(request("undo-again", "session.undo", {{"summary", true}})).value("ok").toBool()); }
		{ StudioRuntime runtime(session.path()); QVERIFY(runtime.open(error)); pending(current(), path); auto replay = runtime.process(badCommand); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected); QVERIFY(runtime.process(request("repair-after-reopen", "sample.load", args(path))).value("ok").toBool()); QVERIFY(QFile::remove(path)); }
		StudioRuntime finalOpen(session.path()); QVERIFY(finalOpen.open(error)); QVERIFY(!current()->sampleUnresolved()); QCOMPARE(current()->sample().sampleSize(), std::size_t(128)); QCOMPARE(current()->sample().startFrame(), 17); QCOMPARE(current()->sample().endFrame(), 88); QCOMPARE(current()->length().getTicks(), 311);
	}
	void unknownSourceWindowUsesNewDecodedExtentOnRepair()
	{
		QTemporaryDir source; auto clip = seed(); auto doc = plan(source.filePath("missing.wav")); auto element = doc.documentElement(); element.removeAttribute("sample_start"); element.removeAttribute("sample_end"); element.removeAttribute("sample_rate"); clip->loadSettings(element); QVERIFY(!clip->sourceSampleWindow()); QCOMPARE(clip->sourceSampleRate(), 0); QString error; QVERIFY(clip->loadSampleFile(audio(), error, true)); QCOMPARE(clip->sample().startFrame(), 0); QCOMPARE(clip->sample().endFrame(), 128); QCOMPARE(clip->length().getTicks(), 311);
	}
	void nativeBufferPublicationClearsPendingSource()
	{
		QTemporaryDir source; auto clip = seed(); const auto doc = plan(source.filePath("missing.wav")); clip->loadSettings(doc.documentElement()); const auto buffer = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(.125f)), 24000); clip->setSampleBuffer(buffer); QVERIFY(!clip->sampleUnresolved()); QVERIFY(clip->sampleFile().isEmpty()); QCOMPARE(clip->sample().buffer().get(), buffer.get()); QCOMPARE(clip->sample().amplification(), 1.f); QVERIFY(!clip->sample().reversed());
	}
	void fatalGenerationSeparatesRetainedWarningsFromCopyFailures()
	{
		auto song = Engine::getSong(); const auto before = song->fatalErrorGeneration(); song->collectUnresolvedResource(audio()); QCOMPARE(song->fatalErrorGeneration(), before); song->collectError("fatal copy callback"); QVERIFY(song->fatalErrorGeneration() != before); const auto fatal = song->fatalErrorGeneration(); song->clearErrors(); QCOMPARE(song->fatalErrorGeneration(), fatal); song->collectError("fatal copy callback"); QVERIFY(song->fatalErrorGeneration() != fatal);
	}
	void decodedFileAndEmbeddedSettingsRetainNativeAudio_data()
	{ QTest::addColumn<bool>("embedded"); QTest::newRow("file") << false; QTest::newRow("embedded") << true; }
	void decodedFileAndEmbeddedSettingsRetainNativeAudio()
	{
		QFETCH(bool, embedded); auto original = seed(); QString error; QVERIFY(original->loadSampleFile(audio(), error)); if (embedded) { const auto source = original->sample().buffer(); original->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(source->begin(), source->end()), source->sampleRate())); } original->setAutoResize(false); QVERIFY(original->setSampleWindow(17, 88)); original->sample().setAmplification(-.75f); original->sample().setReversed(true); original->movePosition(192); original->changeLength(311); original->setStartTimeOffset(-13); original->setName("Native roundtrip"); const auto expected = original->sample().buffer(); QDomDocument doc; auto element = doc.createElement("sampleclip"); doc.appendChild(element); original->saveSettings(doc, element); auto restored = seed(); restored->loadSettings(element);
		QVERIFY(!restored->sampleUnresolved()); QCOMPARE(restored->sample().sampleSize(), expected->size()); QCOMPARE(restored->sample().data()[0][0], expected->data()[0][0]); QCOMPARE(restored->sample().startFrame(), 17); QCOMPARE(restored->sample().endFrame(), 88); QCOMPARE(restored->sample().amplification(), -.75f); QVERIFY(restored->sample().reversed()); QCOMPARE(restored->length().getTicks(), 311); QCOMPARE(restored->startTimeOffset().getTicks(), -13); QCOMPARE(restored->name(), QString("Native roundtrip")); QVERIFY(!Engine::getSong()->hasErrors());
	}
	void legacyEmbeddedFallbackSurvivesAMissingExternalFilename()
	{
		QTemporaryDir source; const auto buffer = SampleBuffer::fromFile(audio()); QVERIFY(buffer); auto doc = plan(source.filePath("missing.wav"), false); auto element = doc.documentElement(); element.setAttribute("data", buffer->toBase64()); auto clip = seed(); clip->loadSettings(element); QVERIFY(!clip->sampleUnresolved()); QCOMPARE(clip->sample().sampleSize(), std::size_t(128)); QCOMPARE(clip->sample().startFrame(), 17); QCOMPARE(clip->sample().endFrame(), 88); QCOMPARE(clip->sample().data()[0][0], buffer->data()[0][0]); QVERIFY(clip->sampleFile().isEmpty()); QVERIFY(!Engine::getSong()->hasFatalErrors());
	}
	void structuredClearRemovesPendingAudioWithoutRemovingTheClip()
	{
		QTemporaryDir source; auto clip = seed(); const auto doc = plan(source.filePath("missing.wav")); clip->loadSettings(doc.documentElement()); const auto result = StudioCapabilities::execute("sample.load", args(QString{})); QVERIFY2(result.value("ok").toBool(), QJsonDocument(result).toJson().constData()); QVERIFY(!clip->sampleUnresolved()); QVERIFY(clip->sampleFile().isEmpty()); QVERIFY(clip->sample().buffer()->empty()); QCOMPARE(clip->startPosition().getTicks(), 192); QCOMPARE(clip->length().getTicks(), 311); QCOMPARE(clip->startTimeOffset().getTicks(), 0); QCOMPARE(clip->name(), QString("Missing arrangement")); QVERIFY(!result.value("result").toObject().value("unresolved").toBool());
	}
};
QTEST_GUILESS_MAIN(SampleClipResourceTest)
#include "SampleClipResourceTest.moc"
