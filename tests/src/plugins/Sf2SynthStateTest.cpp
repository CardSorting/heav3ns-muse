/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <array>
#include <cmath>
#include <limits>
#include <vector>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "../../../plugins/Sf2Player/Sf2SynthState.h"
#include "../../../plugins/Sf2Player/Sf2BankSnapshot.h"

namespace lmms
{

class Sf2SynthStateTest : public QObject
{
	Q_OBJECT

	static std::array<double, 2> measure(fluid_synth_t* synth, int rate)
	{
		std::vector<float> stereo(256 * 2);
		std::vector<float> samples;
		double square = 0;
		for (int block = 0; block < 16; ++block)
		{
			if (fluid_synth_write_float(synth, 256, stereo.data(), 0, 2, stereo.data(), 1, 2) != FLUID_OK)
			{
				return {0, 0};
			}
			for (int frame = 0; frame < 256; ++frame)
			{
				const auto sample = stereo[frame * 2];
				if (!std::isfinite(sample)) { return {0, 0}; }
				if (block >= 4) { samples.push_back(sample); square += double(sample) * sample; }
			}
		}
		std::vector<int> crossings;
		for (std::size_t frame = 1; frame < samples.size(); ++frame)
		{
			if (samples[frame - 1] <= 0 && samples[frame] > 0) { crossings.push_back(static_cast<int>(frame)); }
		}
		const double frequency = crossings.size() > 1
			? double(rate) * (crossings.size() - 1) / (crossings.back() - crossings.front()) : 0;
		return {std::sqrt(square / samples.size()), frequency};
	}

	static bool startBank(fluid_synth_t* synth, const QString& path)
	{
		fluid_synth_set_gain(synth, .8f);
		fluid_synth_reverb_on(synth, -1, 0);
		fluid_synth_chorus_on(synth, -1, 0);
		const auto encoded = path.toUtf8();
		const int bank = fluid_synth_sfload(synth, encoded.constData(), false);
		return bank >= 0 && fluid_synth_program_select(synth, 1, bank, 0, 0) == FLUID_OK
			&& fluid_synth_noteon(synth, 1, 69, 100) == FLUID_OK;
	}

private slots:
	void copiedSettingsPreserveAllValueTypesAndLongStrings()
	{
		Sf2SynthState::Error error;
		auto current = Sf2SynthState::create(nullptr, 44100, error); QVERIFY(current);
		QVERIFY(fluid_settings_setint(current->settings.get(), "synth.polyphony", 512) == FLUID_OK);
		QVERIFY(fluid_settings_setnum(current->settings.get(), "synth.gain", .75) == FLUID_OK);
		QVERIFY(fluid_settings_setstr(current->settings.get(), "synth.midi-bank-select", "mma") == FLUID_OK);
		const QByteArray longName(4096, 'x');
		QVERIFY(fluid_settings_setstr(current->settings.get(), "midi.portname", longName.constData()) == FLUID_OK);
		QVERIFY(fluid_settings_setstr(current->settings.get(), "audio.file.name", nullptr) == FLUID_OK);
		auto candidate = Sf2SynthState::create(current->settings.get(), 48000, error); QVERIFY(candidate);
		QVERIFY(candidate->settings.get() != current->settings.get());
		QVERIFY(candidate->synth.get() != current->synth.get());
		QCOMPARE(candidate->polyphony, 512); QCOMPARE(candidate->sampleRate, 48000);
		double oldRate = 0, gain = 0;
		QVERIFY(fluid_settings_getnum(current->settings.get(), "synth.sample-rate", &oldRate) == FLUID_OK);
		QCOMPARE(oldRate, 44100.);
		QVERIFY(fluid_settings_getnum(candidate->settings.get(), "synth.gain", &gain) == FLUID_OK);
		QCOMPARE(gain, .75);
		char* value = nullptr;
		QVERIFY(fluid_settings_dupstr(candidate->settings.get(), "midi.portname", &value) == FLUID_OK);
		{
			const std::unique_ptr<char, decltype(&fluid_free)> owned(value, fluid_free);
			QCOMPARE(QByteArray(value), longName);
		}
		value = nullptr;
		QVERIFY(fluid_settings_dupstr(candidate->settings.get(), "synth.midi-bank-select", &value) == FLUID_OK);
		{
			const std::unique_ptr<char, decltype(&fluid_free)> owned(value, fluid_free);
			QCOMPARE(QByteArray(value), QByteArray("mma"));
		}
		value = nullptr;
		QVERIFY(fluid_settings_dupstr(candidate->settings.get(), "audio.file.name", &value) == FLUID_OK);
		const std::unique_ptr<char, decltype(&fluid_free)> owned(value, fluid_free);
		QVERIFY(value == nullptr);
	}

	void failedPreflightKeepsTheNativeHeldVoiceAndBank_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("settings-allocation") << 0;
		QTest::newRow("synth-allocation") << 1;
		QTest::newRow("invalid-sample-rate") << 2;
	}
	void failedPreflightKeepsTheNativeHeldVoiceAndBank()
	{
		QFETCH(int, failure);
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto path = directory.filePath("held.sf2");
		QVERIFY(QFile::copy(QString(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2", path));
		Sf2SynthState::Error error;
		auto current = Sf2SynthState::create(nullptr, 44100, error); QVERIFY(current);
		QVERIFY(startBank(current->synth.get(), path));
		const auto before = measure(current->synth.get(), 44100);
		QVERIFY(before[0] > .0001 && std::abs(before[1] - 440) < 4);
		std::array<fluid_voice_t*, 512> voices{};
		fluid_synth_get_voicelist(current->synth.get(), voices.data(), static_cast<int>(voices.size()), -1);
		QVERIFY(voices.front()); const auto voice = voices.front(); const auto id = fluid_voice_get_id(voice);
		QVERIFY(QFile::remove(path));
		const auto settingsFactory = failure == 0
			? +[]() -> fluid_settings_t* { return nullptr; } : &new_fluid_settings;
		const auto synthFactory = failure == 1
			? +[](fluid_settings_t*) -> fluid_synth_t* { return nullptr; } : &new_fluid_synth;
		auto candidate = Sf2SynthState::create(current->settings.get(),
			failure == 2 ? std::numeric_limits<double>::quiet_NaN() : 48000, error, settingsFactory, synthFactory);
		QVERIFY(!candidate);
		QVERIFY(error == (failure == 0 ? Sf2SynthState::Error::SettingsAllocation
			: failure == 1 ? Sf2SynthState::Error::SynthAllocation : Sf2SynthState::Error::InvalidSampleRate));
		QCOMPARE(fluid_synth_sfcount(current->synth.get()), 1);
		QCOMPARE(fluid_voice_get_id(voice), id); QVERIFY(fluid_voice_is_playing(voice));
		const auto after = measure(current->synth.get(), 44100);
		QVERIFY(after[0] > before[0] / 2 && std::abs(after[1] - 440) < 4);
		double oldRate = 0;
		QVERIFY(fluid_settings_getnum(current->settings.get(), "synth.sample-rate", &oldRate) == FLUID_OK);
		QCOMPARE(oldRate, 44100.);
	}

	void deletingThePriorSynthKeepsCandidateSettingsCallbacks()
	{
		Sf2SynthState::Error error;
		auto current = Sf2SynthState::create(nullptr, 44100, error); QVERIFY(current);
		auto candidate = Sf2SynthState::create(current->settings.get(), 48000, error); QVERIFY(candidate);
		current.reset();
		QVERIFY(fluid_settings_setint(candidate->settings.get(), "synth.polyphony", 32) == FLUID_OK);
		QCOMPARE(fluid_synth_get_polyphony(candidate->synth.get()), 32);
	}

	void candidateRendersThroughTheNativeBankAtItsOwnSampleRate_data()
	{
		QTest::addColumn<int>("rate");
		QTest::newRow("44100") << 44100;
		QTest::newRow("48000") << 48000;
		QTest::newRow("192000-with-native-fallback") << 192000;
	}
	void candidateRendersThroughTheNativeBankAtItsOwnSampleRate()
	{
		QFETCH(int, rate);
		Sf2SynthState::Error error;
		auto current = Sf2SynthState::create(nullptr, 44100, error); QVERIFY(current);
		auto candidate = Sf2SynthState::create(current->settings.get(), rate, error); QVERIFY(candidate);
		current.reset();
		QVERIFY(candidate->sampleRate > 0);
		QVERIFY(startBank(candidate->synth.get(), QString(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2"));
		const auto audio = measure(candidate->synth.get(), candidate->sampleRate);
		QVERIFY(audio[0] > .0001 && std::abs(audio[1] - 440) < 4);
	}

	void snapshotKeepsAcceptedBytesAfterSourceReplacementAndRemovesItsFile()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath(QString(QChar(0x97f3)) + "-source.sf2");
		QVERIFY(QFile::copy(QString(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2", source));
		QFile original(source); QVERIFY(original.open(QIODevice::ReadOnly));
		const auto bytes = original.readAll(); original.close();
		QVERIFY(bytes.size() > 64 * 1024);
		QString error;
		auto snapshot = Sf2BankSnapshot::capture(source, error); QVERIFY2(snapshot, qPrintable(error));
		const auto cachedPath = snapshot->path(); QVERIFY(cachedPath != source);
		QVERIFY(original.open(QIODevice::WriteOnly | QIODevice::Truncate));
		QVERIFY(original.write("Source replaced after native snapshot") > 0); original.close();
		QVERIFY(QFile::remove(source));
		QFile cached(cachedPath); QVERIFY(cached.open(QIODevice::ReadOnly));
		QCOMPARE(cached.readAll(), bytes); cached.close();
		snapshot.reset(); QVERIFY(!QFile::exists(cachedPath));
		QVERIFY(!Sf2BankSnapshot::capture(source, error)); QVERIFY(!error.isEmpty());
	}

	void rejectedCandidateBankKeepsTheWorkingNativeVoice_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("missing-file") << 0;
		QTest::newRow("invalid-bank-bytes") << 1;
		QTest::newRow("missing-bank") << 2;
		QTest::newRow("missing-program") << 3;
	}
	void rejectedCandidateBankKeepsTheWorkingNativeVoice()
	{
		QFETCH(int, failure);
		QString errorText;
		auto snapshot = Sf2BankSnapshot::capture(QString(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2", errorText);
		QVERIFY2(snapshot, qPrintable(errorText));
		Sf2SynthState::Error error;
		auto current = Sf2SynthState::create(nullptr, 44100, error); QVERIFY(current);
		QVERIFY(startBank(current->synth.get(), snapshot->path()));
		const auto before = measure(current->synth.get(), 44100); QVERIFY(before[0] > .0001);
		std::array<fluid_voice_t*, 512> voices{};
		fluid_synth_get_voicelist(current->synth.get(), voices.data(), static_cast<int>(voices.size()), -1);
		QVERIFY(voices.front()); const auto voice = voices.front(); const auto id = fluid_voice_get_id(voice);
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto invalid = directory.filePath("invalid.sf2");
		QFile invalidFile(invalid); QVERIFY(invalidFile.open(QIODevice::WriteOnly));
		QVERIFY(invalidFile.write("Not a SoundFont") > 0); invalidFile.close();
		auto invalidSnapshot = Sf2BankSnapshot::capture(invalid, errorText); QVERIFY(invalidSnapshot);
		const auto path = failure == 0 ? directory.filePath("missing.sf2")
			: failure == 1 ? invalidSnapshot->path() : snapshot->path();
		const auto encoded = path.toUtf8();
		auto candidate = Sf2SynthState::create(current->settings.get(), 48000, error); QVERIFY(candidate);
		QVERIFY(!candidate->loadBank(encoded.constData(), 1, failure == 2 ? 999 : 0, failure == 3 ? 127 : 0, error));
		QVERIFY(error == (failure < 2 ? Sf2SynthState::Error::BankLoad : Sf2SynthState::Error::BankSelection));
		QCOMPARE(fluid_synth_sfcount(candidate->synth.get()), failure < 2 ? 0 : 1);
		candidate.reset();
		QCOMPARE(fluid_synth_sfcount(current->synth.get()), 1);
		QCOMPARE(fluid_voice_get_id(voice), id); QVERIFY(fluid_voice_is_playing(voice));
		const auto after = measure(current->synth.get(), 44100);
		QVERIFY(after[0] > before[0] / 2 && std::abs(after[1] - 440) < 4);
	}

	void preparedBankSurvivesPriorSynthDestructionAndSourceRemoval()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("source.sf2");
		QVERIFY(QFile::copy(QString(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2", source));
		QString errorText;
		auto snapshot = Sf2BankSnapshot::capture(source, errorText); QVERIFY2(snapshot, qPrintable(errorText));
		const auto cachedPath = snapshot->path(); const auto encoded = cachedPath.toUtf8();
		Sf2SynthState::Error error;
		auto current = Sf2SynthState::create(nullptr, 44100, error); QVERIFY(current);
		QVERIFY(current->loadBank(encoded.constData(), 1, 0, 0, error));
		const auto oldBank = fluid_synth_get_sfont_by_id(current->synth.get(), current->fontId);
		QVERIFY(oldBank);
		QVERIFY(QFile::remove(source));
		auto candidate = Sf2SynthState::create(current->settings.get(), 48000, error); QVERIFY(candidate);
		QVERIFY(candidate->loadBank(encoded.constData(), 1, 1024, 7, error));
		QVERIFY(fluid_synth_get_sfont_by_id(candidate->synth.get(), candidate->fontId) != oldBank);
		current.reset();
		fluid_synth_set_gain(candidate->synth.get(), .8f);
		fluid_synth_reverb_on(candidate->synth.get(), -1, 0);
		fluid_synth_chorus_on(candidate->synth.get(), -1, 0);
		QVERIFY(fluid_synth_noteon(candidate->synth.get(), 1, 69, 100) == FLUID_OK);
		const auto audio = measure(candidate->synth.get(), 48000);
		QVERIFY(audio[0] > .0001 && std::abs(audio[1] - 880) < 6);
		candidate.reset(); snapshot.reset();
		QVERIFY(!QFile::exists(cachedPath));
	}
};

} // namespace lmms

QTEST_GUILESS_MAIN(lmms::Sf2SynthStateTest)
#include "Sf2SynthStateTest.moc"
