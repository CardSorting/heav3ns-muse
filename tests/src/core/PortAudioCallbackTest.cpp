/* Native PortAudio input/DSP tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <limits>
#include <vector>

#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleRecordHandle.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "../../../src/core/audio/PortAudioCallback.h"

using namespace lmms;

class PortAudioCallbackTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	SampleClip* m_clip = nullptr;
	std::shared_ptr<const SampleBuffer> m_previous;

	static float inputValue(std::size_t frame, int channel)
	{
		return channel == 0 ? .125f + static_cast<float>(frame % 64) / 1024
			: -.25f - static_cast<float>((frame + channel) % 32) / 512;
	}

	void finishTake()
	{
		Engine::audioEngine()->finishSampleRecordings(); QString error; SampleRecordHandle::finalizePendingRecordings(error);
	}

private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init();
	}
	void cleanupTestCase() { NotePlayHandleManager::free(); }
	void init()
	{
		Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->clearProject();
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		m_clip = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		m_previous = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(.125f)), 24000); m_clip->setSampleBuffer(m_previous);
	}
	void cleanup() { Engine::destroy(); m_clip = nullptr; m_previous.reset(); }

	void actualCallbackPcmReachesNativeRecordingWithoutADeviceProbe_data()
	{
		QTest::addColumn<int>("inputChannels"); QTest::addColumn<int>("outputChannels"); QTest::addColumn<int>("frames");
		QTest::newRow("mono") << 1 << 2 << 777;
		QTest::newRow("stereo") << 2 << 2 << 777;
		QTest::newRow("four-channels") << 4 << 4 << 777;
		QTest::newRow("input-only") << 2 << 0 << 777;
		QTest::newRow("partial-period-mono-output") << 2 << 1 << 13;
	}

	void actualCallbackPcmReachesNativeRecordingWithoutADeviceProbe()
	{
		QFETCH(int, inputChannels); QFETCH(int, outputChannels); QFETCH(int, frames); const auto engine = Engine::audioEngine();
		std::vector<float> input(static_cast<std::size_t>(frames) * inputChannels), output(static_cast<std::size_t>(frames) * outputChannels, 123.f);
		for (int frame = 0; frame < frames; ++frame)
		{
			for (int channel = 0; channel < inputChannels; ++channel) { input[static_cast<std::size_t>(frame) * inputChannels + channel] = inputValue(frame, channel); }
		}
		QVERIFY(engine->addPlayHandle(new SampleRecordHandle(m_clip)));
		QCOMPARE(detail::processPortAudioBuffer(*engine, inputChannels, outputChannels, input.data(), output.empty() ? nullptr : output.data(), frames, false), paContinue);
		finishTake(); QVERIFY(!Engine::getSong()->hasErrors()); QCOMPARE(m_clip->sample().sampleSize(), static_cast<std::size_t>(frames));
		QCOMPARE(m_clip->sample().sampleRate(), static_cast<int>(engine->inputSampleRate()));
		for (int frame = 0; frame < frames; ++frame)
		{
			QCOMPARE(m_clip->sample().data()[frame].left(), inputValue(frame, 0));
			QCOMPARE(m_clip->sample().data()[frame].right(), inputValue(frame, inputChannels == 1 ? 0 : 1));
		}
		for (const auto value : output) { QCOMPARE(value, 0.f); }
	}

	void lostOrInvalidCallbackInputCannotPublishASurvivingPrefix_data()
	{
		QTest::addColumn<int>("fault");
		QTest::newRow("missing-input") << 0; QTest::newRow("reported-input-loss") << 1;
		QTest::newRow("nonfinite-last-frame") << 2; QTest::newRow("nonfinite-extra-channel") << 3;
	}

	void lostOrInvalidCallbackInputCannotPublishASurvivingPrefix()
	{
		QFETCH(int, fault); const auto engine = Engine::audioEngine(); const int channels = fault == 3 ? 4 : 2;
		std::vector<float> input(777 * channels, .25f), output(777 * 2, 123.f);
		if (fault >= 2) { input.back() = std::numeric_limits<float>::quiet_NaN(); }
		QVERIFY(engine->addPlayHandle(new SampleRecordHandle(m_clip))); const auto epoch = engine->inputErrorEpoch();
		QCOMPARE(detail::processPortAudioBuffer(*engine, channels, 2, fault == 0 ? nullptr : input.data(), output.data(), 777, fault == 1), paContinue);
		finishTake(); QVERIFY(engine->inputErrorEpoch() > epoch); QVERIFY(Engine::getSong()->hasErrors());
		QCOMPARE(m_clip->sample().toBase64(), m_previous->toBase64());
		for (const auto value : output) { QCOMPARE(value, 0.f); }
	}

	void invalidCallbackShapesAreRejectedBeforeBufferAccess_data()
	{
		QTest::addColumn<int>("shape");
		QTest::newRow("missing-output") << 0; QTest::newRow("native-frame-limit") << 1;
		QTest::newRow("native-channel-limit") << 2; QTest::newRow("no-channels") << 3;
	}

	void invalidCallbackShapesAreRejectedBeforeBufferAccess()
	{
		QFETCH(int, shape); float sentinel = .75f; const auto engine = Engine::audioEngine();
		const auto frames = shape == 1 ? static_cast<unsigned long>(std::numeric_limits<int>::max()) + 1ul : 1ul;
		const ch_cnt_t inputChannels = shape == 2 ? DynamicChannelCount : shape == 3 ? 0 : 2;
		const ch_cnt_t outputChannels = shape == 3 ? 0 : 2;
		QCOMPARE(detail::processPortAudioBuffer(*engine, inputChannels, outputChannels, &sentinel, shape == 0 ? nullptr : &sentinel, frames, false), paAbort);
		QCOMPARE(sentinel, .75f); QCOMPARE(m_clip->sample().toBase64(), m_previous->toBase64());
		QCOMPARE(detail::processPortAudioBuffer(*engine, 0, 0, nullptr, nullptr, 0, false), paContinue);
	}
};

QTEST_GUILESS_MAIN(PortAudioCallbackTest)
#include "PortAudioCallbackTest.moc"
