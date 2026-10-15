/* Host preset file lifecycle. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QTemporaryDir>
#include <QFileInfo>
#include "VstPresetStorage.h"

using namespace lmms;
namespace
{
void writeBytes(const QString& path, const QByteArray& bytes)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush())
	{ throw std::runtime_error("Test fixture file write failed"); }
}
QByteArray readBytes(const QString& path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) { throw std::runtime_error("Test fixture file read failed"); }
	return file.readAll();
}
QByteArray presetBytes(bool bank = false)
{
	VstPreset preset; preset.pluginId = 0xfedcba98; preset.pluginVersion = 9;
	if (bank) { preset.kind = VstPresetKind::Bank; }
	else { preset.name = "Unchanged sound"; preset.parameters = {.25f, .75f}; }
	const auto data = encodeVstPreset(preset);
	return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
}
}

class VstPresetStorageTest : public QObject
{
	Q_OBJECT
private slots:
	void cleanup() { QVERIFY(FileResourceSnapshot::drainDeferredRemovals()); }

	void acceptedInputSurvivesSourceReplacement()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("input.other");
		const auto original = presetBytes(); writeBytes(path, original); QString privatePath; int requests = 0;
		const auto info = readVstPresetStorage(path, true, [&](const QString& accepted, const char* mode) {
			++requests; privatePath = accepted; QCOMPARE(QString::fromLatin1(mode), QString("load")); QVERIFY(accepted != path);
			writeBytes(path, "replaced source"); QCOMPARE(readBytes(accepted), original);
		});
		QCOMPARE(requests, 1); QCOMPARE(info.format, QString("fxp")); QCOMPARE(info.pluginId, QString("fedcba98"));
		QCOMPARE(info.sha256, QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex()));
		QCOMPARE(readBytes(path), QByteArray("replaced source")); QVERIFY(FileResourceSnapshot::drainDeferredRemovals()); QVERIFY(!QFileInfo::exists(privatePath));
	}

	void boundedCaptureUsesOpenedSizeAndAcceptedReadScope()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("source");
		QString error; writeBytes(path, "1234"); bool replaced = false;
		{
			FileResourceIO::Scope scope([&] { if (!replaced) { replaced = true; writeBytes(path, QByteArray(20, 'x')); } return false; });
			QVERIFY(!FileResourceSnapshot::captureBounded(path, 10, error));
		}
		QVERIFY(replaced); QVERIFY(!error.isEmpty());
		const auto bytes = presetBytes(); writeBytes(path, bytes);
		const auto accepted = FileResourceSnapshot::captureBounded(path, bytes.size(), error); QVERIFY(accepted);
		writeBytes(path, "different source");
		FileResourceSnapshot::ReadScope readScope(path, accepted);
		const auto copy = FileResourceSnapshot::captureBounded(path, bytes.size(), error); QVERIFY(copy); QCOMPARE(copy->sha256(), accepted->sha256());
		QVERIFY(!FileResourceSnapshot::captureBounded(path, bytes.size() - 1, error));
	}

	void successfulExportUsesPrivateFileAndExplicitKind()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("output.fxp");
		writeBytes(path, "prior"); QString privatePath;
		const auto bytes = presetBytes(true);
		const auto info = saveVstPresetStorage(path, true, [&](const QString& output, const char* mode) {
			privatePath = output; QVERIFY(output != path); QCOMPARE(readBytes(path), QByteArray("prior"));
			QCOMPARE(QString::fromLatin1(mode), QString("save-bank")); writeBytes(output, bytes);
		});
		QCOMPARE(readBytes(path), bytes); QCOMPARE(info.format, QString("fxb")); QCOMPARE(info.bytes, static_cast<qint64>(bytes.size()));
		QCOMPARE(info.sha256, QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
		QVERIFY(!QFileInfo::exists(privatePath));
	}

	void failedExportPreservesDestination_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("worker-error") << 0; QTest::newRow("incomplete-output") << 1;
		QTest::newRow("wrong-kind") << 2; QTest::newRow("cancel-after-worker") << 3;
	}
	void failedExportPreservesDestination()
	{
		QFETCH(int, failure); QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("output.fxp");
		writeBytes(path, "prior"); bool cancelled = false; QString privatePath;
		FileResourceIO::Scope scope([&] { return cancelled; });
		QVERIFY_EXCEPTION_THROWN(saveVstPresetStorage(path, false, [&](const QString& output, const char*) {
			privatePath = output;
			if (failure == 0) { throw std::runtime_error("Worker refused preset"); }
			writeBytes(output, failure == 1 ? QByteArray("CcnK") : presetBytes(failure == 2));
			if (failure == 3) { cancelled = true; }
		}), std::runtime_error);
		QCOMPARE(readBytes(path), QByteArray("prior")); QVERIFY(!QFileInfo::exists(privatePath));
	}

	void invalidOrCancelledInputNeverReachesWorker()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("broken.fxb");
		writeBytes(path, "CcnK"); int requests = 0; const auto request = [&](const QString&, const char*) { ++requests; };
		QVERIFY_EXCEPTION_THROWN(readVstPresetStorage(path, false, request), std::runtime_error); QCOMPARE(requests, 0);
		writeBytes(path, presetBytes());
		FileResourceIO::Scope scope([] { return true; });
		QVERIFY_EXCEPTION_THROWN(readVstPresetStorage(path, false, request), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(saveVstPresetStorage(path, false, request), std::runtime_error); QCOMPARE(requests, 0);
		QCOMPARE(readBytes(path), presetBytes());
	}

	void workerValidationAndPublicationErrorsPropagate()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("input.fxp"); writeBytes(path, presetBytes());
		QVERIFY_EXCEPTION_THROWN(readVstPresetStorage(path, false, [](const QString&, const char*) { throw std::runtime_error("ID mismatch"); }), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(saveVstPresetStorage(directory.filePath("missing/output.fxp"), false,
			[](const QString& output, const char*) { writeBytes(output, presetBytes()); }), std::runtime_error);
		QCOMPARE(readBytes(path), presetBytes());
	}

	void nativeProjectPresetAttributeRequiresCompleteCanonicalData()
	{
		for (bool bank : {false, true})
		{
			const auto bytes = presetBytes(bank); const auto encoded = QString::fromLatin1(bytes.toBase64());
			QCOMPARE(decodeVstPresetState(encoded), bytes);
			QVERIFY_EXCEPTION_THROWN(decodeVstPresetState(encoded + "!"), std::runtime_error);
			QVERIFY_EXCEPTION_THROWN(decodeVstPresetState(QString::fromLatin1(bytes.left(12).toBase64())), std::runtime_error);
		}
		QVERIFY_EXCEPTION_THROWN(decodeVstPresetState(""), std::runtime_error);
	}

	void legacyChunkBase64RetainsBytesAndRejectsCorruption_data()
	{
		QTest::addColumn<QString>("encoded"); QTest::addColumn<QByteArray>("decoded");
		QTest::newRow("canonical") << QString("TQ==") << QByteArray("M");
		QTest::newRow("unpadded") << QString("TQ") << QByteArray("M");
		QTest::newRow("wrapped") << QString(" T\tQ==\r\n") << QByteArray("M");
		QTest::newRow("binary") << QString("AP8=") << QByteArray::fromHex("00ff");
		QTest::newRow("empty") << QString() << QByteArray();
		QTest::newRow("only-padding") << QString("====") << QByteArray();
		QTest::newRow("junk") << QString("TQ==!") << QByteArray();
		QTest::newRow("embedded-padding") << QString("T=Q=") << QByteArray();
		QTest::newRow("partial-padding") << QString("TQ=") << QByteArray();
		QTest::newRow("excess-padding") << QString("TQ===") << QByteArray();
		QTest::newRow("nonzero-pad-bits") << QString("TR==") << QByteArray();
		QTest::newRow("partial-byte") << QString("T") << QByteArray();
		QTest::newRow("non-ascii") << (QString("TQ==") + QChar(0x0100)) << QByteArray();
	}
	void legacyChunkBase64RetainsBytesAndRejectsCorruption()
	{
		QFETCH(QString, encoded); QFETCH(QByteArray, decoded);
		if (decoded.isEmpty()) { QVERIFY_EXCEPTION_THROWN(decodeVstChunkState(encoded), std::runtime_error); }
		else { QCOMPARE(decodeVstChunkState(encoded), decoded); }
	}
};

QTEST_GUILESS_MAIN(VstPresetStorageTest)
#include "VstPresetStorageTest.moc"
