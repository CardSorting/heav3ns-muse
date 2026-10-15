/* Native LFO controller reads across device notification failure/recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include <sndfile.h>
#include "AudioDeviceTransitionTestUtils.h"
#include "AudioFileWave.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "LfoController.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "NativeValueNotification.h"
#include "ProjectRenderer.h"
#include "RemotePlugin.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"

using namespace lmms;
namespace
{
class ProbeLfo final : public LfoController
{
public:
	explicit ProbeLfo(Model* parent = nullptr) : LfoController(parent) { }
	TempoSyncKnobModel& speed() { return m_speedModel; }
	IntModel& multiplier() { return m_multiplierModel; }
	float duration() const { return m_duration; }
	void refresh() { updateDuration(); }
};
// Faults originate in an ordinary native controller getter. This adapter
// neither replaces LFO DSP nor throws from a signal-connected Qt slot.
class Input final : public Controller
{
public:
	Input() : Controller(ControllerType::Lfo, nullptr, "Native LFO timing input fixture") { }
	enum Mode { Healthy, Standard, Nonstandard, Nan, Infinite, Negative };
	std::atomic<int> mode{Healthy}, reads{0};
	std::atomic<float> next{.5f};
	std::function<void()> beforeRead;
	void publish() { notifyValueChange(); }
	float currentValue(int) override
	{
		++reads; if (auto callback = std::exchange(beforeRead, {})) { callback(); }
		switch (mode.load())
		{
		case Standard: throw std::runtime_error("Native LFO timing input fixture");
		case Nonstandard: throw 271;
		case Nan: return std::numeric_limits<float>::quiet_NaN();
		case Infinite: return std::numeric_limits<float>::infinity();
		case Negative: return -1.f;
		default: return next;
		}
	}
};
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool prior(const QString& path) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write("prior export") == 12; }
void arrange()
{
	Engine::getSong()->setTempo(240); Engine::getSong()->setMasterVolume(100);
	std::vector<SampleFrame> pcm(44100);
	for (std::size_t index = 0; index < pcm.size(); ++index)
	{
		pcm[index][0] = .4f * std::sin(6.283185307179586 * 440 * index / 44100);
		pcm[index][1] = .2f * std::sin(6.283185307179586 * 660 * index / 44100);
	}
	auto track = new SampleTrack(Engine::getSong()); auto clip = static_cast<SampleClip*>(track->createClip(TimePos()));
	clip->setSampleBuffer(std::make_shared<SampleBuffer>(pcm, 44100)); clip->changeLength(TimePos(192));
}
ProbeLfo* seed(Input& input)
{
	auto lfo = new ProbeLfo(Engine::getSong()); Engine::getSong()->addController(lfo);
	lfo->speed().setControllerConnection(new ControllerConnection(&input));
	Engine::mixer()->mixerChannel(0)->m_volumeModel.setControllerConnection(new ControllerConnection(lfo));
	return lfo;
}
float expected(ProbeLfo& lfo, Input& input, sample_rate_t rate, int multiplier = 0)
{
	float value = rate * (lfo.speed().minValue() + lfo.speed().range() * input.next.load());
	if (multiplier == 1) { value /= 100.f; } else if (multiplier == 2) { value *= 100.f; }
	return value;
}
QJsonObject render(const QString& path)
{
	return StudioCapabilities::execute("audio.render", {{"path", path}, {"sampleRate", 44100}, {"bitDepth", 24}, {"timeoutMs", 30000}});
}
bool decoded(const QString& path, int rate)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return false; } SF_INFO info{};
	std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close);
	if (!audio || info.channels != 2 || info.samplerate != rate || info.frames < rate / 2 || info.frames > rate * 10) { return false; }
	std::vector<float> pcm(static_cast<std::size_t>(info.frames) * 2);
	if (sf_readf_float(audio.get(), pcm.data(), info.frames) != info.frames || sf_error(audio.get())) { return false; }
	double squares[2]{};
	for (sf_count_t frame = 0; frame < info.frames; ++frame)
	{
		for (int channel = 0; channel < 2; ++channel)
		{
			const auto value = pcm[static_cast<std::size_t>(frame) * 2 + channel];
			if (!std::isfinite(value)) { return false; } squares[channel] += double(value) * value;
		}
	}
	const auto left = std::sqrt(squares[0] / info.frames), right = std::sqrt(squares[1] / info.frames);
	return left > .02 && right > .01 && std::abs(left / right - 2.) < .12;
}
void installWave(const QString& path, sample_rate_t rate)
{
	auto engine = Engine::audioEngine(); bool good = false;
	std::unique_ptr<AudioDevice> device(AudioFileWave::getInst(path, OutputSettings(rate, 160, OutputSettings::BitDepth::Depth24Bit), DEFAULT_CHANNELS, engine, good));
	if (!good || !device) { throw std::runtime_error("Native WAV device fixture initialization failed"); }
	try { engine->setAudioDevice(device.get(), false); }
	catch (...) { if (engine->audioDev() == device.get()) { device.release(); } throw; }
	device.release();
}
}
class LfoRateNotificationTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void released()
	{
		QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice()); QVERIFY(!Engine::getSong()->isExporting());
		QVERIFY(!Engine::getSong()->hasPendingExportCleanup()); QVERIFY(!RemotePlugin::hasActiveRenderContext());
	}
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
		Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors();
		QVERIFY(Engine::audioEngine()->resetDspFailure());
		Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false); Engine::getSong()->setLoopRenderCount(1); arrange();
	}
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); QVERIFY(Engine::audioEngine()->resetDspFailure()); released(); }

	void reportedRateFailuresFollowLaterObserversAndRetainDuration_data()
	{
		QTest::addColumn<int>("mode"); QTest::newRow("standard") << int(Input::Standard); QTest::newRow("nonstandard") << int(Input::Nonstandard);
		QTest::newRow("nan") << int(Input::Nan); QTest::newRow("infinite") << int(Input::Infinite); QTest::newRow("negative") << int(Input::Negative);
	}
	void reportedRateFailuresFollowLaterObserversAndRetainDuration()
	{
		QFETCH(int, mode); Input input; ProbeLfo lfo; lfo.speed().setControllerConnection(new ControllerConnection(&input));
		const auto previous = lfo.duration(); const auto engine = Engine::audioEngine(); int delivered = 0, caught = 0;
		const auto connection = connect(engine, &AudioEngine::sampleRateChanged, this, [&] { ++delivered; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); input.mode = mode;
		try { AudioDeviceTransitionTestAccess::notify(engine, false); }
		catch (const std::invalid_argument&) { caught = 3; }
		catch (const std::runtime_error&) { caught = 1; }
		catch (int value) { QCOMPARE(value, 271); caught = 2; }
		QCOMPARE(caught, mode == Input::Standard ? 1 : mode == Input::Nonstandard ? 2 : 3); QCOMPARE(delivered, 1);
		QCOMPARE(lfo.duration(), previous); input.mode = Input::Healthy; input.next = .25f;
		AudioDeviceTransitionTestAccess::notify(engine, false); QCOMPARE(delivered, 2); QCOMPARE(lfo.duration(), expected(lfo, input, engine->outputSampleRate()));
		QVERIFY(lfo.duration() != previous); Controller::invalidateValueBuffers();
		for (const auto value : *lfo.valueBuffer()) { QVERIFY(std::isfinite(value)); QVERIFY(value >= 0 && value <= 1); }
	}

	void rendererSetupFailuresKeepReservationAndPermitPcmRetry_data()
	{
		QTest::addColumn<int>("mode"); QTest::addColumn<bool>("studio");
		QTest::newRow("renderer-standard") << int(Input::Standard) << false; QTest::newRow("studio-standard") << int(Input::Standard) << true;
		QTest::newRow("renderer-nonstandard") << int(Input::Nonstandard) << false; QTest::newRow("studio-nonstandard") << int(Input::Nonstandard) << true;
		QTest::newRow("renderer-nan") << int(Input::Nan) << false; QTest::newRow("studio-nan") << int(Input::Nan) << true;
	}
	void rendererSetupFailuresKeepReservationAndPermitPcmRetry()
	{
		QFETCH(int, mode); QFETCH(bool, studio); Input input; auto lfo = seed(input); auto engine = Engine::audioEngine();
		QTemporaryDir output; const auto path = output.filePath("published.wav"); QVERIFY(prior(path)); const auto previous = lfo->duration();
		const auto repair = qScopeGuard([&] { input.mode = Input::Healthy; engine->restoreAudioDevice(); }); input.mode = mode;
		if (studio) { const auto response = render(path); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); }
		else
		{
			QVERIFY(engine->storeAudioDevice());
			ProjectRenderer renderer(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit), ProjectRenderer::ExportFileFormat::Wave, output.filePath("failed.wav"));
			QVERIFY(renderer.isReady()); QVERIFY(!renderer.startProcessing()); QVERIFY(!renderer.isRunning()); QVERIFY(renderer.hasExecutionFailure()); QVERIFY(!renderer.finalizeOutput());
			QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(!engine->restoreAudioDevice());
		}
		QCOMPARE(read(path), QByteArray("prior export")); QCOMPARE(lfo->duration(), previous); QVERIFY(engine->hasStoredAudioDevice());
		QCOMPARE(engine->deviceRestoreFailure(), AudioEngine::DeviceRestoreFailure::NotificationException);
		input.mode = Input::Healthy; QVERIFY(engine->restoreAudioDevice()); released();
		const auto response = render(path); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		QVERIFY(decoded(path, 44100)); released();
	}

	void failedRestorationRetriesOnlyTheRejectedPhase_data()
	{
		QTest::addColumn<int>("mode"); QTest::newRow("standard") << int(Input::Standard); QTest::newRow("nonstandard") << int(Input::Nonstandard); QTest::newRow("nan") << int(Input::Nan);
	}
	void failedRestorationRetriesOnlyTheRejectedPhase()
	{
		QFETCH(int, mode); Input input; auto lfo = seed(input); auto engine = Engine::audioEngine(); const auto original = engine->audioDev();
		QTemporaryDir output; QVERIFY(engine->storeAudioDevice()); const auto repair = qScopeGuard([&] { input.mode = Input::Healthy; engine->restoreAudioDevice(); });
		{
			const auto path = output.filePath("before.wav"); ProjectRenderer renderer(OutputSettings(48000, 160, OutputSettings::BitDepth::Depth24Bit), ProjectRenderer::ExportFileFormat::Wave, path);
			QVERIFY(renderer.isReady()); QVERIFY(renderer.startProcessing()); QVERIFY(renderer.wait(30000)); QVERIFY(renderer.finalizeOutput()); QVERIFY(decoded(path, 48000));
			const auto previous = lfo->duration(); input.mode = mode;
			QSignalSpy quality(engine, &AudioEngine::qualitySettingsChanged), rate(engine, &AudioEngine::sampleRateChanged);
			QVERIFY(!engine->restoreAudioDevice()); QVERIFY(engine->hasStoredAudioDevice()); QCOMPARE(engine->audioDev(), original);
			QCOMPARE(engine->deviceRestoreFailure(), AudioEngine::DeviceRestoreFailure::NotificationException); QCOMPARE(lfo->duration(), previous);
			QCOMPARE(quality.size(), 1); QCOMPARE(rate.size(), 1); input.mode = Input::Healthy; QVERIFY(engine->restoreAudioDevice());
			QCOMPARE(quality.size(), 1); QCOMPARE(rate.size(), 2); QCOMPARE(lfo->duration(), expected(*lfo, input, engine->outputSampleRate()));
		}
		const auto retry = output.filePath("retry.wav"); QVERIFY(render(retry).value("ok").toBool()); QVERIFY(decoded(retry, 44100)); released();
	}

	void unframedRateSignalsDiagnoseAndPermitGuardedRetry_data()
	{ QTest::addColumn<int>("mode"); QTest::newRow("standard") << int(Input::Standard); QTest::newRow("nonstandard") << int(Input::Nonstandard); }
	void unframedRateSignalsDiagnoseAndPermitGuardedRetry()
	{
		QFETCH(int, mode); Input input; ProbeLfo lfo; lfo.speed().setControllerConnection(new ControllerConnection(&input)); const auto previous = lfo.duration();
		input.mode = mode; QTest::ignoreMessage(QtWarningMsg, "LFO duration update failed outside a native device notification frame");
		QVERIFY(QMetaObject::invokeMethod(Engine::audioEngine(), "sampleRateChanged", Qt::DirectConnection)); QCOMPARE(lfo.duration(), previous);
		input.mode = Input::Healthy; input.next = .25f; AudioDeviceTransitionTestAccess::notify(Engine::audioEngine(), false);
		QCOMPARE(lfo.duration(), expected(lfo, input, Engine::audioEngine()->outputSampleRate()));
	}

	void ordinaryDurationCallsKeepTheirThrowingContract_data()
	{ QTest::addColumn<int>("mode"); QTest::newRow("standard") << int(Input::Standard); QTest::newRow("nonstandard") << int(Input::Nonstandard); }
	void ordinaryDurationCallsKeepTheirThrowingContract()
	{
		QFETCH(int, mode); Input input; ProbeLfo lfo; lfo.speed().setControllerConnection(new ControllerConnection(&input)); const auto previous = lfo.duration(); input.mode = mode;
		if (mode == Input::Standard) { QVERIFY_EXCEPTION_THROWN(lfo.refresh(), std::runtime_error); }
		else { QVERIFY_EXCEPTION_THROWN(lfo.refresh(), int); }
		QCOMPARE(lfo.duration(), previous); input.mode = Input::Healthy; lfo.refresh(); QCOMPARE(lfo.duration(), expected(lfo, input, Engine::audioEngine()->outputSampleRate()));
	}

	void aNestedDeviceChangeCannotPublishTheOlderDuration()
	{
		Input input; ProbeLfo lfo; lfo.speed().setControllerConnection(new ControllerConnection(&input)); auto engine = Engine::audioEngine(); QTemporaryDir output;
		const auto rate = engine->outputSampleRate() == 48000 ? 44100 : 48000; QVERIFY(engine->storeAudioDevice());
		const auto restore = qScopeGuard([&] { input.beforeRead = {}; engine->restoreAudioDevice(); }); bool changed = false;
		input.beforeRead = [&] { installWave(output.filePath("nested.wav"), rate); changed = true; };
		QVERIFY_EXCEPTION_THROWN(lfo.refresh(), std::runtime_error); QVERIFY(changed); QCOMPARE(engine->outputSampleRate(), rate);
		QCOMPARE(lfo.duration(), expected(lfo, input, rate)); QVERIFY(engine->restoreAudioDevice()); released();
	}

	void nativeValueChangesReturnGetterFailuresAfterQt_data()
	{
		QTest::addColumn<int>("source"); QTest::addColumn<int>("mode");
		QTest::newRow("speed-standard") << 0 << int(Input::Standard); QTest::newRow("multiplier-standard") << 1 << int(Input::Standard); QTest::newRow("controller-standard") << 2 << int(Input::Standard);
		QTest::newRow("speed-nonstandard") << 0 << int(Input::Nonstandard); QTest::newRow("multiplier-nonstandard") << 1 << int(Input::Nonstandard); QTest::newRow("controller-nonstandard") << 2 << int(Input::Nonstandard);
		QTest::newRow("speed-nan") << 0 << int(Input::Nan); QTest::newRow("multiplier-nan") << 1 << int(Input::Nan); QTest::newRow("controller-nan") << 2 << int(Input::Nan);
	}
	void nativeValueChangesReturnGetterFailuresAfterQt()
	{
		QFETCH(int, source); QFETCH(int, mode); Input input; ProbeLfo lfo; lfo.speed().setControllerConnection(new ControllerConnection(&input));
		const auto previous = lfo.duration(); int delivered = 0, caught = 0;
		AutomatableModel* model = source == 0 ? static_cast<AutomatableModel*>(&lfo.speed()) : &lfo.multiplier();
		const auto connection = source == 2 ? connect(&input, &Controller::valueChanged, this, [&] { ++delivered; }, Qt::DirectConnection)
			: connect(model, &Model::dataChanged, this, [&] { ++delivered; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); input.mode = mode;
		try { if (source == 0) { lfo.speed().setValue(3); } else if (source == 1) { lfo.multiplier().setValue(1); } else { input.publish(); } }
		catch (const std::invalid_argument&) { caught = 3; }
		catch (const std::runtime_error&) { caught = 1; }
		catch (int value) { QCOMPARE(value, 271); caught = 2; }
		QCOMPARE(caught, mode == Input::Standard ? 1 : mode == Input::Nonstandard ? 2 : 3); QCOMPARE(delivered, 1); QCOMPARE(lfo.duration(), previous);
		QVERIFY(!NativeValueNotification::report(std::make_exception_ptr(std::runtime_error("Outside native value frame"))));
		input.mode = Input::Healthy; input.next = .25f; lfo.refresh();
		QCOMPARE(lfo.duration(), expected(lfo, input, Engine::audioEngine()->outputSampleRate(), source == 1 ? 1 : 0));
	}

	void nativeDspPeriodsLatchReportedGetterFailuresAndAllowRepair_data()
	{
		QTest::addColumn<int>("mode"); QTest::newRow("standard") << int(Input::Standard); QTest::newRow("nonstandard") << int(Input::Nonstandard); QTest::newRow("nan") << int(Input::Nan);
	}
	void nativeDspPeriodsLatchReportedGetterFailuresAndAllowRepair()
	{
		QFETCH(int, mode); Input input; auto lfo = seed(input); auto engine = Engine::audioEngine(); auto song = Engine::getSong();
		const auto repair = qScopeGuard([&] { input.mode = Input::Healthy; song->stop(); engine->resetDspFailure(); });
		const auto previous = lfo->duration(); const auto period = Controller::runningPeriods(); int delivered = 0;
		const auto connection = connect(&input, &Controller::valueChanged, this, [&] { ++delivered; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); input.mode = mode; song->playSong();
		const auto silent = [](auto output) { return std::all_of(output.begin(), output.end(), [](const auto& frame) { return frame[0] == 0 && frame[1] == 0; }); };
		QVERIFY(silent(engine->renderNextPeriod())); QCOMPARE(delivered, 1); QCOMPARE(lfo->duration(), previous); QCOMPARE(Controller::runningPeriods(), period);
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::MixException); const auto failures = engine->dspStatus().failures;
		QVERIFY(silent(engine->renderNextPeriod())); QCOMPARE(delivered, 1); QCOMPARE(engine->dspStatus().failures, failures);
		input.mode = Input::Healthy; song->stop(); QVERIFY(engine->resetDspFailure()); QCOMPARE(engine->dspStatus().failures, failures); song->playSong();
		engine->renderNextPeriod(); const auto recovered = engine->renderNextPeriod(); QVERIFY(!silent(recovered));
		for (const auto& frame : recovered) { QVERIFY(!frame.containsNaN()); QVERIFY(!frame.containsInf()); }
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::None); QVERIFY(Controller::runningPeriods() > period);
	}

	void validFractionalDurationsAndMultipliersStayNative_data()
	{
		QTest::addColumn<int>("rate"); QTest::addColumn<int>("multiplier");
		QTest::newRow("44100-normal") << 44100 << 0; QTest::newRow("44100-fast") << 44100 << 1; QTest::newRow("44100-slow") << 44100 << 2;
		QTest::newRow("48000-normal") << 48000 << 0; QTest::newRow("48000-fast") << 48000 << 1; QTest::newRow("48000-slow") << 48000 << 2;
	}
	void validFractionalDurationsAndMultipliersStayNative()
	{
		QFETCH(int, rate); QFETCH(int, multiplier); Input input; ProbeLfo lfo; input.next = .12345f;
		lfo.speed().setControllerConnection(new ControllerConnection(&input)); lfo.multiplier().setValue(multiplier);
		auto engine = Engine::audioEngine(); QTemporaryDir output; QVERIFY(engine->storeAudioDevice()); const auto restore = qScopeGuard([&] { engine->restoreAudioDevice(); });
		installWave(output.filePath("rate.wav"), rate); QCOMPARE(lfo.duration(), expected(lfo, input, rate, multiplier)); QVERIFY(std::isfinite(lfo.duration())); QVERIFY(lfo.duration() > 0);
		Controller::invalidateValueBuffers(); for (const auto value : *lfo.valueBuffer()) { QVERIFY(std::isfinite(value)); QVERIFY(value >= 0 && value <= 1); }
		QVERIFY(engine->restoreAudioDevice()); released();
	}
};
QTEST_GUILESS_MAIN(LfoRateNotificationTest)
#include "LfoRateNotificationTest.moc"
