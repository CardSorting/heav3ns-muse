#include "../core/NativeSongTestAccess.h"
/* Native three-oscillator wave preservation and repair. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QVector>
#include <cmath>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "FileResourceSnapshot.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "Note.h"
#include "Oscillator.h"
#include "PluginFactory.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "TripleOscillator.h"
using namespace lmms;
namespace
{
class NativeTrack final : public InstrumentTrack
{
public:
	NativeTrack() : InstrumentTrack(Engine::getSong()) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new TripleOscillator(this); }
};
QByteArray little(quint32 value, int count) { QByteArray out; for (int i = 0; i < count; ++i) { out.append(static_cast<char>(value >> (8 * i))); } return out; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
bool wave(const QString& path) { QByteArray pcm(128, '\0'); pcm[1] = 0x20; return write(path, QByteArray("RIFF") + little(36 + pcm.size(), 4) + "WAVEfmt " + little(16, 4) + little(1, 2) + little(1, 2) + little(44100, 4) + little(88200, 4) + little(2, 2) + little(16, 2) + "data" + little(pcm.size(), 4) + pcm); }
QDomElement settings(QDomDocument& document, int slot, const QString& path, bool pending = true)
{
	auto node = document.createElement("tripleoscillator"); document.appendChild(node); const auto suffix = QString::number(slot); node.setAttribute("userwavefile" + suffix, path); if (pending) { node.setAttribute("userwaveunresolved" + suffix, 1); }
	node.setAttribute("vol" + suffix, 13); node.setAttribute("pan" + suffix, -7); node.setAttribute("coarse" + suffix, 5); node.setAttribute("finel" + suffix, 11); node.setAttribute("finer" + suffix, -12); node.setAttribute("phoffset" + suffix, 47); node.setAttribute("stphdetun" + suffix, 29); node.setAttribute("wavetype" + suffix, static_cast<int>(Oscillator::WaveShape::UserDefined)); node.setAttribute("modalgo" + QString::number(slot + 1), 1); node.setAttribute("useWaveTable" + QString::number(slot + 1), 1); return node;
}
void missing(TripleOscillator* native, int slot, const QString& path, bool pending = true) { QDomDocument document; native->loadSettings(settings(document, slot, path, pending)); }
OscillatorObject* oscillator(TripleOscillator* native, int slot) { return native->findChildren<OscillatorObject*>()[slot]; }
QVector<float> values(OscillatorObject* native) { QVector<float> out; for (auto model : native->findChildren<AutomatableModel*>(QString{}, Qt::FindDirectChildrenOnly)) { out.append(model->storedValue()); } return out; }
std::shared_ptr<const SampleBuffer> buffer(const Plugin::FileResource& resource) { return std::static_pointer_cast<const SampleBuffer>(resource.identity); }
double energy(TripleOscillator* native)
{
	const auto frames = Engine::audioEngine()->framesPerPeriod(); std::vector<SampleFrame> pcm(frames); NotePlayHandle voice(native->instrumentTrack(), 0, frames * 4, Note(TimePos(48), TimePos(), 69)); native->playNote(&voice, pcm.data()); double total = 0;
	for (const auto& frame : pcm) { for (int channel = 0; channel < 2; ++channel) { if (!std::isfinite(frame[channel])) { throw std::runtime_error("Nonfinite native oscillator output"); } total += std::abs(frame[channel]); } } return total;
}
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
QJsonObject decision(const QString& protocol, const QString& id, const QString& name, const QJsonObject& args)
{
	const auto arguments = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
	if (protocol == "responses") { return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", arguments}}}}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{QJsonObject{{"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name}, {"arguments", arguments}}}}}}}}}}}};
}
}
class TripleOscillatorResourceTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	TripleOscillator* seed() { auto track = new NativeTrack; auto native = dynamic_cast<TripleOscillator*>(track->loadInstrument("tripleoscillator")); if (!native) { throw std::runtime_error("Native TripleOscillator was not selected"); } return native; }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); NativeSongTestAccess::setProjectFileName(Engine::getSong(), ""); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void missingWavesRetainPerOscillatorPathsAndControls_data()
	{ QTest::addColumn<int>("slot"); QTest::addColumn<int>("mode"); for (int slot = 0; slot < 3; ++slot) { for (int mode = 0; mode < 3; ++mode) { QTest::newRow(qPrintable(QString::number(slot) + ":" + QString::number(mode))) << slot << mode; } } }
	void missingWavesRetainPerOscillatorPathsAndControls()
	{
		QFETCH(int, slot); QFETCH(int, mode); QTemporaryDir source; const auto path = source.filePath("missing.wav"); if (mode == 2) { QVERIFY(wave(path)); } auto native = seed(); missing(native, slot, path, mode != 0); auto object = oscillator(native, slot); const auto resources = native->fileResources(); QCOMPARE(resources.size(), std::size_t(3)); QCOMPARE(resources[slot].path, path); QVERIFY(resources[slot].unresolved); QVERIFY(object->userWaveBuffer()->empty()); QCOMPARE(Oscillator::userWaveSample(object->userWaveBuffer().get(), .125f), 0.f); QVERIFY(!Engine::getSong()->hasFatalErrors()); QCOMPARE(values(object).size(), 10); QCOMPARE(values(object)[0], 13.f); QCOMPARE(values(object)[1], -7.f);
		QDomDocument saved; auto node = saved.createElement("tripleoscillator"); saved.appendChild(node); native->saveSettings(saved, node); QCOMPARE(node.attribute("userwavefile" + QString::number(slot)), path); QCOMPARE(node.attribute("userwaveunresolved" + QString::number(slot)), QString("1")); const auto before = values(object); auto restored = seed(); restored->loadSettings(node); QVERIFY(restored->fileResources()[slot].unresolved); QCOMPARE(values(oscillator(restored, slot)), before); for (int i = 0; i < 3; ++i) { if (i != slot) { QVERIFY(!restored->fileResources()[i].unresolved); } }
	}
	void checkedRepairsPreserveNativeControlsAndOtherResources_data()
	{ QTest::addColumn<int>("slot"); QTest::addColumn<bool>("structured"); for (int slot = 0; slot < 3; ++slot) { QTest::newRow(qPrintable(QString::number(slot) + "-direct")) << slot << false; QTest::newRow(qPrintable(QString::number(slot) + "-structured")) << slot << true; } }
	void checkedRepairsPreserveNativeControlsAndOtherResources()
	{
		QFETCH(int, slot); QFETCH(bool, structured); QTemporaryDir source; const auto path = source.filePath("repair.wav"); QVERIFY(wave(path)); auto native = seed(); missing(native, slot, source.filePath("missing.wav")); auto object = oscillator(native, slot); if (slot == 1) { native->fileResources()[slot].selector->setValue(static_cast<int>(Oscillator::WaveShape::Sine)); } const auto before = values(object); const auto resources = native->fileResources(); QString error;
		if (structured) { const auto response = StudioCapabilities::execute("instrument.resource.set", {{"track", 0}, {"resource", QString("oscillator%1").arg(slot + 1)}, {"path", path}}); QVERIFY2(response.value("ok").toBool(), QJsonDocument(response).toJson().constData()); } else { QVERIFY(native->setFileResource(QString("oscillator%1").arg(slot + 1), path, error)); }
		QVERIFY(!native->fileResources()[slot].unresolved); QCOMPARE(object->userWaveBuffer()->size(), std::size_t(64)); QCOMPARE(values(object), before); for (int i = 0; i < 3; ++i) { if (i != slot) { QVERIFY(native->fileResources()[i].identity == resources[i].identity); QCOMPARE(native->fileResources()[i].path, resources[i].path); } } QDomDocument document; auto node = document.createElement("tripleoscillator"); native->saveSettings(document, node); QVERIFY(!node.hasAttribute("userwaveunresolved" + QString::number(slot)));
	}
	void failedReplacementAndClearHaveDistinctNativeState_data()
	{ QTest::addColumn<bool>("loaded"); QTest::newRow("pending") << false; QTest::newRow("accepted") << true; }
	void failedReplacementAndClearHaveDistinctNativeState()
	{
		QFETCH(bool, loaded); QTemporaryDir source; const auto path = source.filePath("repair.wav"), lost = source.filePath("missing.wav"), corrupt = source.filePath("corrupt.wav"); QVERIFY(wave(path)); QVERIFY(write(corrupt, "invalid audio")); auto native = seed(); missing(native, 0, lost); QString error; if (loaded) { QVERIFY(native->setFileResource("oscillator1", path, error)); } const auto resource = native->fileResources()[0]; const auto controls = values(oscillator(native, 0)); QVERIFY(!native->setFileResource("oscillator1", corrupt, error)); QVERIFY(!error.isEmpty()); QVERIFY(native->fileResources()[0].identity == resource.identity); QCOMPARE(native->fileResources()[0].path, resource.path); QCOMPARE(native->fileResources()[0].unresolved, !loaded); QVERIFY(native->setFileResource("oscillator1", {}, error)); QVERIFY(!native->fileResources()[0].unresolved); QVERIFY(native->fileResources()[0].path.isEmpty()); QVERIFY(buffer(native->fileResources()[0])->empty()); QCOMPARE(values(oscillator(native, 0)), controls);
	}
	void mixedPendingAndAcceptedWavesProduceExactCheckpointReferences()
	{
		QTemporaryDir source, destination; const auto lost = source.filePath("missing.wav"), path = source.filePath("accepted.wav"); QVERIFY(wave(path)); auto native = seed(); QDomDocument settingsDocument; auto plan = settings(settingsDocument, 0, lost); plan.setAttribute("userwavefile1", path); native->loadSettings(plan); DataFile document(DataFile::Type::SongProject); auto node = document.createElement("tripleoscillator"); document.content().appendChild(node); { DataFile::ResourceSourceScope saving(document, true, true); native->saveSettings(document, node); } QJsonArray assets; QVERIFY(document.copyResources(destination.filePath("assets"), &assets)); QCOMPARE(assets.size(), 1); QVERIFY(document.matchesAssetReferences(assets)); QCOMPARE(node.attribute("userwavefile0"), lost); QVERIFY(node.attribute("userwavefile1").startsWith("local:assets/")); QVERIFY(!document.copyResources(destination.filePath("bundle"))); node.removeAttribute("userwaveunresolved0"); QVERIFY(!document.matchesAssetReferences(assets));
	}
	void malformedPendingStateCannotDiscardAnAcceptedBinding_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("empty") << 0; QTest::newRow("null") << 1; QTest::newRow("local-unavailable") << 2; QTest::newRow("snapshot") << 3; QTest::newRow("same-retained-name") << 4; QTest::newRow("sample-backed") << 5; }
	void malformedPendingStateCannotDiscardAnAcceptedBinding()
	{
		QFETCH(int, kind); QTemporaryDir source, destination; auto native = seed(); QString path = kind == 0 ? QString{} : kind == 1 ? QString(QChar::Null) : kind == 2 ? QString("local:lost.wav") : source.filePath("wave.wav"); QString error; std::shared_ptr<FileResourceSnapshot> snapshot; std::shared_ptr<const SampleBuffer> sample; if (kind >= 3) { QVERIFY(wave(path)); snapshot = FileResourceSnapshot::capture(path, error); QVERIFY(snapshot); sample = SampleBuffer::fromFile(path); if (kind == 4) { path = snapshot->path(); } }
		DataFile document(DataFile::Type::SongProject); auto node = document.createElement("tripleoscillator"); document.content().appendChild(node); node.setAttribute("userwavefile1", path); node.setAttribute("userwaveunresolved1", 1); if (snapshot) { DataFile::ResourceSourceScope saving(document, true, kind == 5); DataFile::registerResourceSnapshot(document, node, "userwavefile1", snapshot, kind == 5 ? sample : nullptr); } { DataFile::ResourceReadScope reading(document); QVERIFY_EXCEPTION_THROWN(native->loadSettings(node), std::invalid_argument); } QJsonArray assets; QVERIFY(!document.copyResources(destination.filePath("assets"), &assets)); QVERIFY(assets.isEmpty());
	}
	void serializersObserveEveryReturnedControlLifetime_data()
	{ QTest::addColumn<int>("field"); QTest::addColumn<bool>("saving"); for (int field = 0; field < 10; ++field) { QTest::newRow(qPrintable(QString::number(field) + "-load")) << field << false; QTest::newRow(qPrintable(QString::number(field) + "-save")) << field << true; } }
	void serializersObserveEveryReturnedControlLifetime()
	{
		QFETCH(int, field); QFETCH(bool, saving); const char* keys[] = {"vol0", "pan0", "coarse0", "finel0", "finer0", "phoffset0", "stphdetun0", "wavetype0", "modalgo1", "useWaveTable1"}; auto native = seed(); const QPointer<TripleOscillator> owner(native); const QPointer<InstrumentTrack> track(native->instrumentTrack()); auto models = oscillator(native, 0)->findChildren<AutomatableModel*>(QString{}, Qt::FindDirectChildrenOnly); if (!saving && field == 9) { models[field]->setValue(0); } QTemporaryDir source; QDomDocument document; auto node = settings(document, 0, source.filePath("missing.wav")); bool retired = false;
		if (saving) { for (const auto key : keys) { node.removeAttribute(key); } FileResourceIO::Scope work([&] { if (!retired && node.hasAttribute(keys[field])) { retired = true; delete track.data(); } return false; }); QVERIFY_EXCEPTION_THROWN(native->saveSettings(document, node), std::runtime_error); }
		else { connect(models[field], &AutomatableModel::dataChanged, this, [&] { if (!retired) { retired = true; delete track.data(); } }); QVERIFY_EXCEPTION_THROWN(native->loadSettings(node), std::runtime_error); } QVERIFY(retired); QVERIFY(!owner); QVERIFY(!track);
	}
	void decodingCannotResumeOnRetiredOrReplacedNativeActors_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("instrument") << 0; QTest::newRow("track") << 1; QTest::newRow("oscillator") << 2; QTest::newRow("project-path") << 3; }
	void decodingCannotResumeOnRetiredOrReplacedNativeActors()
	{
		QFETCH(int, kind); QTemporaryDir source; const auto path = source.filePath("repair.wav"); QVERIFY(wave(path)); auto native = seed(); const QPointer<TripleOscillator> owner(native); const QPointer<InstrumentTrack> track(native->instrumentTrack()); bool changed = false; QString error; { FileResourceIO::Scope work([&] { if (!changed) { changed = true; if (kind == 0) { delete owner.data(); } if (kind == 1) { delete track.data(); } if (kind == 2) { delete oscillator(owner.data(), 2); } if (kind == 3) { NativeSongTestAccess::setProjectFileName(Engine::getSong(), source.filePath("other.mmp")); } } return false; }); QVERIFY_EXCEPTION_THROWN(native->setFileResource("oscillator1", path, error), std::runtime_error); } QVERIFY(changed);
	}
	void settingsAndWaveTransitionsRefuseReentry()
	{
		auto native = seed(); auto object = oscillator(native, 0); QTemporaryDir source; QDomDocument document; const auto node = settings(document, 0, source.filePath("missing.wav")); bool nested = false; connect(object->findChildren<AutomatableModel*>(QString{}, Qt::FindDirectChildrenOnly)[0], &AutomatableModel::dataChanged, this, [&] { if (nested) { return; } nested = true; QString error; QVERIFY_EXCEPTION_THROWN(native->setFileResource("oscillator2", {}, error), std::logic_error); QVERIFY_EXCEPTION_THROWN(object->setUserWave({}, error), std::logic_error); QVERIFY_EXCEPTION_THROWN(native->loadSettings(node), std::logic_error); QDomDocument saved; auto element = saved.createElement("tripleoscillator"); QVERIFY_EXCEPTION_THROWN(native->saveSettings(saved, element), std::logic_error); }); native->loadSettings(node); QVERIFY(nested); QString error; QVERIFY(native->setFileResource("oscillator2", {}, error));
	}
	void cancellationAndThrownWorkReleaseAdmission_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("cancel") << 0; QTest::newRow("standard") << 1; QTest::newRow("nonstandard") << 2; }
	void cancellationAndThrownWorkReleaseAdmission()
	{
		QFETCH(int, kind); auto native = seed(); QString error; bool caught = false; try { FileResourceIO::Scope work([&] { if (kind == 1) { throw std::runtime_error("Fixture waveform work"); } if (kind == 2) { throw 97; } return true; }); native->setFileResource("oscillator1", {}, error); } catch (...) { caught = true; } QVERIFY(caught); QVERIFY(native->setFileResource("oscillator1", {}, error));
	}
	void borrowedArgumentsAreCopiedAndOtherOscillatorControlsAreChecked_data()
	{ QTest::addColumn<bool>("control"); QTest::newRow("copied-path") << false; QTest::newRow("other-oscillator-control") << true; }
	void borrowedArgumentsAreCopiedAndOtherOscillatorControlsAreChecked()
	{
		QFETCH(bool, control); QTemporaryDir source; QString path = source.filePath("wave.wav"), key = "oscillator1"; QVERIFY(wave(path)); auto native = seed(); bool changed = false; QString error; { FileResourceIO::Scope work([&] { if (!changed) { changed = true; if (control) { native->fileResources()[2].selector->setValue(static_cast<int>(Oscillator::WaveShape::Triangle)); } else { key = "unknown"; path = source.filePath("absent.wav"); } } return false; }); if (control) { QVERIFY_EXCEPTION_THROWN(native->setFileResource(key, path, error), std::runtime_error); } else { QVERIFY(native->setFileResource(key, path, error)); } } QVERIFY(changed); QCOMPARE(buffer(native->fileResources()[0])->size(), control ? std::size_t(0) : std::size_t(64));
	}
	void acceptedSerializationUsesOwnedWaveAfterOriginalRemoval()
	{
		QTemporaryDir source; const auto path = source.filePath("wave.wav"); QVERIFY(wave(path)); auto native = seed(); QString error; QVERIFY(native->setFileResource("oscillator3", path, error)); DataFile document(DataFile::Type::SongProject); auto node = document.createElement("tripleoscillator"); document.content().appendChild(node); { DataFile::ResourceSourceScope saving(document, true, true); native->saveSettings(document, node); } QVERIFY(QFile::remove(path)); auto restored = seed(); { DataFile::ResourceReadScope reading(document); restored->loadSettings(node); } QVERIFY(!restored->fileResources()[2].unresolved); QCOMPARE(buffer(restored->fileResources()[2])->size(), std::size_t(64)); QCOMPARE(buffer(restored->fileResources()[2])->data()[0][0], buffer(native->fileResources()[2])->data()[0][0]);
	}
	void inventoryKeepsFilePresenceSeparateFromPendingWave()
	{
		QTemporaryDir source; const auto path = source.filePath("missing.wav"); auto native = seed(); missing(native, 2, path); QVERIFY(wave(path)); const auto response = StudioCapabilities::execute("project.inspect", {{"section", "resources"}}); QVERIFY(response.value("ok").toBool()); const auto page = response.value("result").toObject(); QCOMPARE(page.value("declaredUnresolvedCount").toInt(), 1); const auto row = page.value("resources").toArray()[2].toObject(); QVERIFY(row.value("unresolved").toBool()); QCOMPARE(row.value("path").toString(), path); QCOMPARE(row.value("key").toString(), QString("oscillator3")); QCOMPARE(row.value("file").toObject().value("status").toString(), QString("readableFile")); QCOMPARE(row.value("repairOperation").toString(), QString("instrument.resource.set"));
	}
	void existingCorruptWaveRemainsFatalBesideRetainedMissingWaves()
	{
		QTemporaryDir source; const auto corrupt = source.filePath("corrupt.wav"); QVERIFY(write(corrupt, "invalid audio")); auto native = seed(); QDomDocument document; auto node = settings(document, 0, source.filePath("missing.wav")); node.setAttribute("userwavefile1", corrupt); native->loadSettings(node); QVERIFY(Engine::getSong()->hasFatalErrors()); QVERIFY(native->fileResources()[0].unresolved); QVERIFY(!native->fileResources()[1].unresolved);
	}
	void nativeNoteBuffersLoseOldWaveContentWhilePendingAndRegainItAfterRepair_data()
	{ QTest::addColumn<bool>("antialias"); QTest::newRow("direct-wave") << false; QTest::newRow("antialias-table") << true; }
	void nativeNoteBuffersLoseOldWaveContentWhilePendingAndRegainItAfterRepair()
	{
		QFETCH(bool, antialias); QTemporaryDir source; const auto path = source.filePath("wave.wav"); QVERIFY(wave(path)); auto native = seed(); QDomDocument document; auto node = settings(document, 0, path, false); node.setAttribute("modalgo1", static_cast<int>(Oscillator::ModulationAlgo::SignalMix)); node.setAttribute("useWaveTable1", antialias ? 1 : 0); node.setAttribute("vol1", 0); node.setAttribute("vol2", 0); native->loadSettings(node); QVERIFY(energy(native) > 0.); node.setAttribute("userwavefile0", source.filePath("missing.wav")); node.setAttribute("userwaveunresolved0", 1); native->loadSettings(node); QCOMPARE(energy(native), 0.); QString error; QVERIFY(native->setFileResource("oscillator1", path, error)); QVERIFY(energy(native) > 0.);
	}
	void queuedQtEventsStayOutsideDecodeAndSettings()
	{
		auto native = seed(); bool queued = false; QString error; QTimer::singleShot(0, this, [&] { queued = true; }); { FileResourceIO::Scope work({}, true); QVERIFY(native->setFileResource("oscillator1", {}, error)); QVERIFY(!queued); } QCoreApplication::processEvents(); QVERIFY(queued);
	}
	void runtimeUsesActualModuleForHistoryAndOwnedReopening()
	{
		QVERIFY2(!getPluginFactory()->pluginInfo("tripleoscillator").isNull(), "The built native TripleOscillator module must be discoverable"); QTemporaryDir source, session; const auto path = source.filePath("repair.wav"), lost = source.filePath("missing.wav"), project = source.filePath("missing.mmp"), corrupt = source.filePath("bad.wav"); QVERIFY(wave(path)); QVERIFY(write(corrupt, "invalid audio")); auto native = seed(); missing(native, 1, lost, false); QVERIFY(Engine::getSong()->saveProjectFile(project)); QString error; QJsonObject receipt; const auto repair = request("repair", "instrument.resource.set", {{"track", 0}, {"resource", "oscillator2"}, {"path", path}});
		{ StudioRuntime runtime(session.path()); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("load", "project.load", {{"path", project}, {"summary", true}})).value("ok").toBool()); receipt = runtime.process(repair); QVERIFY(receipt.value("ok").toBool()); QVERIFY(!static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument()->fileResources()[1].unresolved); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QVERIFY(static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument()->fileResources()[1].unresolved); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); auto replay = runtime.process(repair); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); const auto before = manifest(session); const auto failure = runtime.process(request("bad", "instrument.resource.set", {{"track", 0}, {"resource", "oscillator2"}, {"path", corrupt}})); QVERIFY(!failure.value("ok").toBool()); QCOMPARE(manifest(session).value("history"), before.value("history")); QVERIFY(!runtime.stopping()); }
		QVERIFY(QFile::remove(path)); StudioRuntime reopened(session.path()); QVERIFY2(reopened.open(error), qPrintable(error)); const auto resource = static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument()->fileResources()[1]; QVERIFY(!resource.unresolved); QCOMPARE(buffer(resource)->size(), std::size_t(64));
	}
	void scriptedAgentsFindAndRepairAnObservedNativeOscillator_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void scriptedAgentsFindAndRepairAnObservedNativeOscillator()
	{
		QFETCH(QString, protocol); QVERIFY2(!getPluginFactory()->pluginInfo("tripleoscillator").isNull(), "The built native module must be discoverable"); QTemporaryDir source, session; const auto path = source.filePath("found.wave.wav"), project = source.filePath("missing.mmp"); QVERIFY(wave(path)); auto native = seed(); missing(native, 2, source.filePath("missing.wave.wav")); QVERIFY(Engine::getSong()->saveProjectFile(project)); QString error, key; int track = -1, round = 0; QJsonArray evidence;
		StudioRuntime runtime(session.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			QJsonObject output; for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray()) { const auto item = value.toObject(); if (item.value("role") == QJsonValue("tool") || item.value("type") == QJsonValue("function_call_output")) { output = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); } } const auto call = [&](const QString& id, const QString& name, const QJsonObject& args) { evidence.append(id); return decision(protocol, id, name, args); }; ++round;
			if (round == 1) { return call("discover", "project_inspect", {{"section", "resources"}}); } if (!output.value("ok").toBool()) { throw std::runtime_error("Native resource operation failed"); }
			if (round == 2) { for (const auto& value : output.value("result").toObject().value("resources").toArray()) { const auto row = value.toObject(); if (row.value("unresolved").toBool()) { key = row.value("key").toString(); track = row.value("owner").toObject().value("track").toInt(-1); } } if (key.isEmpty() || track < 0) { throw std::runtime_error("No native missing oscillator observed"); } return call("find", "asset_search", {{"root", source.path()}, {"query", "found.wave"}, {"extensions", QJsonArray{"wav"}}}); }
			if (round == 3) { const auto page = output.value("result").toObject(); if (page.value("total").toInt() != 1 || !page.value("scanComplete").toBool()) { throw std::runtime_error("Expected one scoped wave candidate"); } return call("repair", "studio_execute", {{"op", "instrument.resource.set"}, {"args", QJsonObject{{"track", track}, {"resource", key}, {"path", page.value("matches").toArray()[0].toObject().value("path")}}}}); }
			if (round == 4) { return call("verify", "project_inspect", {{"section", "resources"}}); } if (output.value("result").toObject().value("declaredUnresolvedCount").toInt(-1) != 0) { throw std::runtime_error("Repair not observed"); } return decision(protocol, "finish", "agent_finish", {{"status", "complete"}, {"summary", "Repaired the observed oscillator reference; musical evaluation was not performed."}, {"evidence", evidence}});
		}); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("load", "project.load", {{"path", project}, {"summary", true}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-three-oscillator-repair-fixture"}}}})).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Find found.wave.wav in " + source.path() + " and repair the missing oscillator, preserving the native controls."}})).value("ok").toBool()); for (int i = 0; i < 64; ++i) { runtime.advanceAgent(); const auto status = manifest(session).value("agent").toObject().value("job").toObject().value("status").toString(); if (status == "complete" || status == "failed" || status == "paused" || status == "cancelled") { break; } } QCOMPARE(manifest(session).value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete"));
	}
};
QTEST_GUILESS_MAIN(TripleOscillatorResourceTest)
#include "TripleOscillatorResourceTest.moc"
