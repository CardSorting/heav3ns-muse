/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <array>
#include <cstring>
#include <limits>
#include "../../../plugins/GigPlayer/GigPcm.h"

namespace lmms
{
class GigPcmTest : public QObject
{
	Q_OBJECT
private slots:
	void native16BitBoundariesDecodeUnalignedMonoAndStereo_data()
	{
		QTest::addColumn<int>("channels");
		QTest::newRow("mono") << 1; QTest::newRow("stereo") << 2;
	}
	void native16BitBoundariesDecodeUnalignedMonoAndStereo()
	{
		QFETCH(int, channels);
		const std::array<std::int16_t, 6> values{-32768, 32767, 0, 1, -1, -16384};
		std::array<std::uint8_t, 25> storage{};
		for (std::size_t index = 0; index < values.size() * channels; ++index)
		{
			const auto value = values[index % values.size()]; std::memcpy(storage.data() + 1 + index * 2, &value, 2);
		}
		std::array<SampleFrame, 6> output;
		QVERIFY(gigPcm::decode(std::span(storage).subspan(1, values.size() * channels * 2), 16, channels, .5f, output));
		for (std::size_t frame = 0; frame < output.size(); ++frame)
		{
			QCOMPARE(output[frame][0], float(values[(frame * channels) % values.size()]) / 131072.f);
			QCOMPARE(output[frame][1], float(values[(frame * channels + (channels - 1)) % values.size()]) / 131072.f);
		}
	}

	void littleEndian24BitBoundariesKeepSignAndChannels_data()
	{
		QTest::addColumn<int>("channels");
		QTest::newRow("mono") << 1; QTest::newRow("stereo") << 2;
	}
	void littleEndian24BitBoundariesKeepSignAndChannels()
	{
		QFETCH(int, channels);
		const std::array<std::int32_t, 6> values{-8388608, 8388607, 0, 1, -1, -4194304};
		std::array<std::uint8_t, 36> input{};
		for (std::size_t index = 0; index < values.size() * channels; ++index)
		{
			const auto bits = static_cast<std::uint32_t>(values[index % values.size()]);
			input[index * 3] = bits & 0xff; input[index * 3 + 1] = (bits >> 8) & 0xff; input[index * 3 + 2] = (bits >> 16) & 0xff;
		}
		std::array<SampleFrame, 6> output;
		QVERIFY(gigPcm::decode(std::span(input).first(values.size() * channels * 3), 24, channels, 1.f, output));
		for (std::size_t frame = 0; frame < output.size(); ++frame)
		{
			QCOMPARE(output[frame][0], float(values[(frame * channels) % values.size()]) / 16777216.f);
			QCOMPARE(output[frame][1], float(values[(frame * channels + (channels - 1)) % values.size()]) / 16777216.f);
		}
	}

	void incompleteOrInvalidShapesClearPriorOutput_data()
	{
		QTest::addColumn<int>("depth"); QTest::addColumn<int>("channels"); QTest::addColumn<int>("bytes"); QTest::addColumn<float>("gain");
		QTest::newRow("short-16-mono") << 16 << 1 << 3 << 1.f;
		QTest::newRow("short-16-stereo") << 16 << 2 << 7 << 1.f;
		QTest::newRow("short-24-mono") << 24 << 1 << 5 << 1.f;
		QTest::newRow("short-24-stereo") << 24 << 2 << 11 << 1.f;
		QTest::newRow("8-bit") << 8 << 1 << 12 << 1.f;
		QTest::newRow("32-bit") << 32 << 1 << 12 << 1.f;
		QTest::newRow("no-channels") << 16 << 0 << 12 << 1.f;
		QTest::newRow("three-channels") << 16 << 3 << 12 << 1.f;
		QTest::newRow("nonfinite-gain") << 16 << 1 << 12 << std::numeric_limits<float>::infinity();
	}
	void incompleteOrInvalidShapesClearPriorOutput()
	{
		QFETCH(int, depth); QFETCH(int, channels); QFETCH(int, bytes); QFETCH(float, gain);
		std::array<std::uint8_t, 12> input; input.fill(0x7f);
		std::array<SampleFrame, 2> output; for (auto& frame : output) { frame[0] = .75f; frame[1] = -.75f; }
		QVERIFY(!gigPcm::decode(std::span(input).first(bytes), depth, channels, gain, output));
		for (const auto& frame : output) { QCOMPARE(frame[0], 0.f); QCOMPARE(frame[1], 0.f); }
	}
};
} // namespace lmms
QTEST_GUILESS_MAIN(lmms::GigPcmTest)
#include "GigPcmTest.moc"
