/* Native parameter/controller caches after explicit DSP failure reset.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "LfoController.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "PlayHandle.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "ValueBuffer.h"

using namespace lmms;
namespace
{
class CacheController final : public Controller
{
public:
	CacheController() : Controller(ControllerType::Dummy, nullptr, "Native buffer recovery fixture") { setSampleExact(true); }
	float next = .25f;
	bool fail = false;
	int updates = 0;
protected:
	void updateValueBuffer() override
	{
		++updates; m_valueBuffer.fill(next); m_bufferLastUpdated = s_periods;
		if (fail) { throw std::runtime_error("Native controller cache update fixture"); }
	}
};
class ParameterPlay final : public PlayHandle
{
public:
	ParameterPlay(AudioBusHandle* bus, FloatModel* model) : PlayHandle(Type::SamplePlayHandle), parameter(model) { setAudioBusHandle(bus); }
	FloatModel* parameter;
	bool fail = true, nonstandard = false;
	int calls = 0;
	float last = 0;
	bool isFinished() const override { return false; }
	bool affinityMatters() const override { return true; }
	bool isFromTrack(const Track*) const override { return false; }
	void play(SampleFrame* output) override
	{
		++calls; const auto buffer = parameter->valueBuffer();
		for (f_cnt_t frame = 0; frame < Engine::audioEngine()->framesPerPeriod(); ++frame)
		{
			last = buffer ? buffer->values()[frame] : parameter->value();
			output[frame] = SampleFrame(.25f * last / parameter->maxValue());
		}
		if (fail) { if (nonstandard) { throw 17; } throw std::runtime_error("Native failed period populated parameter cache"); }
	}
};
void cc(MidiController& controller, int value) { controller.processInEvent(MidiEvent(MidiControlChange, 0, 74, value), {}); }
MidiController* bind(FloatModel& model)
{
	auto controller = new MidiController(nullptr); controller->midiPort()->setInputController(74); model.setControllerConnection(new ControllerConnection(controller));
	cc(*controller, 63); Controller::triggerFrameCounter(); controller->currentValue(0); Controller::triggerFrameCounter(); controller->currentValue(0); return controller;
}
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory)
{ QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
bool silent(std::span<const SampleFrame> output)
{ return std::all_of(output.begin(), output.end(), [](const auto& frame) { return frame[0] == 0 && frame[1] == 0; }); }
}

class DspCacheRecoveryTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors();
		QVERIFY(Engine::audioEngine()->resetDspFailure());
	}
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); QVERIFY(Engine::audioEngine()->resetDspFailure()); }

	void actualFailedPeriodRefreshesOwnedMidiAndParameterBuffers_data()
	{
		QTest::addColumn<bool>("logarithmic"); QTest::addColumn<bool>("nonstandard");
		QTest::newRow("linear-standard") << false << false; QTest::newRow("log-standard") << true << false;
		QTest::newRow("linear-nonstandard") << false << true; QTest::newRow("log-nonstandard") << true << true;
	}
	void actualFailedPeriodRefreshesOwnedMidiAndParameterBuffers()
	{
		QFETCH(bool, logarithmic); QFETCH(bool, nonstandard); const auto engine = Engine::audioEngine(); FloatModel parameter(42, 1, 100, 0, nullptr);
		parameter.setScaleLogarithmic(logarithmic); auto controller = bind(parameter); AudioBusHandle bus("Native cache recovery");
		auto play = new ParameterPlay(&bus, &parameter); play->nonstandard = nonstandard; QVERIFY(engine->addPlayHandle(play));
		const auto remove = qScopeGuard([&] { engine->removePlayHandle(play); }); const auto period = Controller::runningPeriods();
		QVERIFY(silent(engine->renderNextPeriod())); QCOMPARE(play->calls, 1); QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::InstrumentJobException);
		QCOMPARE(Controller::runningPeriods(), period); const auto failures = engine->dspStatus().failures; const float previous = play->last; QVERIFY(previous < 100);
		cc(*controller, 127); const auto stale = parameter.valueBuffer(); QVERIFY(stale); QCOMPARE(stale->values()[stale->length() - 1], previous);
		play->fail = false; const auto reset = StudioCapabilities::execute("audio.processing.reset", {}); QVERIFY2(reset.value("ok").toBool(), qPrintable(reset.value("error").toString()));
		QCOMPARE(engine->dspStatus().failures, failures); QCOMPARE(Controller::runningPeriods(), period); QCOMPARE(parameter.storedValue(), 42.f);
		const auto raw = controller->valueBuffer(); const float rawLast = raw->values()[raw->length() - 1]; QVERIFY(rawLast > 63.f / 127.f);
		const float expected = parameter.scaledValue(parameter.minValue() + parameter.range() * rawLast);
		const auto refreshed = parameter.valueBuffer(); QVERIFY(refreshed); QCOMPARE(refreshed->values()[refreshed->length() - 1], expected); QVERIFY(expected > previous);
		engine->renderNextPeriod(); QCOMPARE(play->calls, 2); QCOMPARE(play->last, expected); const auto recovered = engine->renderNextPeriod(); QVERIFY(!silent(recovered)); QCOMPARE(play->last, 100.f);
		for (const auto& frame : recovered) { QVERIFY(!frame.containsNaN()); QVERIFY(!frame.containsInf()); }
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::None);
	}

	void scalarNullAndInterpolationCachesRecomputeAfterRepair_data()
	{ QTest::addColumn<bool>("interpolated"); QTest::newRow("cached-null") << false; QTest::newRow("cached-interpolation") << true; }
	void scalarNullAndInterpolationCachesRecomputeAfterRepair()
	{
		QFETCH(bool, interpolated); FloatModel model(.25f, 0, 1, 0, nullptr); const auto engine = Engine::audioEngine();
		if (interpolated) { model.setValue(.5f); }
		const auto cached = model.valueBuffer(); QCOMPARE(cached != nullptr, interpolated); const auto period = Controller::runningPeriods();
		engine->reportDspFailure(AudioEngine::DspFailure::NoteSetupException); model.setValue(.75f); QCOMPARE(model.valueBuffer(), cached);
		QVERIFY(engine->resetDspFailure()); QCOMPARE(Controller::runningPeriods(), period); const auto buffer = model.valueBuffer(); QVERIFY(buffer);
		ValueBuffer expected(buffer->length()); expected.interpolate(interpolated ? .5f : .25f, .75f);
		QCOMPARE(buffer->values()[0], expected.values()[0]); QCOMPARE(buffer->values()[buffer->length() - 1], expected.values()[expected.length() - 1]); QCOMPARE(model.valueBuffer(), buffer);
		AutomatableModel::incrementPeriodCounter(); QVERIFY(!model.valueBuffer()); QCOMPARE(model.value(), .75f);
	}

	void linkedParameterCachesFollowFreshNativeControllerSamples_data()
	{ QTest::addColumn<bool>("logarithmic"); QTest::newRow("linear") << false; QTest::newRow("logarithmic") << true; }
	void linkedParameterCachesFollowFreshNativeControllerSamples()
	{
		QFETCH(bool, logarithmic); FloatModel source(42, 1, 100, 0, nullptr), target(42, 1, 100, 0, nullptr);
		source.setScaleLogarithmic(logarithmic); target.setScaleLogarithmic(logarithmic); auto controller = bind(source); target.linkToModel(&source);
		AutomatableModel::incrementPeriodCounter(); // Observe the new native link on its next normal parameter period.
		const auto old = target.valueBuffer(); QVERIFY(old); const float previous = old->values()[old->length() - 1]; QVERIFY(previous < 100);
		Engine::audioEngine()->reportDspFailure(AudioEngine::DspFailure::MixException); cc(*controller, 127);
		QCOMPARE(target.valueBuffer()->values()[old->length() - 1], previous); QVERIFY(Engine::audioEngine()->resetDspFailure());
		const auto raw = controller->valueBuffer(); const float rawLast = raw->values()[raw->length() - 1]; QVERIFY(rawLast > 63.f / 127.f);
		const float expected = source.scaledValue(source.minValue() + source.range() * rawLast);
		const auto fresh = target.valueBuffer(); QVERIFY(fresh); QCOMPARE(fresh->values()[fresh->length() - 1], expected); QVERIFY(expected > previous);
		QCOMPARE(source.valueBuffer()->values()[fresh->length() - 1], expected); QCOMPARE(target.storedValue(), 42.f);
	}

	void noFaultResetPreservesCurrentCachesAndNormalNextPeriod()
	{
		CacheController controller; FloatModel model(42, 1, 100, 0, nullptr); model.setControllerConnection(new ControllerConnection(&controller));
		Controller::triggerFrameCounter(); const auto cached = model.valueBuffer(); QVERIFY(cached); QCOMPARE(controller.updates, 1); const float previous = cached->values()[0];
		controller.next = .75f; const auto period = Controller::runningPeriods(); QVERIFY(Engine::audioEngine()->resetDspFailure());
		QCOMPARE(Controller::runningPeriods(), period); QCOMPARE(model.valueBuffer()->values()[0], previous); QCOMPARE(controller.updates, 1);
		Controller::triggerFrameCounter(); AutomatableModel::incrementPeriodCounter(); QVERIFY(model.valueBuffer()->values()[0] > previous); QCOMPARE(controller.updates, 2);
	}

	void refusedResetsLeaveCachedStateUntilEligibleRetry_data()
	{
		QTest::addColumn<QString>("reason"); QTest::newRow("foreign-thread") << QString("thread");
		QTest::newRow("playing") << QString("playing"); QTest::newRow("stored-device") << QString("device");
	}
	void refusedResetsLeaveCachedStateUntilEligibleRetry()
	{
		QFETCH(QString, reason); CacheController controller; FloatModel model(42, 1, 100, 0, nullptr); model.setControllerConnection(new ControllerConnection(&controller));
		Controller::triggerFrameCounter(); const float previous = model.valueBuffer()->values()[0]; const auto engine = Engine::audioEngine();
		engine->reportDspFailure(AudioEngine::DspFailure::MixException); controller.next = .75f; bool accepted = true;
		if (reason == "thread") { std::thread worker([&] { accepted = engine->resetDspFailure(); }); worker.join(); }
		else if (reason == "playing") { Engine::getSong()->playSong(); accepted = engine->resetDspFailure(); Engine::getSong()->stop(); }
		else { QVERIFY(engine->storeAudioDevice()); accepted = engine->resetDspFailure(); QVERIFY(engine->restoreAudioDevice()); engine->audioDev()->stopProcessing(); }
		QVERIFY(!accepted); QCOMPARE(model.valueBuffer()->values()[0], previous); QCOMPARE(controller.updates, 1);
		QVERIFY(engine->resetDspFailure()); QVERIFY(model.valueBuffer()->values()[0] > previous); QCOMPARE(controller.updates, 2);
	}

	void failedRecomputationCannotValidateAStaleGeneration()
	{
		CacheController controller; FloatModel model(42, 1, 100, 0, nullptr); model.setControllerConnection(new ControllerConnection(&controller));
		Controller::triggerFrameCounter(); QVERIFY(model.valueBuffer()); QCOMPARE(controller.updates, 1);
		Engine::audioEngine()->reportDspFailure(AudioEngine::DspFailure::MixException); QVERIFY(Engine::audioEngine()->resetDspFailure());
		controller.fail = true; controller.next = .75f; const auto period = Controller::runningPeriods();
		QVERIFY_EXCEPTION_THROWN(model.valueBuffer(), std::runtime_error); QCOMPARE(controller.updates, 2);
		controller.fail = false; const auto fresh = model.valueBuffer(); QVERIFY(fresh); QCOMPARE(controller.updates, 3);
		QCOMPARE(fresh->values()[0], model.minValue() + model.range() * .75f); QCOMPARE(Controller::runningPeriods(), period);
		model.valueBuffer(); QCOMPARE(controller.updates, 3);
	}

	void resetReceiptReplayCannotInvalidateNewFailureOrLiveCaches()
	{
		QTemporaryDir directory; StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error));
		CacheController controller; FloatModel model(42, 1, 100, 0, nullptr); model.setControllerConnection(new ControllerConnection(&controller));
		Controller::triggerFrameCounter(); QVERIFY(model.valueBuffer()); const auto engine = Engine::audioEngine(); engine->reportDspFailure(AudioEngine::DspFailure::MixException);
		controller.next = .75f; const auto before = manifest(directory); const auto reset = request("reset", "audio.processing.reset");
		const auto response = runtime.process(reset); QVERIFY(response.value("ok").toBool()); const float cached = model.valueBuffer()->values()[0]; QCOMPARE(controller.updates, 2);
		engine->reportDspFailure(AudioEngine::DspFailure::MixException); controller.next = .1f; const auto failures = engine->dspStatus().failures;
		const auto replay = runtime.process(reset); QVERIFY(replay.value("replayed").toBool()); QCOMPARE(replay.value("result"), response.value("result"));
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::MixException); QCOMPARE(engine->dspStatus().failures, failures);
		QCOMPARE(model.valueBuffer()->values()[0], cached); QCOMPARE(controller.updates, 2); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QVERIFY(runtime.process(request("fresh-reset", "audio.processing.reset")).value("ok").toBool()); QVERIFY(model.valueBuffer()->values()[0] < cached); QCOMPARE(controller.updates, 3);
	}

	void nativeLfoRecomputesOnceWithoutRewindingMusicalClock()
	{
		LfoController controller(nullptr); Controller::triggerFrameCounter(); const auto first = *controller.valueBuffer(); const auto period = Controller::runningPeriods();
		Engine::audioEngine()->reportDspFailure(AudioEngine::DspFailure::MixException); QVERIFY(Engine::audioEngine()->resetDspFailure());
		const auto next = *controller.valueBuffer(); QVERIFY(next != first); QCOMPARE(Controller::runningPeriods(), period); QVERIFY(*controller.valueBuffer() == next);
		for (const float value : next) { QVERIFY(std::isfinite(value)); QVERIFY(value >= 0 && value <= 1); }
	}
};
QTEST_GUILESS_MAIN(DspCacheRecoveryTest)
#include "DspCacheRecoveryTest.moc"
