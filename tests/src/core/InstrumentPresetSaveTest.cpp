/* Native preset serialization and atomic publication. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <functional>
#include <stdexcept>
#include <utility>
#include "AudioEngine.h"
#include "Clip.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "DummyEffect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "MidiClient.h"
#include "NativeTrackSaveSnapshot.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "SerializingObject.h"
#include "Song.h"
#include "NativeSongTestAccess.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace lmms
{
class StudioMidiLearnTestAccess
{
public:
	static MidiClient* exchange(MidiClient* client) { return std::exchange(Engine::audioEngine()->m_midiClient, client); }
};
}
namespace
{
const Plugin::Descriptor fixtureDescriptor{"preset-save-fixture", "Preset save fixture", "Native export fixture", "LMMS tests", 1, Plugin::Type::Instrument, nullptr, "", nullptr};
class SavingInstrument final : public Instrument
{
public:
	explicit SavingInstrument(InstrumentTrack* target) : Instrument(target, &fixtureDescriptor) { }
	FloatModel gain{.25f, 0, 1, .01f, this, "Gain"};
	std::function<void()> onSave;
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
	QString nodeName() const override { return "preset-save-fixture"; }
	void saveSettings(QDomDocument& document, QDomElement& element) override
	{ const auto callback = onSave; const QPointer<SavingInstrument> owner(this); gain.saveSettings(document, element, "gain"); if (owner && callback) { callback(); } }
	void loadSettings(const QDomElement& element) override { gain.loadSettings(element, "gain"); }
};
class SavingTrack final : public InstrumentTrack
{
public:
	SavingTrack() : InstrumentTrack(Engine::getSong()) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new SavingInstrument(this); }
};
class RawInput final : public MidiClient
{
public:
	bool raw = true;
	int subscriptions = 0;
	bool isRaw() const override { return raw; }
	QStringList readablePorts() const override { return raw ? QStringList{} : QStringList{"fixture-input", "Manual"}; }
	void subscribeReadablePort(MidiPort*, const QString&, bool) override { ++subscriptions; }
	void processOutEvent(const MidiEvent&, const TimePos&, const MidiPort*) override { }
};
class FailingPortHook final : public SerializingObjectHook
{
public:
	void saveSettings(QDomDocument&, QDomElement&) override { throw std::runtime_error("fixture MIDI port serializer"); }
	void loadSettings(const QDomElement&) override { }
};
bool write(const QString& path, const QByteArray& bytes)
{ QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
}
class InstrumentPresetSaveTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	SavingTrack* seed() { auto target = new SavingTrack; target->loadInstrument("preset-save-fixture"); target->volumeModel()->setValue(42); return target; }
	SavingInstrument* plugin(SavingTrack* target) const { return static_cast<SavingInstrument*>(target->instrument()); }
	QJsonObject save(const QString& path, int index = 0, const std::function<bool()>& cancelled = {}) const { return StudioCapabilities::execute("preset.save", {{"track", index}, {"path", path}}, cancelled); }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); ConfigManager::inst()->setValue("app", "disablebackup", "0"); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void committedPresetRetainsLiveStateAndNormalizesPortableFlags()
	{
		auto target = seed(); target->setMuted(true); target->setMutedBeforeSolo(true); const auto id = target->volumeModel()->id(); QTemporaryDir directory; const auto path = directory.filePath("saved.xpf"); QVERIFY(write(path, "old")); NativeSongTestAccess::setModified(Engine::getSong(), false);
		const auto response = save(path); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QVERIFY(result.value("committed").toBool()); QCOMPARE(result.value("path").toString(), path); QCOMPARE(read(path + ".bak"), QByteArray("old"));
		DataFile file(path); QCOMPARE(file.content().attribute("muted"), QString("0")); QCOMPARE(file.content().attribute("solo"), QString("0")); QCOMPARE(file.content().attribute("mutedBeforeSolo"), QString("0")); QVERIFY(file.content().firstChildElement("instrumenttrack").firstChildElement("midiport").isNull());
		QVERIFY(target->isMutedByModel()); QVERIFY(target->isMutedBeforeSolo()); QCOMPARE(target->volumeModel()->id(), id); QCOMPARE(target->volumeModel()->storedValue(), 42.f); QVERIFY(!Engine::getSong()->isModified());
	}
	void serializationRetirementPreservesTheExistingArtifact_data()
	{ QTest::addColumn<bool>("track"); QTest::newRow("track") << true; QTest::newRow("instrument") << false; }
	void serializationRetirementPreservesTheExistingArtifact()
	{
		QFETCH(bool, track); auto target = seed(); const QPointer<InstrumentTrack> owner(target); QTemporaryDir directory; const auto path = directory.filePath("saved.xpf"); QVERIFY(write(path, "old")); bool invoked = false;
		plugin(target)->onSave = [&] { invoked = true; if (track) { delete target; } else { delete target->instrument(); } };
		const auto response = save(path); QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QCOMPARE(read(path), QByteArray("old")); if (owner) { QVERIFY(!owner->isReplacingInstrument()); QVERIFY(!owner->instrument()); }
	}
	void changedNativeSourceCannotPublishMixedPresetData_data()
	{ QTest::addColumn<int>("action"); QTest::newRow("earlier-value") << 0; QTest::newRow("plugin-value") << 1; QTest::newRow("step") << 2; QTest::newRow("input-use") << 3; QTest::newRow("links") << 4; QTest::newRow("name") << 5; QTest::newRow("new-control") << 6; QTest::newRow("effects") << 7; }
	void changedNativeSourceCannotPublishMixedPresetData()
	{
		QFETCH(int, action); auto target = seed(); QTemporaryDir directory; const auto path = directory.filePath("saved.xpf"); QVERIFY(write(path, "old")); bool invoked = false;
		if (action == 3) { target->volumeModel()->setControllerConnection(new ControllerConnection(nullptr)); }
		plugin(target)->onSave = [&] {
			if (invoked) { return; } invoked = true;
			if (action == 0) { target->volumeModel()->setValue(65); } if (action == 1) { plugin(target)->gain.setValue(.7f); } if (action == 2) { plugin(target)->gain.setStep(.1f); } if (action == 3) { target->volumeModel()->setUseControllerValue(false); }
			if (action == 4) { target->volumeModel()->linkToModel(target->panningModel()); } if (action == 5) { target->setName("Changed during save"); } if (action == 6) { new FloatModel(1, 0, 2, 0, plugin(target), "New control"); }
			if (action == 7) { QDomDocument document; auto node = document.createElement("effect"); node.setAttribute("name", "missing"); QVERIFY(target->audioBusHandle()->effects()->appendEffect(new DummyEffect(target->audioBusHandle()->effects(), node))); }
		};
		const auto response = save(path); plugin(target)->onSave = {}; QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QCOMPARE(read(path), QByteArray("old")); QVERIFY(!target->isReplacingInstrument());
	}
	void serializerExceptionsReleaseAdmissionAndKeepOldBytes_data()
	{ QTest::addColumn<bool>("standard"); QTest::newRow("standard") << true; QTest::newRow("nonstandard") << false; }
	void serializerExceptionsReleaseAdmissionAndKeepOldBytes()
	{
		QFETCH(bool, standard); auto target = seed(); QTemporaryDir directory; const auto path = directory.filePath("saved.xpf"); QVERIFY(write(path, "old")); plugin(target)->onSave = [&] { if (standard) { throw std::runtime_error("fixture preset save"); } throw 7; };
		QVERIFY(!save(path).value("ok").toBool()); plugin(target)->onSave = {}; QCOMPARE(read(path), QByteArray("old")); QVERIFY(!target->isReplacingInstrument()); QVERIFY(save(path).value("ok").toBool());
	}
	void serializationRefusesNestedSaveRestoreAndInstrumentReplacement()
	{
		auto target = seed(); bool invoked = false; plugin(target)->onSave = [&] {
			invoked = true; DataFile file(DataFile::Type::InstrumentTrackSettings); QVERIFY(!target->loadInstrument("nested")); QVERIFY_EXCEPTION_THROWN(target->savePreset(file, file.content()), std::logic_error);
			QVERIFY_EXCEPTION_THROWN(target->loadPreset(file.content()), std::logic_error); QVERIFY_EXCEPTION_THROWN(target->replaceInstrument(file), std::logic_error); QVERIFY_EXCEPTION_THROWN(target->loadTrackSpecificSettings(file.content()), std::logic_error);
		}; QTemporaryDir directory; const auto response = save(directory.filePath("saved.xpf")); plugin(target)->onSave = {}; QVERIFY(invoked); QVERIFY(response.value("ok").toBool());
	}
	void fullTrackSaveChecksClipLifetimeWhilePresetsOmitClips_data()
	{ QTest::addColumn<bool>("full"); QTest::newRow("track") << true; QTest::newRow("preset") << false; }
	void fullTrackSaveChecksClipLifetimeWhilePresetsOmitClips()
	{
		QFETCH(bool, full); auto target = seed(); auto clip = target->createClip(0); plugin(target)->onSave = [&] { delete clip; };
		DataFile file(DataFile::Type::InstrumentTrackSettings); bool caught = false; try { if (full) { target->saveSettings(file, file.content()); } else { target->savePreset(file, file.content()); } } catch (const std::exception&) { caught = true; }
		plugin(target)->onSave = {}; QCOMPARE(caught, full); QVERIFY(!target->isReplacingInstrument()); QVERIFY(target->getClips().empty());
	}
	void interpolationReadsDoNotChangeTheSerializedStoredValue()
	{
		auto target = seed(); plugin(target)->gain.setValue(.6f); plugin(target)->onSave = [&] { plugin(target)->gain.valueBuffer(); }; QTemporaryDir directory;
		const auto response = save(directory.filePath("saved.xpf")); plugin(target)->onSave = {}; QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(plugin(target)->gain.storedValue(), .6f);
	}
	void atomicWriterValidationCanRefuseBothPublicationPhases_data()
	{ QTest::addColumn<bool>("late"); QTest::addColumn<bool>("standard"); QTest::newRow("early-standard") << false << true; QTest::newRow("early-nonstandard") << false << false; QTest::newRow("late-standard") << true << true; QTest::newRow("late-nonstandard") << true << false; }
	void atomicWriterValidationCanRefuseBothPublicationPhases()
	{
		QFETCH(bool, late); QFETCH(bool, standard); ConfigManager::inst()->setValue("app", "disablebackup", "1"); QTemporaryDir directory; const auto path = directory.filePath("saved.xpf"); QVERIFY(write(path, "old")); DataFile file(DataFile::Type::InstrumentTrackSettings); int calls = 0; bool caught = false;
		try { file.writeFile(path, false, [&] { if (++calls == (late ? 2 : 1)) { if (standard) { throw std::runtime_error("fixture precommit validation"); } throw 7; } }); } catch (...) { caught = true; }
		QVERIFY(caught); QCOMPARE(calls, late ? 2 : 1); QCOMPARE(read(path), QByteArray("old"));
	}
	void finalWorkCallbackCannotPublishRetiredChangedOrCancelledSource_data()
	{ QTest::addColumn<int>("action"); QTest::newRow("value") << 0; QTest::newRow("owner") << 1; QTest::newRow("cancel") << 2; QTest::newRow("nonstandard") << 3; }
	void finalWorkCallbackCannotPublishRetiredChangedOrCancelledSource()
	{
		QFETCH(int, action); auto target = seed(); QTemporaryDir directory; const auto path = directory.filePath("saved.xpf"); QVERIFY(write(path, "old")); bool invoked = false;
		const auto response = save(path, 0, [&] {
			if (!invoked && QFile::exists(path + ".bak")) { invoked = true; if (action == 0) { target->volumeModel()->setValue(65); } if (action == 1) { delete target; } if (action == 3) { throw 7; } }
			return invoked && action == 2;
		});
		QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QCOMPARE(read(path), QByteArray("old")); QCOMPARE(read(path + ".bak"), QByteArray("old"));
	}
	void presetSerializationDoesNotToggleLiveAutomaticInput()
	{
		RawInput input; auto previous = StudioMidiLearnTestAccess::exchange(&input); ConfigManager::inst()->setValue("midi", "midiautoassign", "fixture-input");
		const auto restore = qScopeGuard([&] { Engine::getSong()->clearProject(); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); StudioMidiLearnTestAccess::exchange(previous); });
		auto target = seed(); target->autoAssignMidiDevice(true); QVERIFY(target->midiPort()->isReadable()); QSignalSpy changes(target->midiPort(), &MidiPort::modeChanged); QTemporaryDir directory;
		QVERIFY(save(directory.filePath("saved.xpf")).value("ok").toBool()); QCOMPARE(changes.count(), 0); QVERIFY(target->midiPort()->isReadable());
	}
	void fullTrackPortFailureRestoresTheAutomaticAssignment()
	{
		RawInput input; auto previous = StudioMidiLearnTestAccess::exchange(&input); ConfigManager::inst()->setValue("midi", "midiautoassign", "fixture-input");
		const auto restore = qScopeGuard([&] { Engine::getSong()->clearProject(); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); StudioMidiLearnTestAccess::exchange(previous); });
		auto target = seed(); target->autoAssignMidiDevice(true); FailingPortHook hook; target->midiPort()->setHook(&hook); const auto detach = qScopeGuard([&] { target->midiPort()->setHook(nullptr); }); DataFile file(DataFile::Type::InstrumentTrackSettings);
		QVERIFY_EXCEPTION_THROWN(target->saveSettings(file, file.content()), std::runtime_error); QVERIFY(target->midiPort()->isReadable()); QVERIFY(!target->isReplacingInstrument()); target->midiPort()->setHook(nullptr);
		DataFile retry(DataFile::Type::InstrumentTrackSettings); target->saveSettings(retry, retry.content()); QVERIFY(target->midiPort()->isReadable());
	}
	void fullTrackSerializationPreservesCurrentMidiInput_data()
	{
		QTest::addColumn<bool>("raw"); QTest::addColumn<int>("state"); QTest::addColumn<bool>("checkpoint");
		for (bool checkpoint : {false, true}) for (bool raw : {false, true}) for (int state = 0; state < (raw ? 3 : 4); ++state)
		{ QTest::newRow(qPrintable(QString(checkpoint ? "checkpoint-" : "track-") + (raw ? "raw" : "named") + QString::number(state))) << raw << state << checkpoint; }
	}
	void fullTrackSerializationPreservesCurrentMidiInput()
	{
		QFETCH(bool, raw); QFETCH(int, state); QFETCH(bool, checkpoint); RawInput input; input.raw = raw;
		auto previous = StudioMidiLearnTestAccess::exchange(&input);
		ConfigManager::inst()->setValue("midi", "midiautoassign", "fixture-input");
		const auto restore = qScopeGuard([&] { Engine::getSong()->clearProject(); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); StudioMidiLearnTestAccess::exchange(previous); });
		auto target = seed(); target->autoAssignMidiDevice(true); auto port = target->midiPort();
		if (state == 0) { if (raw) { port->setReadable(false); } else { port->subscribeReadablePort("fixture-input", false); } }
		if (state == 1) { target->autoAssignMidiDevice(false); if (raw) { port->setReadable(true); } else { port->subscribeReadablePort("Manual"); } }
		if (state == 3) { port->setReadable(false); }
		const bool readable = port->isReadable(); const auto mode = port->mode(); const auto requested = port->requestedReadablePorts();
		QSignalSpy changes(port, &MidiPort::modeChanged); input.subscriptions = 0;
		QDomElement saved;
		if (checkpoint)
		{
			QTemporaryDir directory; QJsonArray assets; const auto path = directory.filePath("checkpoint.mmp");
			QVERIFY2(Engine::getSong()->saveProjectFile(path, false, true, &assets), qPrintable(Engine::getSong()->errorSummary()));
			DataFile file(path); saved = file.elementsByTagName("midiport").at(0).toElement();
		}
		else
		{
			DataFile file(DataFile::Type::InstrumentTrackSettings); target->saveSettings(file, file.content());
			saved = file.content().firstChildElement("instrumenttrack").firstChildElement("midiport");
		}
		QCOMPARE(port->isReadable(), readable); QCOMPARE(port->mode(), mode); QCOMPARE(port->requestedReadablePorts(), requested);
		if (checkpoint || state != 2) { QCOMPARE(changes.count(), 0); QCOMPARE(input.subscriptions, 0); }
		QVERIFY(!saved.isNull());
		if (raw) { QCOMPARE(saved.attribute("readable").toInt(), !checkpoint && state == 2 ? 0 : int(readable)); }
		else
		{
			QStringList names; const auto subscriptions = saved.firstChildElement("midisubscriptions");
			for (auto item = subscriptions.firstChildElement("input"); !item.isNull(); item = item.nextSiblingElement("input")) { names.append(item.attribute("name")); }
			QCOMPARE(names, state == 1 ? QStringList{"Manual"} : checkpoint && state == 2 ? QStringList{"fixture-input"} : QStringList{});
		}
	}
	void recordingAndIndexChangesRetainTheSelectedExportIdentity()
	{
		auto earlier = new InstrumentTrack(Engine::getSong()); auto target = seed(); int capture = 0; QVERIFY(target->beginNoteRecording(&capture)); const auto release = qScopeGuard([&] { target->endNoteRecording(&capture); }); bool invoked = false;
		plugin(target)->onSave = [&] { if (!invoked) { invoked = true; delete earlier; } }; QTemporaryDir directory; const auto response = save(directory.filePath("saved.xpf"), 1); plugin(target)->onSave = {};
		QVERIFY(invoked); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(response.value("result").toObject().value("track").toInt(), 0);
	}
	void runtimeReplayDoesNotRewriteAnExportedPreset()
	{
		QTemporaryDir directory; QString error; QJsonObject command, receipt; const auto path = directory.filePath("saved.xpf");
		{ StudioRuntime runtime(directory.filePath("session")); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool()); QVERIFY(runtime.process(request("instrument", "instrument.load", {{"track", 0}, {"instrument", "tripleoscillator"}})).value("ok").toBool());
			command = request("save", "preset.save", {{"track", 0}, {"path", path}}); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); QVERIFY(receipt.value("result").toObject().value("committed").toBool()); QVERIFY(write(path, "external change")); auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); }
		StudioRuntime reopened(directory.filePath("session")); QVERIFY(reopened.open(error)); auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); QCOMPARE(read(path), QByteArray("external change"));
	}
};
QTEST_GUILESS_MAIN(InstrumentPresetSaveTest)
#include "InstrumentPresetSaveTest.moc"
