/* Native SoundFont missing-bank preservation and repair. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
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
#include "Note.h"
#include "NotePlayHandle.h"
#include "PluginFactory.h"
#include "Sf2Player.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
class NativeTrack final : public InstrumentTrack
{
public:
	NativeTrack() : InstrumentTrack(Engine::getSong()) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new Sf2Instrument(this); }
};
QDomElement settings(QDomDocument& document, const QString& path, bool pending = true)
{
	auto node = document.createElement("sf2player"); document.appendChild(node); node.setAttribute("src", path); if (pending) { node.setAttribute("unresolved", 1); }
	node.setAttribute("bank", 1024); node.setAttribute("patch", 7); node.setAttribute("gain", .73); node.setAttribute("reverbOn", 1); node.setAttribute("reverbRoomSize", .37); node.setAttribute("reverbDamping", .43); node.setAttribute("reverbWidth", .61); node.setAttribute("reverbLevel", .47); node.setAttribute("chorusOn", 1); node.setAttribute("chorusNum", 5); node.setAttribute("chorusLevel", 3.3); node.setAttribute("chorusSpeed", 1.1); node.setAttribute("chorusDepth", 17); return node;
}
void missing(Sf2Instrument* native, const QString& path, bool pending = true) { QDomDocument document; native->loadSettings(settings(document, path, pending)); }
QVector<float> values(Instrument* native) { QVector<float> out; for (auto model : native->findChildren<AutomatableModel*>(QString{}, Qt::FindDirectChildrenOnly)) { out.append(model->storedValue()); } return out; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
double energy(Sf2Instrument* native)
{
	const auto frames = Engine::audioEngine()->framesPerPeriod(); std::vector<SampleFrame> pcm(frames); NotePlayHandle voice(native->instrumentTrack(), 0, frames * 4, Note(TimePos(48), TimePos(), 69));
	const auto cleanup = qScopeGuard([&] { native->deleteNotePluginData(&voice); }); native->playNote(&voice, pcm.data()); double total = 0;
	for (int period = 0; period < 4; ++period) { native->play(pcm.data()); for (const auto& frame : pcm) { for (int channel = 0; channel < 2; ++channel) { if (!std::isfinite(frame[channel])) { throw std::runtime_error("Nonfinite native SoundFont output"); } total += std::abs(frame[channel]); } } } return total;
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
class Sf2PendingBankTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QString bank() const { return QString::fromUtf8(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2"; }
	Sf2Instrument* seed() { auto track = new NativeTrack; auto native = dynamic_cast<Sf2Instrument*>(track->loadInstrument("sf2player")); if (!native) { throw std::runtime_error("Native SoundFont was not selected"); } return native; }
	Instrument* current() const { return static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument(); }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setSF2File(""); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); QVERIFY(QFile::exists(bank())); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); Engine::getSong()->setProjectFileName(""); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void missingBanksRetainPathAndAllNativeControls_data()
	{ QTest::addColumn<int>("mode"); QTest::newRow("legacy-missing") << 0; QTest::newRow("marked-missing") << 1; QTest::newRow("marked-appeared") << 2; }
	void missingBanksRetainPathAndAllNativeControls()
	{
		QFETCH(int, mode); QTemporaryDir source; const auto lost = source.filePath("missing.sf2"); if (mode == 2) { QVERIFY(QFile::copy(bank(), lost)); } auto native = seed(); QString error; QVERIFY(native->setFileResource("soundfont", bank(), error)); missing(native, lost, mode != 0); const auto resource = native->fileResources()[0]; QVERIFY(resource.unresolved); QCOMPARE(resource.path, lost); QVERIFY(!resource.identity); QVERIFY(native->programSnapshot().empty()); QCOMPARE(native->getBank(), 1024); QCOMPARE(native->getPatch(), 7); QCOMPARE(values(native).size(), 13); QVERIFY(Engine::getSong()->hasErrors()); QVERIFY(!Engine::getSong()->hasFatalErrors()); QCOMPARE(energy(native), 0.);
		QDomDocument document; auto node = document.createElement("sf2player"); document.appendChild(node); native->saveSettings(document, node); QCOMPARE(node.attribute("src"), lost); QCOMPARE(node.attribute("unresolved"), QString("1")); auto restored = seed(); restored->loadSettings(node); QVERIFY(restored->fileResources()[0].unresolved); QCOMPARE(values(restored), values(native)); QCOMPARE(restored->getCurrentPatchName(), QString{});
	}
	void repairPreservesSavedBankProgramAndEffects_data()
	{ QTest::addColumn<int>("route"); QTest::newRow("native-setter") << 0; QTest::newRow("structured-resource") << 1; QTest::newRow("native-open-file") << 2; }
	void repairPreservesSavedBankProgramAndEffects()
	{
		QFETCH(int, route); QTemporaryDir source; auto native = seed(); missing(native, source.filePath("missing.sf2")); const auto controls = values(native); QString error;
		if (route == 0) { QVERIFY(native->setFileResource("soundfont", bank(), error)); } else if (route == 1) { const auto response = StudioCapabilities::execute("instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", bank()}}); QVERIFY2(response.value("ok").toBool(), QJsonDocument(response).toJson().constData()); } else { native->openFile(bank(), false); }
		QVERIFY(!native->fileResources()[0].unresolved); QVERIFY(native->fileResources()[0].identity); QCOMPARE(native->programSnapshot().size(), std::size_t(2)); QCOMPARE(values(native), controls); QCOMPARE(native->getCurrentPatchName(), QString("Native octave")); QDomDocument document; auto node = document.createElement("sf2player"); node.setAttribute("unresolved", 1); native->saveSettings(document, node); QVERIFY(!node.hasAttribute("unresolved"));
	}
	void unavailableSavedProgramRejectsRepairWithoutDiscardingPendingState_data()
	{ QTest::addColumn<bool>("patch"); QTest::newRow("unavailable-bank") << false; QTest::newRow("unavailable-program") << true; }
	void unavailableSavedProgramRejectsRepairWithoutDiscardingPendingState()
	{
		QFETCH(bool, patch); QTemporaryDir source; auto native = seed(); const auto lost = source.filePath("missing.sf2"); missing(native, lost); native->childModel(patch ? "patch" : "bank")->setValue(patch ? 99 : 111); const auto controls = values(native); QString error; QVERIFY(!native->setFileResource("soundfont", bank(), error)); QVERIFY(!error.isEmpty()); QVERIFY(native->fileResources()[0].unresolved); QCOMPARE(native->fileResources()[0].path, lost); QVERIFY(!native->fileResources()[0].identity); QCOMPARE(values(native), controls); QVERIFY(native->programSnapshot().empty()); native->childModel("bank")->setValue(1024); native->childModel("patch")->setValue(7); QVERIFY(native->setFileResource("soundfont", bank(), error)); QCOMPARE(native->getCurrentPatchName(), QString("Native octave"));
	}
	void rejectedFilesAndExplicitClearPreserveDistinctNativeState_data()
	{ QTest::addColumn<bool>("accepted"); QTest::newRow("pending") << false; QTest::newRow("accepted") << true; }
	void rejectedFilesAndExplicitClearPreserveDistinctNativeState()
	{
		QFETCH(bool, accepted); QTemporaryDir source; const auto lost = source.filePath("missing.sf2"), corrupt = source.filePath("bad.sf2"); QVERIFY(write(corrupt, "invalid soundfont")); auto native = seed(); missing(native, lost); QString error; if (accepted) { QVERIFY(native->setFileResource("soundfont", bank(), error)); } const auto before = native->fileResources()[0]; const auto controls = values(native); QVERIFY(!native->setFileResource("soundfont", corrupt, error)); QVERIFY(!native->setFileResource("soundfont", source.filePath("absent.sf2"), error)); QCOMPARE(native->fileResources()[0].path, before.path); QCOMPARE(native->fileResources()[0].unresolved, !accepted); QVERIFY(native->fileResources()[0].identity == before.identity); QCOMPARE(values(native), controls); QVERIFY(native->setFileResource("soundfont", {}, error)); QVERIFY(!native->fileResources()[0].unresolved); QVERIFY(native->fileResources()[0].path.isEmpty()); QVERIFY(!native->fileResources()[0].identity); QCOMPARE(values(native), controls);
	}
	void emptySettingsClearAPendingBankWithoutRetainingItsMarker()
	{
		QTemporaryDir source; auto native = seed(); missing(native, source.filePath("missing.sf2")); QDomDocument document; auto node = document.createElement("sf2player"); document.appendChild(node); native->loadSettings(node); QVERIFY(!native->fileResources()[0].unresolved); QVERIFY(native->fileResources()[0].path.isEmpty()); QVERIFY(!native->fileResources()[0].identity); QVERIFY(native->programSnapshot().empty()); native->saveSettings(document, node); QVERIFY(!node.hasAttribute("unresolved"));
	}
	void checkpointsRetainUnboundBankReferencesWithoutInventingAssets_data()
	{ QTest::addColumn<QString>("extension"); QTest::newRow("sf2") << QString("sf2"); QTest::newRow("sf3") << QString("sf3"); }
	void checkpointsRetainUnboundBankReferencesWithoutInventingAssets()
	{
		QFETCH(QString, extension); QTemporaryDir source, destination; const auto lost = source.filePath("missing." + extension); auto native = seed(); missing(native, lost); DataFile document(DataFile::Type::SongProject); auto node = document.createElement("sf2player"); document.content().appendChild(node); { DataFile::ResourceSourceScope saving(document); native->saveSettings(document, node); } QVERIFY(write(lost, "new unrelated file")); QJsonArray assets; QVERIFY(document.copyResources(destination.filePath("assets"), &assets)); QVERIFY(assets.isEmpty()); QVERIFY(document.matchesAssetReferences(assets)); QCOMPARE(node.attribute("src"), lost); QVERIFY(!document.copyResources(destination.filePath("bundle"))); auto restored = seed(); { DataFile::ResourceReadScope reading(document); restored->loadSettings(node); } QVERIFY(restored->fileResources()[0].unresolved); QVERIFY(!Engine::getSong()->hasFatalErrors()); node.removeAttribute("unresolved"); QVERIFY(!document.matchesAssetReferences(assets));
	}
	void malformedPendingReferencesAreRefused_data()
	{ QTest::addColumn<int>("mode"); QTest::newRow("empty") << 0; QTest::newRow("embedded-null") << 1; QTest::newRow("unresolved-project-base") << 2; }
	void malformedPendingReferencesAreRefused()
	{
		QFETCH(int, mode); auto native = seed(); QDomDocument document; const auto path = mode == 0 ? QString{} : mode == 1 ? QString("bank") + QChar::Null + ".sf2" : QString("local:bank.sf2"); const auto node = settings(document, path); QVERIFY_EXCEPTION_THROWN(native->loadSettings(node), std::invalid_argument); QVERIFY(!native->fileResources()[0].unresolved); QVERIFY(native->fileResources()[0].path.isEmpty());
	}
	void acceptedBindingCannotBeDowngradedToPending()
	{
		auto native = seed(); QString error; QVERIFY(native->setFileResource("soundfont", bank(), error)); DataFile document(DataFile::Type::SongProject); auto node = document.createElement("sf2player"); document.content().appendChild(node); { DataFile::ResourceSourceScope saving(document); native->saveSettings(document, node); } node.setAttribute("unresolved", 1); auto restored = seed(); { DataFile::ResourceReadScope reading(document); QVERIFY_EXCEPTION_THROWN(restored->loadSettings(node), std::invalid_argument); } QTemporaryDir destination; QJsonArray assets; QVERIFY(!document.copyResources(destination.filePath("assets"), &assets)); QVERIFY(assets.isEmpty());
	}
	void settingsReplacementUsesItsOwnPatchRatherThanPendingRepairConstraints()
	{
		QTemporaryDir source; auto native = seed(); missing(native, source.filePath("missing.sf2")); native->childModel("bank")->setValue(111); native->childModel("patch")->setValue(99); QDomDocument document; native->loadSettings(settings(document, bank(), false)); QVERIFY(!Engine::getSong()->hasFatalErrors()); QVERIFY(!native->fileResources()[0].unresolved); QCOMPARE(native->getBank(), 1024); QCOMPARE(native->getPatch(), 7); QCOMPARE(native->getCurrentPatchName(), QString("Native octave"));
	}
	void existingCorruptBankStillProducesFatalNativeDiagnostic()
	{
		QTemporaryDir source; const auto bad = source.filePath("bad.sf2"); QVERIFY(write(bad, "invalid soundfont")); auto native = seed(); missing(native, bad, false); QVERIFY(Engine::getSong()->hasFatalErrors()); QVERIFY(!native->fileResources()[0].unresolved);
	}
	void nativeSynthRebuildPreservesPendingStateAndSilentOutput()
	{
		QTemporaryDir source; const auto lost = source.filePath("missing.sf2"); auto native = seed(); missing(native, lost); const auto controls = values(native); native->reloadSynth(); QVERIFY(native->fileResources()[0].unresolved); QCOMPARE(native->fileResources()[0].path, lost); QCOMPARE(values(native), controls); QCOMPARE(energy(native), 0.); QVERIFY(!Engine::getSong()->hasFatalErrors());
	}
	void pendingNotificationsObserveOwnerRetirement_data()
	{ QTest::addColumn<int>("stage"); QTest::newRow("file-loading") << 0; QTest::newRow("patch-changed") << 1; QTest::newRow("file-changed") << 2; }
	void pendingNotificationsObserveOwnerRetirement()
	{
		QFETCH(int, stage); QTemporaryDir source; const QPointer<Sf2Instrument> owner(seed()); auto native = owner.data(); const QPointer<InstrumentTrack> track(owner->instrumentTrack()); bool retired = false; const auto retire = [&] { if (!retired) { retired = true; delete track.data(); } }; if (stage == 0) { connect(native, &Sf2Instrument::fileLoading, this, retire); } if (stage == 1) { connect(native, &Sf2Instrument::patchChanged, this, retire); } if (stage == 2) { connect(native, &Sf2Instrument::fileChanged, this, retire); } QDomDocument document; const auto node = settings(document, source.filePath("missing.sf2")); QVERIFY_EXCEPTION_THROWN(native->loadSettings(node), std::runtime_error); QVERIFY(retired); QVERIFY(!owner); QVERIFY(!track);
	}
	void failedAdmissionKeepsAnAcceptedBankAvailable_data()
	{ QTest::addColumn<bool>("exception"); QTest::newRow("cancel") << false; QTest::newRow("exception") << true; }
	void failedAdmissionKeepsAnAcceptedBankAvailable()
	{
		QFETCH(bool, exception); QTemporaryDir source; auto native = seed(); QString error; QVERIFY(native->setFileResource("soundfont", bank(), error)); const auto before = native->fileResources()[0]; QDomDocument document; const auto node = settings(document, source.filePath("missing.sf2")); { FileResourceIO::Scope work([&] { if (exception) { throw std::runtime_error("Pending bank fixture"); } return true; }); QVERIFY_EXCEPTION_THROWN(native->loadSettings(node), std::runtime_error); } QVERIFY(native->fileResources()[0].identity == before.identity); QVERIFY(!native->fileResources()[0].unresolved); native->loadSettings(node); QVERIFY(native->fileResources()[0].unresolved);
	}
	void inspectionSeparatesPendingDeclarationFromCurrentFilesystem()
	{
		QTemporaryDir source; const auto lost = source.filePath("missing.sf2"); auto native = seed(); missing(native, lost); QVERIFY(QFile::copy(bank(), lost)); const auto response = StudioCapabilities::execute("project.inspect", {{"section", "resources"}}); QVERIFY(response.value("ok").toBool()); const auto page = response.value("result").toObject(); QCOMPARE(page.value("declaredUnresolvedCount").toInt(), 1); const auto row = page.value("resources").toArray()[0].toObject(); QVERIFY(row.value("unresolved").toBool()); QCOMPARE(row.value("key").toString(), QString("soundfont")); QCOMPARE(row.value("path").toString(), lost); QCOMPARE(row.value("file").toObject().value("status").toString(), QString("readableFile")); QCOMPARE(row.value("repairOperation").toString(), QString("instrument.resource.set"));
	}
	void mixedAcceptedAndPendingBanksUseOnlyRealOwnedAssets()
	{
		QTemporaryDir source, destination; auto pending = seed(); missing(pending, source.filePath("missing.sf2")); auto accepted = seed(); QString error; QVERIFY(accepted->setFileResource("soundfont", bank(), error)); DataFile document(DataFile::Type::SongProject); { DataFile::ResourceSourceScope saving(document); for (auto native : {pending, accepted}) { auto node = document.createElement("sf2player"); document.content().appendChild(node); native->saveSettings(document, node); } } QJsonArray assets; QVERIFY(document.copyResources(destination.filePath("assets"), &assets)); QCOMPARE(assets.size(), 1); QVERIFY(document.matchesAssetReferences(assets)); QVERIFY(!document.copyResources(destination.filePath("bundle")));
	}
	void nativeNoteAudioReturnsAfterRepairWithoutChangingTheSavedPatch()
	{
		QTemporaryDir source; auto native = seed(); QString error; QVERIFY(native->setFileResource("soundfont", bank(), error)); QVERIFY(native->selectProgram(1024, 7, error)); QVERIFY(energy(native) > 0.); missing(native, source.filePath("missing.sf2")); QCOMPARE(energy(native), 0.); QVERIFY(native->setFileResource("soundfont", bank(), error)); QCOMPARE(native->getBank(), 1024); QCOMPARE(native->getPatch(), 7); QVERIFY(energy(native) > 0.);
	}
	void runtimePreservesMissingBankHistoryAndOwnedRepairAfterSourceRemoval()
	{
		QVERIFY2(!getPluginFactory()->pluginInfo("sf2player").isNull(), "The built native SoundFont module must be discoverable"); QTemporaryDir source, session; const auto path = source.filePath("repair.sf2"), lost = source.filePath("missing.sf2"), project = source.filePath("missing.mmp"), corrupt = source.filePath("bad.sf2"); QVERIFY(QFile::copy(bank(), path)); QVERIFY(write(corrupt, "invalid soundfont")); auto native = seed(); missing(native, lost, false); const auto controls = values(native); QVERIFY(Engine::getSong()->saveProjectFile(project)); QString error; QJsonObject receipt; const auto repair = request("repair", "instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", path}});
		{ StudioRuntime runtime(session.path()); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("load", "project.load", {{"path", project}, {"summary", true}})).value("ok").toBool()); QVERIFY(current()->fileResources()[0].unresolved); QCOMPARE(values(current()), controls); receipt = runtime.process(repair); QVERIFY(receipt.value("ok").toBool()); QVERIFY(!current()->fileResources()[0].unresolved); QCOMPARE(values(current()), controls); QVERIFY(QFile::copy(bank(), lost)); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QVERIFY(current()->fileResources()[0].unresolved); QCOMPARE(current()->fileResources()[0].path, lost); QCOMPARE(values(current()), controls); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); auto replay = runtime.process(repair); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); const auto before = manifest(session); QVERIFY(!runtime.process(request("bad", "instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", corrupt}})).value("ok").toBool()); QCOMPARE(manifest(session).value("history"), before.value("history")); QVERIFY(!runtime.stopping()); }
		QVERIFY(QFile::remove(path)); StudioRuntime reopened(session.path()); QVERIFY2(reopened.open(error), qPrintable(error)); QVERIFY(!current()->fileResources()[0].unresolved); QCOMPARE(current()->programs().size(), std::size_t(2)); QCOMPARE(values(current()), controls);
	}
	void scriptedAgentsFindAndRepairAnObservedNativeBank_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void scriptedAgentsFindAndRepairAnObservedNativeBank()
	{
		QFETCH(QString, protocol); QVERIFY2(!getPluginFactory()->pluginInfo("sf2player").isNull(), "The built native module must be discoverable"); QTemporaryDir source, session; const auto path = source.filePath("found.bank.sf2"), project = source.filePath("missing.mmp"); QVERIFY(QFile::copy(bank(), path)); auto native = seed(); missing(native, source.filePath("missing.bank.sf2")); const auto controls = values(native); QVERIFY(Engine::getSong()->saveProjectFile(project)); QString error, key; int track = -1, round = 0; QJsonArray evidence;
		StudioRuntime runtime(session.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			QJsonObject output; for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray()) { const auto item = value.toObject(); if (item.value("role") == QJsonValue("tool") || item.value("type") == QJsonValue("function_call_output")) { output = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); } } const auto call = [&](const QString& id, const QString& name, const QJsonObject& args) { evidence.append(id); return decision(protocol, id, name, args); }; ++round;
			if (round == 1) { return call("discover", "project_inspect", {{"section", "resources"}}); } if (!output.value("ok").toBool()) { throw std::runtime_error("Native SoundFont resource operation failed"); }
			if (round == 2) { for (const auto& value : output.value("result").toObject().value("resources").toArray()) { const auto row = value.toObject(); if (row.value("unresolved").toBool()) { key = row.value("key").toString(); track = row.value("owner").toObject().value("track").toInt(-1); } } if (key != "soundfont" || track < 0) { throw std::runtime_error("No missing native SoundFont observed"); } return call("find", "asset_search", {{"root", source.path()}, {"query", "found.bank"}, {"extensions", QJsonArray{"sf2"}}}); }
			if (round == 3) { const auto page = output.value("result").toObject(); if (page.value("total").toInt() != 1 || !page.value("scanComplete").toBool()) { throw std::runtime_error("Expected one scoped bank candidate"); } return call("repair", "studio_execute", {{"op", "instrument.resource.set"}, {"args", QJsonObject{{"track", track}, {"resource", key}, {"path", page.value("matches").toArray()[0].toObject().value("path")}}}}); }
			if (round == 4) { return call("verify", "project_inspect", {{"section", "resources"}}); } if (output.value("result").toObject().value("declaredUnresolvedCount").toInt(-1) != 0) { throw std::runtime_error("Bank repair was not observed"); } return decision(protocol, "finish", "agent_finish", {{"status", "complete"}, {"summary", "Repaired the native SoundFont reference and preserved controls; musical evaluation was not performed."}, {"evidence", evidence}});
		}); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("load", "project.load", {{"path", project}, {"summary", true}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-soundfont-repair-fixture"}}}})).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Find found.bank.sf2 in " + source.path() + " and repair the unresolved SoundFont while preserving its bank, patch and effects."}})).value("ok").toBool()); for (int i = 0; i < 9; ++i) { runtime.advanceAgent(); } QCOMPARE(manifest(session).value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete")); QCOMPARE(values(current()), controls);
	}
};
QTEST_GUILESS_MAIN(Sf2PendingBankTest)
#include "Sf2PendingBankTest.moc"
