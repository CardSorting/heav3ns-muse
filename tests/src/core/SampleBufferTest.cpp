/* Native embedded-sample tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <cstring>
#include <limits>
#include <vector>
#include "SampleBuffer.h"
#include "FileResourceIO.h"
#include "DataFile.h"

namespace lmms
{
class SampleBufferTest : public QObject
{
	Q_OBJECT
	static std::vector<SampleFrame> frames()
	{
		std::vector<SampleFrame> data(16703);
		for (std::size_t index = 0; index < data.size(); ++index)
		{
			data[index] = {static_cast<float>(index % 257) / 256.f, -static_cast<float>(index % 127) / 126.f};
		}
		data[0] = {-0.f, std::numeric_limits<float>::denorm_min()};
		data.back() = {2.5f, std::numeric_limits<float>::max()};
		return data;
	}
	static QByteArray raw(const std::vector<SampleFrame>& data)
	{
		return {reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size() * sizeof(SampleFrame))};
	}
private slots:
	void nativeBitsAndRateSurviveMultiBlockEmbeddedDecoding_data()
	{
		QTest::addColumn<int>("variant"); QTest::addColumn<int>("rate");
		QTest::newRow("canonical-24000") << 0 << 24000;
		QTest::newRow("wrapped-44100") << 1 << 44100;
		QTest::newRow("unpadded-96000") << 2 << 96000;
	}
	void nativeBitsAndRateSurviveMultiBlockEmbeddedDecoding()
	{
		QFETCH(int, variant); QFETCH(int, rate);
		const auto source = frames(); const auto bytes = raw(source); const auto canonical = bytes.toBase64();
		QByteArray encoded;
		if (variant == 1)
		{
			for (int index = 0; index < canonical.size(); index += 71) { encoded += canonical.mid(index, 71) + " \t\r\n"; }
		}
		else if (variant == 2) { encoded = bytes.toBase64(QByteArray::OmitTrailingEquals); }
		else { encoded = canonical; }
		QString error("Unacknowledged");
		const auto buffer = SampleBuffer::fromBase64(QString::fromLatin1(encoded), rate, error);
		QVERIFY2(buffer, qPrintable(error)); QVERIFY(error.isEmpty()); QCOMPARE(buffer->sampleRate(), static_cast<sample_rate_t>(rate));
		QCOMPARE(buffer->size(), source.size());
		QVERIFY(std::memcmp(buffer->data(), bytes.constData(), bytes.size()) == 0);
		QCOMPARE(buffer->toBase64(), QString::fromLatin1(canonical));
		QVERIFY(buffer->audioFile().isEmpty() && buffer->acceptedFile().isEmpty());
		const auto legacy = SampleBuffer::fromBase64(QString::fromLatin1(encoded), rate);
		QCOMPARE(legacy->toBase64(), buffer->toBase64());
	}

	void invalidEncodingOrLateNonfiniteAudioDoesNotPublish_data()
	{
		QTest::addColumn<QString>("encoded");
		const auto bytes = raw(frames()); const auto valid = bytes.toBase64();
		QTest::newRow("invalid-character") << QString::fromLatin1(QByteArray(valid).insert(3, "!"));
		QTest::newRow("unicode-character") << (QString::fromLatin1(valid) + QChar(0x97f3));
		QTest::newRow("trailing-data") << QString::fromLatin1(valid + "A");
		QTest::newRow("excess-padding") << QString::fromLatin1(valid + "=");
		QTest::newRow("illegal-length") << QString("A");
		QTest::newRow("partial-frame") << QString::fromLatin1(bytes.left(7).toBase64());
		QTest::newRow("one-and-a-half-frames") << QString::fromLatin1(bytes.left(12).toBase64());
		QTest::newRow("misplaced-padding") << QString::fromLatin1(QByteArray(valid).insert(3, "="));
		QTest::newRow("late-invalid-character") << QString::fromLatin1(valid + "!");
		auto nonfinite = frames(); nonfinite.back().left() = std::numeric_limits<float>::quiet_NaN();
		QTest::newRow("late-nan") << QString::fromLatin1(raw(nonfinite).toBase64());
		nonfinite = frames(); nonfinite.back().right() = std::numeric_limits<float>::infinity();
		QTest::newRow("late-positive-infinity") << QString::fromLatin1(raw(nonfinite).toBase64());
		nonfinite.back().right() = -std::numeric_limits<float>::infinity();
		QTest::newRow("late-negative-infinity") << QString::fromLatin1(raw(nonfinite).toBase64());
	}
	void invalidEncodingOrLateNonfiniteAudioDoesNotPublish()
	{
		QFETCH(QString, encoded); QString error;
		QVERIFY(!SampleBuffer::fromBase64(encoded, 24000, error)); QVERIFY(!error.isEmpty());
		const auto recovered = SampleBuffer::fromBase64(QString::fromLatin1(raw(frames()).toBase64()), 24000, error);
		QVERIFY(recovered && error.isEmpty()); QCOMPARE(recovered->size(), std::size_t(16703));
	}

	void invalidRatesDoNotCreateAnEmbeddedBuffer_data()
	{
		QTest::addColumn<int>("rate"); QTest::newRow("zero") << 0; QTest::newRow("negative") << -44100;
	}
	void invalidRatesDoNotCreateAnEmbeddedBuffer()
	{
		QFETCH(int, rate); QString error;
		QVERIFY(!SampleBuffer::fromBase64(QString::fromLatin1(raw(frames()).toBase64()), rate, error));
		QVERIFY(error.contains("rate"));
	}

	void emptyOrWrappedEmptyInputRetainsItsExplicitRate()
	{
		for (const auto input : {QString{}, QString(" \t\r\n\f\v")})
		{
			QString error("Old error"); const auto buffer = SampleBuffer::fromBase64(input, 24000, error);
			QVERIFY(buffer && buffer->empty() && error.isEmpty()); QCOMPARE(buffer->sampleRate(), sample_rate_t(24000));
			QVERIFY(buffer->toBase64().isEmpty());
		}
	}

	void cancellationStopsDataAndWhitespaceWithoutPublishing_data()
	{
		QTest::addColumn<bool>("whitespace"); QTest::newRow("pcm") << false; QTest::newRow("whitespace") << true;
	}
	void cancellationStopsDataAndWhitespaceWithoutPublishing()
	{
		QFETCH(bool, whitespace);
		const auto valid = QString::fromLatin1(raw(frames()).toBase64());
		const auto input = whitespace ? QString(16384 * 16, ' ') : valid;
		QString error; int checks = 0;
		{
			FileResourceIO::Scope work([&] { return ++checks >= 12; });
			QVERIFY(!SampleBuffer::fromBase64(input, 24000, error)); QVERIFY(error.contains("cancelled"));
		}
		QCOMPARE(checks, 12); QVERIFY(SampleBuffer::fromBase64(valid, 24000, error)); QVERIFY(error.isEmpty());
	}

	void checkedEncodingMatchesNativeQtAcrossGroupAndBlockBoundaries_data()
	{
		QTest::addColumn<int>("count");
		for (const auto count : {0, 1, 2, 3, 1535, 1536, 1537, 16703})
		{
			QTest::newRow(qPrintable(QString::number(count))) << count;
		}
	}
	void checkedEncodingMatchesNativeQtAcrossGroupAndBlockBoundaries()
	{
		QFETCH(int, count); auto source = frames(); source.resize(count); const SampleBuffer buffer(source, 24000);
		QString encoded("Unacknowledged"), error("Old error"); QVERIFY2(buffer.toBase64(encoded, error), qPrintable(error));
		QVERIFY(error.isEmpty()); QCOMPARE(encoded, QString::fromLatin1(raw(source).toBase64()));
		const auto restored = SampleBuffer::fromBase64(encoded, 24000, error); QVERIFY(restored && error.isEmpty());
		QCOMPARE(restored->size(), source.size());
		if (!source.empty()) { QVERIFY(std::memcmp(restored->data(), source.data(), source.size() * sizeof(SampleFrame)) == 0); }
	}

	void checkedEncodingNeverPublishesPartialOutput_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("late-nan") << 0; QTest::newRow("late-infinity") << 1; QTest::newRow("cancellation") << 2;
	}
	void checkedEncodingNeverPublishesPartialOutput()
	{
		QFETCH(int, failure); auto source = frames();
		if (failure == 0) { source.back().left() = std::numeric_limits<float>::quiet_NaN(); }
		if (failure == 1) { source.back().right() = std::numeric_limits<float>::infinity(); }
		const SampleBuffer buffer(source, 24000); QString encoded("Previous result"), error; int checks = 0;
		{
			FileResourceIO::Scope work([&] { return failure == 2 && ++checks >= 9; });
			QVERIFY(!buffer.toBase64(encoded, error)); QVERIFY(encoded.isEmpty() && !error.isEmpty());
		}
		if (failure == 2) { QCOMPARE(checks, 9); QVERIFY(error.contains("cancelled")); }
		const SampleBuffer valid(frames(), 24000); QVERIFY(valid.toBase64(encoded, error)); QVERIFY(!encoded.isEmpty() && error.isEmpty());
	}

	void serializationErrorsFollowCopiesAndPreventNativeOutput_data()
	{
		QTest::addColumn<int>("type");
		QTest::newRow("project") << static_cast<int>(DataFile::Type::SongProject);
		QTest::newRow("preset") << static_cast<int>(DataFile::Type::InstrumentTrackSettings);
		QTest::newRow("journal") << static_cast<int>(DataFile::Type::JournalData);
	}
	void serializationErrorsFollowCopiesAndPreventNativeOutput()
	{
		QFETCH(int, type); DataFile document(static_cast<DataFile::Type>(type));
		auto element = document.createElement("sampleclip"); document.content().appendChild(element); element.setAttribute("data", "Accepted value");
		auto source = frames(); source.back().right() = std::numeric_limits<float>::infinity(); const SampleBuffer invalid(source, 24000);
		QVERIFY(!invalid.saveEmbeddedData(document, element, "data")); QCOMPARE(element.attribute("data"), QString("Accepted value"));
		const auto error = document.serializationError(); QVERIFY(error.contains("nonfinite"));
		DataFile::recordSerializationError(document, "Later diagnostic"); QCOMPARE(document.serializationError(), error);
		DataFile copied = document; QCOMPARE(copied.serializationError(), error); QVERIFY(copied.toString().isEmpty());
		const auto cloned = document.cloneNode(true).toDocument(); QCOMPARE(DataFile::serializationError(cloned), error);
		QString output; QTextStream stream(&output); copied.write(stream); stream.flush();
		QCOMPARE(stream.status(), QTextStream::WriteFailed); QVERIFY(output.isEmpty()); QVERIFY(!copied.validate("mmp"));
		const auto diagnostic = static_cast<const QDomDocument&>(document).toString().toUtf8();
		DataFile rejected(diagnostic); QCOMPARE(rejected.type(), DataFile::Type::Unknown); QCOMPARE(rejected.serializationError(), error);
	}

	void failedEncodingCannotTouchExistingFileBackupOrBundle_data()
	{
		QTest::addColumn<QString>("extension");
		QTest::newRow("project") << QString("mmp"); QTest::newRow("compressed-project") << QString("mmpz"); QTest::newRow("preset") << QString("xpf");
	}
	void failedEncodingCannotTouchExistingFileBackupOrBundle()
	{
		QFETCH(QString, extension); QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto path = directory.filePath("accepted." + extension); const auto backupPath = path + ".bak";
		const auto write = [](const QString& name, const QByteArray& bytes) { QFile file(name); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); };
		const auto read = [](const QString& name) { QFile file(name); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; };
		QVERIFY(write(path, "Accepted native file")); QVERIFY(write(backupPath, "Older native backup"));
		DataFile document(extension == "xpf" ? DataFile::Type::InstrumentTrackSettings : DataFile::Type::SongProject);
		auto element = document.createElement("sampleclip"); document.content().appendChild(element);
		auto source = frames(); source.back().left() = std::numeric_limits<float>::quiet_NaN();
		const SampleBuffer invalid(source, 24000); QVERIFY(!invalid.saveEmbeddedData(document, element, "data"));
		QVERIFY(!document.writeFile(path)); QCOMPARE(read(path), QByteArray("Accepted native file")); QCOMPARE(read(backupPath), QByteArray("Older native backup"));
		QVERIFY(!document.writeFile(directory.filePath("bundle.mmp"), true)); QVERIFY(!QDir(directory.path()).exists("bundle"));
	}
};
}

QTEST_GUILESS_MAIN(lmms::SampleBufferTest)
#include "SampleBufferTest.moc"
