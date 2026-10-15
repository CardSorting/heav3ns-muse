/* Native capture-buffer tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <algorithm>
#include <cstring>
#include <limits>
#include "SampleRecordingBuffer.h"
#include "SampleBuffer.h"

namespace lmms
{
class SampleRecordingBufferTest : public QObject
{
	Q_OBJECT
private slots:
	void nativeInputBlocksAreOwnedAndRetainEveryChannelAndSourceRate()
	{
		std::vector<SampleFrame> input(16703);
		for (std::size_t frame = 0; frame < input.size(); ++frame)
		{
			input[frame] = {static_cast<float>(frame % 257) / 128.f, -static_cast<float>(frame % 127) / 64.f};
		}
		input[0].left() = -0.f; const auto expected = input;
		SampleRecordingBuffer recording; std::size_t offset = 0;
		for (const auto count : {1, 511, 4096, 2091, 10004})
		{
			QVERIFY(recording.append(input.data() + offset, count, 24000)); offset += count;
		}
		QCOMPARE(offset, expected.size()); std::fill(input.begin(), input.end(), SampleFrame(.75f));
		QString error("Old error"); const auto result = recording.finalize(error);
		QVERIFY2(result, qPrintable(error)); QVERIFY(error.isEmpty()); QCOMPARE(result->size(), expected.size());
		QCOMPARE(result->sampleRate(), sample_rate_t(24000));
		QVERIFY(std::memcmp(result->data(), expected.data(), expected.size() * sizeof(SampleFrame)) == 0);
		QVERIFY(result->audioFile().isEmpty() && result->acceptedFile().isEmpty());
	}

	void invalidInputStopsTheTakeWithoutPublishingItsPrefix_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("null-input") << 0; QTest::newRow("zero-rate") << 1;
		QTest::newRow("rate-change") << 2; QTest::newRow("late-nan") << 3; QTest::newRow("other-channel-infinity") << 4;
	}
	void invalidInputStopsTheTakeWithoutPublishingItsPrefix()
	{
		QFETCH(int, failure); const std::vector<SampleFrame> first(512, SampleFrame(.25f, -.125f));
		SampleRecordingBuffer recording; QVERIFY(recording.append(first.data(), first.size(), 24000));
		SampleFrame next(.5f, -.5f);
		if (failure == 3) { next.left() = std::numeric_limits<float>::quiet_NaN(); }
		if (failure == 4) { next.right() = std::numeric_limits<float>::infinity(); }
		QVERIFY(!recording.append(failure == 0 ? nullptr : &next, 1, failure == 1 ? 0 : failure == 2 ? 48000 : 24000));
		QVERIFY(recording.failed() && !recording.error().isEmpty()); QCOMPARE(recording.framesRecorded(), f_cnt_t(512));
		QVERIFY(!recording.append(first.data(), first.size(), 24000));
		QString error; QVERIFY(!recording.finalize(error)); QCOMPARE(error, recording.error());
	}

	void framePlanningRejectsOverflowWithoutAllocatingAnEnormousTake()
	{
		const auto maximum = std::min(static_cast<f_cnt_t>(std::numeric_limits<int>::max()), static_cast<f_cnt_t>(std::vector<SampleFrame>().max_size()));
		QVERIFY(SampleRecordingBuffer::canAppendFrames(0, 0));
		QVERIFY(SampleRecordingBuffer::canAppendFrames(maximum - 1, 1));
		QVERIFY(!SampleRecordingBuffer::canAppendFrames(maximum, 1));
		QVERIFY(!SampleRecordingBuffer::canAppendFrames(maximum - 1, 2));
		QVERIFY(!SampleRecordingBuffer::canAppendFrames(std::numeric_limits<f_cnt_t>::max(), 0));
		QVERIFY(!SampleRecordingBuffer::canAppendFrames(0, std::numeric_limits<f_cnt_t>::max()));
	}

	void emptyInputDoesNotInventAudioOrPinItsRate()
	{
		SampleRecordingBuffer recording; QVERIFY(recording.append(nullptr, 0, 0));
		QCOMPARE(recording.framesRecorded(), f_cnt_t(0)); QCOMPARE(recording.sampleRate(), sample_rate_t(0));
		QString error; QVERIFY(!recording.finalize(error)); QVERIFY(!error.isEmpty());
		const SampleFrame actual(.25f); QVERIFY(recording.append(&actual, 1, 48000));
		const auto result = recording.finalize(error); QVERIFY(result && error.isEmpty()); QCOMPARE(result->sampleRate(), sample_rate_t(48000));
	}

	void movingCaptureOwnershipRetainsTheNativeTake()
	{
		SampleRecordingBuffer original; const std::vector<SampleFrame> frames(1537, SampleFrame(.25f, -.5f));
		QVERIFY(original.append(frames.data(), frames.size(), 24000));
		SampleRecordingBuffer moved(std::move(original)); QString error; const auto result = moved.finalize(error);
		QVERIFY(result && error.isEmpty()); QCOMPARE(result->size(), frames.size()); QCOMPARE(result->sampleRate(), sample_rate_t(24000));
		QVERIFY(std::memcmp(result->data(), frames.data(), frames.size() * sizeof(SampleFrame)) == 0);
	}
};
}

QTEST_GUILESS_MAIN(lmms::SampleRecordingBufferTest)
#include "SampleRecordingBufferTest.moc"
