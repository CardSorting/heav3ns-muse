/* Native DrumSynth regression tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>
#include "DrumSynth.h"
#include "FileResourceIO.h"

namespace lmms
{
class DrumSynthTest : public QObject
{
	Q_OBJECT
	static QByteArray tone()
	{
		return "[General]\nVersion=DrumSynth v2.0\nStretch=100\nLevel=0\n"
			"[Tone]\nOn=1\nLevel=100\nF1=440\nF2=440\nDroop=0\nPhase=90\nEnvelope=0,100 44100,0\n"
			"[Noise]\nOn=0\n[Overtones]\nOn=0\n[NoiseBand]\nOn=0\n[NoiseBand2]\nOn=0\n"
			"[Distortion]\nOn=0\nRate=0\nBits=0\n";
	}
	static bool write(const QString& path, const QByteArray& bytes)
	{
		QFile file(path);
		return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
	}
	static std::vector<int16_t> decode(DrumSynth& synth, const QString& path, int channels, sample_rate_t rate)
	{
		int16_t* samples = nullptr;
		const auto frames = synth.GetDSFileSamples(path, samples, channels, rate);
		const std::unique_ptr<int16_t[]> owner(samples);
		if (frames <= 0 || !samples) { return {}; }
		return {samples, samples + static_cast<std::size_t>(frames) * channels};
	}
private slots:
	void nativeToneRetainsRateDurationAndDuplicatedChannels_data()
	{
		QTest::addColumn<int>("rate"); QTest::addColumn<int>("channels"); QTest::addColumn<bool>("versionOne");
		QTest::newRow("v1-mono-44100") << 44100 << 1 << true;
		QTest::newRow("v2-stereo-24000") << 24000 << 2 << false;
		QTest::newRow("v2-six-channels-96000") << 96000 << 6 << false;
	}
	void nativeToneRetainsRateDurationAndDuplicatedChannels()
	{
		QFETCH(int, rate); QFETCH(int, channels); QFETCH(bool, versionOne);
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto path = directory.filePath(QString(QChar(0x9f13)) + ".ds");
		auto profile = tone(); if (versionOne) { profile.replace("v2.0", "v1.0"); }
		QVERIFY(write(path, profile)); DrumSynth synth;
		const auto samples = decode(synth, path, channels, rate); QVERIFY(!samples.empty());
		const auto frames = samples.size() / channels;
		QVERIFY(frames >= static_cast<std::size_t>(rate + 1200));
		QVERIFY(frames <= static_cast<std::size_t>(rate + 2400));
		int positiveCrossings = 0, peak = 0;
		for (std::size_t frame = 0; frame < frames; ++frame)
		{
			for (int channel = 1; channel < channels; ++channel)
			{
				QCOMPARE(samples[frame * channels + channel], samples[frame * channels]);
			}
			peak = std::max(peak, std::abs(static_cast<int>(samples[frame * channels])));
			if (frame > 0 && frame < static_cast<std::size_t>(rate / 2)
				&& samples[(frame - 1) * channels] <= 0 && samples[frame * channels] > 0) { ++positiveCrossings; }
		}
		QVERIFY(peak > 5000); QVERIFY(positiveCrossings >= 219 && positiveCrossings <= 221);
		QVERIFY(QFile::remove(path));
	}

	void cancellationDuringNativeGenerationDoesNotPublishAndAllowsReuse()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("tone.ds");
		QVERIFY(write(path, tone())); DrumSynth synth; int checks = 0;
		std::vector<int16_t> baseline;
		{ FileResourceIO::Scope scope([&] { ++checks; return false; }); baseline = decode(synth, path, 2, 44100); }
		QVERIFY(!baseline.empty()); QVERIFY(checks > 100);
		// The final checks are native 1,200-frame generation blocks and publication.
		const auto stopAt = checks - 16; int stoppedChecks = 0; int16_t* unpublished = nullptr;
		{
			FileResourceIO::Scope scope([&] { return ++stoppedChecks >= stopAt; });
			QCOMPARE(synth.GetDSFileSamples(path, unpublished, 2, 44100), 0);
		}
		QVERIFY(unpublished == nullptr); QCOMPARE(stoppedChecks, stopAt);
		QVERIFY(decode(synth, path, 2, 44100) == baseline);
		QVERIFY(QFile::remove(path));
	}

	void nestedNativeDecoderKeepsTheOuterWorkingState()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto outerPath = directory.filePath("outer.ds"), innerPath = directory.filePath("inner.ds");
		QVERIFY(write(outerPath, tone()));
		auto other = tone(); other.replace("F1=440", "F1=880").replace("F2=440", "F2=880").replace("Level=100", "Level=60");
		QVERIFY(write(innerPath, other)); DrumSynth outer; int count = 0;
		std::vector<int16_t> baseline;
		{ FileResourceIO::Scope scope([&] { ++count; return false; }); baseline = decode(outer, outerPath, 2, 44100); }
		QVERIFY(!baseline.empty()); const auto nestedAt = count - 16;
		count = 0; bool entered = false; std::vector<int16_t> nested, result;
		{
			FileResourceIO::Scope scope([&] {
				if (++count >= nestedAt && !entered)
				{
					entered = true; FileResourceIO::Suspension independent;
					DrumSynth inner; nested = decode(inner, innerPath, 1, 24000);
				}
				return false;
			});
			result = decode(outer, outerPath, 2, 44100);
		}
		QVERIFY(entered && !nested.empty()); QVERIFY(result == baseline);
	}

	void malformedProfilesDoNotPublishSamples_data()
	{
		QTest::addColumn<QByteArray>("profile");
		QTest::newRow("invalid-version") << QByteArray(tone()).replace("DrumSynth v2.0", "Invalid");
		QTest::newRow("too-many-envelope-points") << QByteArray(tone()).replace("0,100 44100,0", QByteArray("0,100 ").repeated(32));
		QTest::newRow("nonfinite-time") << QByteArray(tone()).replace("44100,0", "nan,0");
		QTest::newRow("nonfinite-level") << QByteArray(tone()).replace("0,100", "0,inf");
		QTest::newRow("negative-time") << QByteArray(tone()).replace("44100,0", "-1,0");
		QTest::newRow("decreasing-time") << QByteArray(tone()).replace("0,100 44100,0", "10,100 5,0");
		QTest::newRow("zero-tone-duration") << QByteArray(tone()).replace("44100,0", "0,0");
		QTest::newRow("enormous-duration") << QByteArray(tone()).replace("44100,0", "3e38,0");
		QTest::newRow("integer-overflow") << QByteArray(tone()).replace("Level=100", "Level=9999999999999");
		QTest::newRow("negative-distortion-rate") << QByteArray(tone()).replace("Rate=0", "Rate=-1");
		QTest::newRow("oversized-distortion-rate") << QByteArray(tone()).replace("Rate=0", "Rate=1200");
		QTest::newRow("invalid-band-frequency") << QByteArray(tone()).replace("[NoiseBand]\nOn=0", "[NoiseBand]\nOn=1\nF=-1");
		QTest::newRow("nonfinite-frequency") << QByteArray(tone()).replace("F1=440", "F1=nan");
		QTest::newRow("overflowing-quantizer") << QByteArray(tone()).replace("[Distortion]\nOn=0", "[Distortion]\nOn=1").replace("Bits=0", "Bits=10000");
	}
	void malformedProfilesDoNotPublishSamples()
	{
		QFETCH(QByteArray, profile); QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto path = directory.filePath("malformed.ds"); QVERIFY(write(path, profile));
		DrumSynth synth; int16_t* unpublished = nullptr;
		QCOMPARE(synth.GetDSFileSamples(path, unpublished, 2, 44100), 0); QVERIFY(unpublished == nullptr);
		QVERIFY(write(path, tone())); QVERIFY(!decode(synth, path, 2, 44100).empty());
	}

	void everyBundledNativeProfileDecodesWithoutPublishingPartialOutput()
	{
		QDirIterator files(QString(LMMS_TEST_DRUMSYNTH_DIR), {"*.ds"}, QDir::Files, QDirIterator::Subdirectories);
		int tested = 0; DrumSynth synth;
		while (files.hasNext())
		{
			const auto path = files.next(); int16_t* unpublished = nullptr; bool exposedEarly = false; int frames;
			{
				FileResourceIO::Scope scope([&] { exposedEarly = exposedEarly || unpublished != nullptr; return false; });
				frames = synth.GetDSFileSamples(path, unpublished, 2, 44100);
			}
			const std::unique_ptr<int16_t[]> owner(unpublished);
			QVERIFY2(frames > 0 && owner && !exposedEarly, qPrintable(path)); ++tested;
		}
		QVERIFY(tested >= 764);
	}

	void invalidOutputArgumentsPreserveCallerOwnership_data()
	{
		QTest::addColumn<int>("channels"); QTest::addColumn<quint32>("rate");
		QTest::newRow("zero-channels") << 0 << quint32(44100);
		QTest::newRow("negative-channels") << -1 << quint32(44100);
		QTest::newRow("zero-rate") << 2 << quint32(0);
		QTest::newRow("unrepresentable-rate") << 2 << std::numeric_limits<quint32>::max();
	}
	void invalidOutputArgumentsPreserveCallerOwnership()
	{
		QFETCH(int, channels); QFETCH(quint32, rate);
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("tone.ds");
		QVERIFY(write(path, tone())); DrumSynth synth; int16_t* unpublished = nullptr;
		QCOMPARE(synth.GetDSFileSamples(path, unpublished, channels, rate), 0); QVERIFY(unpublished == nullptr);
		int16_t original = 1234; int16_t* ownedByCaller = &original;
		QCOMPARE(synth.GetDSFileSamples(path, ownedByCaller, 2, 44100), 0);
		QVERIFY(ownedByCaller == &original); QCOMPARE(original, int16_t(1234));
		QVERIFY(!decode(synth, path, 2, 44100).empty());
	}
};
}

QTEST_GUILESS_MAIN(lmms::DrumSynthTest)
#include "DrumSynthTest.moc"
