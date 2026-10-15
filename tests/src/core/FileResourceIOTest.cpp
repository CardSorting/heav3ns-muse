/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>
#include "FileResourceIO.h"
#include "FileResourceSnapshot.h"
#include "StudioAssetStore.h"

namespace lmms
{
class FileResourceIOTest : public QObject
{
	Q_OBJECT
	static bool write(const QString& path, const QByteArray& bytes)
	{
		QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
	}
	static QByteArray read(const QString& path)
	{
		QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
	}
private slots:
	void exactMultiChunkHashAndCopyNeverOverwriteAnExistingFile()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("input.wav"), destination = directory.filePath("copy.wav");
		QByteArray bytes(196621, '\0');
		for (int index = 0; index < bytes.size(); ++index) { bytes[index] = static_cast<char>(index % 251); }
		QVERIFY(write(source, bytes)); QString hash, error;
		QVERIFY(FileResourceIO::hashFile(source, hash, error));
		QCOMPARE(hash, QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
		QVERIFY(FileResourceIO::copyFile(source, destination, error)); QCOMPARE(read(destination), bytes);
		QVERIFY(write(source, "Replaced source"));
		QVERIFY(!FileResourceIO::copyFile(source, destination, error)); QCOMPARE(read(destination), bytes);
	}

	void cancellationBetweenChunksRemovesOnlyThePartialCopy()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("input.wav"), destination = directory.filePath("copy.wav");
		const QByteArray bytes(196621, 'a'); QVERIFY(write(source, bytes)); QString error;
		{
			FileResourceIO::Scope work([&] { return QFileInfo(destination).size() > 0; });
			QVERIFY(!FileResourceIO::copyFile(source, destination, error)); QVERIFY(error.contains("cancelled"));
		}
		QVERIFY(!QFile::exists(destination)); QCOMPARE(read(source), bytes);
	}

	void snapshotAndAssetStagingStopWithoutPublishingAReference()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto source = directory.filePath("input.wav");
		QVERIFY(write(source, QByteArray(196621, 'a'))); QString error;
		{
			int checks = 0; FileResourceIO::Scope work([&] { return ++checks >= 7; });
			QVERIFY(!FileResourceSnapshot::capture(source, error)); QVERIFY(error.contains("cancelled"));
		}
		const auto store = directory.filePath("assets"); QJsonObject reference;
		{
			int checks = 0; FileResourceIO::Scope work([&] { return ++checks >= 7; });
			QVERIFY(!StudioAssetStore::preserve(source, store, "wav", reference, error));
			QVERIFY(error.contains("cancelled")); QVERIFY(reference.isEmpty());
		}
		QCOMPARE(QDir(store).entryList(QDir::Files | QDir::Hidden).size(), 0);
	}

	void inheritedStopAndRecoverySuspensionRestoreThePreviousScope()
	{
		QString error; QVERIFY(FileResourceIO::check(error));
		{
			FileResourceIO::Scope outer([] { return true; });
			FileResourceIO::Scope inner([] { return false; });
			QVERIFY(!FileResourceIO::check(error));
			{ FileResourceIO::Suspension recovery; QVERIFY(FileResourceIO::check(error)); }
			QVERIFY(!FileResourceIO::check(error));
		}
		QVERIFY(FileResourceIO::check(error));
	}

	void eventPollingDeliversCancellationAndSuppressesNestedPolling()
	{
		QObject owner; bool stopped = false, nested = false; QString error;
		FileResourceIO::Scope work([&] { return stopped; }, true);
		QTimer::singleShot(0, &owner, [&] {
			FileResourceIO::Scope child({}, true); QString reason;
			nested = FileResourceIO::check(reason); stopped = true;
		});
		QVERIFY(!FileResourceIO::check(error)); QVERIFY(stopped && nested); QVERIFY(error.contains("cancelled"));
	}

	void streamingStopsAfterOneChunkAndLeavesHashResultsUnacknowledged()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto source = directory.filePath("input.wav");
		const QByteArray bytes(196621, 'a'); QVERIFY(write(source, bytes));
		QFile input(source); QVERIFY(input.open(QIODevice::ReadOnly));
		QBuffer output; QVERIFY(output.open(QIODevice::WriteOnly)); QString error;
		{
			FileResourceIO::Scope work([&] { return output.pos() >= 65536; });
			QVERIFY(!FileResourceIO::stream(input, &output, nullptr, bytes.size(), error));
		}
		QCOMPARE(output.pos(), qint64(65536)); QCOMPARE(output.data(), bytes.left(65536));
		QString hash("Unacknowledged"); int checks = 0;
		FileResourceIO::Scope work([&] { return ++checks >= 7; });
		QVERIFY(!FileResourceIO::hashFile(source, hash, error)); QVERIFY(hash.isEmpty());
	}
};
}

QTEST_GUILESS_MAIN(lmms::FileResourceIOTest)
#include "FileResourceIOTest.moc"
