/* Native MIDI learn and transaction tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include "AudioEngine.h"
#include "ModelValueNotificationTestUtils.h"
#include "AutoDetectMidiController.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClient.h"
#include "NotePlayHandle.h"
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
class LearnInput final : public MidiClient
{
public:
	bool raw = true, observed = true, active = true, queued = true;
	QStringList inputs{"Knobs A", "Knobs, B"};
	std::vector<QPointer<MidiPort>> ports;
	bool isRaw() const override { return raw; }
	bool hasQueuedInputDelivery() const override { return queued; }
	InputBackendStatus inputBackendStatus() const override { return {observed, active, 0}; }
	QStringList readablePorts() const override { return raw ? QStringList{} : inputs; }
	void processOutEvent(const MidiEvent&, const TimePos&, const MidiPort*) override { }
	void addPort(MidiPort* port) override { MidiClient::addPort(port); ports.emplace_back(port); }
	void removePort(MidiPort* port) override { MidiClient::removePort(port); std::erase_if(ports, [port](const auto& candidate) { return !candidate || candidate == port; }); }
	AutoDetectMidiController* detector() const
	{
		for (const auto& port : ports) { if (port) { if (auto detector = dynamic_cast<AutoDetectMidiController*>(port->parent())) { return detector; } } }
		return nullptr;
	}
	void feed(int channel = 15, int control = 0, int value = 96, const QString& source = "Knobs, B", bool pump = true, bool forceSource = false)
	{
		const auto snapshot = ports;
		for (const auto& port : snapshot)
		{
			if (port && (raw || forceSource || port->requestedReadablePorts().contains(source)))
			{
				dispatchInputEvent(MidiEvent(MidiControlChange, channel, control, value), {}, port, raw ? QString() : source);
			}
		}
		if (pump) { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
	}
	void fail() { failInputEvents(InputDeliveryFailure::BackendInput); }
};
}

class StudioMidiLearnTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	LearnInput m_input;
	MidiClient* m_priorClient = nullptr;
	FloatModel* model() const { return static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->volumeModel(); }
	QJsonObject args() const { return {{"parameter", static_cast<int>(model()->id())}, {"timeoutMs", 1000}}; }
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& arguments = {}) const { return {{"id", id}, {"op", op}, {"args", arguments}}; }
	QJsonObject manifest(const QTemporaryDir& session) const
	{
		QFile file(session.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
	}
	void bindPrior()
	{
		QVERIFY(StudioCapabilities::execute("controller.add", {{"type", "lfo"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("controller.connect", {{"parameter", static_cast<int>(model()->id())}, {"controller", 0}}).value("ok").toBool());
	}
	QJsonObject runLearn(const std::function<bool()>& cancelled = {}, const std::function<void(AutoDetectMidiController*)>& beforeFeed = {})
	{
		bool fed = false; QTimer timer; timer.setInterval(1);
		connect(&timer, &QTimer::timeout, this, [&] {
			const QPointer<AutoDetectMidiController> active(m_input.detector()); if (fed || !active) { return; } fed = true;
			if (beforeFeed) { beforeFeed(active.data()); } if (active) { m_input.feed(); }
		});
		timer.start(); const auto response = StudioCapabilities::execute("controller.midi.learn", args(), cancelled); timer.stop(); return response;
	}
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->stop();
		Engine::getSong()->clearProject(); new InstrumentTrack(Engine::getSong()); model()->setValue(42);
		m_input.raw = m_input.observed = m_input.active = m_input.queued = true;
		m_input.inputs = QStringList{"Knobs A", "Knobs, B"}; m_priorClient = StudioMidiLearnTestAccess::exchange(&m_input);
	}
	void cleanup()
	{
		Engine::getSong()->stop(); Engine::getSong()->clearProject(); StudioMidiLearnTestAccess::exchange(m_priorClient); m_priorClient = nullptr;
		QVERIFY(m_input.ports.empty());
	}

	void sharedDetectorKeepsFirstOrLatestValidCcAndCopiesItsValue_data()
	{
		QTest::addColumn<bool>("firstOnly"); QTest::newRow("headless-first") << true; QTest::newRow("gui-latest") << false;
	}
	void sharedDetectorKeepsFirstOrLatestValidCcAndCopiesItsValue()
	{
		QFETCH(bool, firstOnly); AutoDetectMidiController detector(nullptr); detector.reset(firstOnly);
		detector.processInEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {});
		detector.processInEvent(MidiEvent(MidiControlChange, -1, 74, 100), {});
		detector.processInEvent(MidiEvent(MidiControlChange, 16, 74, 100), {});
		detector.processInEvent(MidiEvent(MidiControlChange, 0, -1, 100), {});
		detector.processInEvent(MidiEvent(MidiControlChange, 0, 128, 100), {});
		detector.processInEvent(MidiEvent(MidiControlChange, 0, 74, -1), {});
		detector.processInEvent(MidiEvent(MidiControlChange, 0, 74, 256), {});
		detector.processInEvent(MidiEvent(MidiControlChange, 0, 74, 128), {}); QVERIFY(!detector.detection().received);
		m_input.feed(); QCOMPARE(detector.detection().controller, 0); QCOMPARE(detector.detection().channel, 16);
		m_input.feed(2, 74, 127); QCOMPARE(detector.detection().controller, firstOnly ? 0 : 74);
		detector.useDetected(); const auto detected = detector.detection();
		std::unique_ptr<MidiController> copied(detector.copyToMidiController(nullptr, true));
		QCOMPARE(copied->midiPort()->inputChannel(), detected.channel); QCOMPARE(copied->midiPort()->inputController(), detected.controller);
		QCOMPARE(copied->currentValue(0), static_cast<float>(detected.value) / 127.f);
		detector.reset(); QVERIFY(!detector.detection().received); QCOMPARE(detector.midiPort()->inputController(), MidiController::NONE);
	}

	void malformedRawEventsCannotConsumeOrReplaceDetection_data()
	{
		QTest::addColumn<bool>("firstOnly"); QTest::addColumn<bool>("alreadyDetected");
		QTest::newRow("first-before-match") << true << false; QTest::newRow("first-after-match") << true << true;
		QTest::newRow("latest-before-match") << false << false; QTest::newRow("latest-after-match") << false << true;
	}
	void malformedRawEventsCannotConsumeOrReplaceDetection()
	{
		QFETCH(bool, firstOnly); QFETCH(bool, alreadyDetected); AutoDetectMidiController detector(nullptr); detector.reset(firstOnly);
		if (alreadyDetected) { m_input.feed(2, 74, 63); QVERIFY(detector.detection().received); }
		const auto before = detector.detection(); QSignalSpy changed(&detector, &Controller::valueChanged);
		for (const auto& event : std::vector<MidiEvent>{
			MidiEvent(MidiControlChange, 0, -1, 1), MidiEvent(MidiControlChange, 0, 128, 1), MidiEvent(MidiControlChange, 0, 256, 1),
			MidiEvent(MidiControlChange, 0, -128, 1), MidiEvent(MidiControlChange, 0, 32767, 1),
			MidiEvent(MidiControlChange, 0, 74, -1), MidiEvent(MidiControlChange, 0, 74, 128), MidiEvent(MidiControlChange, 0, 74, 256),
			MidiEvent(MidiControlChange, 0, 74, -256), MidiEvent(MidiControlChange, 0, 74, 32767),
			MidiEvent(MidiControlChange, -1, 74, 1), MidiEvent(MidiControlChange, 16, 74, 1), MidiEvent(MidiProgramChange, 0, 74, 1)})
		{ detector.processInEvent(event, {}); }
		QCOMPARE(changed.size(), 0); QCOMPARE(detector.detection().received, before.received); QCOMPARE(detector.detection().channel, before.channel);
		QCOMPARE(detector.detection().controller, before.controller); QCOMPARE(detector.detection().value, before.value); QCOMPARE(detector.detection().port, before.port);
		m_input.feed(3, 75, 127); const auto detected = detector.detection(); QVERIFY(detected.received);
		const bool retainedFirst = firstOnly && alreadyDetected;
		QCOMPARE(detected.channel, retainedFirst ? 3 : 4); QCOMPARE(detected.controller, retainedFirst ? 74 : 75); QCOMPARE(detected.value, retainedFirst ? 63 : 127);
		detector.useDetected(); std::unique_ptr<MidiController> copied(detector.copyToMidiController(nullptr, true));
		QCOMPARE(copied->midiPort()->inputChannel(), detected.channel); QCOMPARE(copied->midiPort()->inputController(), detected.controller);
		QCOMPARE(copied->currentValue(0), float(detected.value) / 127.f);
	}

	void nativeLearningFiltersInputsAndPublishesOnlyTheFirstMatch_data()
	{
		QTest::addColumn<bool>("named"); QTest::addColumn<bool>("filtered");
		QTest::newRow("raw-omni") << false << false; QTest::newRow("raw-channel") << false << true;
		QTest::newRow("named-all") << true << false; QTest::newRow("named-specific") << true << true;
	}
	void nativeLearningFiltersInputsAndPublishesOnlyTheFirstMatch()
	{
		QFETCH(bool, named); QFETCH(bool, filtered); m_input.raw = !named; bindPrior();
		const QPointer<ControllerConnection> prior(model()->controllerConnection()); auto arguments = args();
		if (filtered) { arguments.insert("inputChannel", 16); if (named) { arguments.insert("port", "Knobs, B"); } }
		bool fed = false, preserved = true; QTimer timer; timer.setInterval(1); QObject::connect(&timer, &QTimer::timeout, this, [&] {
			if (fed || !m_input.detector()) { return; } fed = true;
			if (filtered) { m_input.feed(1, 74, 1, "Knobs A"); }
			m_input.feed(); m_input.feed(15, 75, 0); preserved = model()->controllerConnection() == prior.data();
		}); timer.start(); const auto response = StudioCapabilities::execute("controller.midi.learn", arguments); timer.stop();
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(fed); QVERIFY(preserved); QVERIFY(!prior);
		const auto result = response.value("result").toObject(); QCOMPARE(result.value("inputController").toInt(), 0); QCOMPARE(result.value("inputChannel").toInt(), 16);
		QCOMPARE(result.value("learnedValue").toInt(), 96); QVERIFY(!result.value("connectionVerified").toBool());
		const auto midi = dynamic_cast<MidiController*>(model()->controllerConnection()->getController()); QVERIFY(midi);
		QCOMPARE(midi->currentValue(0), 96.f / 127.f); QCOMPARE(model()->storedValue(), 42.f); QVERIFY(!m_input.detector());
		QCOMPARE(m_input.ports.size(), std::size_t(1)); QCOMPARE(Engine::getSong()->controllers().front()->connectionCount(), 0);
		QCOMPARE(midi->midiPort()->requestedReadablePorts(), named ? QStringList{"Knobs, B"} : QStringList{});
	}

	void learningFailureKeepsThePriorBindingAndRetiresItsDetector_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("timeout") << 0; QTest::newRow("cancel") << 1;
		QTest::newRow("backend-stopped") << 2; QTest::newRow("backend-fault") << 3; QTest::newRow("queue-overflow") << 4;
		QTest::newRow("target-deleted") << 5; QTest::newRow("binding-changed") << 6; QTest::newRow("transport-started") << 7;
		QTest::newRow("selected-input-lost") << 8;
	}
	void learningFailureKeepsThePriorBindingAndRetiresItsDetector()
	{
		QFETCH(int, failure); bindPrior(); const QPointer<AutomatableModel> target(model());
		const QPointer<ControllerConnection> prior(model()->controllerConnection()); auto arguments = args();
		if (failure == 0) { arguments.insert("timeoutMs", 20); }
		if (failure == 8) { m_input.raw = false; arguments.insert("port", "Knobs A"); }
		bool acted = false, cancel = false; QTimer timer; timer.setInterval(1); QObject::connect(&timer, &QTimer::timeout, this, [&] {
			if (acted || !m_input.detector()) { return; } acted = true;
			if (failure == 1) { cancel = true; }
			if (failure == 2) { m_input.active = false; }
			if (failure == 3) { m_input.fail(); }
			if (failure == 4) { for (unsigned int i = 0; i <= MidiClient::InputQueueLimit; ++i) { m_input.feed(15, 0, 96, {}, false); } }
			if (failure == 5) { delete Engine::getSong()->tracks().at(0); }
			if (failure == 6) { delete target->controllerConnection(); }
			if (failure == 7) { Engine::getSong()->playSong(); }
			if (failure == 8) { m_input.inputs.removeAll("Knobs A"); QMetaObject::invokeMethod(m_input.detector()->midiPort(), "updateReadablePorts", Qt::DirectConnection); }
		}); timer.start(); const auto response = StudioCapabilities::execute("controller.midi.learn", arguments, [&] { return cancel; }); timer.stop();
		QVERIFY(!response.value("ok").toBool()); QVERIFY(!m_input.detector()); QVERIFY(m_input.ports.empty());
		if (failure != 5 && failure != 6) { QVERIFY(target); QCOMPARE(target->controllerConnection(), prior.data()); QCOMPARE(target->storedValue(), 42.f); }
	}

	void learnAdmissionRejectsUnavailableInputAndInvalidOptions_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("inactive") << 0; QTest::newRow("unobserved") << 1;
		QTest::newRow("unqueued") << 2; QTest::newRow("raw-named-port") << 3; QTest::newRow("unknown-port") << 4;
		QTest::newRow("no-inputs") << 5; QTest::newRow("zero-timeout") << 6; QTest::newRow("excessive-timeout") << 7;
		QTest::newRow("invalid-channel") << 8; QTest::newRow("empty-port") << 9;
	}
	void learnAdmissionRejectsUnavailableInputAndInvalidOptions()
	{
		QFETCH(int, failure); bindPrior(); const QPointer<ControllerConnection> prior(model()->controllerConnection()); auto arguments = args();
		if (failure == 0) { m_input.active = false; } if (failure == 1) { m_input.observed = false; } if (failure == 2) { m_input.queued = false; }
		if (failure == 3) { arguments.insert("port", "Knobs A"); }
		if (failure == 4) { m_input.raw = false; arguments.insert("port", "Unknown"); }
		if (failure == 5) { m_input.raw = false; m_input.inputs.clear(); }
		if (failure == 6) { arguments.insert("timeoutMs", 0); } if (failure == 7) { arguments.insert("timeoutMs", 600001); }
		if (failure == 8) { arguments.insert("inputChannel", 17); } if (failure == 9) { arguments.insert("port", ""); }
		QVERIFY(!StudioCapabilities::execute("controller.midi.learn", arguments).value("ok").toBool());
		QCOMPARE(model()->controllerConnection(), prior.data()); QVERIFY(m_input.ports.empty());
	}

	void ambiguousOrLostNamedDetectionCannotPublish_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("missing-source") << 0; QTest::newRow("unknown-source") << 1; QTest::newRow("source-lost-after-cc") << 2;
	}
	void ambiguousOrLostNamedDetectionCannotPublish()
	{
		QFETCH(int, failure); m_input.raw = false; bindPrior(); const QPointer<ControllerConnection> prior(model()->controllerConnection());
		bool fed = false; QTimer timer; timer.setInterval(1); QObject::connect(&timer, &QTimer::timeout, this, [&] {
			if (fed || !m_input.detector()) { return; } fed = true;
			m_input.feed(15, 0, 96, failure == 0 ? QString() : failure == 1 ? QString("Unknown") : QString("Knobs, B"), true, true);
			if (failure == 2) { m_input.inputs.removeAll("Knobs, B"); QMetaObject::invokeMethod(m_input.detector()->midiPort(), "updateReadablePorts", Qt::DirectConnection); }
		}); timer.start(); const auto response = StudioCapabilities::execute("controller.midi.learn", args()); timer.stop();
		QVERIFY(!response.value("ok").toBool()); QCOMPARE(model()->controllerConnection(), prior.data()); QVERIFY(m_input.ports.empty());
	}

	void cancellationAfterPreparingTheBindingCannotReplaceThePriorConnection()
	{
		bindPrior(); const QPointer<ControllerConnection> prior(model()->controllerConnection()); bool fed = false;
		QTimer timer; timer.setInterval(1); QObject::connect(&timer, &QTimer::timeout, this, [&] {
			if (!fed && m_input.detector()) { fed = true; m_input.feed(); }
		}); timer.start(); const auto response = StudioCapabilities::execute("controller.midi.learn", args(), [&] { return m_input.ports.size() > 1; }); timer.stop();
		QVERIFY(fed); QVERIFY(!response.value("ok").toBool()); QCOMPARE(model()->controllerConnection(), prior.data()); QVERIFY(m_input.ports.empty());
	}

	void nestedLearnCannotStealTheActiveDetector()
	{
		bool attempted = false, retained = false; QJsonObject nested;
		QTimer timer; timer.setInterval(1); QObject::connect(&timer, &QTimer::timeout, this, [&] {
			if (attempted || !m_input.detector()) { return; } attempted = true;
			const QPointer<AutoDetectMidiController> active(m_input.detector());
			nested = StudioCapabilities::execute("controller.midi.learn", args());
			retained = active && m_input.detector() == active.data(); m_input.feed();
		}); timer.start(); const auto response = StudioCapabilities::execute("controller.midi.learn", args()); timer.stop();
		QVERIFY(attempted); QVERIFY(retained); QVERIFY(!nested.value("ok").toBool());
		QVERIFY(nested.value("error").toString().contains("Another native MIDI learn"));
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		QVERIFY(model()->controllerConnection()); QVERIFY(!m_input.detector()); QCOMPARE(m_input.ports.size(), std::size_t(1));
	}

	void waitingRefusesChangesToTheCompleteLinkedBaseline_data()
	{
		QTest::addColumn<int>("action"); QTest::newRow("value") << 0; QTest::newRow("initial") << 1; QTest::newRow("step") << 2;
		QTest::newRow("scale") << 3; QTest::newRow("range") << 4; QTest::newRow("unlink") << 5; QTest::newRow("peer-death") << 6;
		QTest::newRow("input-use") << 7; QTest::newRow("backend-change") << 8;
	}
	void waitingRefusesChangesToTheCompleteLinkedBaseline()
	{
		QFETCH(int, action); bindPrior(); const QPointer<ControllerConnection> prior(model()->controllerConnection());
		auto peer = std::make_unique<FloatModel>(42, 0, 200, 0, nullptr); QVERIFY(model()->linkToModel(peer.get()));
		const auto restoreClient = qScopeGuard([&] { StudioMidiLearnTestAccess::exchange(&m_input); });
		const auto response = runLearn({}, [&](AutoDetectMidiController*) {
			if (action == 0) { peer->setValue(60); } if (action == 1) { peer->setInitValue(60); } if (action == 2) { peer->setStep(0.5f); }
			if (action == 3) { peer->setScaleType(AutomatableModel::ScaleType::Logarithmic); } if (action == 4) { peer->setRange(0, 300); }
			if (action == 5) { peer->unlink(); } if (action == 6) { peer.reset(); } if (action == 7) { peer->setUseControllerValue(false); }
			if (action == 8) { StudioMidiLearnTestAccess::exchange(m_priorClient); }
		});
		QVERIFY(!response.value("ok").toBool()); QCOMPARE(model()->controllerConnection(), prior.data()); QVERIFY(!m_input.detector()); QVERIFY(m_input.ports.empty());
	}
	void finalNotificationSeesRetiredDetectorAndPriorResources_data()
	{
		QTest::addColumn<bool>("owned"); QTest::addColumn<bool>("blocked"); QTest::newRow("shared") << false << false;
		QTest::newRow("owned") << true << false; QTest::newRow("blocked-shared") << false << true; QTest::newRow("blocked-owned") << true << true;
	}
	void finalNotificationSeesRetiredDetectorAndPriorResources()
	{
		QFETCH(bool, owned); QFETCH(bool, blocked);
		if (owned) { QVERIFY(StudioCapabilities::execute("controller.midi.connect", {{"parameter", static_cast<int>(model()->id())}, {"inputController", 74}}).value("ok").toBool()); } else { bindPrior(); }
		const QPointer<ControllerConnection> prior(model()->controllerConnection()); const QPointer<Controller> previous(prior->getController());
		model()->setUseControllerValue(false); model()->blockSignals(blocked); int notifications = 0;
		const auto connection = connect(model(), &Model::dataChanged, this, [&] {
			if (model()->controllerConnection() == prior.data()) { return; }
			++notifications; QVERIFY(!prior); QCOMPARE(bool(previous), !owned); QVERIFY(!m_input.detector()); QCOMPARE(m_input.ports.size(), std::size_t(1));
			QVERIFY(model()->useControllerValue()); QCOMPARE(model()->storedValue(), 42.f); QCOMPARE(model()->controllerConnection()->getController()->currentValue(0), 96.f / 127.f);
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = runLearn(); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		QVERIFY(response.value("result").toObject().value("changed").toBool()); QCOMPARE(notifications, blocked ? 0 : 1); QCOMPARE(model()->signalsBlocked(), blocked);
		QVERIFY(!prior); QVERIFY(!m_input.detector()); QCOMPARE(m_input.ports.size(), std::size_t(1));
	}
	void finalNotificationFaultsCannotReturnAStaleSuccess_data()
	{
		QTest::addColumn<int>("action"); QTest::newRow("target-death") << 0; QTest::newRow("controller-death") << 1;
		QTest::newRow("value") << 2; QTest::newRow("unlink") << 3; QTest::newRow("standard") << 4; QTest::newRow("nonstandard") << 5;
		QTest::newRow("input-use") << 6; QTest::newRow("input-controller") << 7;
	}
	void finalNotificationFaultsCannotReturnAStaleSuccess()
	{
		QFETCH(int, action); bindPrior(); FloatModel peer(42, 0, 200, 0, nullptr); QVERIFY(model()->linkToModel(&peer));
		const QPointer<AutomatableModel> target(model()); const QPointer<ControllerConnection> prior(target->controllerConnection()); QPointer<ControllerConnection> candidate; bool invoked = false;
		ModelValueNotificationProbe fault(target.data(), [&] { if (action == 4) { throw std::runtime_error("fixture learned notification"); } if (action == 5) { throw 7; } });
		const auto connection = connect(target.data(), &Model::dataChanged, this, [&] {
			if (invoked) { return; } invoked = true; candidate = target->controllerConnection(); QVERIFY(!prior); QVERIFY(!m_input.detector());
			if (action == 0) { delete Engine::getSong()->tracks().at(0); } if (action == 1) { delete candidate->getController(); }
			if (action == 2) { target->setValue(60); } if (action == 3) { peer.unlink(); }
			if (action == 6) { target->setUseControllerValue(false); }
			if (action == 7) { static_cast<MidiController*>(candidate->getController())->midiPort()->setInputController(71); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = runLearn(); QVERIFY(!response.value("ok").toBool()); QVERIFY(invoked); QVERIFY(!prior); QVERIFY(!m_input.detector());
		if (action <= 1) { QVERIFY(!candidate); } if (target) { QVERIFY(!target->signalsBlocked()); }
	}
	void aDetectorDeletedWhileWaitingIsNotOwnedTwice()
	{
		bindPrior(); const QPointer<ControllerConnection> prior(model()->controllerConnection()); const auto response = runLearn({}, [](AutoDetectMidiController* active) { delete active; });
		QVERIFY(!response.value("ok").toBool()); QCOMPARE(model()->controllerConnection(), prior.data()); QVERIFY(!m_input.detector()); QVERIFY(m_input.ports.empty());
	}
	void projectNotificationCannotInvalidateAReportedLearnSuccess_data()
	{ QTest::addColumn<int>("action"); QTest::newRow("target") << 0; QTest::newRow("binding") << 1; QTest::newRow("value") << 2; QTest::newRow("backend") << 3; }
	void projectNotificationCannotInvalidateAReportedLearnSuccess()
	{
		QFETCH(int, action); bindPrior(); const QPointer<AutomatableModel> target(model()); NativeSongTestAccess::setModified(Engine::getSong(), false); bool invoked = false;
		const auto restoreClient = qScopeGuard([&] { StudioMidiLearnTestAccess::exchange(&m_input); });
		const auto connection = connect(Engine::getSong(), &Song::modified, this, [&] {
			if (invoked) { return; } invoked = true; QVERIFY(!m_input.detector());
			if (action == 0) { delete Engine::getSong()->tracks().at(0); } if (action == 1) { delete target->controllerConnection(); }
			if (action == 2) { target->setValue(60); } if (action == 3) { StudioMidiLearnTestAccess::exchange(m_priorClient); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = runLearn(); QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QVERIFY(!m_input.detector());
		if (action <= 1) { QVERIFY(m_input.ports.empty()); }
	}
	void detectorRetirementCanInvalidatePreparation_data()
	{ QTest::addColumn<int>("action"); QTest::newRow("target") << 0; QTest::newRow("candidate") << 1; QTest::newRow("peer") << 2; }
	void detectorRetirementCanInvalidatePreparation()
	{
		QFETCH(int, action); bindPrior(); FloatModel peer(42, 0, 200, 0, nullptr); QVERIFY(model()->linkToModel(&peer));
		const QPointer<AutomatableModel> target(model()); const QPointer<ControllerConnection> prior(target->controllerConnection()); bool retired = false;
		const auto response = runLearn({}, [&](AutoDetectMidiController* active) {
			connect(active, &QObject::destroyed, this, [&] {
				if (retired) { return; } retired = true;
				if (action == 0) { delete Engine::getSong()->tracks().at(0); }
				if (action == 1) { const auto ports = m_input.ports; for (const auto& port : ports) { if (port) { delete static_cast<MidiController*>(port->parent()); } } }
				if (action == 2) { peer.setStep(0.5f); }
			});
		});
		QVERIFY(retired); QVERIFY(!response.value("ok").toBool()); QVERIFY(!m_input.detector()); QVERIFY(m_input.ports.empty());
		if (target) { QCOMPARE(target->controllerConnection(), prior.data()); }
	}
	void callbacksAfterCandidateConstructionCannotOverwriteTheBaseline_data()
	{
		QTest::addColumn<int>("action"); QTest::newRow("candidate") << 0; QTest::newRow("step") << 1; QTest::newRow("backend") << 2;
		QTest::newRow("detector") << 3; QTest::newRow("standard") << 4; QTest::newRow("nonstandard") << 5;
	}
	void callbacksAfterCandidateConstructionCannotOverwriteTheBaseline()
	{
		QFETCH(int, action); bindPrior(); const QPointer<ControllerConnection> prior(model()->controllerConnection()); bool invoked = false;
		const auto restoreClient = qScopeGuard([&] { StudioMidiLearnTestAccess::exchange(&m_input); });
		const auto response = runLearn([&] {
			if (invoked) { return false; } QPointer<MidiController> candidate;
			for (const auto& port : m_input.ports) { if (port && !dynamic_cast<AutoDetectMidiController*>(port->parent())) { candidate = dynamic_cast<MidiController*>(port->parent()); } }
			if (!candidate) { return false; } invoked = true;
			if (action == 0) { delete candidate.data(); } if (action == 1) { model()->setStep(0.5f); }
			if (action == 2) { StudioMidiLearnTestAccess::exchange(m_priorClient); } if (action == 3) { delete m_input.detector(); }
			if (action == 4) { throw std::runtime_error("fixture learned preparation"); } if (action == 5) { throw 7; }
			return false;
		});
		QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QCOMPARE(model()->controllerConnection(), prior.data()); QVERIFY(!m_input.detector()); QVERIFY(m_input.ports.empty());
	}
	void sessionRecoversFailedLearnedPublicationAndReplaysTheFailure_data()
	{ QTest::addColumn<bool>("standard"); QTest::newRow("standard") << true; QTest::newRow("nonstandard") << false; }
	void sessionRecoversFailedLearnedPublicationAndReplaysTheFailure()
	{
		QFETCH(bool, standard); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error));
		QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("value", "parameter.set", {{"parameter", static_cast<int>(model()->id())}, {"value", 42}})).value("ok").toBool());
		bool invoked = false, fed = false; ModelValueNotificationProbe fault(model(), [&] {
			if (invoked) { return; } invoked = true; if (standard) { throw std::runtime_error("fixture learned session publication"); } throw 7;
		});
		QTimer timer; timer.setInterval(1); connect(&timer, &QTimer::timeout, this, [&] { if (!fed && m_input.detector()) { fed = true; m_input.feed(); } });
		const auto command = request("learn", "controller.midi.learn", args()); timer.start(); const auto receipt = runtime.process(command); timer.stop();
		QVERIFY(invoked); QVERIFY(!receipt.value("ok").toBool()); QVERIFY(!model()->controllerConnection()); QCOMPARE(model()->storedValue(), 42.f); QVERIFY(m_input.ports.empty());
		auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); QVERIFY(!runtime.stopping());
	}

	void learnTransactionsPreserveHistoryAndReplayWithoutListening_data()
	{
		QTest::addColumn<bool>("cancel"); QTest::newRow("successful-history") << false; QTest::newRow("cancelled-history") << true;
	}
	void learnTransactionsPreserveHistoryAndReplayWithoutListening()
	{
		QFETCH(bool, cancel); QTemporaryDir session; QJsonObject learn;
		{
			StudioRuntime runtime(session.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			learn = request("learn", "controller.midi.learn", args()); const auto prior = manifest(session); QVERIFY(!prior.isEmpty());
			bool fed = false; QTimer timer; timer.setInterval(1); QObject::connect(&timer, &QTimer::timeout, this, [&] {
				if (fed || !m_input.detector()) { return; } fed = true;
				if (cancel) { runtime.process(request("cancel", "session.shutdown")); } else { m_input.feed(); }
			}); timer.start(); const auto response = runtime.process(learn); timer.stop(); QCOMPARE(response.value("ok").toBool(), !cancel);
			QVERIFY(!m_input.detector());
			if (cancel)
			{
				QVERIFY(!model()->controllerConnection()); QCOMPARE(manifest(session).value("history"), prior.value("history"));
				QCOMPARE(manifest(session).value("cursor"), prior.value("cursor")); return;
			}
			const QPointer<ControllerConnection> retained(model()->controllerConnection()); QVERIFY(retained);
			QVERIFY(runtime.process(learn).value("replayed").toBool()); QCOMPARE(model()->controllerConnection(), retained.data());
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool()); QVERIFY(!model()->controllerConnection());
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool()); QVERIFY(model()->controllerConnection());
		}
		StudioRuntime recovered(session.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		const auto native = dynamic_cast<MidiController*>(model()->controllerConnection()->getController()); QVERIFY(native);
		QCOMPARE(native->midiPort()->inputChannel(), 16); QCOMPARE(native->midiPort()->inputController(), 0); QCOMPARE(native->currentValue(0), 0.f);
		const QPointer<ControllerConnection> retained(model()->controllerConnection()); QVERIFY(recovered.process(learn).value("replayed").toBool());
		QCOMPARE(model()->controllerConnection(), retained.data()); QVERIFY(!m_input.detector());
	}
};
QTEST_GUILESS_MAIN(StudioMidiLearnTest)
#include "StudioMidiLearnTest.moc"
