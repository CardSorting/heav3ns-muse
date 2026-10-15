/* Native automation recording tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <limits>
#include <stdexcept>
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClient.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRecording.h"
#include "StudioRuntime.h"

using namespace lmms;
namespace lmms
{
class MidiPortTestAccess
{
public:
	static void useClient(MidiPort* port, MidiClient* client)
	{
		port->m_midiClient->removePort(port); port->m_midiClient = client; client->addPort(port);
	}
};
}
namespace
{
class AutomationClock final : public AudioDevice
{
public:
	explicit AutomationClock(AudioEngine* engine) : AudioDevice(DEFAULT_CHANNELS, engine) { }
	~AutomationClock() override { stopProcessing(); }
private:
	void startProcessingImpl() override { }
	void stopProcessingImpl() override { }
};
class AutomationMidiInput final : public MidiClientRaw
{
public:
	bool active = true;
	InputBackendStatus inputBackendStatus() const override { return {true, active, 0}; }
	void feed(unsigned char value)
	{
		parseData(0xb0); parseData(7); parseData(value); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	}
	void fail() { failInputEvents(InputDeliveryFailure::BackendInput); }
private:
	void sendByte(unsigned char) override { }
};
}

class StudioAutomationRecordingTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	AutomationMidiInput m_input;
	AutomationClock* m_clock = nullptr;
	AutomationClip* m_clip = nullptr;
	FloatModel* m_model = nullptr;
	QByteArray curve(AutomationClip* clip) const
	{
		QDomDocument doc; auto root = doc.createElement("automation"); doc.appendChild(root); clip->saveSettings(doc, root);
		// XML attribute order is unspecified; compare every DOM field and child in order.
		const std::function<QJsonObject(const QDomNode&)> state = [&](const QDomNode& node) {
			QJsonObject attributes; const auto map = node.attributes();
			for (int index = 0; index < map.count(); ++index) { const auto item = map.item(index); attributes.insert(item.nodeName(), item.nodeValue()); }
			QJsonArray children; for (auto child = node.firstChild(); !child.isNull(); child = child.nextSibling()) { children.append(state(child)); }
			return QJsonObject{{"type", node.nodeType()}, {"name", node.nodeName()}, {"value", node.nodeValue()}, {"attributes", attributes}, {"children", children}};
		};
		return QJsonDocument(state(doc)).toJson(QJsonDocument::Compact);
	}
	QJsonObject options() const { return {{"track", 1}, {"clip", 0}, {"durationMs", 100}, {"timeoutMs", 1000}, {"maximumPoints", 100}}; }
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) const { return {{"id", id}, {"op", op}, {"args", args}}; }
	QJsonObject manifest(const QTemporaryDir& session) const
	{
		QFile file(session.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; }
		return QJsonDocument::fromJson(file.readAll()).object();
	}
	void period() { Engine::audioEngine()->renderNextPeriod(); }
	void assertReleased()
	{
		QVERIFY(Engine::getSong()->isStopped()); QVERIFY(!Engine::getSong()->isRecording());
		QVERIFY(!m_clip->isRecording()); QVERIFY(!m_clip->hasReservedRecording());
	}
	void refresh()
	{
		m_model = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->volumeModel();
		m_clip = static_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClip(0));
	}
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("audioengine", "samplerate", "48000");
		ConfigManager::inst()->setValue("audioengine", "framesperaudiobuffer", "256"); NotePlayHandleManager::init();
	}
	void cleanupTestCase() { NotePlayHandleManager::free(); }
	void init()
	{
		Engine::init(true); m_clock = new AutomationClock(Engine::audioEngine()); Engine::audioEngine()->setAudioDevice(m_clock, true);
		Engine::getSong()->clearProject(); Engine::getSong()->setTempo(120); m_input.active = true; AutomationClip::setQuantization(1);
		m_model = (new InstrumentTrack(Engine::getSong()))->volumeModel();
		auto track = new AutomationTrack(Engine::getSong()); m_clip = static_cast<AutomationClip*>(track->createClip(0));
		QVERIFY(m_clip->addObject(m_model)); m_clip->putValue(0, 10, false); m_clip->putValue(96, 90, false);
	}
	void cleanup() { AutomationClip::setQuantization(1); Engine::destroy(); m_clock = nullptr; m_clip = nullptr; m_model = nullptr; }

	void stagedOverdubPreservesPriorCurveUntilCommitAndReparentsNodes()
	{
		const auto before = curve(m_clip); m_clip->reserveRecordingTake(16, 0);
		m_clip->recordValue(12, 25); m_clip->recordValue(24, 75); QCOMPARE(curve(m_clip), before);
		QCOMPARE(m_clip->recordingSamples(), std::uint64_t(2)); m_clip->commitRecordingTake();
		QCOMPARE(m_clip->valueAt(0), 10.f); QCOMPARE(m_clip->valueAt(12), 25.f); QCOMPARE(m_clip->valueAt(24), 75.f);
		QCOMPARE(m_clip->valueAt(96), 90.f); QVERIFY(!m_clip->hasReservedRecording());
		auto& node = m_clip->getTimeMap()[24]; node.setInValue(60); node.setOutValue(70);
		QCOMPARE(m_clip->getTimeMap().value(24).getInValue(), 60.f);
		m_clip->putValue(30, 50, false); QCOMPARE(m_clip->valueAt(30), 50.f);
	}

	void invalidOrCancelledStagingCannotPublish_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("point-budget") << 0; QTest::newRow("nonfinite") << 1;
		QTest::newRow("empty") << 2; QTest::newRow("cancel-before-publication") << 3;
	}
	void invalidOrCancelledStagingCannotPublish()
	{
		QFETCH(int, failure); const auto before = curve(m_clip); m_clip->reserveRecordingTake(failure == 0 ? 2 : 16, 0);
		if (failure != 2) { m_clip->recordValue(12, failure == 1 ? std::numeric_limits<float>::quiet_NaN() : 25.f); }
		QVERIFY_EXCEPTION_THROWN(m_clip->commitRecordingTake([&] {
			if (failure == 3) { throw std::runtime_error("fixture publication cancellation"); }
		}), std::runtime_error);
		QCOMPARE(curve(m_clip), before); m_clip->discardRecordingTake(); assertReleased();
	}

	void stagedQuantizationAndTrimUseSourceCoordinates()
	{
		m_clip->putValue(25, 80, false, true); AutomationClip::setQuantization(24);
		m_clip->setAutoResize(false); m_clip->setStartTimeOffset(-24); m_clip->reserveRecordingTake(16, 12);
		m_clip->recordValue(1, 25); m_clip->recordValue(13, 75); m_clip->commitRecordingTake();
		QCOMPARE(m_clip->valueAt(24), 25.f); QCOMPARE(m_clip->valueAt(36), 75.f); QCOMPARE(m_clip->startTimeOffset().getTicks(), -24);
		QCOMPARE(m_clip->getTimeMap().value(25).getInValue(), 80.f);
		m_clip->clear(); m_clip->reserveRecordingTake(2, 0); m_clip->recordValue(0, 0); m_clip->commitRecordingTake();
		QVERIFY(m_clip->getTimeMap().contains(24));
	}

	void nativeSongAndNonzeroPatternRecordEditableValues_data()
	{
		QTest::addColumn<bool>("pattern"); QTest::addColumn<bool>("logarithmic");
		QTest::newRow("song-linear") << false << false; QTest::newRow("song-logarithmic") << false << true;
		QTest::newRow("second-pattern") << true << false;
	}
	void nativeSongAndNonzeroPatternRecordEditableValues()
	{
		QFETCH(bool, pattern); QFETCH(bool, logarithmic); auto args = options();
		if (pattern)
		{
			auto track = Track::create(Track::Type::Automation, Engine::patternStore()); Engine::getSong()->addPatternTrack();
			m_clip = static_cast<AutomationClip*>(track->getClip(1)); QVERIFY(m_clip->addObject(m_model));
			m_clip->putValue(0, 10, false); args.insert("container", "pattern"); args.insert("track", 0); args.insert("clip", 1);
			QVERIFY(m_clip->startPosition() > 0); Engine::patternStore()->setCurrentPattern(0);
		}
		m_model->setScaleLogarithmic(logarithmic); const auto before = curve(m_clip); bool priorPreserved = true;
		int periods = 0; QTimer clock; clock.setInterval(1); QObject::connect(&clock, &QTimer::timeout, this, [&] {
			if (!Engine::getSong()->isRecording()) { return; }
			priorPreserved &= curve(m_clip) == before; m_model->setValue(periods < 8 ? 25 : 75); period(); ++periods;
		}); clock.start(); const auto result = StudioCapabilities::execute("automation.record", args); clock.stop();
		QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString())); QVERIFY(priorPreserved);
		QCOMPARE(m_clip->valueAt(0), m_model->inverseScaledValue(25));
		QCOMPARE(m_clip->valueAt(8), m_model->inverseScaledValue(75));
		QVERIFY(result.value("result").toObject().value("samples").toDouble() > 0); assertReleased();
		if (pattern)
		{
			QCOMPARE(Engine::patternStore()->currentPattern(), 0);
			QCOMPARE(Engine::patternStore()->automatedValuesAt(0, 1).value(m_model), 25.f);
		}
	}

	void nativeMidiControllerRecordingChecksDeliveryAndFirstSample_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("healthy") << 0; QTest::newRow("input-lost") << 1;
		QTest::newRow("delivery-failed") << 2;
	}
	void nativeMidiControllerRecordingChecksDeliveryAndFirstSample()
	{
		QFETCH(int, failure); const int parameter = static_cast<int>(m_model->id());
		QVERIFY(StudioCapabilities::execute("controller.midi.connect", {{"parameter", parameter}, {"inputController", 7}}).value("ok").toBool());
		auto controller = dynamic_cast<MidiController*>(m_model->controllerConnection()->getController()); QVERIFY(controller);
		MidiPortTestAccess::useClient(controller->midiPort(), &m_input);
		QVERIFY(StudioCapabilities::execute("midi.configure", {{"parameter", parameter}, {"inputChannel", 0}, {"readable", true}}).value("ok").toBool());
		m_input.feed(127); controller->currentValue(0); period(); controller->currentValue(0); period(); QCOMPARE(controller->currentValue(0), 1.f);
		m_model->setUseControllerValue(false);
		const auto before = curve(m_clip); int periods = 0; QTimer clock; clock.setInterval(1);
		QObject::connect(&clock, &QTimer::timeout, this, [&] {
			if (!Engine::getSong()->isRecording()) { return; }
			if (periods == 0) { m_input.feed(127); }
			if (periods == 4 && failure == 1) { m_input.active = false; }
			if (periods == 4 && failure == 2) { m_input.fail(); }
			period(); ++periods;
		}); clock.start(); const auto result = StudioCapabilities::execute("automation.record", options()); clock.stop();
		QCOMPARE(result.value("ok").toBool(), failure == 0); QVERIFY(!m_model->useControllerValue()); assertReleased();
		if (failure) { QCOMPARE(curve(m_clip), before); }
		else
		{
			QCOMPARE(m_clip->valueAt(0), m_model->maxValue());
			QCOMPARE(m_clip->valueAt(8), m_model->maxValue());
		}
	}

	void nativeFailureLeavesPriorCurveAndDisarmsTake_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("cancel") << 0; QTest::newRow("timeout") << 1;
		QTest::newRow("audio-lost") << 2; QTest::newRow("dsp-failed") << 3; QTest::newRow("binding-removed") << 4;
		QTest::newRow("transport-restarted") << 5; QTest::newRow("disarmed") << 6; QTest::newRow("point-budget") << 7;
	}
	void nativeFailureLeavesPriorCurveAndDisarmsTake()
	{
		QFETCH(int, failure); auto args = options(); const auto before = curve(m_clip); bool cancel = false; int periods = 0;
		if (failure == 1) { args.insert("timeoutMs", 30); } if (failure == 7) { args.insert("maximumPoints", 2); }
		QTimer clock; clock.setInterval(1); QObject::connect(&clock, &QTimer::timeout, this, [&] {
			m_model->setValue(periods < 3 ? 25 : 75);
			if (periods == 3)
			{
				if (failure == 0) { cancel = true; }
				if (failure == 2) { m_clock->reportProcessingFailure(AudioDevice::ProcessingFailure::Disconnected); }
				if (failure == 3) { Engine::audioEngine()->reportDspFailure(AudioEngine::DspFailure::InstrumentJobException); }
				if (failure == 4) { m_clip->removeObject(m_model); }
				if (failure == 5) { Engine::getSong()->playSong(); Engine::getSong()->record(); }
				if (failure == 6) { m_clip->setRecording(false); }
			}
			if (failure != 1) { period(); } ++periods;
		}); clock.start(); const auto result = StudioCapabilities::execute("automation.record", args, [&] { return cancel; }); clock.stop();
		QVERIFY(!result.value("ok").toBool());
		if (failure == 4) { QVERIFY(m_clip->addObject(m_model)); }
		QCOMPARE(curve(m_clip), before); assertReleased();
	}

	void captureStopsAtSongClipEndAndRejectsMutedOrUnboundTargets()
	{
		m_clip->setAutoResize(false); m_clip->changeLength(12); auto args = options(); args.insert("durationMs", 1000);
		QTimer clock; clock.setInterval(1); QObject::connect(&clock, &QTimer::timeout, this, [&] { m_model->setValue(25); period(); });
		clock.start(); const auto result = StudioCapabilities::execute("automation.record", args); clock.stop();
		QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
		QCOMPARE(result.value("result").toObject().value("stopReason").toString(), QString("clipEnd")); assertReleased();
		m_clip->setMuted(true); QVERIFY(!StudioCapabilities::execute("automation.record", args).value("ok").toBool());
		m_clip->setMuted(false); m_clip->clearObjects(); QVERIFY(!StudioCapabilities::execute("automation.record", args).value("ok").toBool());
	}

	void completedAutomationTakeHasDurableUndoAndReplay()
	{
		QTemporaryDir session, fixture; const auto project = fixture.filePath("prior.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(project));
		const auto take = request("take", "automation.record", options()); QByteArray prior, accepted;
		{
			StudioRuntime runtime(session.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("load", "project.load", {{"path", project}})).value("ok").toBool()); refresh(); prior = curve(m_clip);
			QTimer clock; clock.setInterval(1); QObject::connect(&clock, &QTimer::timeout, this, [&] {
				if (!Engine::getSong()->isRecording()) { return; } m_model->setValue(75); period();
			}); clock.start(); const auto result = runtime.process(take); clock.stop();
			QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString())); accepted = curve(m_clip); QVERIFY(accepted != prior);
			QVERIFY(runtime.process(take).value("replayed").toBool()); QCOMPARE(curve(m_clip), accepted);
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool()); refresh(); QCOMPARE(curve(m_clip), prior);
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool()); refresh(); QCOMPARE(curve(m_clip), accepted);
		}
		StudioRuntime recovered(session.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error)); refresh();
		QCOMPARE(curve(m_clip), accepted); QVERIFY(recovered.process(take).value("replayed").toBool()); assertReleased();
	}

	void reservedTakeRejectsOtherCaptureAndRendering()
	{
		m_clip->reserveRecordingTake(16, 0); m_clip->recordValue(12, 25);
		QVERIFY_EXCEPTION_THROWN(StudioRecording::requireIdle(), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(StudioRecording::requireNoCapture(), std::runtime_error);
		QVERIFY(!StudioCapabilities::execute("automation.record", options()).value("ok").toBool());
		QVERIFY(m_clip->hasReservedRecording());
		const auto state = StudioCapabilities::execute("transport.inspect", {}).value("result").toObject().value("recording").toObject();
		QCOMPARE(state.value("reservedAutomationClips").toInt(), 1);
		m_clip->discardRecordingTake(); assertReleased();
	}

	void cancelledAutomationTakeRestoresCheckpointAndHistory()
	{
		QTemporaryDir session, fixture; const auto project = fixture.filePath("prior.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(project));
		StudioRuntime runtime(session.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
		QVERIFY(runtime.process(request("load", "project.load", {{"path", project}})).value("ok").toBool()); refresh();
		const auto before = curve(m_clip); const auto prior = manifest(session); QVERIFY(!prior.isEmpty());
		int periods = 0; QTimer clock; clock.setInterval(1); QObject::connect(&clock, &QTimer::timeout, this, [&] {
			if (!Engine::getSong()->isRecording()) { return; }
			m_model->setValue(75); if (periods == 2) { runtime.process(request("cancel", "session.shutdown")); }
			period(); ++periods;
		}); clock.start(); const auto response = runtime.process(request("cancelled-take", "automation.record", options())); clock.stop();
		QVERIFY(!response.value("ok").toBool()); refresh(); QCOMPARE(curve(m_clip), before);
		QCOMPARE(manifest(session).value("history"), prior.value("history"));
		QCOMPARE(manifest(session).value("cursor"), prior.value("cursor")); assertReleased();
	}
};
QTEST_GUILESS_MAIN(StudioAutomationRecordingTest)
#include "StudioAutomationRecordingTest.moc"
