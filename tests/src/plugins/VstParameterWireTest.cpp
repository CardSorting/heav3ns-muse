/* Native VST parameter message validation. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <bit>
#include <cstdint>
#include "VstParameterWire.h"

using namespace lmms;
class VstParameterWireTest : public QObject
{
	Q_OBJECT
private slots:
	void floatWireRoundTrip_data()
	{
		QTest::addColumn<float>("value");
		QTest::newRow("zero") << 0.f; QTest::newRow("negative-zero") << -0.f; QTest::newRow("one") << 1.f;
		QTest::newRow("above-half") << std::nextafter(.5f, 1.f); QTest::newRow("below-one") << std::nextafter(1.f, 0.f);
		QTest::newRow("subnormal") << std::numeric_limits<float>::denorm_min();
		QTest::newRow("minimum-normal") << std::numeric_limits<float>::min();
	}
	void floatWireRoundTrip()
	{
		QFETCH(float, value); const auto encoded = vstParameterText(value);
		QCOMPARE(std::bit_cast<std::uint32_t>(vstParameterValue(encoded)), std::bit_cast<std::uint32_t>(value));
		QVERIFY(encoded.find(',') == std::string::npos);
	}

	void completeValidationBeforeWrites_data()
	{
		QTest::addColumn<int>("change");
		QTest::newRow("count-text") << 0; QTest::newRow("count-overflow") << 1;
		QTest::newRow("index-text") << 2; QTest::newRow("index-order") << 3;
		QTest::newRow("missing-value") << 4; QTest::newRow("value-text") << 5;
		QTest::newRow("trailing-text") << 6; QTest::newRow("nan") << 7;
		QTest::newRow("above-one") << 8; QTest::newRow("comma") << 9;
		QTest::newRow("extra-field") << 10; QTest::newRow("negative-count") << 11;
	}
	void completeValidationBeforeWrites()
	{
		QFETCH(int, change); std::vector<std::string> fields{"2", "0", "name: first", ".25", "1", "name: second", ".75"};
		switch (change)
		{
			case 0: fields[0] = "two"; break; case 1: fields[0] = "999999999999999999999"; break;
			case 2: fields[4] = "bad"; break; case 3: fields[4] = "0"; break;
			case 4: fields.pop_back(); break; case 5: fields[6] = "bad"; break;
			case 6: fields[6] = ".75junk"; break; case 7: fields[6] = "nan"; break;
			case 8: fields[6] = "1.01"; break; case 9: fields[6] = "0,75"; break;
			case 10: fields.push_back("extra"); break; case 11: fields[0] = "-2"; break;
		}
		int writes = 0;
		QVERIFY_EXCEPTION_THROWN(applyVstParameterDump(2, fields.size(), [&](int field) { return fields.at(field); },
			[&](int, float) { ++writes; }), std::runtime_error);
		QCOMPARE(writes, 0);
	}

	void settersCompleteInOrderAndExceptionsPropagate()
	{
		const std::vector<std::string> fields{"2", "0", "", ".25", "1", "", ".75"};
		std::vector<float> values;
		applyVstParameterDump(2, fields.size(), [&](int field) { return fields.at(field); }, [&](int index, float value) {
			QCOMPARE(index, static_cast<int>(values.size())); values.push_back(value);
		});
		QVERIFY(values == std::vector<float>({.25f, .75f}));
		QVERIFY_EXCEPTION_THROWN(applyVstParameterDump(2, fields.size(), [&](int field) { return fields.at(field); },
			[](int, float) { throw std::runtime_error("Native setter failed"); }), std::runtime_error);
		int writes = 0; applyVstParameterDump(0, 1, [](int) { return "0"; }, [&](int, float) { ++writes; }); QCOMPARE(writes, 0);
		QVERIFY_EXCEPTION_THROWN(vstParameterInteger({}), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(vstParameterValue({}), std::runtime_error);
	}

	void textFieldsAreDelimitedByBytes_data()
	{
		QTest::addColumn<QByteArray>("packed"); QTest::addColumn<QStringList>("expected");
		QTest::newRow("empty-layout") << QByteArray() << QStringList();
		QTest::newRow("empty-fields") << QByteArray("00") << QStringList({"", ""});
		QTest::newRow("digits") << QByteArray("312314") << QStringList({"123", "4"});
		QTest::newRow("utf8") << QByteArray("3\xc2\xb5s4\xf0\x9f\x8e\xb5")
			<< QStringList({QString::fromUtf8("\xc2\xb5s"), QString::fromUtf8("\xf0\x9f\x8e\xb5")});
		QTest::newRow("maximum-bytes") << QByteArray("8123456780") << QStringList({"12345678", ""});
	}
	void textFieldsAreDelimitedByBytes()
	{
		QFETCH(QByteArray, packed); QFETCH(QStringList, expected);
		const auto actual = decodeVstParameterTextList(std::string_view(packed.constData(), packed.size()), expected.size());
		QCOMPARE(actual.size(), static_cast<std::size_t>(expected.size()));
		for (int index = 0; index < expected.size(); ++index) { QCOMPARE(QString::fromStdString(actual[index]), expected[index]); }
	}
	void malformedTextFieldsAreRejected_data()
	{
		QTest::addColumn<QByteArray>("packed"); QTest::addColumn<int>("count");
		QTest::newRow("nondigit") << QByteArray("xdB") << 1;
		QTest::newRow("too-long") << QByteArray("9123456789") << 1;
		QTest::newRow("truncated") << QByteArray("3ab") << 1;
		QTest::newRow("missing-field") << QByteArray("2dB") << 2;
		QTest::newRow("trailing-data") << QByteArray("2dB0x") << 2;
		QTest::newRow("extra-field") << QByteArray("00") << 1;
		QTest::newRow("negative-count") << QByteArray() << -1;
		QTest::newRow("impossible-count") << QByteArray("0") << 3;
		QTest::newRow("non-ascii-length") << QByteArray::fromHex("ff") << 1;
	}
	void malformedTextFieldsAreRejected()
	{
		QFETCH(QByteArray, packed); QFETCH(int, count);
		QVERIFY_EXCEPTION_THROWN(decodeVstParameterTextList(std::string_view(packed.constData(), packed.size()), count), std::runtime_error);
	}
};

QTEST_GUILESS_MAIN(VstParameterWireTest)
#include "VstParameterWireTest.moc"
