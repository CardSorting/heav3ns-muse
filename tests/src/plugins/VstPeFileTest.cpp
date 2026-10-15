/* Native VST PE classification boundaries. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QTemporaryDir>
#include "VstPeFile.h"

using namespace lmms;
namespace
{
QByteArray header(quint16 machine = 0x8664, int offset = 128)
{
	QByteArray bytes(offset + 24, '\0'); bytes[0] = 'M'; bytes[1] = 'Z';
	qToLittleEndian<quint32>(offset, bytes.data() + 0x3c);
	bytes[offset] = 'P'; bytes[offset + 1] = 'E';
	qToLittleEndian<quint16>(machine, bytes.data() + offset + 4);
	return bytes;
}
void writeFile(const QString& path, const QByteArray& bytes)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush())
	{ throw std::runtime_error("Cannot write native binary test fixture"); }
}
}
class VstPeFileTest : public QObject
{
	Q_OBJECT
private slots:
	void readsCompleteHeadersWithoutAlignmentAssumptions_data()
	{
		QTest::addColumn<int>("machine"); QTest::addColumn<int>("offset");
		QTest::newRow("win32") << 0x014c << 128;
		QTest::newRow("win64") << 0x8664 << 128;
		QTest::newRow("unaligned") << 0x8664 << 65;
		QTest::newRow("unknown-machine") << 0xaa64 << 64;
	}
	void readsCompleteHeadersWithoutAlignmentAssumptions()
	{
		QFETCH(int, machine); QFETCH(int, offset); QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto path = directory.filePath(QString::fromUtf8("plugin-\xc2\xb5.dll"));
		writeFile(path, header(machine, offset)); QCOMPARE(readVstPeMachine(path), static_cast<quint16>(machine));
	}
	void refusesEveryTruncatedPrefix()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("truncated.dll");
		const auto bytes = header();
		for (int length = 0; length < bytes.size(); ++length)
		{
			writeFile(path, bytes.left(length));
			QVERIFY_EXCEPTION_THROWN(readVstPeMachine(path), std::runtime_error);
		}
		writeFile(path, bytes); QCOMPARE(readVstPeMachine(path), quint16{0x8664});
	}
	void refusesInvalidHeaders_data()
	{
		QTest::addColumn<int>("change");
		QTest::newRow("dos-signature") << 0; QTest::newRow("pe-signature") << 1;
		QTest::newRow("offset-at-eof") << 2; QTest::newRow("maximum-offset") << 3;
		QTest::newRow("signed-negative-offset") << 4;
	}
	void refusesInvalidHeaders()
	{
		QFETCH(int, change); QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("invalid.dll");
		auto bytes = header();
		switch (change)
		{
			case 0: bytes[0] = 'X'; break;
			case 1: bytes[128] = 'X'; break;
			case 2: qToLittleEndian<quint32>(bytes.size(), bytes.data() + 0x3c); break;
			case 3: qToLittleEndian<quint32>(0xffffffffu, bytes.data() + 0x3c); break;
			case 4: qToLittleEndian<quint32>(0x80000000u, bytes.data() + 0x3c); break;
		}
		writeFile(path, bytes); QVERIFY_EXCEPTION_THROWN(readVstPeMachine(path), std::runtime_error);
	}
	void refusesMissingFile()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		QVERIFY_EXCEPTION_THROWN(readVstPeMachine(directory.filePath("missing.dll")), std::runtime_error);
	}
};
QTEST_GUILESS_MAIN(VstPeFileTest)
#include "VstPeFileTest.moc"
