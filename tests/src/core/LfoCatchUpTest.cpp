/* Native LFO phase catch-up, seeks and marker-export PCM.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <thread>
#include <QDomDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "LfoController.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "Oscillator.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "ValueBuffer.h"
#include "lmms_math.h"

using namespace lmms;
namespace lmms
{
class LfoCatchUpTestAccess
{
public:
	// Controlled elapsed native clock, not an elapsed-time/resource benchmark.
	static void period(long value) { const auto changes = Engine::audioEngine()->requestChangesGuard(); Controller::s_periods = value; }
};
}
namespace
{
void configure(LfoController& controller, int multiplier = 0, int wave = static_cast<int>(Oscillator::WaveShape::Saw))
{
	QDomDocument document; auto element = document.createElement("lfocontroller"); element.setAttribute("type", 1);
	element.setAttribute("name", "Native phase fixture"); element.setAttribute("speed", 2); element.setAttribute("base", .5);
	element.setAttribute("amount", 1); element.setAttribute("phase", 37); element.setAttribute("multiplier", multiplier); element.setAttribute("wave", wave);
	controller.loadSettings(element);
}
double duration(int multiplier)
{
	float value = Engine::audioEngine()->outputSampleRate() * 2.f;
	if (multiplier == 1) { value /= 100.f; } else if (multiplier == 2) { value *= 100.f; }
	return value;
}
std::vector<float> values(Controller& controller)
{ const auto buffer = controller.valueBuffer(); return {buffer->begin(), buffer->end()}; }
}

class LfoCatchUpTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	long priorPeriod = 0;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
		Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); priorPeriod = Controller::runningPeriods(); LfoCatchUpTestAccess::period(0);
		Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false);
	}
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); LfoCatchUpTestAccess::period(priorPeriod); }

	void delayedReadsKeepFractionalPhaseAndNativeWaveform_data()
	{
		QTest::addColumn<qint64>("skipped"); QTest::addColumn<int>("multiplier");
		QTest::newRow("normal-delayed") << qint64(100000) << 0;
		QTest::newRow("fast-delayed") << qint64(100000) << 1;
		QTest::newRow("slow-delayed") << qint64(100000) << 2;
		QTest::newRow("wide-native-period-difference") << (qint64(std::numeric_limits<int>::max()) + 4096) << 0;
	}
	void delayedReadsKeepFractionalPhaseAndNativeWaveform()
	{
		QFETCH(qint64, skipped); QFETCH(int, multiplier);
		if (skipped >= std::numeric_limits<long>::max()) { QSKIP("This native long clock cannot represent the extended period fixture"); }
		LfoController controller(Engine::getSong()); configure(controller, multiplier); const auto initial = values(controller); QVERIFY(!initial.empty());
		const auto frames = Engine::audioEngine()->framesPerPeriod(); const auto cycle = duration(multiplier);
		LfoCatchUpTestAccess::period(static_cast<long>(skipped + 1)); const auto caught = values(controller); QCOMPARE(caught.size(), initial.size());
		const double phase = absFraction((static_cast<double>(skipped) + 1) * static_cast<double>(frames) / cycle + 37.f / 360.f);
		for (std::size_t index = 0; index < caught.size(); ++index)
		{
			const auto expected = .5f + .5f * Oscillator::sawSample(static_cast<float>(phase + index / cycle));
			QVERIFY(std::isfinite(caught[index])); QVERIFY2(std::abs(caught[index] - expected) < .0002f, "Skipped periods changed the native fractional waveform phase");
		}
		const auto range = std::minmax_element(caught.begin(), caught.end()); QVERIFY(*range.second > *range.first);
	}

	void lateNativeSeeksMatchPeriodicWaveSamples_data()
	{
		QTest::addColumn<int>("wave"); QTest::newRow("sine") << static_cast<int>(Oscillator::WaveShape::Sine);
		QTest::newRow("saw") << static_cast<int>(Oscillator::WaveShape::Saw);
	}
	void lateNativeSeeksMatchPeriodicWaveSamples()
	{
		QFETCH(int, wave); auto song = Engine::getSong(); song->playSong(); LfoController controller(song); configure(controller, 0, wave);
		song->setPlayPos(960000, Song::PlayMode::Song); const auto buffer = values(controller);
		const double phase = absFraction(static_cast<double>(song->getFrames()) / duration(0)); const float offset = 37.f / 360.f;
		for (std::size_t index = 0; index < buffer.size(); ++index)
		{
			const float samplePhase = static_cast<float>(phase + offset + index / duration(0));
			const float sample = wave == static_cast<int>(Oscillator::WaveShape::Sine) ? Oscillator::sinSample(samplePhase) : Oscillator::sawSample(samplePhase);
			QVERIFY(std::abs(buffer[index] - (.5f + .5f * sample)) < .0002f);
		}
		const auto range = std::minmax_element(buffer.begin(), buffer.end()); QVERIFY(*range.second > *range.first);
	}

	void workerPlaybackChangesResyncBeforeTheNextNativeRead_data()
	{ QTest::addColumn<bool>("jump"); QTest::newRow("playback-start") << false; QTest::newRow("playing-seek") << true; }
	void workerPlaybackChangesResyncBeforeTheNextNativeRead()
	{
		QFETCH(bool, jump); auto song = Engine::getSong(); LfoController controller(song);
		configure(controller, 0, static_cast<int>(Oscillator::WaveShape::Sine)); float observed = -1.f;
		song->setPlayPos(jump ? 0 : 960013, Song::PlayMode::Song);
		std::thread render([&] {
			const auto changes = Engine::audioEngine()->requestChangesGuard(); song->playSong();
			if (jump) { song->setPlayPos(960013, Song::PlayMode::Song); }
			observed = values(controller).front();
		}); render.join();
		const auto phase = absFraction(static_cast<double>(song->getFrames()) / duration(0));
		const auto expected = .5f + .5f * Oscillator::sinSample(static_cast<float>(phase + 37.f / 360.f));
		QVERIFY(std::abs(observed - expected) < .0002f);
	}

	void repeatedReadsReuseNativePeriodAndNextPeriodAdvances()
	{
		LfoController controller(Engine::getSong()); configure(controller); LfoCatchUpTestAccess::period(100001);
		const auto first = values(controller); QVERIFY(values(controller) == first); QCOMPARE(controller.currentValue(0), first.front());
		Controller::triggerFrameCounter(); const auto next = values(controller); QVERIFY(next != first);
		const float expected = static_cast<float>(absFraction(static_cast<double>(first.front()) + first.size() / duration(0)));
		QVERIFY(std::abs(next.front() - expected) < .0002f); QVERIFY(values(controller) == next);
	}

	void caughtUpWaveRemainsNativeParameterSampleData_data()
	{ QTest::addColumn<bool>("logarithmic"); QTest::newRow("linear") << false; QTest::newRow("logarithmic") << true; }
	void caughtUpWaveRemainsNativeParameterSampleData()
	{
		QFETCH(bool, logarithmic); LfoController controller(Engine::getSong()); configure(controller); FloatModel model(42, 1, 100, 0, nullptr);
		model.setScaleLogarithmic(logarithmic); model.setControllerConnection(new ControllerConnection(&controller));
		values(controller); LfoCatchUpTestAccess::period(100001); AutomatableModel::incrementPeriodCounter(); const auto buffer = model.valueBuffer(); QVERIFY(buffer);
		const auto raw = values(controller); QCOMPARE(buffer->length(), static_cast<int>(raw.size()));
		for (std::size_t index = 0; index < raw.size(); ++index)
		{ const float expected = model.scaledValue(model.minValue() + model.range() * raw[index]); QVERIFY(std::abs(buffer->values()[index] - expected) < .001f); }
	}

	void nativeMarkerRenderKeepsPeriodicModulationAfterLateSeekAndReload_data()
	{ QTest::addColumn<bool>("reload"); QTest::newRow("live-project") << false; QTest::newRow("saved-reloaded-project") << true; }
	void nativeMarkerRenderKeepsPeriodicModulationAfterLateSeekAndReload()
	{
		QFETCH(bool, reload); auto song = Engine::getSong(); QTemporaryDir directory; QVERIFY(directory.isValid());
		QVERIFY(StudioCapabilities::execute("song.set", {{"tempo", 600}, {"masterVolume", 100}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		auto clip = static_cast<SampleClip*>(song->tracks().front()->getClip(0)); clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(48000, SampleFrame(.25f)), 48000));
		QVERIFY(StudioCapabilities::execute("clip.edit", {{"track", 0}, {"clip", 0}, {"length", 192}, {"autoResize", false}}).value("ok").toBool());
		auto controller = new LfoController(song); song->addController(controller); configure(*controller, 0, static_cast<int>(Oscillator::WaveShape::Sine));
		Engine::mixer()->mixerChannel(0)->m_volumeModel.setControllerConnection(new ControllerConnection(controller));
		song->getTimeline(Song::PlayMode::Song).setLoopPoints(0, 192);
		QJsonObject args{{"path", directory.filePath("early.wav")}, {"sampleRate", 48000}, {"bitDepth", 24}, {"betweenMarkers", true}, {"timeoutMs", 30000}};
		const auto early = StudioCapabilities::execute("audio.render", args); QVERIFY2(early.value("ok").toBool(), qPrintable(early.value("error").toString()));
		const auto baseline = SampleBuffer::fromFile(directory.filePath("early.wav")); QVERIFY(baseline && !baseline->empty());
		QVERIFY(StudioCapabilities::execute("clip.edit", {{"track", 0}, {"clip", 0}, {"position", 960000}}).value("ok").toBool());
		song->getTimeline(Song::PlayMode::Song).setLoopPoints(960000, 960192);
		if (reload)
		{
			const auto path = directory.filePath("late.mmp"); QVERIFY(song->saveProjectFile(path)); QVERIFY(song->loadProject(path)); QVERIFY(!song->hasErrors());
			Engine::audioEngine()->audioDev()->stopProcessing(); QCOMPARE(song->controllers().size(), std::size_t{1});
			QVERIFY(Engine::mixer()->mixerChannel(0)->m_volumeModel.controllerConnection());
		}
		args.insert("path", directory.filePath("late.wav")); const auto late = StudioCapabilities::execute("audio.render", args); QVERIFY2(late.value("ok").toBool(), qPrintable(late.value("error").toString()));
		QCOMPARE(late.value("result").toObject().value("renderRange").toObject().value("begin").toInt(), 960000);
		const auto rendered = SampleBuffer::fromFile(directory.filePath("late.wav")); QVERIFY(rendered && !rendered->empty()); QCOMPARE(rendered->size(), baseline->size());
		float minimum = std::numeric_limits<float>::max(), maximum = -minimum;
		for (std::size_t index = 0; index < baseline->size(); ++index)
		{
			minimum = std::min(minimum, baseline->data()[index][0]); maximum = std::max(maximum, baseline->data()[index][0]);
			for (int channel = 0; channel < 2; ++channel)
			{ QVERIFY(std::isfinite(rendered->data()[index][channel])); QVERIFY2(std::abs(rendered->data()[index][channel] - baseline->data()[index][channel]) < .00001f, qPrintable(QString("Frame %1 channel %2: late=%3 early=%4").arg(index).arg(channel).arg(rendered->data()[index][channel], 0, 'g', 9).arg(baseline->data()[index][channel], 0, 'g', 9))); }
		}
		QVERIFY(maximum > .001f); QVERIFY(maximum - minimum > .001f); QVERIFY(!song->hasErrors());
	}
};
QTEST_GUILESS_MAIN(LfoCatchUpTest)
#include "LfoCatchUpTest.moc"
