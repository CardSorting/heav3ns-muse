/* Native controller serializers, resources and returned lifetimes. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDataStream>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <cmath>
#include <functional>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "LfoController.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "Oscillator.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "NativeSongTestAccess.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
class Lfo final : public LfoController
{
public:
	explicit Lfo(Model* parent = nullptr) : LfoController(parent) { }
	std::function<void()> onRename;
	void setName(const QString& name) override { Controller::setName(name); const auto callback = onRename; if (callback) { callback(); } }
	AutomatableModel* field(int index) { switch (index) { case 0: return &m_baseModel; case 1: return &m_speedModel; case 2: return &m_amountModel; case 3: return &m_phaseModel; case 4: return &m_waveModel; default: return &m_multiplierModel; } }
};
class Midi final : public MidiController
{
public:
	explicit Midi(Model* parent = nullptr) : MidiController(parent) { }
	std::function<void()> onRename;
	void setName(const QString& name) override { Controller::setName(name); const auto callback = onRename; if (callback) { callback(); } }
};
QDomElement settings(QDomDocument& document, Controller::ControllerType type = Controller::ControllerType::Lfo)
{
	auto node = document.createElement("controller"); node.setAttribute("type", static_cast<int>(type)); node.setAttribute("name", "Restored native controller");
	node.setAttribute("base", .2); node.setAttribute("speed", 3); node.setAttribute("amount", .4); node.setAttribute("phase", 48); node.setAttribute("wave", 1); node.setAttribute("multiplier", 1); document.appendChild(node); return node;
}
bool wave(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::WriteOnly)) { return false; } QDataStream stream(&file); stream.setByteOrder(QDataStream::LittleEndian);
	stream.writeRawData("RIFF", 4); stream << quint32(36 + 128); stream.writeRawData("WAVEfmt ", 8); stream << quint32(16) << quint16(1) << quint16(1) << quint32(44100) << quint32(88200) << quint16(2) << quint16(16); stream.writeRawData("data", 4); stream << quint32(128);
	for (int index = 0; index < 64; ++index) { stream << qint16(8192); } return stream.status() == QDataStream::Ok;
}
[[noreturn]] void raise(bool nonstandard) { if (nonstandard) { throw 17; } throw std::runtime_error("fixture controller settings failure"); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
}
class ControllerSettingsLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	static void fields() { QTest::addColumn<int>("field"); QTest::newRow("base") << 0; QTest::newRow("speed") << 1; QTest::newRow("amount") << 2; QTest::newRow("phase") << 3; QTest::newRow("wave") << 4; QTest::newRow("multiplier") << 5; }
	static void exceptions() { QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void nativeLfoSettingsRoundTripRetainsModelsWaveAndFiniteSamples()
	{
		QTemporaryDir directory; const auto path = directory.filePath("wave.wav"); QVERIFY(wave(path)); Lfo original; QString error; QVERIFY(original.setUserWave(path, error)); original.waveModel().setValue(static_cast<int>(Oscillator::WaveShape::UserDefined)); original.field(0)->setValue(.3f); QDomDocument document; auto node = document.createElement("controller"); document.appendChild(node); original.saveSettings(document, node);
		Lfo restored; restored.loadSettings(node); QCOMPARE(restored.field(0)->storedValue(), original.field(0)->storedValue()); QCOMPARE(restored.userWave().toBase64(), original.userWave().toBase64()); QVERIFY(!restored.userWave().empty()); Controller::triggerFrameCounter(); QVERIFY(std::isfinite(restored.currentValue(0))); for (float value : *restored.valueBuffer()) { QVERIFY(std::isfinite(value)); }
	}
	void loadFieldNotificationsCanRetireTheController_data() { fields(); }
	void loadFieldNotificationsCanRetireTheController()
	{
		QFETCH(int, field); auto target = new Lfo; const QPointer<Lfo> owner(target); bool invoked = false; connect(target->field(field), &AutomatableModel::dataChanged, this, [&] { if (!invoked) { invoked = true; delete target; } }); QDomDocument document; const auto node = settings(document);
		QVERIFY_EXCEPTION_THROWN(target->loadSettings(node), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner);
	}
	void saveFieldWorkCanRetireTheController_data() { fields(); }
	void saveFieldWorkCanRetireTheController()
	{
		QFETCH(int, field); const char* keys[] = {"base", "speed", "amount", "phase", "wave", "multiplier"}; auto target = new Lfo; const QPointer<Lfo> owner(target); QDomDocument document; auto node = document.createElement("controller"); document.appendChild(node); bool invoked = false;
		{ FileResourceIO::Scope work([&] { if (!invoked && node.hasAttribute(keys[field])) { invoked = true; delete target; } return false; }); QVERIFY_EXCEPTION_THROWN(target->saveSettings(document, node), std::runtime_error); }
		QVERIFY(invoked); QVERIFY(!owner);
	}
	void baseRenameRetirementStopsBothNativeDerivedLoaders_data()
	{ QTest::addColumn<bool>("midi"); QTest::newRow("lfo") << false; QTest::newRow("midi-before-port") << true; }
	void baseRenameRetirementStopsBothNativeDerivedLoaders()
	{
		QFETCH(bool, midi); Controller* target = nullptr; if (midi) { auto native = new Midi; target = native; native->onRename = [native] { delete native; }; } else { auto native = new Lfo; target = native; native->onRename = [native] { delete native; }; }
		const QPointer<Controller> owner(target); QDomDocument document; const auto node = settings(document, midi ? Controller::ControllerType::Midi : Controller::ControllerType::Lfo); QVERIFY_EXCEPTION_THROWN(target->loadSettings(node), std::runtime_error); QVERIFY(!owner);
	}
	void nestedSettingsAndWaveformReplacementAreRefused()
	{
		Lfo target; QDomDocument document; const auto node = settings(document); bool invoked = false;
		connect(target.field(0), &AutomatableModel::dataChanged, this, [&] { if (invoked) { return; } invoked = true; QVERIFY_EXCEPTION_THROWN(target.loadSettings(node), std::logic_error); QDomDocument saved; auto element = saved.createElement("controller"); QVERIFY_EXCEPTION_THROWN(target.saveSettings(saved, element), std::logic_error); QString error; QVERIFY_EXCEPTION_THROWN(target.setUserWave({}, error), std::logic_error); });
		target.loadSettings(node); QVERIFY(invoked); QString error; QVERIFY(target.setUserWave({}, error));
	}
	void callbackExceptionsReleaseSettingsAdmission_data() { exceptions(); }
	void callbackExceptionsReleaseSettingsAdmission()
	{
		QFETCH(bool, nonstandard); Lfo target; bool armed = true; connect(target.field(0), &AutomatableModel::dataChanged, this, [&] { if (armed) { armed = false; raise(nonstandard); } }); QDomDocument document; const auto node = settings(document); bool caught = false; try { target.loadSettings(node); } catch (...) { caught = true; }
		QVERIFY(caught); QVERIFY(!armed); target.loadSettings(node); QDomDocument saved; auto element = saved.createElement("controller"); target.saveSettings(saved, element); QString error; QVERIFY(target.setUserWave({}, error));
	}
	void waveformDecodeAndNotificationRetirementAreObserved_data()
	{ QTest::addColumn<bool>("decode"); QTest::newRow("decode-work") << true; QTest::newRow("publication-notification") << false; }
	void waveformDecodeAndNotificationRetirementAreObserved()
	{
		QFETCH(bool, decode); QTemporaryDir directory; const auto path = directory.filePath("wave.wav"); QVERIFY(wave(path)); auto target = new Lfo; const QPointer<Lfo> owner(target); bool invoked = false; QString error;
		if (decode) { FileResourceIO::Scope work([&] { if (!invoked) { invoked = true; delete target; } return false; }); QVERIFY_EXCEPTION_THROWN(target->setUserWave(path, error), std::runtime_error); }
		else { connect(target, &Model::dataChanged, this, [&] { invoked = true; delete target; }); QVERIFY_EXCEPTION_THROWN(target->setUserWave(path, error), std::runtime_error); }
		QVERIFY(invoked); QVERIFY(!owner);
	}
	void failedWaveformDecodeRetainsThePriorNativeBuffer()
	{
		QTemporaryDir directory; const auto path = directory.filePath("wave.wav"); QVERIFY(wave(path)); Lfo target; QString error; QVERIFY(target.setUserWave(path, error)); const auto before = &target.userWave(); const auto pcm = target.userWave().toBase64(); QVERIFY(!target.setUserWave(directory.filePath("missing.wav"), error)); QVERIFY(!error.isEmpty()); QCOMPARE(&target.userWave(), before); QCOMPARE(target.userWave().toBase64(), pcm); QVERIFY(target.setUserWave({}, error)); QVERIFY(target.userWave().empty());
	}
	void factoryFailureDeletesOnlyItsUnadoptedNativeController_data() { exceptions(); }
	void factoryFailureDeletesOnlyItsUnadoptedNativeController()
	{
		QFETCH(bool, nonstandard); Model parent(nullptr); QPointer<LfoController> observed; QDomDocument document; const auto node = settings(document); bool caught = false;
		{ FileResourceIO::Scope work([&] { if (!observed) { observed = parent.findChild<LfoController*>(); } if (observed) { raise(nonstandard); } return false; }); try { Controller::create(node, &parent); } catch (...) { caught = true; } }
		QVERIFY(caught); QVERIFY(!observed); QVERIFY(parent.findChildren<LfoController*>().empty()); auto retry = Controller::create(node, &parent); QVERIFY(retry); delete retry;
	}
	void factoryAdoptionSurvivesAChangedNativeCallback()
	{
		Model parent(nullptr); QPointer<LfoController> observed; QDomDocument document; const auto node = settings(document); bool caught = false;
		{ FileResourceIO::Scope work([&] { if (!observed) { observed = parent.findChild<LfoController*>(); } if (observed) { Engine::getSong()->addController(observed.data()); throw std::runtime_error("fixture adopted candidate"); } return false; }); try { Controller::create(node, &parent); } catch (...) { caught = true; } }
		QVERIFY(caught); QVERIFY(observed); QVERIFY(Engine::getSong()->containsController(observed.data())); Engine::getSong()->removeController(observed.data()); QVERIFY(!observed);
	}
	void sharedDummyIsNeverDeletedByFactoryFailure()
	{
		auto dummy = Controller::create(Controller::ControllerType::Dummy, nullptr); const QPointer<Controller> owner(dummy); const auto name = dummy->name(); const auto restore = qScopeGuard([&] { if (owner) { owner->setName(name); } }); QDomDocument document; const auto node = settings(document, Controller::ControllerType::Dummy);
		{ FileResourceIO::Scope work([]() -> bool { throw std::runtime_error("fixture shared dummy"); }); QVERIFY_EXCEPTION_THROWN(Controller::create(node, nullptr), std::runtime_error); }
		QVERIFY(owner); QCOMPARE(Controller::create(Controller::ControllerType::Dummy, nullptr), dummy);
	}
	void factoryParentRetirementPreservesAnExplicitlyDetachedCandidate()
	{
		auto parent = new Model(nullptr); const QPointer<Model> owner(parent); QPointer<LfoController> observed; QDomDocument document; const auto node = settings(document);
		{ FileResourceIO::Scope work([&] { if (owner && !observed) { observed = owner->findChild<LfoController*>(); if (observed) { observed->setParent(nullptr); delete parent; } } return false; }); QVERIFY_EXCEPTION_THROWN(Controller::create(node, parent), std::runtime_error); }
		QVERIFY(!owner); QVERIFY(observed); QVERIFY(!observed->parent()); delete observed.data();
	}
	void returnedMidiPortFailureReleasesControllerAdmission()
	{
		Midi target; QDomDocument document; auto node = settings(document, Controller::ControllerType::Midi); node.setAttribute("outputsource", 9); QVERIFY_EXCEPTION_THROWN(target.loadSettings(node), std::invalid_argument); node.setAttribute("outputsource", 0); target.loadSettings(node); QDomDocument saved; auto element = saved.createElement("controller"); target.saveSettings(saved, element);
	}
	void waveformCapabilityRejectsSelectionAndDirtyNotificationRetirement_data()
	{ QTest::addColumn<bool>("dirty"); QTest::newRow("selection") << false; QTest::newRow("dirty-notification") << true; }
	void waveformCapabilityRejectsSelectionAndDirtyNotificationRetirement()
	{
		QFETCH(bool, dirty); auto song = Engine::getSong(); auto target = new Lfo(song); song->addController(target); NativeSongTestAccess::setModified(song, false); bool invoked = false; QMetaObject::Connection connection;
		if (dirty) { connection = connect(song, &Song::modified, this, [&] { invoked = true; song->removeController(target); }); } else { connection = connect(&target->waveModel(), &AutomatableModel::dataChanged, this, [&] { invoked = true; song->removeController(target); }); }
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto response = StudioCapabilities::execute("controller.waveform.set", {{"controller", 0}, {"path", ""}, {"select", !dirty}}); QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QVERIFY(song->controllers().empty());
	}
	void laterRackRegistrationCannotRetireAnEarlierRestoredController_data()
	{ QTest::addColumn<bool>("earlier"); QTest::newRow("earlier") << true; QTest::newRow("new-target") << false; }
	void laterRackRegistrationCannotRetireAnEarlierRestoredController()
	{
		QFETCH(bool, earlier); QTemporaryDir directory; QTemporaryDir input; const auto path = input.filePath("incoming.mmp"); DataFile file(DataFile::Type::SongProject); auto rack = file.createElement("controllers"); file.content().appendChild(rack);
		for (const auto name : {"Loaded A", "Loaded B"}) { auto node = file.createElement("lfocontroller"); node.setAttribute("type", static_cast<int>(Controller::ControllerType::Lfo)); node.setAttribute("name", name); rack.appendChild(node); } QVERIFY(file.writeFile(path));
		StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("baseline", "controller.add", {{"type", "lfo"}, {"name", "Keep me"}})).value("ok").toBool()); const auto before = manifest(directory); bool invoked = false;
		const auto connection = connect(Engine::getSong(), &Song::controllerAdded, this, [&](Controller* target) { if (!invoked && target->name() == "Loaded B") { invoked = true; Engine::getSong()->removeController(earlier ? Engine::getSong()->controllers().front() : target); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!runtime.process(request("load", "project.load", {{"path", path}})).value("ok").toBool()); QVERIFY(invoked); QVERIFY(!runtime.stopping()); QCOMPARE(Engine::getSong()->controllers().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->controllers().front()->name(), QString("Keep me")); QCOMPARE(manifest(directory).value("history"), before.value("history"));
	}
	void runtimeRecoversWaveformPublicationFailureAndReplaysIt()
	{
		QTemporaryDir directory; QJsonObject receipt; const auto command = request("wave", "controller.waveform.set", {{"controller", 0}, {"path", ""}, {"select", true}});
		{ StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("baseline", "controller.add", {{"type", "lfo"}, {"name", "Keep me"}})).value("ok").toBool()); auto target = static_cast<LfoController*>(Engine::getSong()->controllers().front()); bool invoked = false;
			const auto connection = connect(target, &Model::dataChanged, this, [&] { invoked = true; Engine::getSong()->removeController(target); }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); receipt = runtime.process(command); QVERIFY(invoked); QVERIFY(!receipt.value("ok").toBool()); QVERIFY(!runtime.stopping()); QCOMPARE(Engine::getSong()->controllers().front()->name(), QString("Keep me")); auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); }
		StudioRuntime reopened(directory.path()); QString error; QVERIFY(reopened.open(error)); auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); QCOMPARE(Engine::getSong()->controllers().front()->name(), QString("Keep me"));
	}
};
QTEST_GUILESS_MAIN(ControllerSettingsLifecycleTest)
#include "ControllerSettingsLifecycleTest.moc"
