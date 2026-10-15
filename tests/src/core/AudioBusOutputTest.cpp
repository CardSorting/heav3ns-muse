/* Native period-aligned bus outputs. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "AutomatableModel.h"
#include "ConfigManager.h"
#include "Effect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "SampleTrack.h"
#include "Song.h"
#include "TrackRenderScope.h"

using namespace lmms;
namespace
{
enum class Fault { None, Exception, Nonfinite };

class RegistrationDevice final : public AudioDevice
{
public:
	RegistrationDevice() : AudioDevice(DEFAULT_CHANNELS, Engine::audioEngine()) { }
	bool failing = false, nonstandard = false;
	void registerPort(AudioBusHandle*) override
	{
		if (!failing) { return; }
		if (nonstandard) { throw 17; }
		throw std::runtime_error("Native bus registration fixture");
	}
protected:
	void startProcessingImpl() override { }
	void stopProcessingImpl() override { }
};

float ramp(f_cnt_t frame) { return static_cast<float>(frame % 1009 + 1) / 8192.f; }

class NativePlay final : public PlayHandle
{
public:
	explicit NativePlay(AudioBusHandle* bus) : PlayHandle(Type::SamplePlayHandle) { setAudioBusHandle(bus); }
	std::atomic<int> calls{0};
	std::atomic<Fault> fault{Fault::None};
	bool useRamp = false;
	SampleFrame value{.25f, -.125f};
	std::function<void()> onPlay;
	bool isFinished() const override { return false; }
	bool affinityMatters() const override { return true; }
	bool isFromTrack(const Track*) const override { return false; }
	void play(SampleFrame* output) override
	{
		++calls;
		if (onPlay) { onPlay(); }
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		for (f_cnt_t frame = 0; frame < frames; ++frame)
		{
			const auto v = ramp(m_produced + frame);
			output[frame] = useRamp ? SampleFrame(v, -v / 2.f) : value;
		}
		m_produced += frames;
		if (fault == Fault::Exception) { throw std::runtime_error("Native bus output fixture"); }
		if (fault == Fault::Nonfinite) { output[frames - 1][1] = std::numeric_limits<float>::quiet_NaN(); }
	}
private:
	f_cnt_t m_produced = 0;
};

const Plugin::Descriptor effectDescriptor{"bus-output-fixture", "Bus output fixture", "Native processing fixture", "LMMS tests",
	1, Plugin::Type::Effect, nullptr, "", nullptr};

class NativeEffect final : public Effect
{
public:
	explicit NativeEffect(EffectChain* chain) : Effect(&effectDescriptor, chain, nullptr) { }
	std::atomic<Fault> fault{Fault::None};
	float gain = 1.f;
	SampleFrame tail{};
	EffectControls* controls() override { return nullptr; }
protected:
	ProcessStatus processImpl(SampleFrame* output, f_cnt_t frames) override
	{
		if (fault == Fault::Exception) { throw std::runtime_error("Native bus effect fixture"); }
		for (f_cnt_t frame = 0; frame < frames; ++frame)
		{
			output[frame][0] = output[frame][0] * gain + tail[0];
			output[frame][1] = output[frame][1] * gain + tail[1];
		}
		if (fault == Fault::Nonfinite) { output[frames - 1][0] = std::numeric_limits<float>::infinity(); }
		return ProcessStatus::Continue;
	}
};

struct GuardedPlanar
{
	GuardedPlanar(ch_cnt_t channels, f_cnt_t frames) : count(frames), storage(channels, std::vector<float>(frames + 2, 37.f))
	{ for (auto& channel : storage) { pointers.push_back(channel.data() + 1); } }
	GuardedPlanar(const GuardedPlanar&) = delete;
	GuardedPlanar& operator=(const GuardedPlanar&) = delete;
	PlanarBufferView<float> view() { return {pointers.data(), static_cast<ch_cnt_t>(pointers.size()), count}; }
	float sample(ch_cnt_t channel, f_cnt_t frame) const { return storage[channel][frame + 1]; }
	bool all(float value = 0.f) const
	{
		for (const auto& channel : storage)
		{ if (!std::all_of(channel.begin() + 1, channel.end() - 1, [=](float sample) { return sample == value; })) { return false; } }
		return true;
	}
	bool guards() const
	{
		return std::all_of(storage.begin(), storage.end(), [](const auto& channel) { return channel.front() == 37.f && channel.back() == 37.f; });
	}
	f_cnt_t count;
	std::vector<std::vector<float>> storage;
	std::vector<float*> pointers;
};

void render(GuardedPlanar& master, GuardedPlanar& bus, const std::shared_ptr<const AudioBusOutput>& source)
{
	const std::array destinations{AudioEngine::BusDestination{source, bus.view()}};
	Engine::audioEngine()->renderNextBuffer(master.view(), destinations);
}

bool waitFor(const std::atomic_bool& flag)
{
	QElapsedTimer timer; timer.start();
	while (!flag.load() && timer.elapsed() < 2000) { std::this_thread::yield(); }
	return flag.load();
}
}

class AudioBusOutputTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void configure(const QString& frames = "256")
	{
		ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("ui", "disableautoquit", "1");
		ConfigManager::inst()->setValue("audioengine", "framesperaudiobuffer", frames);
	}
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid());
		NotePlayHandleManager::init();
	}
	void cleanupTestCase() { NotePlayHandleManager::free(); }
	void init()
	{
		configure();
		Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing();
		Engine::getSong()->setLoadOnLaunch(false); Engine::getSong()->clearProject(); Engine::audioEngine()->setMasterGain(1.f);
	}
	void cleanup() { Engine::destroy(); }

	void callbacksShareTheDelayedMasterCursor_data()
	{
		QTest::addColumn<int>("chunk"); QTest::addColumn<int>("channels");
		for (const int chunk : {1, 17, 255, 256, 257, 511, 777, 1024})
		{
			for (const int channels : {1, 2, 4})
			{ QTest::newRow(qPrintable(QString("%1-frames-%2-channels").arg(chunk).arg(channels))) << chunk << channels; }
		}
	}
	void callbacksShareTheDelayedMasterCursor()
	{
		QFETCH(int, chunk); QFETCH(int, channels);
		const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		AudioBusHandle bus("Aligned bus"); const auto source = bus.externalOutput();
		auto play = new NativePlay(&bus); play->useRamp = true;
		QVERIFY(engine->addPlayHandle(play)); const auto release = qScopeGuard([&] { engine->removePlayHandle(play); });
		const auto total = period * 6 + 19;
		for (f_cnt_t offset = 0; offset < total; )
		{
			const auto frames = std::min<f_cnt_t>(chunk, total - offset);
			GuardedPlanar master(channels, frames), tap(2, frames); render(master, tap, source);
			for (f_cnt_t frame = 0; frame < frames; ++frame)
			{
				const auto v = offset + frame < period ? 0.f : ramp(offset + frame - period);
				QCOMPARE(tap.sample(0, frame), v); QCOMPARE(tap.sample(1, frame), -v / 2.f);
				for (int channel = 0; channel < channels; ++channel)
				{ QCOMPARE(master.sample(channel, frame), channels == 1 ? v / 4.f : channel == 0 ? v : channel == 1 ? -v / 2.f : 0.f); }
			}
			QVERIFY(master.guards() && tap.guards()); offset += frames;
		}
		QCOMPARE(play->calls.load(), static_cast<int>((total + period - 1) / period));
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::None);
	}

	void nativeGainPanAndEffectsPrecedeIndependentMixerAndMasterOutput()
	{
		const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		FloatModel volume(50.f, 0.f, 200.f, 1.f), pan(50.f, -100.f, 100.f, 1.f);
		AudioBusHandle a("First", true, &volume, &pan), b("Second");
		const auto sourceA = a.externalOutput(), sourceB = b.externalOutput();
		auto first = new NativePlay(&a), second = new NativePlay(&b);
		first->value = SampleFrame(.5f, .25f); second->value = SampleFrame(.125f, -.25f);
		QVERIFY(engine->addPlayHandle(first)); const auto releaseFirst = qScopeGuard([&] { engine->removePlayHandle(first); });
		QVERIFY(engine->addPlayHandle(second)); const auto releaseSecond = qScopeGuard([&] { engine->removePlayHandle(second); });
		auto effect = new NativeEffect(a.effects()); effect->gain = .5f; QVERIFY(a.effects()->appendEffect(effect));
		const auto masterChannel = Engine::mixer()->mixerChannel(0); masterChannel->m_volumeModel.setInitValue(.5f);
		auto masterEffect = new NativeEffect(&masterChannel->m_fxChain); masterEffect->gain = .25f;
		QVERIFY(masterChannel->m_fxChain.appendEffect(masterEffect)); engine->setMasterGain(2.f);
		GuardedPlanar master(2, period * 2), tapA(2, period * 2), tapB(2, period * 2), duplicate(2, period * 2), missing(2, period * 2);
		const std::array destinations{AudioEngine::BusDestination{sourceA, tapA.view()}, AudioEngine::BusDestination{sourceB, tapB.view()},
			AudioEngine::BusDestination{sourceA, duplicate.view()}, AudioEngine::BusDestination{{}, missing.view()}};
		engine->renderNextBuffer(master.view(), destinations);
		for (f_cnt_t frame = 0; frame < period * 2; ++frame)
		{
			const bool audible = frame >= period;
			QCOMPARE(tapA.sample(0, frame), audible ? .0625f : 0.f); QCOMPARE(tapA.sample(1, frame), audible ? .0625f : 0.f);
			QCOMPARE(tapB.sample(0, frame), audible ? .125f : 0.f); QCOMPARE(tapB.sample(1, frame), audible ? -.25f : 0.f);
			QCOMPARE(duplicate.sample(0, frame), tapA.sample(0, frame)); QCOMPARE(duplicate.sample(1, frame), tapA.sample(1, frame));
			QCOMPARE(master.sample(0, frame), audible ? .046875f : 0.f); QCOMPARE(master.sample(1, frame), audible ? -.046875f : 0.f);
		}
		QVERIFY(missing.all()); QVERIFY(master.guards() && tapA.guards() && tapB.guards() && duplicate.guards() && missing.guards());
	}

	void mutedAndRenderExcludedBusesDiscardTheirPreviousPeriod_data()
	{
		QTest::addColumn<bool>("excluded"); QTest::newRow("native-mute") << false; QTest::newRow("native-stem-exclusion") << true;
	}
	void mutedAndRenderExcludedBusesDiscardTheirPreviousPeriod()
	{
		QFETCH(bool, excluded); const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		const auto selected = new SampleTrack(Engine::getSong()), target = new SampleTrack(Engine::getSong());
		const auto source = target->audioBusHandle()->externalOutput(); auto play = new NativePlay(target->audioBusHandle());
		QVERIFY(engine->addPlayHandle(play)); const auto release = qScopeGuard([&] { engine->removePlayHandle(play); });
		GuardedPlanar warmMaster(2, period * 2), warmTap(2, period * 2); render(warmMaster, warmTap, source); QVERIFY(!warmTap.all());
		std::unique_ptr<TrackRenderScope> scope;
		if (excluded) { scope = std::make_unique<TrackRenderScope>(selected); } else { target->setMuted(true); }
		GuardedPlanar master(2, period * 2), tap(2, period * 2); render(master, tap, source);
		for (f_cnt_t frame = 0; frame < period * 2; ++frame)
		{
			QCOMPARE(tap.sample(0, frame), frame < period ? .25f : 0.f); QCOMPARE(tap.sample(1, frame), frame < period ? -.125f : 0.f);
			QCOMPARE(master.sample(0, frame), tap.sample(0, frame)); QCOMPARE(master.sample(1, frame), tap.sample(1, frame));
		}
		scope.reset(); target->setMuted(false);
		GuardedPlanar resumedMaster(2, period * 2), resumedTap(2, period * 2); render(resumedMaster, resumedTap, source);
		for (f_cnt_t frame = 0; frame < period * 2; ++frame) { QCOMPARE(resumedTap.sample(0, frame), frame < period ? 0.f : .25f); }
	}

	void disableAndEnableDoNotReplayACachedPartialPeriod()
	{
		const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		AudioBusHandle bus("Switchable"); const auto source = bus.externalOutput(); auto play = new NativePlay(&bus);
		QVERIFY(engine->addPlayHandle(play)); const auto release = qScopeGuard([&] { engine->removePlayHandle(play); });
		GuardedPlanar warmMaster(2, period + 7), warmTap(2, period + 7); render(warmMaster, warmTap, source);
		bus.setExtOutputEnabled(false); const auto calls = play->calls.load();
		GuardedPlanar master(2, period - 7), tap(2, period - 7); render(master, tap, source);
		QVERIFY(tap.all()); QVERIFY(!master.all()); QCOMPARE(play->calls.load(), calls);
		bus.setExtOutputEnabled(true); QCOMPARE(bus.externalOutput().get(), source.get());
		GuardedPlanar resumedMaster(2, period * 2), resumedTap(2, period * 2); render(resumedMaster, resumedTap, source);
		for (f_cnt_t frame = 0; frame < period * 2; ++frame) { QCOMPARE(resumedTap.sample(0, frame), frame < period ? 0.f : .25f); }
	}

	void acquiringAnOutputMidPeriodStartsWithSilence()
	{
		const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		AudioBusHandle bus("Late consumer"); auto play = new NativePlay(&bus);
		QVERIFY(engine->addPlayHandle(play)); const auto release = qScopeGuard([&] { engine->removePlayHandle(play); });
		GuardedPlanar warm(2, period + 7); engine->renderNextBuffer(warm.view());
		const auto source = bus.externalOutput();
		GuardedPlanar master(2, period * 3 - 7), tap(2, period * 3 - 7); render(master, tap, source);
		for (f_cnt_t frame = 0; frame < tap.count; ++frame) { QCOMPARE(tap.sample(0, frame), frame < period * 2 - 7 ? 0.f : .25f); }
		QCOMPARE(play->calls.load(), 4);
	}

	void aRetainedDestinationCannotFollowAReusedBusAddress()
	{
		const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		alignas(AudioBusHandle) std::array<std::byte, sizeof(AudioBusHandle)> storage;
		auto bus = new (storage.data()) AudioBusHandle("Former track"); NativePlay* play = nullptr;
		const auto release = qScopeGuard([&] { if (play) { engine->removePlayHandle(play); } if (bus) { bus->~AudioBusHandle(); } });
		const auto retired = bus->externalOutput(); play = new NativePlay(bus); QVERIFY(engine->addPlayHandle(play));
		GuardedPlanar warmMaster(2, period + 7), warmTap(2, period + 7); render(warmMaster, warmTap, retired);
		engine->removePlayHandle(play); play = nullptr; bus->~AudioBusHandle(); bus = nullptr;
		bus = new (storage.data()) AudioBusHandle("Replacement track"); const auto replacement = bus->externalOutput();
		QVERIFY(retired.get() != replacement.get()); play = new NativePlay(bus); play->value = SampleFrame(.5f, -.25f);
		QVERIFY(engine->addPlayHandle(play));
		GuardedPlanar master(2, period * 3 - 7), oldTap(2, period * 3 - 7), newTap(2, period * 3 - 7);
		const std::array destinations{AudioEngine::BusDestination{retired, oldTap.view()}, AudioEngine::BusDestination{replacement, newTap.view()}};
		engine->renderNextBuffer(master.view(), destinations); QVERIFY(oldTap.all());
		for (f_cnt_t frame = 0; frame < newTap.count; ++frame) { QCOMPARE(newTap.sample(0, frame), frame < period * 2 - 7 ? 0.f : .5f); }
	}

	void aLateNativeFailureSilencesEveryDestinationAndRecovers_data()
	{
		QTest::addColumn<int>("stage"); QTest::addColumn<bool>("nonfinite"); QTest::addColumn<bool>("sanitize");
		for (int stage = 0; stage < 3; ++stage)
		{
			for (bool nonfinite : {false, true})
			{
				for (bool sanitize : {false, true})
				{ QTest::newRow(qPrintable(QString("stage-%1-nonfinite-%2-sanitize-%3").arg(stage).arg(nonfinite).arg(sanitize))) << stage << nonfinite << sanitize; }
			}
		}
	}
	void aLateNativeFailureSilencesEveryDestinationAndRecovers()
	{
		QFETCH(int, stage); QFETCH(bool, nonfinite); QFETCH(bool, sanitize);
		const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod(); engine->setSanitizationEnabled(sanitize);
		AudioBusHandle bus("Faulted"); const auto source = bus.externalOutput(); auto play = new NativePlay(&bus);
		QVERIFY(engine->addPlayHandle(play)); const auto release = qScopeGuard([&] { engine->removePlayHandle(play); });
		NativeEffect* effect = nullptr;
		if (stage)
		{
			const auto chain = stage == 1 ? bus.effects() : &Engine::mixer()->mixerChannel(0)->m_fxChain;
			effect = new NativeEffect(chain); QVERIFY(chain->appendEffect(effect));
		}
		GuardedPlanar warmMaster(2, period + 7), warmTap(2, period + 7); render(warmMaster, warmTap, source); QVERIFY(!warmTap.all());
		const auto fault = nonfinite ? Fault::Nonfinite : Fault::Exception;
		if (effect) { effect->fault = fault; } else { play->fault = fault; }
		GuardedPlanar master(2, period * 2), tap(2, period * 2); render(master, tap, source);
		QVERIFY(master.all() && tap.all()); QVERIFY(master.guards() && tap.guards());
		const auto expected = nonfinite ? (stage ? AudioEngine::DspFailure::CorruptedEffect : AudioEngine::DspFailure::NonfiniteOutput)
			: stage == 0 ? AudioEngine::DspFailure::InstrumentJobException : stage == 1 ? AudioEngine::DspFailure::EffectJobException : AudioEngine::DspFailure::MixerJobException;
		QCOMPARE(engine->dspStatus().failure, expected); const auto calls = play->calls.load();
		render(master, tap, source); QVERIFY(master.all() && tap.all()); QCOMPARE(play->calls.load(), calls);
		if (effect) { effect->fault = Fault::None; } else { play->fault = Fault::None; }
		QVERIFY(engine->resetDspFailure());
		GuardedPlanar recoveredMaster(2, period * 3), recoveredTap(2, period * 3); render(recoveredMaster, recoveredTap, source);
		QCOMPARE(recoveredTap.sample(0, recoveredTap.count - 1), .25f); QCOMPARE(recoveredMaster.sample(0, recoveredMaster.count - 1), .25f);
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::None);
	}

	void legacyMasterDestinationsAlsoDiscardAnAudiblePrefix_data()
	{
		QTest::addColumn<bool>("planar"); QTest::addColumn<int>("channels");
		for (bool planar : {false, true})
		{
			for (int channels : {1, 2, 4})
			{ QTest::newRow(qPrintable(QString("planar-%1-channels-%2").arg(planar).arg(channels))) << planar << channels; }
		}
	}
	void legacyMasterDestinationsAlsoDiscardAnAudiblePrefix()
	{
		QFETCH(bool, planar); QFETCH(int, channels);
		const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		AudioBusHandle bus("Legacy destination"); auto play = new NativePlay(&bus);
		QVERIFY(engine->addPlayHandle(play)); const auto release = qScopeGuard([&] { engine->removePlayHandle(play); });
		GuardedPlanar warm(2, period + 7); engine->renderNextBuffer(warm.view()); QVERIFY(!warm.all()); play->fault = Fault::Exception;
		if (planar)
		{
			GuardedPlanar master(channels, period * 2); engine->renderNextBuffer(master.view()); QVERIFY(master.all() && master.guards());
		}
		else
		{
			std::vector<float> output(channels * period * 2 + 2, 37.f);
			engine->renderNextBuffer(InterleavedBufferView<float>{output.data() + 1, static_cast<ch_cnt_t>(channels), period * 2});
			QVERIFY(std::all_of(output.begin() + 1, output.end() - 1, [](float value) { return value == 0.f; }));
			QCOMPARE(output.front(), 37.f); QCOMPARE(output.back(), 37.f);
		}
	}

	void malformedDestinationsDoNotConsumeOrOverwriteAudio_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto fault : {"bus-mono", "bus-empty-channels", "bus-frames", "bus-null-array", "bus-null-channel", "master-empty-channels", "master-null-array", "master-null-channel"})
		{ QTest::newRow(fault) << QString(fault); }
	}
	void malformedDestinationsDoNotConsumeOrOverwriteAudio()
	{
		QFETCH(QString, fault); const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		AudioBusHandle bus("Invalid destination"); const auto source = bus.externalOutput(); auto play = new NativePlay(&bus);
		QVERIFY(engine->addPlayHandle(play)); const auto release = qScopeGuard([&] { engine->removePlayHandle(play); });
		GuardedPlanar master(2, period * 2), good(2, period * 2), bad(2, period * 2);
		auto masterView = master.view(), badView = bad.view();
		std::array<float*, 2> nullChannel{bad.pointers[0], nullptr};
		if (fault == "bus-mono") { badView = {bad.pointers.data(), 1, bad.count}; }
		if (fault == "bus-empty-channels") { badView = {bad.pointers.data(), 0, bad.count}; }
		if (fault == "bus-frames") { badView = {bad.pointers.data(), 2, bad.count - 1}; }
		if (fault == "bus-null-array") { badView = {nullptr, 2, bad.count}; }
		if (fault == "bus-null-channel") { badView = {nullChannel.data(), 2, bad.count}; }
		if (fault == "master-empty-channels") { masterView = {master.pointers.data(), 0, master.count}; }
		if (fault == "master-null-array") { masterView = {nullptr, 2, master.count}; }
		if (fault == "master-null-channel") { masterView = {nullChannel.data(), 2, master.count}; }
		const std::array destinations{AudioEngine::BusDestination{source, good.view()}, AudioEngine::BusDestination{source, badView}};
		QVERIFY_EXCEPTION_THROWN(engine->renderNextBuffer(masterView, destinations), std::invalid_argument);
		QCOMPARE(play->calls.load(), 0); QVERIFY(master.all(37.f) && good.all(37.f) && bad.all(37.f));
		render(master, good, source); QCOMPARE(good.sample(0, 0), 0.f); QCOMPARE(good.sample(0, period), .25f);
	}

	void nativeEffectTailsContinueAfterThePlayHandleEnds()
	{
		const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		AudioBusHandle bus("Effect tail"); const auto source = bus.externalOutput(); auto play = new NativePlay(&bus);
		QVERIFY(engine->addPlayHandle(play)); const auto release = qScopeGuard([&] { if (play) { engine->removePlayHandle(play); } });
		auto effect = new NativeEffect(bus.effects()); effect->gain = .5f; effect->tail = SampleFrame(.0625f, -.03125f);
		QVERIFY(bus.effects()->appendEffect(effect));
		GuardedPlanar warmMaster(2, period * 2), warmTap(2, period * 2); render(warmMaster, warmTap, source);
		engine->removePlayHandle(play); play = nullptr;
		GuardedPlanar master(2, period * 3), tap(2, period * 3); render(master, tap, source);
		for (f_cnt_t frame = 0; frame < tap.count; ++frame)
		{
			QCOMPARE(tap.sample(0, frame), frame < period ? .1875f : .0625f);
			QCOMPARE(tap.sample(1, frame), frame < period ? -.09375f : -.03125f);
			QCOMPARE(master.sample(0, frame), tap.sample(0, frame));
		}
	}

	void nullAndZeroLengthDestinationsDoNotInventAudio()
	{
		const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		AudioBusHandle bus("Zero length"); auto play = new NativePlay(&bus);
		QVERIFY(engine->addPlayHandle(play)); const auto release = qScopeGuard([&] { engine->removePlayHandle(play); });
		const std::array empty{AudioEngine::BusDestination{{}, {nullptr, 2, 0}}};
		engine->renderNextBuffer(PlanarBufferView<float>{nullptr, 2, 0}, empty); QCOMPARE(play->calls.load(), 0);
		GuardedPlanar master(2, period * 2), missing(2, period * 2); render(master, missing, {});
		QVERIFY(missing.all()); QVERIFY(!master.all()); QVERIFY(master.guards() && missing.guards());
	}

	void onlyTheModelOwnerCanAcquireAnOutput()
	{
		AudioBusHandle bus("Owner"); bool rejected = false;
		std::thread worker([&] { try { bus.externalOutput(); } catch (const std::logic_error&) { rejected = true; } }); worker.join();
		QVERIFY(rejected); const auto source = bus.externalOutput(); QVERIFY(source); QCOMPARE(source.get(), bus.externalOutput().get());
	}

	void retirementWaitsForTheNativeRenderAndThenSilencesTheRetainedSource()
	{
		const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		auto bus = std::make_unique<AudioBusHandle>("Retiring during render"); const auto source = bus->externalOutput();
		auto play = new NativePlay(bus.get()); QVERIFY(engine->addPlayHandle(play));
		const auto releasePlay = qScopeGuard([&] { if (play) { engine->removePlayHandle(play); } });
		std::atomic_bool entered{false}, release{false}, attempting{false}, retired{false}, held{false}, failed{false};
		play->onPlay = [&] { entered = true; while (!release.load()) { std::this_thread::yield(); } };
		GuardedPlanar master(2, period * 2), tap(2, period * 2);
		std::thread renderer([&] { try { render(master, tap, source); } catch (...) { failed = true; } });
		const auto joinRender = qScopeGuard([&] { release = true; if (renderer.joinable()) { renderer.join(); } });
		QVERIFY(waitFor(entered));
		std::thread releaser([&]
		{
			if (waitFor(attempting)) { QThread::msleep(20); held = !retired.load(); }
			release = true;
		});
		const auto joinRelease = qScopeGuard([&] { if (releaser.joinable()) { releaser.join(); } });
		attempting = true; engine->removePlayHandle(play); play = nullptr; bus.reset(); retired = true;
		renderer.join(); releaser.join(); QVERIFY(held.load() && !failed.load());
		render(master, tap, source); QVERIFY(tap.all()); QVERIFY(master.guards() && tap.guards());
	}

	void aFaultEpochCannotResurrectCachedAudioAfterReset_data()
	{
		QTest::addColumn<bool>("resetFirst"); QTest::addColumn<bool>("legacy");
		QTest::newRow("latched-bus") << false << false; QTest::newRow("reset-bus") << true << false;
		QTest::newRow("latched-master") << false << true; QTest::newRow("reset-master") << true << true;
	}
	void aFaultEpochCannotResurrectCachedAudioAfterReset()
	{
		QFETCH(bool, resetFirst); QFETCH(bool, legacy); const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		AudioBusHandle bus("Cached fault"); const auto source = bus.externalOutput(); auto play = new NativePlay(&bus);
		QVERIFY(engine->addPlayHandle(play)); const auto release = qScopeGuard([&] { engine->removePlayHandle(play); });
		GuardedPlanar warmMaster(2, period + 7), warmTap(2, period + 7); render(warmMaster, warmTap, source); const auto calls = play->calls.load();
		engine->reportDspFailure(AudioEngine::DspFailure::InstrumentJobException);
		if (resetFirst) { QVERIFY(engine->resetDspFailure()); }
		GuardedPlanar master(2, 3), tap(2, 3);
		if (legacy) { engine->renderNextBuffer(master.view()); } else { render(master, tap, source); QVERIFY(tap.all()); }
		QVERIFY(master.all()); QCOMPARE(play->calls.load(), calls);
		QVERIFY(engine->resetDspFailure()); render(master, tap, source); QVERIFY(master.all() && tap.all()); QCOMPARE(play->calls.load(), calls);
		GuardedPlanar recoveredMaster(2, period * 3), recoveredTap(2, period * 3); render(recoveredMaster, recoveredTap, source);
		QCOMPARE(recoveredTap.sample(0, recoveredTap.count - 1), .25f);
	}

	void aRetainedOutputSurvivesEngineRetirementAsSilence()
	{
		auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		const auto track = new SampleTrack(Engine::getSong()); const auto source = track->audioBusHandle()->externalOutput();
		auto play = new NativePlay(track->audioBusHandle()); QVERIFY(engine->addPlayHandle(play));
		const auto release = qScopeGuard([&] { if (play) { engine->removePlayHandle(play); } });
		GuardedPlanar warmMaster(2, period * 2), warmTap(2, period * 2); render(warmMaster, warmTap, source); QVERIFY(!warmTap.all());
		engine->removePlayHandle(play); play = nullptr; Engine::destroy();
		configure("128"); Engine::init(true); engine = Engine::audioEngine();
		engine->audioDev()->stopProcessing(); QVERIFY(engine->framesPerPeriod() != period);
		GuardedPlanar master(2, engine->framesPerPeriod()), tap(2, engine->framesPerPeriod()); render(master, tap, source);
		QVERIFY(master.all() && tap.all());
	}

	void recursiveNativeRenderCannotConsumeThePendingBusPeriod()
	{
		const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		AudioBusHandle bus("Reentry"); const auto source = bus.externalOutput(); auto play = new NativePlay(&bus);
		QVERIFY(engine->addPlayHandle(play)); const auto release = qScopeGuard([&] { engine->removePlayHandle(play); });
		GuardedPlanar nestedMaster(2, period), nestedTap(2, period); int rejected = 0;
		const auto connection = QObject::connect(engine, &AudioEngine::nextAudioBuffer, engine, [&]
		{
			try { render(nestedMaster, nestedTap, source); }
			catch (const std::logic_error&) { ++rejected; }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		GuardedPlanar master(2, period * 2), tap(2, period * 2); render(master, tap, source);
		QCOMPARE(rejected, 2); QVERIFY(nestedMaster.all(37.f) && nestedTap.all(37.f));
		QCOMPARE(tap.sample(0, 0), 0.f); QCOMPARE(tap.sample(0, period), .25f); QCOMPARE(play->calls.load(), 2);
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::None);
	}

	void failedBusConstructionCannotLeaveAnEngineJobBehind_data()
	{
		QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard-exception") << false; QTest::newRow("nonstandard-exception") << true;
	}
	void failedBusConstructionCannotLeaveAnEngineJobBehind()
	{
		QFETCH(bool, nonstandard); const auto engine = Engine::audioEngine(); const auto period = engine->framesPerPeriod();
		auto device = std::make_unique<RegistrationDevice>(); device->nonstandard = nonstandard; const auto backend = device.get();
		engine->setAudioDevice(device.release(), false); backend->failing = true; bool failed = false;
		try { AudioBusHandle rejected("Failed registration"); }
		catch (const std::runtime_error&) { failed = !nonstandard; }
		catch (int value) { failed = nonstandard && value == 17; }
		QVERIFY(failed); GuardedPlanar empty(2, period); engine->renderNextBuffer(empty.view()); QVERIFY(empty.all());
		backend->failing = false; AudioBusHandle bus("Recovered registration"); const auto source = bus.externalOutput();
		auto play = new NativePlay(&bus); QVERIFY(engine->addPlayHandle(play));
		const auto release = qScopeGuard([&] { engine->removePlayHandle(play); });
		GuardedPlanar master(2, period * 2), tap(2, period * 2); render(master, tap, source);
		QCOMPARE(tap.sample(0, 0), 0.f); QCOMPARE(tap.sample(0, period), .25f); QCOMPARE(master.sample(0, period), .25f);
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::None);
	}
};

QTEST_GUILESS_MAIN(AudioBusOutputTest)
#include "AudioBusOutputTest.moc"
