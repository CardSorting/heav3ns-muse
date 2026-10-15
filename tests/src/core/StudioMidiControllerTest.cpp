/* Native parameter-owned MIDI controller tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QEvent>
#include <QDomDocument>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QTemporaryDir>
#include <cmath>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClient.h"
#include "MidiController.h"
#include "MidiPort.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "ValueBuffer.h"

using namespace lmms;
namespace lmms
{
class MidiPortTestAccess
{
public:
	static void useClient(MidiPort* port, MidiClient* client)
	{
		port->m_midiClient->removePort(port); port->m_midiClient = client; client->addPort(port);
		port->updateReadablePorts(); port->updateWritablePorts();
	}
};
}
namespace
{
class ControllerRawInput final : public MidiClientRaw
{
public:
	bool active = true;
	InputBackendStatus inputBackendStatus() const override { return {true, active, 0}; }
	void feed(int channel, int control, int value, bool pump = true)
	{
		parseData(static_cast<unsigned char>(0xb0 | channel)); parseData(static_cast<unsigned char>(control)); parseData(static_cast<unsigned char>(value));
		if (pump) { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
	}
	void fail(MidiPort* port) { failInputEvents(InputDeliveryFailure::BackendInput, port); }
private:
	void sendByte(unsigned char) override { }
};
class ControllerNamedInput final : public MidiClient
{
public:
	QStringList inputs{"Knobs, A", "Knobs B"};
	QMap<QString, bool> requested;
	bool hasQueuedInputDelivery() const override { return true; }
	InputBackendStatus inputBackendStatus() const override { return {true, true, 0}; }
	QStringList readablePorts() const override { return inputs; }
	void subscribeReadablePort(MidiPort*, const QString& name, bool enabled) override { requested.insert(name, enabled); }
	void processOutEvent(const MidiEvent&, const TimePos&, const MidiPort*) override { }
	void feed(MidiPort* port, const QString& name, int value)
	{
		dispatchInputEvent(MidiEvent(MidiControlChange, 0, 74, value), {}, port, name);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	}
};
class ReadOnlyParameter final : public FloatModel
{
public:
	explicit ReadOnlyParameter(Model* parent) : FloatModel(0, 0, 1, 0.01f, parent) { }
	bool isWritable() const override { return false; }
};
}

class StudioMidiControllerTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	ControllerRawInput m_raw;
	ControllerNamedInput m_named;
	FloatModel* model() const { return static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->volumeModel(); }
	QJsonObject selector() const { return {{"parameter", static_cast<int>(model()->id())}}; }
	MidiController* midi() const
	{
		const auto connection = model()->controllerConnection();
		return connection ? dynamic_cast<MidiController*>(connection->getController()) : nullptr;
	}
	QJsonObject connectArgs(int control = 74) const { auto args = selector(); args.insert("inputController", control); return args; }
	QJsonObject call(const QString& op, const QJsonObject& args = {}) const { return StudioCapabilities::execute(op, args); }
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) const { return {{"id", id}, {"op", op}, {"args", args}}; }
	QJsonObject manifest(const QTemporaryDir& session) const
	{
		QFile file(session.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
	}
	float settledValue()
	{
		Controller::triggerFrameCounter(); midi()->currentValue(0);
		Controller::triggerFrameCounter(); return model()->value();
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
		Engine::getSong()->clearProject(); Engine::getSong()->getSaveOptions().setDefaultOptions();
		m_raw.active = true; m_named.inputs = QStringList{"Knobs, A", "Knobs B"}; m_named.requested.clear();
		new InstrumentTrack(Engine::getSong()); model()->setValue(42);
	}
	void cleanup() { Engine::getSong()->getSaveOptions().setDefaultOptions(); Engine::getSong()->clearProject(); }

	void nativeCcConnectionsExposeDiscoveryAndFilterInput_data()
	{
		QTest::addColumn<bool>("logarithmic"); QTest::addColumn<int>("channel");
		QTest::newRow("linear-omni") << false << 0; QTest::newRow("linear-channel-16") << false << 16;
		QTest::newRow("log-channel-3") << true << 3; QTest::newRow("log-omni") << true << 0;
	}
	void nativeCcConnectionsExposeDiscoveryAndFilterInput()
	{
		QFETCH(bool, logarithmic); QFETCH(int, channel); model()->setScaleLogarithmic(logarithmic);
		auto args = connectArgs(); args.insert("inputChannel", channel);
		const auto result = call("controller.midi.connect", args); QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
		QVERIFY(midi()); QVERIFY(Engine::getSong()->controllers().empty()); QCOMPARE(midi()->connectionCount(), 1);
		const auto metadata = call("parameter.inspect", selector()).value("result").toObject().value("controller").toObject();
		QVERIFY(metadata.value("index").isNull()); QCOMPARE(metadata.value("midiSelector").toObject(), selector());
		MidiPortTestAccess::useClient(midi()->midiPort(), &m_raw);
		const int nativeChannel = channel ? channel - 1 : 9;
		m_raw.feed(nativeChannel, 74, 127); QCOMPARE(settledValue(), model()->maxValue());
		m_raw.feed(nativeChannel, 75, 0); QCOMPARE(settledValue(), model()->maxValue());
		if (channel) { m_raw.feed((nativeChannel + 1) % 16, 74, 0); QCOMPARE(settledValue(), model()->maxValue()); }
		m_raw.feed(nativeChannel, 74, 0); QCOMPARE(settledValue(), model()->minValue()); QCOMPARE(model()->storedValue(), 42.f);
		const auto observed = call("midi.inspect", selector()).value("result").toObject();
		QCOMPARE(observed.value("inputController").toInt(), 74); QVERIFY(!observed.value("connectionVerified").toBool());
		auto settings = selector(); settings.insert("readable", false); QVERIFY(call("midi.configure", settings).value("ok").toBool());
		m_raw.feed(nativeChannel, 74, 127); QCOMPARE(settledValue(), model()->minValue());
	}

	void malformedDirectCcPreservesNativeBindingAndBuffer_data()
	{
		QTest::addColumn<int>("control"); QTest::addColumn<int>("value"); QTest::addColumn<int>("channel"); QTest::addColumn<int>("selected"); QTest::addColumn<int>("type");
		QTest::newRow("negative-controller") << -1 << 1 << 0 << 127 << int(MidiControlChange);
		QTest::newRow("large-controller") << 128 << 1 << 0 << 0 << int(MidiControlChange);
		QTest::newRow("high-byte-controller") << 256 << 1 << 0 << 0 << int(MidiControlChange);
		QTest::newRow("negative-controller-alias") << -128 << 1 << 0 << 0 << int(MidiControlChange);
		QTest::newRow("controller-signed-maximum") << 32767 << 1 << 0 << 127 << int(MidiControlChange);
		QTest::newRow("negative-value") << 74 << -1 << 0 << 74 << int(MidiControlChange);
		QTest::newRow("large-value") << 74 << 128 << 0 << 74 << int(MidiControlChange);
		QTest::newRow("high-byte-value") << 74 << 256 << 0 << 74 << int(MidiControlChange);
		QTest::newRow("negative-value-alias") << 74 << -256 << 0 << 74 << int(MidiControlChange);
		QTest::newRow("value-signed-maximum") << 74 << 32767 << 0 << 74 << int(MidiControlChange);
		QTest::newRow("negative-channel") << 74 << 1 << -1 << 74 << int(MidiControlChange);
		QTest::newRow("large-channel") << 74 << 1 << 16 << 74 << int(MidiControlChange);
		QTest::newRow("unrelated-program") << 74 << 1 << 0 << 74 << int(MidiProgramChange);
	}
	void malformedDirectCcPreservesNativeBindingAndBuffer()
	{
		QFETCH(int, control); QFETCH(int, value); QFETCH(int, channel); QFETCH(int, selected); QFETCH(int, type);
		QVERIFY(call("controller.midi.connect", connectArgs(selected)).value("ok").toBool());
		const QPointer<ControllerConnection> binding(model()->controllerConnection()); const QPointer<MidiController> controller(midi()); QVERIFY(controller);
		controller->processInEvent(MidiEvent(MidiControlChange, 0, selected, 63), {}); const auto before = settledValue();
		QSignalSpy changed(controller.data(), &Controller::valueChanged);
		controller->processInEvent(MidiEvent(static_cast<MidiEventTypes>(type), channel, control, value), {}, 5); QCOMPARE(changed.size(), 0);
		QCOMPARE(model()->controllerConnection(), binding.data()); QCOMPARE(midi(), controller.data()); QCOMPARE(settledValue(), before);
		const auto buffer = controller->valueBuffer(); QVERIFY(buffer && buffer->length() > 0);
		for (const auto sample : *buffer) { QVERIFY(std::isfinite(sample)); QCOMPARE(sample, 63.f / 127.f); }
		QCOMPARE(model()->storedValue(), 42.f); QCOMPARE(controller->midiPort()->inputController(), selected); QCOMPARE(controller->midiPort()->inputChannel(), 0);
	}

	void directCcRetainsChannelAndControllerFilters_data()
	{
		QTest::addColumn<int>("selectedChannel"); QTest::addColumn<int>("channel"); QTest::addColumn<int>("control"); QTest::addColumn<bool>("accepted");
		QTest::newRow("omni-channel-16") << 0 << 15 << 74 << true;
		QTest::newRow("selected-channel-16") << 16 << 15 << 74 << true;
		QTest::newRow("other-channel") << 16 << 0 << 74 << false;
		QTest::newRow("other-controller") << 0 << 15 << 75 << false;
	}
	void directCcRetainsChannelAndControllerFilters()
	{
		QFETCH(int, selectedChannel); QFETCH(int, channel); QFETCH(int, control); QFETCH(bool, accepted);
		auto args = connectArgs(); args.insert("inputChannel", selectedChannel); QVERIFY(call("controller.midi.connect", args).value("ok").toBool());
		midi()->processInEvent(MidiEvent(MidiControlChange, selectedChannel ? selectedChannel - 1 : 0, 74, 63), {}); const auto before = settledValue();
		QSignalSpy changed(midi(), &Controller::valueChanged); midi()->processInEvent(MidiEvent(MidiControlChange, channel, control, 127), {});
		QCOMPARE(changed.size(), accepted ? 1 : 0); QCOMPARE(settledValue(), accepted ? model()->maxValue() : before); QCOMPARE(model()->storedValue(), 42.f);
	}

	void acceptedDirectCcKeepsFiniteNormalizedBuffer_data()
	{
		QTest::addColumn<int>("value"); QTest::newRow("minimum") << 0; QTest::newRow("middle") << 63; QTest::newRow("maximum") << 127;
	}
	void acceptedDirectCcKeepsFiniteNormalizedBuffer()
	{
		QFETCH(int, value); QVERIFY(call("controller.midi.connect", connectArgs(0)).value("ok").toBool());
		QSignalSpy changed(midi(), &Controller::valueChanged); midi()->processInEvent(MidiEvent(MidiControlChange, 15, 0, value), {}); QCOMPARE(changed.size(), 1);
		settledValue(); const auto buffer = midi()->valueBuffer(); QVERIFY(buffer && buffer->length() > 0);
		for (const auto sample : *buffer) { QVERIFY(std::isfinite(sample)); QCOMPARE(sample, float(value) / 127.f); }
		QCOMPARE(model()->storedValue(), 42.f);
	}

	void rejectedRawCcPreservesMappedParameterSamples_data()
	{
		QTest::addColumn<bool>("logarithmic"); QTest::newRow("linear") << false; QTest::newRow("logarithmic") << true;
	}
	void rejectedRawCcPreservesMappedParameterSamples()
	{
		QFETCH(bool, logarithmic); model()->setScaleLogarithmic(logarithmic); QVERIFY(call("controller.midi.connect", connectArgs()).value("ok").toBool());
		midi()->processInEvent(MidiEvent(MidiControlChange, 0, 74, 63), {}); settledValue(); AutomatableModel::incrementPeriodCounter();
		const auto buffer = model()->valueBuffer(); QVERIFY(buffer && buffer->length() > 0); const auto before = *buffer;
		QSignalSpy changed(midi(), &Controller::valueChanged); midi()->processInEvent(MidiEvent(MidiControlChange, 0, 74, -1), {}); QCOMPARE(changed.size(), 0);
		settledValue(); AutomatableModel::incrementPeriodCounter(); const auto after = model()->valueBuffer(); QVERIFY(after); QCOMPARE(after->length(), before.length());
		for (int i = 0; i < after->length(); ++i)
		{
			const auto sample = after->value(i); QVERIFY(std::isfinite(sample)); QVERIFY(sample >= model()->minValue() && sample <= model()->maxValue());
			QCOMPARE(sample, before.value(i));
		}
		QCOMPARE(model()->storedValue(), 42.f);
	}

	void invalidConnectionArgumentsPreserveTheExistingBinding_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("missing-cc") << 0; QTest::newRow("negative-cc") << 1; QTest::newRow("high-cc") << 2;
		QTest::newRow("negative-channel") << 3; QTest::newRow("high-channel") << 4; QTest::newRow("fractional-cc") << 5;
		QTest::newRow("nonboolean-readable") << 6; QTest::newRow("unsupported-scale") << 7; QTest::newRow("readonly-target") << 8;
	}
	void invalidConnectionArgumentsPreserveTheExistingBinding()
	{
		QFETCH(int, failure); QVERIFY(call("controller.midi.connect", connectArgs()).value("ok").toBool());
		const QPointer<ControllerConnection> connection(model()->controllerConnection()); const QPointer<MidiController> original(midi());
		auto args = connectArgs(); ReadOnlyParameter readonly(Engine::getSong());
		if (failure == 0) { args.remove("inputController"); }
		if (failure == 1) { args.insert("inputController", -1); } if (failure == 2) { args.insert("inputController", 128); }
		if (failure == 3) { args.insert("inputChannel", -1); } if (failure == 4) { args.insert("inputChannel", 17); }
		if (failure == 5) { args.insert("inputController", 1.5); } if (failure == 6) { args.insert("readable", 1); }
		if (failure == 7) { model()->setScaleType(AutomatableModel::ScaleType::Decibel); }
		if (failure == 8) { args.insert("parameter", static_cast<int>(readonly.id())); }
		const auto result = call("controller.midi.connect", args); model()->setScaleType(AutomatableModel::ScaleType::Linear);
		QVERIFY(!result.value("ok").toBool()); QCOMPARE(model()->controllerConnection(), connection.data()); QCOMPARE(midi(), original.data());
		QCOMPARE(midi()->midiPort()->inputController(), 74); QVERIFY(!readonly.controllerConnection());
	}

	void selectorAndControllerOnlyFieldsAreValidatedBeforeMutation_data()
	{
		QTest::addColumn<QString>("operation"); QTest::addColumn<QJsonObject>("extra");
		QTest::newRow("inspect-track-and-parameter") << QString("midi.inspect") << QJsonObject{{"track", 0}};
		QTest::newRow("inspect-container-and-parameter") << QString("midi.inspect") << QJsonObject{{"container", "song"}};
		QTest::newRow("reset-track-and-parameter") << QString("midi.input.reset") << QJsonObject{{"track", 0}};
		QTest::newRow("configure-output") << QString("midi.configure") << QJsonObject{{"inputChannel", 3}, {"writable", false}};
		QTest::newRow("configure-voice-setting") << QString("midi.configure") << QJsonObject{{"inputChannel", 3}, {"fixedInputVelocity", 80}};
		QTest::newRow("configure-invalid-input") << QString("midi.configure") << QJsonObject{{"inputChannel", 3}, {"inputController", 128}};
		QTest::newRow("subscribe-output") << QString("midi.subscribe") << QJsonObject{{"direction", "output"}, {"port", "Knobs, A"}, {"enabled", true}};
		QTest::newRow("subscribe-unknown") << QString("midi.subscribe") << QJsonObject{{"direction", "input"}, {"port", "Unknown"}, {"enabled", true}};
	}
	void selectorAndControllerOnlyFieldsAreValidatedBeforeMutation()
	{
		QFETCH(QString, operation); QFETCH(QJsonObject, extra); QVERIFY(call("controller.midi.connect", connectArgs()).value("ok").toBool());
		MidiPortTestAccess::useClient(midi()->midiPort(), &m_named); const auto before = call("midi.inspect", selector()).value("result");
		auto args = selector(); for (auto it = extra.begin(); it != extra.end(); ++it) { args.insert(it.key(), it.value()); }
		QVERIFY(!call(operation, args).value("ok").toBool()); QCOMPARE(call("midi.inspect", selector()).value("result"), before);
		QVERIFY(!call("midi.inspect", {}).value("ok").toBool());
		const auto unbound = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->panningModel();
		QVERIFY(!call("midi.inspect", {{"parameter", static_cast<int>(unbound->id())}}).value("ok").toBool());
	}

	void rewiringReleasesOwnedControllersAndRetainsRackOwnership()
	{
		QVERIFY(call("controller.midi.connect", connectArgs()).value("ok").toBool());
		QPointer<MidiController> first(midi()); QPointer<ControllerConnection> firstConnection(model()->controllerConnection());
		QVERIFY(call("controller.midi.connect", connectArgs(71)).value("ok").toBool()); QVERIFY(!first); QVERIFY(!firstConnection);
		QPointer<MidiController> second(midi()); QPointer<ControllerConnection> secondConnection(model()->controllerConnection());
		QVERIFY(call("controller.add", {{"type", "lfo"}}).value("ok").toBool()); const QPointer<Controller> rack(Engine::getSong()->controllers().front());
		auto args = selector(); args.insert("controller", 0); QVERIFY(call("controller.connect", args).value("ok").toBool());
		QVERIFY(!second); QCOMPARE(model()->controllerConnection(), secondConnection.data()); QCOMPARE(rack->connectionCount(), 1);
		QVERIFY(call("controller.midi.connect", connectArgs()).value("ok").toBool()); QVERIFY(rack); QCOMPARE(rack->connectionCount(), 0);
		QVERIFY(!secondConnection); const QPointer<MidiController> ownedFinal(midi());
		QVERIFY(call("controller.disconnect", selector()).value("ok").toBool()); QVERIFY(!ownedFinal); QVERIFY(!model()->controllerConnection());
		QCOMPARE(model()->value(), 42.f); QCOMPARE(Engine::getSong()->controllers().size(), std::size_t(1));
	}

	void namedSubscriptionsFollowNativeIntentAndSurviveUnavailablePorts()
	{
		QVERIFY(call("controller.midi.connect", connectArgs()).value("ok").toBool()); auto port = midi()->midiPort();
		MidiPortTestAccess::useClient(port, &m_named);
		auto args = selector(); args.insert("direction", "input"); args.insert("port", "Knobs, A"); args.insert("enabled", true);
		QVERIFY(call("midi.subscribe", args).value("ok").toBool()); QCOMPARE(port->requestedReadablePorts(), QStringList{"Knobs, A"});
		QVERIFY(m_named.requested.value("Knobs, A")); m_named.feed(port, "Knobs, A", 127); QCOMPARE(settledValue(), model()->maxValue());
		m_named.inputs.removeAll("Knobs, A"); QVERIFY(QMetaObject::invokeMethod(port, "updateReadablePorts", Qt::DirectConnection));
		const auto endpoints = call("midi.inspect", selector()).value("result").toObject().value("readablePorts").toArray(); bool found = false;
		for (const auto& value : endpoints)
		{
			const auto endpoint = value.toObject(); if (endpoint.value("name") != "Knobs, A") { continue; }
			found = true; QVERIFY(endpoint.value("selected").toBool()); QVERIFY(!endpoint.value("available").toBool());
		}
		QVERIFY(found); args.insert("enabled", false); QVERIFY(call("midi.subscribe", args).value("ok").toBool());
		QVERIFY(port->requestedReadablePorts().isEmpty());
		MidiPortTestAccess::useClient(port, &m_raw); args.insert("enabled", true); QVERIFY(!call("midi.subscribe", args).value("ok").toBool());
	}

	void inputFailureAndRecoveryDoNotInventControllerValues_data()
	{
		QTest::addColumn<bool>("overflow"); QTest::newRow("backend-failure") << false; QTest::newRow("queue-overflow") << true;
	}
	void inputFailureAndRecoveryDoNotInventControllerValues()
	{
		QFETCH(bool, overflow); QVERIFY(call("controller.midi.connect", connectArgs(120)).value("ok").toBool());
		auto port = midi()->midiPort(); MidiPortTestAccess::useClient(port, &m_raw);
		m_raw.feed(0, 120, 127); QCOMPARE(settledValue(), model()->maxValue());
		if (overflow) { for (unsigned int i = 0; i <= MidiClient::InputQueueLimit; ++i) { m_raw.feed(0, 120, 0, false); } }
		else { m_raw.fail(port); }
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QVERIFY(m_raw.inputDeliveryStatus(port).failed); QCOMPARE(settledValue(), model()->maxValue());
		m_raw.active = false; QVERIFY(!call("midi.input.reset", selector()).value("ok").toBool());
		m_raw.active = true; const auto reset = call("midi.input.reset", selector()); QVERIFY(reset.value("ok").toBool());
		QVERIFY(reset.value("result").toObject().value("recoveryChanged").toBool()); QCOMPARE(settledValue(), model()->maxValue());
		QVERIFY(!m_raw.inputDeliveryStatus(port).failed); m_raw.feed(0, 120, 0); QCOMPARE(settledValue(), model()->minValue());
	}

	void nativeConnectionsAndSubscriptionsSurviveHistoryRecoveryAndReplay_data()
	{
		QTest::addColumn<bool>("discardForExport"); QTest::newRow("ordinary-save-options") << false; QTest::newRow("discard-midi-export-option") << true;
	}
	void nativeConnectionsAndSubscriptionsSurviveHistoryRecoveryAndReplay()
	{
		QFETCH(bool, discardForExport); QTemporaryDir session, exports; QJsonObject connectRequest;
		{
			StudioRuntime runtime(session.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			Engine::getSong()->getSaveOptions().discardMIDIConnections.setValue(discardForExport);
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("track-midi", "midi.configure", {{"track", 0}, {"inputChannel", 5}, {"outputChannel", 6}})).value("ok").toBool());
			connectRequest = request("connect", "controller.midi.connect", connectArgs());
			QVERIFY(runtime.process(connectRequest).value("ok").toBool()); const QPointer<MidiController> original(midi());
			QVERIFY(runtime.process(connectRequest).value("replayed").toBool()); QCOMPARE(midi(), original.data());
			auto settings = selector(); settings.insert("inputChannel", 16); settings.insert("inputController", 71);
			QVERIFY(runtime.process(request("configure", "midi.configure", settings)).value("ok").toBool());
			MidiPortTestAccess::useClient(midi()->midiPort(), &m_named);
			auto subscription = selector(); subscription.insert("direction", "input"); subscription.insert("port", "Knobs, A"); subscription.insert("enabled", true);
			QVERIFY(runtime.process(request("subscribe", "midi.subscribe", subscription)).value("ok").toBool());
			const auto shared = exports.filePath("shared.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(shared));
			QFile file(shared); QVERIFY(file.open(QIODevice::ReadOnly)); QDomDocument document; QVERIFY(document.setContent(file.readAll()));
			QCOMPARE(document.elementsByTagName("Midicontroller").size(), discardForExport ? 0 : 1);
			QCOMPARE(document.elementsByTagName("midiport").size(), discardForExport ? 0 : 1);
			QVERIFY(runtime.process(request("undo-subscription", "session.undo")).value("ok").toBool());
			QVERIFY(midi()->midiPort()->requestedReadablePorts().isEmpty());
			QVERIFY(runtime.process(request("undo-configuration", "session.undo")).value("ok").toBool()); QCOMPARE(midi()->midiPort()->inputController(), 74);
			QVERIFY(runtime.process(request("redo-configuration", "session.redo")).value("ok").toBool());
			QVERIFY(runtime.process(request("redo-subscription", "session.redo")).value("ok").toBool());
			const auto before = manifest(session); QVERIFY(!before.isEmpty()); settings = selector(); settings.insert("inputController", 72); settings.insert("outputChannel", 9);
			QVERIFY(!runtime.process(request("invalid-configuration", "midi.configure", settings)).value("ok").toBool());
			QCOMPARE(midi()->midiPort()->inputController(), 71); QCOMPARE(manifest(session).value("history"), before.value("history"));
			QCOMPARE(manifest(session).value("cursor"), before.value("cursor"));
		}
		StudioRuntime recovered(session.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		QVERIFY(midi()); QCOMPARE(midi()->midiPort()->inputChannel(), 16); QCOMPARE(midi()->midiPort()->inputController(), 71);
		QCOMPARE(midi()->midiPort()->requestedReadablePorts(), QStringList{"Knobs, A"}); QVERIFY(Engine::getSong()->controllers().empty());
		const auto trackPort = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->midiPort();
		QCOMPARE(trackPort->inputChannel(), 5); QCOMPARE(trackPort->outputChannel(), 6);
		QCOMPARE(Engine::getSong()->getSaveOptions().discardMIDIConnections.value(), discardForExport);
		const QPointer<MidiController> retained(midi()); QVERIFY(recovered.process(connectRequest).value("replayed").toBool()); QCOMPARE(midi(), retained.data());
	}
};
QTEST_GUILESS_MAIN(StudioMidiControllerTest)
#include "StudioMidiControllerTest.moc"
