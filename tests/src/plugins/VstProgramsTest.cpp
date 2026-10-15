/* Native VST program queries and selection. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <limits>
#include "VstPrograms.h"

using namespace lmms;

class VstProgramsTest : public QObject
{
	Q_OBJECT
private slots:
	void pagesBeyond256RetainSeparatorsAndUnsavedCurrentName()
	{
		int indexed = 0, currentReads = 0;
		const auto page = readVstPrograms(700, 512, 510, 4, [&](int index) -> std::optional<std::string> {
			++indexed; return std::to_string(index) + ": A|B";
		}, [&] { ++currentReads; return "Unsaved | edit"; });
		QCOMPARE(page.total, 700); QCOMPARE(page.current, 512); QCOMPARE(page.offset, 510); QCOMPARE(page.programs.size(), std::size_t{4});
		QCOMPARE(indexed, 3); QCOMPARE(currentReads, 1);
		QCOMPARE(QString::fromStdString(page.programs[0].name), QString("510: A|B"));
		QCOMPARE(QString::fromStdString(page.programs[2].name), QString("Unsaved | edit")); QVERIFY(page.programs[2].nameAvailable);
		const auto tail = readVstPrograms(700, 512, 699, 1024, [](int) { return std::optional<std::string>{"Tail"}; }, [] { return "Current"; });
		QCOMPARE(tail.programs.size(), std::size_t{1}); QCOMPARE(tail.programs[0].index, 699);
	}

	void unavailableNamesDoNotRequireSelectingOtherPrograms()
	{
		const auto page = readVstPrograms(4, 2, 0, 4, [](int) { return std::optional<std::string>{}; }, [] { return "Live unsaved sound"; });
		for (const auto& program : page.programs)
		{
			QCOMPARE(program.nameAvailable, program.index == 2);
			if (program.index != 2) { QVERIFY(program.name.empty()); }
		}
	}

	void zeroAndOutOfRangePagesNeverQueryThePlugin()
	{
		int reads = 0;
		const auto indexed = [&](int) -> std::optional<std::string> { ++reads; return "unexpected"; };
		const auto current = [&] { ++reads; return "unexpected"; };
		const auto zero = readVstPrograms(0, 0, 0, 1, indexed, current); QCOMPARE(zero.current, -1); QVERIFY(zero.programs.empty());
		const auto after = readVstPrograms(700, 500, std::numeric_limits<int>::max(), 1024, indexed, current);
		QVERIFY(after.programs.empty()); QCOMPARE(reads, 0);
	}

	void invalidQueryBoundsFailBeforeCallbacks_data()
	{
		QTest::addColumn<int>("count"); QTest::addColumn<int>("current"); QTest::addColumn<int>("offset"); QTest::addColumn<int>("limit");
		QTest::newRow("negative-count") << -1 << 0 << 0 << 1;
		QTest::newRow("negative-current") << 2 << -1 << 0 << 1;
		QTest::newRow("excess-current") << 2 << 2 << 0 << 1;
		QTest::newRow("negative-offset") << 2 << 0 << -1 << 1;
		QTest::newRow("empty-limit") << 2 << 0 << 0 << 0;
		QTest::newRow("excess-limit") << 2 << 0 << 0 << 1025;
	}
	void invalidQueryBoundsFailBeforeCallbacks()
	{
		QFETCH(int, count); QFETCH(int, current); QFETCH(int, offset); QFETCH(int, limit); int reads = 0;
		QVERIFY_EXCEPTION_THROWN(readVstPrograms(count, current, offset, limit,
			[&](int) -> std::optional<std::string> { ++reads; return {}; }, [&] { ++reads; return ""; }), std::runtime_error);
		QCOMPARE(reads, 0);
	}

	void checkedSelectionUsesNativeIndexAndVerifiesReadback()
	{
		int selected = 4, writes = 0;
		const auto write = [&](int index) { ++writes; selected = index; }; const auto read = [&] { return selected; };
		QCOMPARE(selectVstProgram(700, 512, write, read), 512); QCOMPARE(writes, 1);
		for (int invalid : {-1, 700, std::numeric_limits<int>::max()})
		{ QVERIFY_EXCEPTION_THROWN(selectVstProgram(700, invalid, write, read), std::out_of_range); }
		QVERIFY_EXCEPTION_THROWN(selectVstProgram(0, 0, write, read), std::out_of_range); QCOMPARE(writes, 1); QCOMPARE(selected, 512);
		QVERIFY_EXCEPTION_THROWN(selectVstProgram(700, 20, [](int) {}, read), std::runtime_error); QCOMPARE(selected, 512);
	}

	void callbackFailuresAreReported()
	{
		QVERIFY_EXCEPTION_THROWN(readVstPrograms(2, 0, 1, 1,
			[](int) -> std::optional<std::string> { throw std::runtime_error("native query failed"); }, [] { return "Current"; }), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(selectVstProgram(2, 1, [](int) { throw std::runtime_error("native selection failed"); }, [] { return 0; }), std::runtime_error);
	}
};

QTEST_GUILESS_MAIN(VstProgramsTest)
#include "VstProgramsTest.moc"
