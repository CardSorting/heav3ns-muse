/* Paged native VST transfer validation. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <bit>
#include <cstdint>
#include "VstParameterTransfer.h"

using namespace lmms;
class VstParameterTransferTest : public QObject
{
	Q_OBJECT
private slots:
	void pageRangesIncludeEmptyLayoutsAndLastPartialPages()
	{
		QCOMPARE(vstParameterRange(0, 0, 256).count, 0);
		QCOMPARE(vstParameterRange(513, 0, 256).count, 256);
		QCOMPARE(vstParameterRange(513, 512, 256).count, 1);
		QCOMPARE(vstParameterRange(513, 513, 256).count, 0);
		QCOMPARE(vstParameterRange(std::numeric_limits<int>::max(), std::numeric_limits<int>::max() - 1, 256).count, 1);
		QVERIFY_EXCEPTION_THROWN(vstParameterRange(-1, 0, 256), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(vstParameterRange(2, -1, 256), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(vstParameterRange(2, 3, 256), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(vstParameterRange(2, 0, 0), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(vstParameterRange(2, 0, 257), std::runtime_error);
	}

	void parameterPagesPreserveIndicesNamesAndFloatBits()
	{
		const float value = std::nextafter(.5f, 1.f);
		for (int offset = 0; offset < 513; offset += VstParameterPageSize)
		{
			const auto range = vstParameterRange(513, offset, VstParameterPageSize);
			std::vector<std::string> fields{"1", "513", std::to_string(offset), std::to_string(range.count)};
			for (int index = offset; index < offset + range.count; ++index)
			{
				fields.push_back(std::to_string(index)); fields.push_back("Gain: \xc2\xb5"); fields.push_back(vstParameterText(value));
			}
			const auto page = readVstParameterPage(513, offset, fields.size(), [&](int field) { return fields.at(field); });
			QCOMPARE(page.size(), static_cast<std::size_t>(range.count));
			for (int relative = 0; relative < range.count; ++relative)
			{
				QCOMPARE(page[relative].index, offset + relative); QCOMPARE(page[relative].name, std::string("Gain: \xc2\xb5"));
				QCOMPARE(std::bit_cast<std::uint32_t>(page[relative].value), std::bit_cast<std::uint32_t>(value));
			}
		}
		const std::vector<std::string> empty{"1", "0", "0", "0"};
		QVERIFY(readVstParameterPage(0, 0, empty.size(), [&](int field) { return empty.at(field); }).empty());
	}

	void malformedParameterPages_data()
	{
		QTest::addColumn<int>("change");
		QTest::newRow("failure") << 0; QTest::newRow("status") << 1; QTest::newRow("total") << 2;
		QTest::newRow("offset") << 3; QTest::newRow("count") << 4; QTest::newRow("missing") << 5;
		QTest::newRow("extra") << 6; QTest::newRow("index-order") << 7; QTest::newRow("index-text") << 8;
		QTest::newRow("long-name") << 9; QTest::newRow("nan") << 10; QTest::newRow("value-text") << 11;
		QTest::newRow("empty") << 12;
	}
	void malformedParameterPages()
	{
		QFETCH(int, change); std::vector<std::string> fields{"1", "2", "0", "2", "0", "gain", ".25", "1", "fault", ".75"};
		switch (change)
		{
			case 0: fields = {"0", "Native getter failed"}; break; case 1: fields[0] = "yes"; break;
			case 2: fields[1] = "3"; break; case 3: fields[2] = "1"; break; case 4: fields[3] = "1"; break;
			case 5: fields.pop_back(); break; case 6: fields.push_back("extra"); break; case 7: fields[7] = "0"; break;
			case 8: fields[7] = "1junk"; break; case 9: fields[8] = std::string(256, 'x'); break;
			case 10: fields[9] = "nan"; break; case 11: fields[9] = ".75junk"; break; case 12: fields.clear(); break;
		}
		QVERIFY_EXCEPTION_THROWN(readVstParameterPage(2, 0, fields.size(), [&](int field) { return fields.at(field); }), std::runtime_error);
	}

	void textPagesValidateBytesAndIndices()
	{
		std::vector<std::string> fields{"1", "258", "256", "2", "256", "\xc2\xb5s", "257", "12345678"};
		const auto read = [&] { return readVstParameterTextPage(258, 256, fields.size(), [&](int field) { return fields.at(field); }); };
		const auto page = read(); QCOMPARE(page.size(), std::size_t{2}); QCOMPARE(page[0], std::string("\xc2\xb5s"));
		fields[7].push_back('9'); QVERIFY_EXCEPTION_THROWN(read(), std::runtime_error);
		fields[7].clear(); QVERIFY(read()[1].empty());
		fields[6] = "256"; QVERIFY_EXCEPTION_THROWN(read(), std::runtime_error);
		fields = {"0", "Native display failed"}; QVERIFY_EXCEPTION_THROWN(read(), std::runtime_error);
	}

	void restoreCommitsAllPagesInNativeOrder()
	{
		VstParameterRestore restore; const float precise = std::nextafter(.5f, 1.f);
		std::vector<float> values(513, precise); values.back() = 0.f;
		restore.begin(values.size(), values.size());
		for (int offset = 0; offset < static_cast<int>(values.size()); )
		{
			const auto range = vstParameterRange(values.size(), offset, VstParameterPageSize);
			restore.append(values.size(), offset, std::span<const float>(values).subspan(offset, range.count)); offset += range.count;
		}
		std::vector<float> written;
		restore.commit(values.size(), [&](int index, float value) { QCOMPARE(index, static_cast<int>(written.size())); written.push_back(value); });
		QVERIFY(written == values);
		QVERIFY_EXCEPTION_THROWN(restore.commit(values.size(), [](int, float) {}), std::runtime_error);
	}

	void invalidRestoreCannotCommit_data()
	{
		QTest::addColumn<int>("change");
		QTest::newRow("out-of-order") << 0; QTest::newRow("duplicate-page") << 1; QTest::newRow("empty-page") << 2;
		QTest::newRow("oversized-page") << 3; QTest::newRow("nan") << 4; QTest::newRow("layout-change") << 5;
		QTest::newRow("repeated-begin") << 6; QTest::newRow("wrong-count") << 7; QTest::newRow("append-without-begin") << 8;
		QTest::newRow("incomplete") << 9; QTest::newRow("commit-layout-change") << 10;
		QTest::newRow("too-many-values") << 11; QTest::newRow("above-one") << 12;
	}
	void invalidRestoreCannotCommit()
	{
		QFETCH(int, change); VstParameterRestore restore; restore.begin(2, 2);
		std::vector<float> values{.25f, .75f}; int writes = 0;
		const auto writer = [&](int, float) { ++writes; };
		const auto invalid = [&] {
			switch (change)
			{
				case 0: restore.append(2, 1, values); break;
				case 1: restore.append(2, 0, std::span<const float>(values).first(1)); restore.append(2, 0, values); break;
				case 2: restore.append(2, 0, {}); break;
				case 3: values.resize(257, .25f); restore.append(2, 0, values); break;
				case 4: values[1] = std::numeric_limits<float>::quiet_NaN(); restore.append(2, 0, values); break;
				case 5: restore.append(3, 0, values); break; case 6: restore.begin(2, 2); break;
				case 7: restore.abort(); restore.begin(2, 3); break; case 8: restore.abort(); restore.append(2, 0, values); break;
				case 9: restore.append(2, 0, std::span<const float>(values).first(1)); restore.commit(2, writer); break;
				case 10: restore.append(2, 0, values); restore.commit(3, writer); break;
				case 11: values.push_back(.5f); restore.append(2, 0, values); break;
				case 12: values[1] = 1.01f; restore.append(2, 0, values); break;
			}
		};
		QVERIFY_EXCEPTION_THROWN(invalid(), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(restore.commit(2, writer), std::runtime_error); QCOMPARE(writes, 0);
		restore.begin(2, 2); const std::vector<float> valid{.25f, .75f}; restore.append(2, 0, valid); restore.commit(2, writer); QCOMPARE(writes, 2);
	}

	void setterExceptionClearsPendingRestore()
	{
		VstParameterRestore restore; const std::vector<float> values{.25f, .75f}; restore.begin(2, 2); restore.append(2, 0, values);
		int writes = 0;
		QVERIFY_EXCEPTION_THROWN(restore.commit(2, [&](int, float) { ++writes; throw std::runtime_error("Native setter failed"); }), std::runtime_error);
		QCOMPARE(writes, 1);
		QVERIFY_EXCEPTION_THROWN(restore.commit(2, [&](int, float) { ++writes; }), std::runtime_error); QCOMPARE(writes, 1);
		restore.begin(2, 2); restore.append(2, 0, values); restore.abort(); restore.abort();
		QVERIFY_EXCEPTION_THROWN(restore.commit(2, [&](int, float) { ++writes; }), std::runtime_error); QCOMPARE(writes, 1);
	}

	void emptyNativeLayoutCommitsWithoutSetters()
	{
		VstParameterRestore restore; int writes = 0; restore.begin(0, 0); restore.commit(0, [&](int, float) { ++writes; }); QCOMPARE(writes, 0);
	}
};

QTEST_GUILESS_MAIN(VstParameterTransferTest)
#include "VstParameterTransferTest.moc"
