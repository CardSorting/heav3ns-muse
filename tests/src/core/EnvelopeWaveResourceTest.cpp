#include "NativeSongTestAccess.h"
/* Native sound-shaping waveform ownership and repair. SPDX-License-Identifier: GPL-2.0-or-later */
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
#include <functional>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "EnvelopeAndLfoParameters.h"
#include "FileResourceIO.h"
#include "FileResourceSnapshot.h"
#include "Instrument.h"
#include "InstrumentSoundShaping.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "Song.h"
#include "SerializingObject.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
EnvelopeAndLfoParameters* parameters(InstrumentTrack* track, const QString& target)
{ auto shaping = track->soundShaping(); return target == "volume" ? &shaping->getVolumeParameters() : target == "cutoff" ? &shaping->getCutoffParameters() : &shaping->getResonanceParameters(); }
QString tag(const QString& target) { return target == "volume" ? "elvol" : target == "cutoff" ? "elcut" : "elres"; }
QByteArray little(quint32 value, int count) { QByteArray out; for (int i = 0; i < count; ++i) { out.append(static_cast<char>(value >> (8 * i))); } return out; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
bool wave(const QString& path) { QByteArray pcm(128, '\0'); pcm[1] = 0x20; return write(path, QByteArray("RIFF") + little(36 + pcm.size(), 4) + "WAVEfmt " + little(16, 4) + little(1, 2) + little(1, 2) + little(44100, 4) + little(88200, 4) + little(2, 2) + little(16, 2) + "data" + little(pcm.size(), 4) + pcm); }
QDomElement settings(QDomDocument& document, const QString& path, bool pending = true, const QString& target = "volume")
{
	auto node = document.createElement(tag(target)); document.appendChild(node); node.setAttribute("userwavefile", path); if (pending) { node.setAttribute("userwaveunresolved", 1); } node.setAttribute("pdel", .07); node.setAttribute("att", .09); node.setAttribute("hold", .13); node.setAttribute("dec", .17); node.setAttribute("sustain", .23); node.setAttribute("rel", .27); node.setAttribute("amt", .31); node.setAttribute("lshp", static_cast<int>(EnvelopeAndLfoParameters::LfoShape::UserDefinedWave)); node.setAttribute("lpdel", .11); node.setAttribute("latt", .19); node.setAttribute("lspd", .29); node.setAttribute("lamt", .41); node.setAttribute("x100", 1); node.setAttribute("ctlenvamt", 1); return node;
}
void missing(EnvelopeAndLfoParameters* owner, const QString& path, bool pending = true) { QDomDocument document; owner->loadSettings(settings(document, path, pending)); }
QVector<float> values(EnvelopeAndLfoParameters* owner) { QVector<float> out; for (auto model : owner->findChildren<AutomatableModel*>(QString{}, Qt::FindDirectChildrenOnly)) { out.append(model->storedValue()); } return out; }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
QJsonObject decision(const QString& protocol, const QString& id, const QString& name, const QJsonObject& args)
{
	const auto arguments = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
	if (protocol == "responses") { return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", arguments}}}}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{QJsonObject{{"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name}, {"arguments", arguments}}}}}}}}}}}};
}
const Plugin::Descriptor envelopeDescriptor{"envelope-observer-fixture", "Envelope observer fixture", "Native getter mutation fixture", "LMMS tests", 1, Plugin::Type::Instrument, nullptr, "", nullptr};
class Observer final : public Instrument
{
public:
	explicit Observer(InstrumentTrack* track) : Instrument(track, &envelopeDescriptor) { }
	std::function<void()> onGet;
	std::vector<FileResource> fileResources() const override { const auto callback = onGet; if (callback) { callback(); } return {}; }
	void saveSettings(QDomDocument&, QDomElement&) override { }
	void loadSettings(const QDomElement&) override { }
	QString nodeName() const override { return "envelope-observer-fixture"; }
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
};
class ObserverTrack final : public InstrumentTrack
{
public:
	ObserverTrack() : InstrumentTrack(Engine::getSong()) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new Observer(this); }
};
class ResourceHook final : public SerializingObjectHook
{
public:
	std::function<void()> callback;
	void saveSettings(QDomDocument&, QDomElement&) override { const auto work = callback; if (work) { work(); } }
	void loadSettings(const QDomElement&) override { }
};
}
class EnvelopeWaveResourceTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); NativeSongTestAccess::setProjectFileName(Engine::getSong(), ""); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void nativeGroupsRetainMissingPathsControlsAndMarkers_data()
	{ QTest::addColumn<QString>("target"); QTest::addColumn<int>("mode"); for (const auto& target : {QString("volume"), QString("cutoff"), QString("resonance")}) { for (int mode = 0; mode < 3; ++mode) { QTest::newRow(qPrintable(target + QString::number(mode))) << target << mode; } } }
	void nativeGroupsRetainMissingPathsControlsAndMarkers()
	{
		QFETCH(QString, target); QFETCH(int, mode); QTemporaryDir source; const auto path = source.filePath("missing.wav"); if (mode == 2) { QVERIFY(wave(path)); } auto track = new InstrumentTrack(Engine::getSong()); auto owner = parameters(track, target); missing(owner, path, mode != 0); QVERIFY(owner->lfoUserWaveUnresolved()); QCOMPARE(owner->lfoUserWaveFile(), path); QVERIFY(owner->getLfoUserWave()->empty()); QVERIFY(!Engine::getSong()->hasFatalErrors()); const auto before = values(owner); QCOMPARE(before.size(), 14); QDomDocument document; auto node = document.createElement(tag(target)); document.appendChild(node); owner->saveSettings(document, node); QCOMPARE(node.attribute("userwavefile"), path); QCOMPARE(node.attribute("userwaveunresolved"), QString("1")); auto other = new InstrumentTrack(Engine::getSong()); auto restored = parameters(other, target); restored->loadSettings(node); QVERIFY(restored->lfoUserWaveUnresolved()); QCOMPARE(values(restored), before);
	}
	void directAndStructuredRepairsPreserveControls_data()
	{ QTest::addColumn<QString>("target"); QTest::addColumn<bool>("structured"); for (const auto& target : {QString("volume"), QString("cutoff"), QString("resonance")}) { QTest::newRow(qPrintable(target + "-direct")) << target << false; QTest::newRow(qPrintable(target + "-structured")) << target << true; } }
	void directAndStructuredRepairsPreserveControls()
	{
		QFETCH(QString, target); QFETCH(bool, structured); QTemporaryDir source; const auto path = source.filePath("found.wav"); QVERIFY(wave(path)); auto track = new InstrumentTrack(Engine::getSong()); auto owner = parameters(track, target); missing(owner, source.filePath("missing.wav")); owner->getLfoWaveModel().setValue(static_cast<int>(EnvelopeAndLfoParameters::LfoShape::SineWave)); const auto before = values(owner); QString error; if (structured) { const auto response = StudioCapabilities::execute("instrument.shaping.waveform.set", {{"track", 0}, {"target", target}, {"path", path}}); QVERIFY2(response.value("ok").toBool(), QJsonDocument(response).toJson().constData()); QVERIFY(!response.value("result").toObject().value("unresolved").toBool()); } else { QVERIFY(owner->setLfoUserWave(path, error)); } QVERIFY(!owner->lfoUserWaveUnresolved()); QCOMPARE(owner->getLfoUserWave()->size(), std::size_t(64)); QCOMPARE(values(owner), before); QVERIFY(Engine::getSong()->isModified());
	}
	void selectionFailureAndClearHaveExplicitSemantics_data()
	{ QTest::addColumn<bool>("select"); QTest::newRow("preserve") << false; QTest::newRow("select-user-wave") << true; }
	void selectionFailureAndClearHaveExplicitSemantics()
	{
		QFETCH(bool, select); QTemporaryDir source; const auto path = source.filePath("wave.wav"), corrupt = source.filePath("bad.wav"); QVERIFY(wave(path)); QVERIFY(write(corrupt, "invalid audio")); auto track = new InstrumentTrack(Engine::getSong()); auto owner = parameters(track, "volume"); missing(owner, source.filePath("missing.wav")); owner->getLfoWaveModel().setValue(static_cast<int>(EnvelopeAndLfoParameters::LfoShape::SineWave)); QString error; QVERIFY(owner->setLfoUserWave(path, error, select)); QCOMPARE(owner->getLfoWaveModel().storedValue(), static_cast<float>(select ? EnvelopeAndLfoParameters::LfoShape::UserDefinedWave : EnvelopeAndLfoParameters::LfoShape::SineWave)); const auto before = values(owner); const auto buffer = owner->getLfoUserWave(); QVERIFY(!owner->setLfoUserWave(corrupt, error)); QVERIFY(owner->getLfoUserWave() == buffer); QCOMPARE(values(owner), before); QVERIFY(owner->setLfoUserWave({}, error)); QVERIFY(!owner->lfoUserWaveUnresolved()); QVERIFY(owner->lfoUserWaveFile().isEmpty()); QVERIFY(owner->getLfoUserWave()->empty()); QCOMPARE(values(owner), before);
	}
	void checkpointTableCoversAllThreeNativeTags_data()
	{ QTest::addColumn<QString>("target"); QTest::newRow("volume") << QString("volume"); QTest::newRow("cutoff") << QString("cutoff"); QTest::newRow("resonance") << QString("resonance"); }
	void checkpointTableCoversAllThreeNativeTags()
	{
		QFETCH(QString, target); QTemporaryDir source, destination; auto track = new InstrumentTrack(Engine::getSong()); auto owner = parameters(track, target); const auto lost = source.filePath("lost.wav"); missing(owner, lost); DataFile document(DataFile::Type::SongProject); auto node = document.createElement(tag(target)); document.content().appendChild(node); { DataFile::ResourceSourceScope saving(document); owner->saveSettings(document, node); } QJsonArray assets; QVERIFY(document.copyResources(destination.filePath("assets"), &assets)); QVERIFY(assets.isEmpty()); QVERIFY(document.matchesAssetReferences(assets)); QVERIFY(!document.copyResources(destination.filePath("bundle"))); node.removeAttribute("userwaveunresolved"); QVERIFY(!document.matchesAssetReferences(assets));
	}
	void acceptedReferencesCannotBeDiscardedByPendingMarkers_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("snapshot") << 0; QTest::newRow("same-retained-name") << 1; QTest::newRow("sample-backed") << 2; }
	void acceptedReferencesCannotBeDiscardedByPendingMarkers()
	{
		QFETCH(int, kind); QTemporaryDir source, destination; const auto path = source.filePath("wave.wav"); QVERIFY(wave(path)); QString error; const auto snapshot = FileResourceSnapshot::capture(path, error); QVERIFY(snapshot); const auto buffer = SampleBuffer::fromFile(path); auto track = new InstrumentTrack(Engine::getSong()); DataFile document(DataFile::Type::SongProject); auto node = document.createElement("elcut"); document.content().appendChild(node); node.setAttribute("userwavefile", kind == 1 ? snapshot->path() : path); node.setAttribute("userwaveunresolved", 1); { DataFile::ResourceSourceScope saving(document, true, kind == 2); DataFile::registerResourceSnapshot(document, node, "userwavefile", snapshot, kind == 2 ? buffer : nullptr); } { DataFile::ResourceReadScope reading(document); QVERIFY_EXCEPTION_THROWN(parameters(track, "cutoff")->loadSettings(node), std::invalid_argument); } QJsonArray assets; QVERIFY(!document.copyResources(destination.filePath("assets"), &assets)); QVERIFY(assets.isEmpty());
	}
	void malformedPendingSettingsAreRefused_data()
	{ QTest::addColumn<QString>("path"); QTest::newRow("empty") << QString(); QTest::newRow("null") << QString(QChar::Null); QTest::newRow("local-unavailable") << QString("local:lost.wav"); }
	void malformedPendingSettingsAreRefused()
	{
		QFETCH(QString, path); auto track = new InstrumentTrack(Engine::getSong()); auto owner = parameters(track, "volume"); const auto before = values(owner); QDomDocument document; QVERIFY_EXCEPTION_THROWN(owner->loadSettings(settings(document, path)), std::invalid_argument); QCOMPARE(values(owner), before); if (path.contains(QChar::Null)) { QString error; QVERIFY(!owner->setLfoUserWave(path, error)); QVERIFY(!error.isEmpty()); }
	}
	void returnedModelCallsCannotContinueAfterTrackRetirement_data()
	{ QTest::addColumn<int>("field"); QTest::addColumn<bool>("saving"); for (int field = 0; field < 14; ++field) { QTest::newRow(qPrintable(QString::number(field) + "-load")) << field << false; QTest::newRow(qPrintable(QString::number(field) + "-save")) << field << true; } }
	void returnedModelCallsCannotContinueAfterTrackRetirement()
	{
		QFETCH(int, field); QFETCH(bool, saving); const char* keys[] = {"pdel", "att", "hold", "dec", "sustain", "rel", "amt", "lpdel", "latt", "lspd", "lamt", "lshp", "x100", "ctlenvamt"}; const QPointer<InstrumentTrack> track(new InstrumentTrack(Engine::getSong())); const QPointer<EnvelopeAndLfoParameters> owner(parameters(track.data(), "volume")); const auto active = owner.data(); auto models = active->findChildren<AutomatableModel*>(QString{}, Qt::FindDirectChildrenOnly); QCOMPARE(models.size(), 14); QTemporaryDir source; QDomDocument document; auto node = settings(document, source.filePath("lost.wav")); bool retired = false;
		if (saving) { for (const auto key : keys) { node.removeAttribute(key); } FileResourceIO::Scope work([&] { if (!retired && node.hasAttribute(keys[field])) { retired = true; delete track.data(); } return false; }); QVERIFY_EXCEPTION_THROWN(active->saveSettings(document, node), std::runtime_error); } else { connect(models[field], &AutomatableModel::dataChanged, this, [&] { if (!retired) { retired = true; delete track.data(); } }); QVERIFY_EXCEPTION_THROWN(active->loadSettings(node), std::runtime_error); } QVERIFY(retired); QVERIFY(!owner); QVERIFY(!track);
	}
	void nativeParentSerializerStopsAfterRetiredEnvelopeSave()
	{
		const QPointer<InstrumentTrack> track(new InstrumentTrack(Engine::getSong())); auto shaping = track->soundShaping(); QDomDocument document; auto node = document.createElement("eldata"); document.appendChild(node); bool retired = false; { FileResourceIO::Scope work([&] { if (!retired && !node.firstChildElement("el").isNull()) { retired = true; delete track.data(); } return false; }); QVERIFY_EXCEPTION_THROWN(shaping->saveSettings(document, node), std::runtime_error); } QVERIFY(retired); QVERIFY(!track);
	}
	void nestedSettingsAndResourceTransitionsAreRefused()
	{
		auto track = new InstrumentTrack(Engine::getSong()); auto owner = parameters(track, "volume"); bool nested = false; QTemporaryDir source; QDomDocument document; const auto node = settings(document, source.filePath("lost.wav")); connect(owner, &Model::dataChanged, this, [&] { if (nested) { return; } nested = true; QString error; QVERIFY_EXCEPTION_THROWN(owner->setLfoUserWave({}, error), std::logic_error); QVERIFY_EXCEPTION_THROWN(owner->loadSettings(node), std::logic_error); QDomDocument saved; auto element = saved.createElement("elvol"); QVERIFY_EXCEPTION_THROWN(owner->saveSettings(saved, element), std::logic_error); }); owner->loadSettings(node); QVERIFY(nested); QString error; QVERIFY(owner->setLfoUserWave({}, error));
	}
	void cancellationAndExceptionCleanupRemainReusable_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("cancel") << 0; QTest::newRow("standard") << 1; QTest::newRow("nonstandard") << 2; }
	void cancellationAndExceptionCleanupRemainReusable()
	{
		QFETCH(int, kind); auto track = new InstrumentTrack(Engine::getSong()); auto owner = parameters(track, "volume"); QString error; bool caught = false; try { FileResourceIO::Scope work([&] { if (kind == 1) { throw std::runtime_error("Fixture envelope work"); } if (kind == 2) { throw 109; } return true; }); owner->setLfoUserWave({}, error); } catch (...) { caught = true; } QVERIFY(caught); QVERIFY(owner->setLfoUserWave({}, error));
	}
	void borrowedInputAndRootChangesAreObserved_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("copied-path") << 0; QTest::newRow("control-change") << 1; QTest::newRow("project-path") << 2; QTest::newRow("project-replacement") << 3; }
	void borrowedInputAndRootChangesAreObserved()
	{
		QFETCH(int, kind); QTemporaryDir source; QString path = source.filePath("wave.wav"); QVERIFY(wave(path)); const QPointer<InstrumentTrack> track(new InstrumentTrack(Engine::getSong())); const QPointer<EnvelopeAndLfoParameters> owner(parameters(track.data(), "volume")); auto active = owner.data(); bool changed = false; QString error; { FileResourceIO::Scope work([&] { if (!changed) { changed = true; if (kind == 0) { path = source.filePath("other.wav"); } if (kind == 1) { owner->getLfoAmountModel().setValue(.125f); } if (kind == 2) { NativeSongTestAccess::setProjectFileName(Engine::getSong(), source.filePath("other.mmp")); } if (kind == 3) { Engine::getSong()->createEmptyProject(); } } return false; }); if (kind == 0) { QVERIFY(active->setLfoUserWave(path, error)); } else { QVERIFY_EXCEPTION_THROWN(active->setLfoUserWave(path, error), std::runtime_error); } } QVERIFY(changed); if (kind == 0) { QCOMPARE(owner->getLfoUserWave()->size(), std::size_t(64)); }
	}
	void cachedNativeModulationUsesPendingAndRepairedContent()
	{
		QTemporaryDir source; const auto path = source.filePath("wave.wav"); QVERIFY(wave(path)); auto track = new InstrumentTrack(Engine::getSong()); auto owner = parameters(track, "volume"); QDomDocument document; auto node = settings(document, path, false); for (const auto key : {"pdel", "att", "amt", "lpdel", "latt", "ctlenvamt", "x100"}) { node.setAttribute(key, 0); } node.setAttribute("lamt", .5); owner->loadSettings(node);
		const auto deviation = [&] { const auto frames = Engine::audioEngine()->framesPerPeriod(); std::vector<float> data(frames); owner->fillLevel(data.data(), 1, frames * 1000, frames); double difference = 0.; for (const auto value : data) { if (!std::isfinite(value)) { throw std::runtime_error("Nonfinite native shaping buffer"); } difference += std::abs(value - 1.f); } return difference; }; QVERIFY(deviation() > 0.); node.setAttribute("userwavefile", source.filePath("missing.wav")); node.setAttribute("userwaveunresolved", 1); owner->loadSettings(node); QCOMPARE(deviation(), 0.); QString error; QVERIFY(owner->setLfoUserWave(path, error)); QVERIFY(deviation() > 0.);
	}
	void ownedSerializationAndLegacySustainRetainNativeSemantics()
	{
		QTemporaryDir source; const auto path = source.filePath("wave.wav"); QVERIFY(wave(path)); auto track = new InstrumentTrack(Engine::getSong()); auto owner = parameters(track, "volume"); QString error; QVERIFY(owner->setLfoUserWave(path, error)); DataFile document(DataFile::Type::SongProject); auto node = document.createElement("elvol"); document.content().appendChild(node); { DataFile::ResourceSourceScope saving(document, true, true); owner->saveSettings(document, node); } QVERIFY(QFile::remove(path)); auto other = new InstrumentTrack(Engine::getSong()); auto restored = parameters(other, "volume"); { DataFile::ResourceReadScope reading(document); restored->loadSettings(node); } QCOMPARE(restored->getLfoUserWave()->size(), std::size_t(64)); QVERIFY(!restored->lfoUserWaveUnresolved()); QDomDocument legacy; auto empty = settings(legacy, {}, false); empty.setAttribute("sus", .25); restored->loadSettings(empty); QCOMPARE(restored->getSustainModel().storedValue(), .75f); QVERIFY(restored->lfoUserWaveFile().isEmpty()); QVERIFY(restored->getLfoUserWave()->empty());
	}
	void nativeParentTagPublicationRetainsAcceptedSourceBindings()
	{
		QTemporaryDir source, destination; const auto path = source.filePath("wave.wav"); QVERIFY(wave(path)); auto track = new InstrumentTrack(Engine::getSong()); QString error; QVERIFY(parameters(track, "cutoff")->setLfoUserWave(path, error)); DataFile document(DataFile::Type::SongProject); QDomElement shape; { DataFile::ResourceSourceScope saving(document, true, true); auto parent = document.content(); shape = track->soundShaping()->saveState(document, parent); } QCOMPARE(shape.firstChildElement("elcut").attribute("userwavefile"), parameters(track, "cutoff")->lfoUserWaveFile()); QVERIFY(QFile::remove(path)); auto other = new InstrumentTrack(Engine::getSong()); { DataFile::ResourceReadScope reading(document); other->soundShaping()->restoreState(shape); } QCOMPARE(parameters(other, "cutoff")->getLfoUserWave()->size(), std::size_t(64)); QJsonArray assets; QVERIFY(document.copyResources(destination.filePath("assets"), &assets)); QCOMPARE(assets.size(), 1); QVERIFY(document.matchesAssetReferences(assets));
	}
	void nativeParentTagPublicationRefusesHookResourceSubstitution()
	{
		QTemporaryDir source; const auto first = source.filePath("first.wav"), second = source.filePath("second.wav"); QVERIFY(wave(first)); QVERIFY(wave(second)); auto track = new InstrumentTrack(Engine::getSong()); auto owner = parameters(track, "volume"); QString error; QVERIFY(owner->setLfoUserWave(first, error)); ResourceHook hook; hook.callback = [&] { if (!owner->setLfoUserWave(second, error)) { throw std::runtime_error("Fixture waveform could not load"); } }; owner->setHook(&hook); const auto detach = qScopeGuard([&] { owner->setHook(nullptr); }); DataFile document(DataFile::Type::SongProject); auto parent = document.content(); { DataFile::ResourceSourceScope saving(document, true, true); QVERIFY_EXCEPTION_THROWN(track->soundShaping()->saveState(document, parent), std::runtime_error); }
	}
	void malformedRepairArgumentsCannotMutateNativeState_data()
	{ QTest::addColumn<QJsonObject>("args"); QTest::newRow("unknown-target") << QJsonObject{{"track", 0}, {"target", "pitch"}, {"path", ""}}; QTest::newRow("missing-target") << QJsonObject{{"track", 0}, {"path", ""}}; QTest::newRow("bad-select") << QJsonObject{{"track", 0}, {"target", "volume"}, {"path", ""}, {"select", "true"}}; QTest::newRow("metadata-key-is-not-argument") << QJsonObject{{"track", 0}, {"target", "volume"}, {"path", ""}, {"resource", "userwave"}}; }
	void malformedRepairArgumentsCannotMutateNativeState()
	{
		QFETCH(QJsonObject, args); QTemporaryDir source; auto track = new InstrumentTrack(Engine::getSong()); auto owner = parameters(track, "volume"); missing(owner, source.filePath("missing.wav")); const auto buffer = owner->getLfoUserWave(); const auto path = owner->lfoUserWaveFile(); const auto controls = values(owner); QVERIFY(!StudioCapabilities::execute("instrument.shaping.waveform.set", args).value("ok").toBool()); QVERIFY(owner->getLfoUserWave() == buffer); QCOMPARE(owner->lfoUserWaveFile(), path); QCOMPARE(values(owner), controls);
	}
	void inventoryIncludesNonemptySongAndPatternShapingWaves_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void inventoryIncludesNonemptySongAndPatternShapingWaves()
	{
		QFETCH(bool, pattern); QTemporaryDir source; const auto path = source.filePath("missing.wav"); auto track = new InstrumentTrack(pattern ? static_cast<TrackContainer*>(Engine::patternStore()) : static_cast<TrackContainer*>(Engine::getSong())); missing(parameters(track, "resonance"), path); QVERIFY(wave(path)); const auto response = StudioCapabilities::execute("project.inspect", {{"section", "resources"}}); QVERIFY(response.value("ok").toBool()); const auto result = response.value("result").toObject(); QCOMPARE(result.value("declaredUnresolvedCount").toInt(), 1); const auto rows = result.value("resources").toArray(); QCOMPARE(rows.size(), 1); const auto row = rows[0].toObject(); QCOMPARE(row.value("owner").toObject().value("target").toString(), QString("resonance")); QCOMPARE(row.value("owner").toObject().value("container").toString(), pattern ? QString("pattern") : QString("song")); QCOMPARE(row.value("repairOperation").toString(), QString("instrument.shaping.waveform.set")); QCOMPARE(row.value("file").toObject().value("status").toString(), QString("readableFile")); QVERIFY(row.value("unresolved").toBool());
	}
	void nativePluginGetterCannotIntroduceUnreportedShapingReferences()
	{
		QTemporaryDir source; auto track = new ObserverTrack; auto observer = static_cast<Observer*>(track->loadInstrument("envelope-observer-fixture")); bool changed = false; observer->onGet = [&] { if (!changed) { changed = true; missing(parameters(track, "cutoff"), source.filePath("missing.wav")); } }; const auto response = StudioCapabilities::execute("project.inspect", {{"section", "resources"}}); QVERIFY(changed); QVERIFY(!response.value("ok").toBool()); observer->onGet = {}; QVERIFY(StudioCapabilities::execute("project.inspect", {{"section", "resources"}}).value("ok").toBool());
	}
	void corruptExistingWavesRemainFatalAndQueuedEventsStayOutsideWork()
	{
		QTemporaryDir source; const auto path = source.filePath("bad.wav"); QVERIFY(write(path, "invalid audio")); auto track = new InstrumentTrack(Engine::getSong()); auto owner = parameters(track, "volume"); QDomDocument document; owner->loadSettings(settings(document, path, false)); QVERIFY(Engine::getSong()->hasFatalErrors()); QVERIFY(!owner->lfoUserWaveUnresolved()); bool queued = false; QString error; QTimer::singleShot(0, this, [&] { queued = true; }); { FileResourceIO::Scope work({}, true); QVERIFY(owner->setLfoUserWave({}, error)); QVERIFY(!queued); } QCoreApplication::processEvents(); QVERIFY(queued);
	}
	void runtimeRepairHistoryFailureReplayAndOwnedReopening()
	{
		QTemporaryDir source, session; const auto path = source.filePath("found.wav"), lost = source.filePath("missing.wav"), project = source.filePath("missing.mmp"), bad = source.filePath("bad.wav"); QVERIFY(wave(path)); QVERIFY(write(bad, "invalid audio")); auto track = new InstrumentTrack(Engine::getSong()); missing(parameters(track, "cutoff"), lost, false); QVERIFY(Engine::getSong()->saveProjectFile(project)); QString error; QJsonObject receipt; const auto repair = request("repair", "instrument.shaping.waveform.set", {{"track", 0}, {"target", "cutoff"}, {"path", path}});
		{ StudioRuntime runtime(session.path()); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("load", "project.load", {{"path", project}, {"summary", true}})).value("ok").toBool()); receipt = runtime.process(repair); QVERIFY(receipt.value("ok").toBool()); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QVERIFY(parameters(static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0]), "cutoff")->lfoUserWaveUnresolved()); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); auto replay = runtime.process(repair); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); const auto before = manifest(session); const auto command = request("bad", "instrument.shaping.waveform.set", {{"track", 0}, {"target", "cutoff"}, {"path", bad}}); const auto failure = runtime.process(command); QVERIFY(!failure.value("ok").toBool()); auto again = runtime.process(command); QVERIFY(again.take("replayed").toBool()); QCOMPARE(again, failure); QCOMPARE(manifest(session).value("history"), before.value("history")); QVERIFY(!runtime.stopping()); }
		QVERIFY(QFile::remove(path)); StudioRuntime reopened(session.path()); QVERIFY2(reopened.open(error), qPrintable(error)); auto current = parameters(static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0]), "cutoff"); QVERIFY(!current->lfoUserWaveUnresolved()); QCOMPARE(current->getLfoUserWave()->size(), std::size_t(64));
	}
	void scriptedAgentsDiscoverFindAndRepairNativeShapingWaves_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void scriptedAgentsDiscoverFindAndRepairNativeShapingWaves()
	{
		QFETCH(QString, protocol); QTemporaryDir source, session; const auto path = source.filePath("found.wave.wav"), project = source.filePath("missing.mmp"); QVERIFY(wave(path)); auto track = new InstrumentTrack(Engine::getSong()); missing(parameters(track, "resonance"), source.filePath("missing.wave.wav")); QVERIFY(Engine::getSong()->saveProjectFile(project)); QString error; QJsonObject target; QJsonArray evidence; int round = 0;
		StudioRuntime runtime(session.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			QJsonObject output; for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray()) { const auto item = value.toObject(); if (item.value("role") == QJsonValue("tool") || item.value("type") == QJsonValue("function_call_output")) { output = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); } } const auto call = [&](const QString& id, const QString& name, const QJsonObject& args) { evidence.append(id); return decision(protocol, id, name, args); }; ++round;
			if (round == 1) { return call("discover", "project_inspect", {{"section", "resources"}}); } if (!output.value("ok").toBool()) { throw std::runtime_error("Native shaping resource operation failed"); }
			if (round == 2) { const auto row = output.value("result").toObject().value("resources").toArray()[0].toObject(); if (!row.value("unresolved").toBool() || row.value("repairOperation") != QJsonValue("instrument.shaping.waveform.set")) { throw std::runtime_error("Expected pending native shaping reference"); } const auto owner = row.value("owner").toObject(); target = {{"container", owner.value("container")}, {"track", owner.value("track")}, {"target", owner.value("target")}}; return call("find", "asset_search", {{"root", source.path()}, {"query", "found.wave"}, {"extensions", QJsonArray{"wav"}}}); }
			if (round == 3) { const auto page = output.value("result").toObject(); if (page.value("total").toInt() != 1 || !page.value("scanComplete").toBool()) { throw std::runtime_error("Expected one scoped native wave candidate"); } auto args = target; args.insert("path", page.value("matches").toArray()[0].toObject().value("path")); return call("repair", "studio_execute", {{"op", "instrument.shaping.waveform.set"}, {"args", args}}); }
			if (round == 4) { return call("verify", "project_inspect", {{"section", "resources"}}); } if (output.value("result").toObject().value("declaredUnresolvedCount").toInt(-1) != 0) { throw std::runtime_error("Repair was not observed"); } return decision(protocol, "finish", "agent_finish", {{"status", "complete"}, {"summary", "Repaired the native shaping reference; musical evaluation was not performed."}, {"evidence", evidence}});
		}); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("load", "project.load", {{"path", project}, {"summary", true}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-envelope-wave-repair-fixture"}}}})).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Find found.wave.wav in " + source.path() + " and repair the unresolved sound-shaping waveform without changing its controls."}})).value("ok").toBool()); for (int i = 0; i < 64; ++i) { runtime.advanceAgent(); const auto status = manifest(session).value("agent").toObject().value("job").toObject().value("status").toString(); if (status == "complete" || status == "failed" || status == "paused" || status == "cancelled") { break; } } QCOMPARE(manifest(session).value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete"));
	}
};
QTEST_GUILESS_MAIN(EnvelopeWaveResourceTest)
#include "EnvelopeWaveResourceTest.moc"
