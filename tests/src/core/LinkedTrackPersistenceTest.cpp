/* Native portable linked settings and copied receiver identity.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QTemporaryDir>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "LfoController.h"
#include "MidiClip.h"
#include "MidiController.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;
namespace
{
InstrumentTrack* linked()
{
	auto track = new InstrumentTrack(Engine::getSong()); track->volumeModel()->setInitValue(25); track->panningModel()->setInitValue(25);
	if (!track->volumeModel()->linkToModel(track->panningModel())) { return nullptr; } return track;
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
}

class LinkedTrackPersistenceTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
		Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void cloneKeepsInternalGroupsIndependentOfOriginalModels()
	{
		auto original = linked(); QVERIFY(original); const auto volumeId = original->volumeModel()->id();
		auto clone = dynamic_cast<InstrumentTrack*>(original->clone()); QVERIFY2(clone, qPrintable(Engine::getSong()->errorSummary()));
		QVERIFY(clone->volumeModel()->isLinkedToModel(clone->panningModel())); QVERIFY(!clone->volumeModel()->isLinkedToModel(original->volumeModel()));
		QVERIFY(clone->volumeModel()->id() != volumeId); QCOMPARE(original->volumeModel()->id(), volumeId);
		clone->volumeModel()->setValue(77); QCOMPARE(clone->panningModel()->storedValue(), 77.f); QCOMPARE(original->volumeModel()->storedValue(), 25.f);
	}

	void crossTrackGroupsRefuseCloningBeforeCreatingAReplacement()
	{
		auto original = new InstrumentTrack(Engine::getSong()), other = new InstrumentTrack(Engine::getSong()); QVERIFY(original->volumeModel()->linkToModel(other->volumeModel()));
		const auto count = Engine::getSong()->tracks().size(); QVERIFY(!original->clone()); QCOMPARE(Engine::getSong()->tracks().size(), count);
		QVERIFY(original->volumeModel()->isLinkedToModel(other->volumeModel())); QVERIFY(Engine::getSong()->hasErrors());
	}

	void nativePresetEntriesPreserveGroupsIdentityAndDestinationState_data()
	{ QTest::addColumn<bool>("structured"); QTest::newRow("native-entry") << false; QTest::newRow("structured-entry") << true; }
	void nativePresetEntriesPreserveGroupsIdentityAndDestinationState()
	{
		QFETCH(bool, structured); QTemporaryDir directory; QVERIFY(directory.isValid()); auto source = linked(); QVERIFY(source); const auto path = directory.filePath("linked.xpf");
		if (structured) { QVERIFY(StudioCapabilities::execute("preset.save", {{"track", 0}, {"path", path}}).value("ok").toBool()); }
		else { DataFile document(DataFile::Type::InstrumentTrackSettings); source->savePreset(document, document.content()); QVERIFY(document.writeFile(path)); }
		auto target = new InstrumentTrack(Engine::getSong()); const auto id = target->volumeModel()->id(); const auto channel = Engine::mixer()->createChannel();
		target->mixerChannelModel()->setValue(channel); target->setMuted(true); auto clip = static_cast<MidiClip*>(target->createClip(0));
		if (structured) { QVERIFY(StudioCapabilities::execute("preset.load", {{"track", 1}, {"path", path}}).value("ok").toBool()); }
		else { target->replaceInstrument(DataFile(path)); }
		QVERIFY2(!Engine::getSong()->hasErrors(), qPrintable(Engine::getSong()->errorSummary())); QCOMPARE(target->volumeModel()->id(), id);
		QVERIFY(target->volumeModel()->isLinkedToModel(target->panningModel())); QVERIFY(!target->volumeModel()->isLinkedToModel(source->volumeModel()));
		QCOMPARE(target->volumeModel()->storedValue(), 25.f); QCOMPARE(target->mixerChannelModel()->value(), channel); QVERIFY(target->isMutedByModel()); QCOMPARE(target->getClip(0), static_cast<Clip*>(clip));
	}

	void modernEmptyPresetClearsInternalGroupsWhileLegacyRetainsThem_data()
	{ QTest::addColumn<bool>("legacy"); QTest::newRow("modern-empty-graph") << false; QTest::newRow("legacy-no-graph") << true; }
	void modernEmptyPresetClearsInternalGroupsWhileLegacyRetainsThem()
	{
		QFETCH(bool, legacy); auto source = new InstrumentTrack(Engine::getSong()); DataFile document(DataFile::Type::InstrumentTrackSettings); source->savePreset(document, document.content());
		auto graph = document.content().firstChildElement("nativeLinkedControls"); QVERIFY(!graph.isNull()); QVERIFY(graph.firstChildElement().isNull());
		if (legacy) { document.content().removeChild(graph); }
		auto target = linked(); QVERIFY(target); target->replaceInstrument(document); QVERIFY(!Engine::getSong()->hasErrors());
		QCOMPARE(target->volumeModel()->isLinkedToModel(target->panningModel()), legacy);
	}

	void unresolvedPresetMemberKeepsDestinationBytesAndLiveGroups()
	{
		QTemporaryDir directory; auto source = new InstrumentTrack(Engine::getSong()), other = new InstrumentTrack(Engine::getSong());
		QVERIFY(source->volumeModel()->linkToModel(other->volumeModel())); const auto path = directory.filePath("unchanged.xpf"); QVERIFY(write(path, "Existing preset bytes"));
		const auto result = StudioCapabilities::execute("preset.save", {{"track", 0}, {"path", path}}); QVERIFY(!result.value("ok").toBool());
		QCOMPARE(read(path), QByteArray("Existing preset bytes")); QVERIFY(source->volumeModel()->isLinkedToModel(other->volumeModel()));
	}

	void presetReplacementLeavesClipOwnedGroupsUntouched()
	{
		auto source = new InstrumentTrack(Engine::getSong()); DataFile document(DataFile::Type::InstrumentTrackSettings); source->savePreset(document, document.content());
		auto target = linked(); QVERIFY(target); auto clip = target->createClip(0);
		FloatModel first(42, 0, 100, 0, clip), second(42, 0, 100, 0, clip); QVERIFY(first.linkToModel(&second));
		target->replaceInstrument(document); QVERIFY(first.isLinkedToModel(&second)); QCOMPARE(target->getClip(0), clip); QVERIFY(!target->volumeModel()->isLinked());
	}

	void clipOwnedGroupsCannotBeSilentlyLostByTrackClone()
	{
		auto source = new InstrumentTrack(Engine::getSong()); auto clip = source->createClip(0);
		FloatModel first(42, 0, 100, 0, clip), second(42, 0, 100, 0, clip); QVERIFY(first.linkToModel(&second)); const auto count = Engine::getSong()->tracks().size();
		QVERIFY(!source->clone()); QCOMPARE(Engine::getSong()->tracks().size(), count); QVERIFY(first.isLinkedToModel(&second)); QVERIFY(Engine::getSong()->hasErrors());
	}

	void embeddedMidiPortSurvivesTrackPortPreservation()
	{
		QTemporaryDir directory; auto source = linked(); QVERIFY(source); auto midi = new MidiController(nullptr); midi->midiPort()->setInputChannel(16); midi->midiPort()->setInputController(74);
		source->volumeModel()->setControllerConnection(new ControllerConnection(midi)); const auto path = directory.filePath("midi.xpf");
		QVERIFY(StudioCapabilities::execute("preset.save", {{"track", 0}, {"path", path}}).value("ok").toBool());
		auto target = new InstrumentTrack(Engine::getSong()); target->midiPort()->setInputChannel(7);
		QVERIFY(StudioCapabilities::execute("preset.load", {{"track", 1}, {"path", path}}).value("ok").toBool());
		QVERIFY(target->volumeModel()->isLinkedToModel(target->panningModel())); const auto connection = target->volumeModel()->controllerConnection(); QVERIFY(connection);
		const auto copied = dynamic_cast<MidiController*>(connection->getController()); QVERIFY(copied); QCOMPARE(copied->midiPort()->inputChannel(), 16); QCOMPARE(copied->midiPort()->inputController(), 74);
		QCOMPARE(target->midiPort()->inputChannel(), 7); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void sharedRackGroupsOutsideTrackScopeDoNotPreventPortableSave()
	{
		auto source = linked(); QVERIFY(source); auto controller = new LfoController(Engine::getSong()); Engine::getSong()->addController(controller);
		const auto fields = controller->findChildren<AutomatableModel*>(); QVERIFY(fields.size() >= 2); QVERIFY(fields.at(0)->linkToModel(fields.at(1)));
		source->volumeModel()->setControllerConnection(new ControllerConnection(controller)); DataFile document(DataFile::Type::InstrumentTrackSettings); source->savePreset(document, document.content());
		QVERIFY2(document.serializationError().isEmpty(), qPrintable(document.serializationError())); QVERIFY(!document.content().firstChildElement("nativeLinkedControls").isNull());
	}

	void scopedCloneAndPresetLoadsSurviveRuntimeHistoryAndFailureRollback()
	{
		QTemporaryDir directory; StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(linked());
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		QVERIFY(runtime.process(request("clone", "track.clone", {{"track", 0}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t{2});
		QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t{1});
		QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t{2});
		auto first = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)), second = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(1));
		QVERIFY(second->volumeModel()->isLinkedToModel(second->panningModel())); QVERIFY(!second->volumeModel()->isLinkedToModel(first->volumeModel()));
		const auto path = directory.filePath("broken.xpf"); QVERIFY(runtime.process(request("save", "preset.save", {{"track", 0}, {"path", path}})).value("ok").toBool());
		QDomDocument document; QVERIFY(document.setContent(read(path))); document.documentElement().firstChildElement("instrumenttracksettings").firstChildElement("nativeLinkedControls").setAttribute("version", 99);
		QVERIFY(write(path, document.toByteArray())); const auto rejected = runtime.process(request("bad-load", "preset.load", {{"track", 1}, {"path", path}})); QVERIFY(!rejected.value("ok").toBool());
		second = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(1)); QVERIFY(second->volumeModel()->isLinkedToModel(second->panningModel())); QVERIFY(!runtime.stopping()); QVERIFY(!Engine::getSong()->hasErrors());
	}
};
QTEST_GUILESS_MAIN(LinkedTrackPersistenceTest)
#include "LinkedTrackPersistenceTest.moc"
