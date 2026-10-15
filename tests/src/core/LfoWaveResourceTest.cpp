/* Native missing LFO wave preservation and checked repair. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDataStream>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <array>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "FileResourceSnapshot.h"
#include "InstrumentTrack.h"
#include "LfoController.h"
#include "NativeSongTestAccess.h"
#include "NotePlayHandle.h"
#include "Oscillator.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
class Lfo final : public LfoController
{
public:
	explicit Lfo(Model* parent = nullptr) : LfoController(parent) { }
	FloatModel& amount() { return m_amountModel; }
	std::array<float, 6> values() const { return {m_baseModel.storedValue(), m_speedModel.storedValue(), m_amountModel.storedValue(), m_phaseModel.storedValue(), m_waveModel.storedValue(), m_multiplierModel.storedValue()}; }
};
bool wave(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) { return false; } QDataStream stream(&file); stream.setByteOrder(QDataStream::LittleEndian);
	stream.writeRawData("RIFF", 4); stream << quint32(36 + 128); stream.writeRawData("WAVEfmt ", 8); stream << quint32(16) << quint16(1) << quint16(1) << quint32(44100) << quint32(88200) << quint16(2) << quint16(16); stream.writeRawData("data", 4); stream << quint32(128);
	for (int i = 0; i < 64; ++i) { stream << qint16(8192); } return stream.status() == QDataStream::Ok;
}
QDomElement settings(QDomDocument& document, const QString& path, bool pending = true)
{
	auto element = document.createElement("lfocontroller"); document.appendChild(element); element.setAttribute("type", static_cast<int>(Controller::ControllerType::Lfo)); element.setAttribute("name", "Retained controller"); element.setAttribute("userwavefile", path); if (pending) { element.setAttribute("userwaveunresolved", 1); }
	element.setAttribute("base", .3); element.setAttribute("speed", 3); element.setAttribute("amount", .4); element.setAttribute("phase", 48); element.setAttribute("wave", static_cast<int>(Oscillator::WaveShape::UserDefined)); element.setAttribute("multiplier", 1); return element;
}
void missing(LfoController* native, const QString& path, bool pending = true) { QDomDocument document; native->loadSettings(settings(document, path, pending)); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
QJsonObject decision(const QString& protocol, const QString& id, const QString& name, const QJsonObject& args)
{
	const auto arguments = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
	if (protocol == "responses") { return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", arguments}}}}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{QJsonObject{{"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name}, {"arguments", arguments}}}}}}}}}}}};
}
}
class LfoWaveResourceTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	Lfo* rack() { auto native = new Lfo(Engine::getSong()); Engine::getSong()->addController(native); return native; }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); NativeSongTestAccess::setProjectFileName(Engine::getSong(), ""); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void missingSettingsRetainControlsAndZeroWaveContribution_data()
	{ QTest::addColumn<bool>("marked"); QTest::addColumn<bool>("present"); QTest::newRow("legacy-missing") << false << false; QTest::newRow("marked-missing") << true << false; QTest::newRow("marked-file-appeared") << true << true; }
	void missingSettingsRetainControlsAndZeroWaveContribution()
	{
		QFETCH(bool, marked); QFETCH(bool, present); QTemporaryDir source; const auto path = source.filePath("lost.wav"); if (present) { QVERIFY(wave(path)); } auto native = rack(); missing(native, path, marked); QVERIFY(native->userWaveUnresolved()); QCOMPARE(native->userWaveFile(), path); QVERIFY(native->userWave().empty()); QVERIFY(Engine::getSong()->hasErrors()); QVERIFY(!Engine::getSong()->hasFatalErrors()); QCOMPARE(native->amount().storedValue(), .4f); QCOMPARE(native->values()[0], .3f);
		for (int i = 0; i < Engine::audioEngine()->framesPerPeriod(); ++i) { QCOMPARE(native->currentValue(i), .3f); }
		QDomDocument document; auto element = document.createElement("lfocontroller"); document.appendChild(element); native->saveSettings(document, element); QCOMPARE(element.attribute("userwavefile"), path); QCOMPARE(element.attribute("userwaveunresolved"), QString("1")); Lfo restored; restored.loadSettings(element); QVERIFY(restored.userWaveUnresolved()); QCOMPARE(restored.values(), native->values());
	}
	void checkedRepairPreservesControlsAndConnections_data()
	{ QTest::addColumn<bool>("structured"); QTest::addColumn<bool>("selected"); QTest::newRow("direct-active") << false << true; QTest::newRow("direct-inactive") << false << false; QTest::newRow("structured-active") << true << true; QTest::newRow("structured-inactive") << true << false; }
	void checkedRepairPreservesControlsAndConnections()
	{
		QFETCH(bool, structured); QFETCH(bool, selected); QTemporaryDir source; const auto path = source.filePath("found.wav"); QVERIFY(wave(path)); auto native = rack(); missing(native, source.filePath("lost.wav")); if (!selected) { native->waveModel().setValue(static_cast<int>(Oscillator::WaveShape::Sine)); } auto track = new InstrumentTrack(Engine::getSong()); track->volumeModel()->setControllerConnection(new ControllerConnection(native)); const QPointer<ControllerConnection> binding(track->volumeModel()->controllerConnection()); const auto before = native->values(); QString error;
		if (structured) { const auto response = StudioCapabilities::execute("controller.waveform.set", {{"controller", 0}, {"path", path}}); QVERIFY2(response.value("ok").toBool(), QJsonDocument(response).toJson().constData()); QVERIFY(!response.value("result").toObject().value("userWave").toObject().value("unresolved").toBool()); } else { QVERIFY(native->setUserWave(path, error)); }
		QVERIFY(!native->userWaveUnresolved()); QCOMPARE(native->values(), before); QCOMPARE(native->userWave().size(), std::size_t(64)); QVERIFY(binding); QCOMPARE(track->volumeModel()->controllerConnection(), binding.data()); QCOMPARE(native->connectionCount(), 1); QDomDocument document; auto node = document.createElement("lfocontroller"); native->saveSettings(document, node); QVERIFY(!node.hasAttribute("userwaveunresolved"));
	}
	void failedRepairRetainsPendingReference_data()
	{ QTest::addColumn<bool>("corrupt"); QTest::newRow("missing") << false; QTest::newRow("corrupt") << true; }
	void failedRepairRetainsPendingReference()
	{
		QFETCH(bool, corrupt); QTemporaryDir source; const auto lost = source.filePath("lost.wav"), replacement = source.filePath("bad.wav"); if (corrupt) { QFile file(replacement); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("invalid audio"), qint64(13)); } auto native = rack(); missing(native, lost); const auto buffer = native->userWaveBuffer(); const auto values = native->values(); QString error; QVERIFY(!native->setUserWave(replacement, error)); QVERIFY(!error.isEmpty()); QVERIFY(native->userWaveUnresolved()); QCOMPARE(native->userWaveFile(), lost); QVERIFY(native->userWaveBuffer() == buffer); QCOMPARE(native->values(), values);
	}
	void clearingRemovesPendingPathWithoutChangingSelection_data()
	{ QTest::addColumn<bool>("structured"); QTest::newRow("direct") << false; QTest::newRow("structured") << true; }
	void clearingRemovesPendingPathWithoutChangingSelection()
	{
		QFETCH(bool, structured); QTemporaryDir source; auto native = rack(); missing(native, source.filePath("lost.wav")); const auto values = native->values(); QString error; if (structured) { QVERIFY(StudioCapabilities::execute("controller.waveform.set", {{"controller", 0}, {"path", ""}}).value("ok").toBool()); } else { QVERIFY(native->setUserWave({}, error)); } QVERIFY(!native->userWaveUnresolved()); QVERIFY(native->userWaveFile().isEmpty()); QVERIFY(native->userWave().empty()); QCOMPARE(native->values(), values);
	}
	void malformedPendingSettingsAreRefusedBeforeControlsChange_data()
	{ QTest::addColumn<QString>("path"); QTest::newRow("empty") << QString(); QTest::newRow("null") << QString(QChar::Null); QTest::newRow("unresolved-local-base") << QString("local:missing.wav"); }
	void malformedPendingSettingsAreRefusedBeforeControlsChange()
	{
		QFETCH(QString, path); Lfo native; const auto values = native.values(); QDomDocument document; const auto node = settings(document, path); QVERIFY_EXCEPTION_THROWN(native.loadSettings(node), std::invalid_argument); QCOMPARE(native.values(), values); QVERIFY(!native.userWaveUnresolved());
	}
	void capturedAcceptedSourcesCannotBeDiscarded_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("snapshot-original") << 0; QTest::newRow("snapshot-retained-name") << 1; QTest::newRow("sample-backed") << 2; }
	void capturedAcceptedSourcesCannotBeDiscarded()
	{
		QFETCH(int, kind); QTemporaryDir source, destination; const auto path = source.filePath("wave.wav"); QVERIFY(wave(path)); QString error; const auto snapshot = FileResourceSnapshot::capture(path, error); QVERIFY(snapshot); const auto sample = SampleBuffer::fromFile(path); QVERIFY(sample && !sample->empty()); DataFile document(DataFile::Type::SongProject); auto node = document.createElement("lfocontroller"); document.content().appendChild(node); node.setAttribute("userwavefile", kind == 1 ? snapshot->path() : path); node.setAttribute("userwaveunresolved", 1); node.setAttribute("type", static_cast<int>(Controller::ControllerType::Lfo)); { DataFile::ResourceSourceScope scope(document, true, kind == 2); DataFile::registerResourceSnapshot(document, node, "userwavefile", snapshot, kind == 2 ? sample : nullptr); } Lfo native; { DataFile::ResourceReadScope reading(document); QVERIFY_EXCEPTION_THROWN(native.loadSettings(node), std::invalid_argument); } QJsonArray assets; QVERIFY(!document.copyResources(destination.filePath("assets"), &assets)); QVERIFY(assets.isEmpty());
	}
	void pendingCheckpointsRequireNoInventedAssetAndBundlesRemainStrict_data()
	{ QTest::addColumn<bool>("present"); QTest::newRow("absent") << false; QTest::newRow("later-present") << true; }
	void pendingCheckpointsRequireNoInventedAssetAndBundlesRemainStrict()
	{
		QFETCH(bool, present); QTemporaryDir source, destination; const auto path = source.filePath("lost.wav"); if (present) { QVERIFY(wave(path)); } Lfo native; missing(&native, path); DataFile document(DataFile::Type::SongProject); auto node = document.createElement("lfocontroller"); document.content().appendChild(node); { DataFile::ResourceSourceScope saving(document); native.saveSettings(document, node); } QJsonArray assets; QVERIFY(document.copyResources(destination.filePath("assets"), &assets)); QVERIFY(assets.isEmpty()); QVERIFY(document.matchesAssetReferences(assets)); QCOMPARE(node.attribute("userwavefile"), path); const auto before = document.toString(); QVERIFY(!document.copyResources(destination.filePath("bundle"))); QCOMPARE(document.toString(), before);
	}
	void pendingManifestTamperingIsRefused_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("marker-removed") << 0; QTest::newRow("relative") << 1; QTest::newRow("different-resource-tag") << 2; QTest::newRow("empty-path") << 3; }
	void pendingManifestTamperingIsRefused()
	{
		QFETCH(int, kind); QTemporaryDir source; DataFile document(DataFile::Type::SongProject); auto node = document.createElement("lfocontroller"); document.content().appendChild(node); node.setAttribute("userwavefile", source.filePath("lost.wav")); node.setAttribute("userwaveunresolved", 1); QVERIFY(document.matchesAssetReferences({})); if (kind == 0) { node.removeAttribute("userwaveunresolved"); } if (kind == 1) { node.setAttribute("userwavefile", "local:lost.wav"); } if (kind == 2) { node.setTagName("tripleoscillator"); node.setAttribute("userwavefile0", node.attribute("userwavefile")); } if (kind == 3) { node.setAttribute("userwavefile", ""); } QVERIFY(!document.matchesAssetReferences({}));
	}
	void dependencyInventorySeparatesFilePresenceFromPendingState_data()
	{ QTest::addColumn<bool>("present"); QTest::newRow("absent") << false; QTest::newRow("appeared") << true; }
	void dependencyInventorySeparatesFilePresenceFromPendingState()
	{
		QFETCH(bool, present); QTemporaryDir source; const auto path = source.filePath("lost.wav"); auto native = rack(); missing(native, path); if (present) { QVERIFY(wave(path)); } const auto response = StudioCapabilities::execute("project.inspect", {{"section", "resources"}}); QVERIFY(response.value("ok").toBool()); const auto page = response.value("result").toObject(); QCOMPARE(page.value("declaredUnresolvedCount").toInt(), 1); const auto row = page.value("resources").toArray()[0].toObject(); QVERIFY(row.value("unresolved").toBool()); QCOMPARE(row.value("path").toString(), path); QCOMPARE(row.value("repairOperation").toString(), QString("controller.waveform.set")); QCOMPARE(row.value("owner").toObject().value("controller").toInt(-1), 0); QCOMPARE(row.value("file").toObject().value("status").toString(), present ? QString("readableFile") : QString("missing")); const auto controllers = StudioCapabilities::execute("controller.list", {}); QVERIFY(controllers.value("result").toObject().value("controllers").toArray()[0].toObject().value("userWave").toObject().value("unresolved").toBool());
	}
	void dirtyFeedbackCannotHideAReplacedResource()
	{
		QTemporaryDir source; const auto path = source.filePath("wave.wav"), lost = source.filePath("lost.wav"); QVERIFY(wave(path)); auto native = rack(); missing(native, lost); NativeSongTestAccess::setModified(Engine::getSong(), false); bool changed = false; const auto connection = connect(Engine::getSong(), &Song::modified, this, [&] { if (!changed) { changed = true; missing(native, lost); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto response = StudioCapabilities::execute("controller.waveform.set", {{"controller", 0}, {"path", path}}); QVERIFY(changed); QVERIFY(!response.value("ok").toBool()); QVERIFY(native->userWaveUnresolved());
	}
	void borrowedPathsAreCopiedAndConcurrentControlEditsAreRefused_data()
	{ QTest::addColumn<bool>("control"); QTest::newRow("borrowed-path") << false; QTest::newRow("control-edit") << true; }
	void borrowedPathsAreCopiedAndConcurrentControlEditsAreRefused()
	{
		QFETCH(bool, control); QTemporaryDir source; QString path = source.filePath("wave.wav"); QVERIFY(wave(path)); Lfo native; bool invoked = false; QString error; { FileResourceIO::Scope work([&] { if (!invoked) { invoked = true; if (control) { native.amount().setValue(.125f); } else { path = source.filePath("changed.wav"); } } return false; }); if (control) { QVERIFY_EXCEPTION_THROWN(native.setUserWave(path, error), std::runtime_error); } else { QVERIFY(native.setUserWave(path, error)); } } QVERIFY(invoked); QCOMPARE(native.userWave().size(), control ? std::size_t(0) : std::size_t(64));
	}
	void nativeProjectChangesInvalidateReturnedResourceWork_data()
	{ QTest::addColumn<bool>("replace"); QTest::newRow("project-path") << false; QTest::newRow("project-generation") << true; }
	void nativeProjectChangesInvalidateReturnedResourceWork()
	{
		QFETCH(bool, replace); QTemporaryDir source; const auto path = source.filePath("wave.wav"); QVERIFY(wave(path)); Lfo native; bool invoked = false; QString error; { FileResourceIO::Scope work([&] { if (!invoked) { invoked = true; if (replace) { Engine::getSong()->createEmptyProject(); } else { NativeSongTestAccess::setProjectFileName(Engine::getSong(), source.filePath("another.mmp")); } } return false; }); QVERIFY_EXCEPTION_THROWN(native.setUserWave(path, error), std::runtime_error); } QVERIFY(invoked); QVERIFY(native.userWave().empty()); QVERIFY(native.setUserWave(path, error));
	}
	void serializationRejectsCooperativeControlChanges()
	{
		Lfo native; QDomDocument document; auto node = document.createElement("lfocontroller"); document.appendChild(node); bool changed = false; { FileResourceIO::Scope work([&] { if (!changed) { changed = true; native.amount().setValue(.125f); } return false; }); QVERIFY_EXCEPTION_THROWN(native.saveSettings(document, node), std::runtime_error); } QVERIFY(changed); native.saveSettings(document, node);
	}
	void acceptedSerializationRetainsOwnedWaveAfterOriginalRemoval()
	{
		QTemporaryDir source; const auto path = source.filePath("wave.wav"); QVERIFY(wave(path)); Lfo native; QString error; QVERIFY(native.setUserWave(path, error)); DataFile document(DataFile::Type::SongProject); auto node = document.createElement("lfocontroller"); document.content().appendChild(node); { DataFile::ResourceSourceScope saving(document, true, true); native.saveSettings(document, node); } QVERIFY(QFile::remove(path)); Lfo restored; { DataFile::ResourceReadScope reading(document); restored.loadSettings(node); } QVERIFY(!restored.userWaveUnresolved()); QCOMPARE(restored.userWave().size(), std::size_t(64)); QCOMPARE(restored.userWave().data()[0][0], native.userWave().data()[0][0]);
	}
	void emptyOrdinarySettingsRemainClear()
	{
		Lfo native; QDomDocument document; native.loadSettings(settings(document, {}, false)); QVERIFY(!native.userWaveUnresolved()); QVERIFY(native.userWaveFile().isEmpty()); QVERIFY(native.userWave().empty()); QVERIFY(!Engine::getSong()->hasErrors());
	}
	void corruptExistingWavesStayFatalAndFailedProjectLoadsRollback_data()
	{ QTest::addColumn<bool>("runtime"); QTest::newRow("native-settings") << false; QTest::newRow("runtime-project-load") << true; }
	void corruptExistingWavesStayFatalAndFailedProjectLoadsRollback()
	{
		QFETCH(bool, runtime); QTemporaryDir source, session; const auto corrupt = source.filePath("bad.wav"); { QFile file(corrupt); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("invalid audio"), qint64(13)); }
		if (!runtime) { Lfo native; QDomDocument document; native.loadSettings(settings(document, corrupt, false)); QVERIFY(Engine::getSong()->hasFatalErrors()); QVERIFY(!native.userWaveUnresolved()); return; }
		auto native = rack(); const auto lost = source.filePath("lost.wav"); missing(native, lost); const auto project = source.filePath("missing.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(project)); DataFile broken(DataFile::Type::SongProject); auto controllers = broken.createElement("controllers"); broken.content().appendChild(controllers); QDomDocument settingsDocument; controllers.appendChild(broken.importNode(settings(settingsDocument, corrupt, false), true)); const auto badProject = source.filePath("bad.mmp"); QVERIFY(broken.writeFile(badProject)); QString error; StudioRuntime studio(session.path()); QVERIFY(studio.open(error)); QVERIFY(studio.process(request("good", "project.load", {{"path", project}, {"summary", true}})).value("ok").toBool()); const auto before = manifest(session); const auto command = request("bad", "project.load", {{"path", badProject}, {"summary", true}}); const auto failure = studio.process(command); QVERIFY(!failure.value("ok").toBool()); auto replay = studio.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, failure); QCOMPARE(manifest(session).value("history"), before.value("history")); auto current = static_cast<LfoController*>(Engine::getSong()->controllers()[0]); QVERIFY(current->userWaveUnresolved()); QCOMPARE(current->userWaveFile(), lost); QVERIFY(!studio.stopping());
	}
	void queuedEventsStayOutsideSettingsAndDecode()
	{
		QTemporaryDir source; const auto path = source.filePath("wave.wav"); QVERIFY(wave(path)); Lfo native; bool queued = false; QString error; QTimer::singleShot(0, this, [&] { queued = true; }); { FileResourceIO::Scope work({}, true); QVERIFY(native.setUserWave(path, error)); QVERIFY(!queued); } QCoreApplication::processEvents(); QVERIFY(queued);
	}
	void runtimeRepairUndoRedoFailureReplayAndReopen_data()
	{ QTest::addColumn<bool>("select"); QTest::newRow("preserve-selection") << false; QTest::newRow("explicit-selection") << true; }
	void runtimeRepairUndoRedoFailureReplayAndReopen()
	{
		QFETCH(bool, select); QTemporaryDir source, directory; const auto path = source.filePath("wave.wav"), lost = source.filePath("lost.wav"), project = source.filePath("missing.mmp"); QVERIFY(wave(path)); auto native = rack(); missing(native, lost, false); native->waveModel().setValue(static_cast<int>(Oscillator::WaveShape::Sine)); auto track = new InstrumentTrack(Engine::getSong()); track->volumeModel()->setControllerConnection(new ControllerConnection(native)); QVERIFY(Engine::getSong()->saveProjectFile(project)); QString error; QJsonObject receipt; const auto repair = request("repair", "controller.waveform.set", {{"controller", 0}, {"path", path}, {"select", select}});
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("load", "project.load", {{"path", project}, {"summary", true}})).value("ok").toBool()); receipt = runtime.process(repair); QVERIFY(receipt.value("ok").toBool()); auto current = static_cast<LfoController*>(Engine::getSong()->controllers()[0]); QVERIFY(!current->userWaveUnresolved()); QCOMPARE(current->waveModel().storedValue(), static_cast<float>(select ? Oscillator::WaveShape::UserDefined : Oscillator::WaveShape::Sine)); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QVERIFY(static_cast<LfoController*>(Engine::getSong()->controllers()[0])->userWaveUnresolved()); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); auto replay = runtime.process(repair); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); const auto before = manifest(directory); const auto rejected = request("bad", "controller.waveform.set", {{"controller", 0}, {"path", lost}}); const auto failure = runtime.process(rejected); QVERIFY(!failure.value("ok").toBool()); auto again = runtime.process(rejected); QVERIFY(again.take("replayed").toBool()); QCOMPARE(again, failure); QCOMPARE(manifest(directory).value("history"), before.value("history")); QVERIFY(!static_cast<LfoController*>(Engine::getSong()->controllers()[0])->userWaveUnresolved()); }
		QVERIFY(QFile::remove(path)); StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); auto current = static_cast<LfoController*>(Engine::getSong()->controllers()[0]); QVERIFY(!current->userWaveUnresolved()); QCOMPARE(current->userWave().size(), std::size_t(64)); auto binding = static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->volumeModel()->controllerConnection(); QVERIFY(binding); QCOMPARE(binding->getController(), current); QCOMPARE(current->waveModel().storedValue(), static_cast<float>(select ? Oscillator::WaveShape::UserDefined : Oscillator::WaveShape::Sine));
	}
	void scriptedAgentsUseDependencyAndLookupResultsForWaveRepair_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void scriptedAgentsUseDependencyAndLookupResultsForWaveRepair()
	{
		QFETCH(QString, protocol); QTemporaryDir source, session; const auto path = source.filePath("found.wave.wav"), project = source.filePath("missing.mmp"); QVERIFY(wave(path)); auto native = rack(); missing(native, source.filePath("missing.wave.wav")); QVERIFY(Engine::getSong()->saveProjectFile(project)); QString error; int round = 0, controller = -1; QJsonArray evidence;
		StudioRuntime runtime(session.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			QJsonObject output; for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray()) { const auto item = value.toObject(); if (item.value("role") == QJsonValue("tool") || item.value("type") == QJsonValue("function_call_output")) { output = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); } } const auto call = [&](const QString& id, const QString& name, const QJsonObject& args) { evidence.append(id); return decision(protocol, id, name, args); }; ++round;
			if (round == 1) { return call("discover", "project_inspect", {{"section", "resources"}}); }
			if (!output.value("ok").toBool()) { throw std::runtime_error("Native dependency or lookup operation failed"); }
			if (round == 2) { const auto row = output.value("result").toObject().value("resources").toArray()[0].toObject(); if (!row.value("unresolved").toBool() || row.value("repairOperation") != QJsonValue("controller.waveform.set")) { throw std::runtime_error("Expected pending native LFO resource"); } controller = row.value("owner").toObject().value("controller").toInt(-1); return call("find", "asset_search", {{"root", source.path()}, {"query", "found.wave"}, {"extensions", QJsonArray{"wav"}}}); }
			if (round == 3) { const auto page = output.value("result").toObject(); if (page.value("total").toInt() != 1 || !page.value("scanComplete").toBool()) { throw std::runtime_error("Expected one observed native wave candidate"); } return call("repair", "studio_execute", {{"op", "controller.waveform.set"}, {"args", QJsonObject{{"controller", controller}, {"path", page.value("matches").toArray()[0].toObject().value("path")}}}}); }
			if (round == 4) { return call("verify", "project_inspect", {{"section", "resources"}}); }
			if (output.value("result").toObject().value("declaredUnresolvedCount").toInt(-1) != 0) { throw std::runtime_error("Repaired LFO resource was not observed"); } return decision(protocol, "finish", "agent_finish", {{"status", "complete"}, {"summary", "Repaired the observed native LFO wave reference; musical evaluation was not performed."}, {"evidence", evidence}});
		}); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("load", "project.load", {{"path", project}, {"summary", true}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-lfo-repair-fixture"}}}})).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Find found.wave.wav in " + source.path() + " and repair the unresolved LFO waveform, preserving its controls."}})).value("ok").toBool()); for (int i = 0; i < 9; ++i) { runtime.advanceAgent(); } QCOMPARE(manifest(session).value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete")); QVERIFY(!static_cast<LfoController*>(Engine::getSong()->controllers()[0])->userWaveUnresolved());
	}
};
QTEST_GUILESS_MAIN(LfoWaveResourceTest)
#include "LfoWaveResourceTest.moc"
