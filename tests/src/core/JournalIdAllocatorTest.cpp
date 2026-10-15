/* Native ID aliasing and large-layout allocation. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QSet>
#include "JournalIdAllocator.h"
#include "ProjectJournal.h"

using namespace lmms;
class JournalIdAllocatorTest : public QObject
{
	Q_OBJECT
private slots:
	void largeLayoutsDoNotRestartTheirCollisionScan()
	{
		constexpr std::uint32_t count = 65537, flag = 1 << 23;
		std::uint32_t cursor = 1, probes = 0;
		QSet<std::uint32_t> reserved, saved;
		for (std::uint32_t index = 0; index < count; ++index)
		{
			const auto id = nextJournalId(cursor, flag, [&](std::uint32_t value) { ++probes; return reserved.contains(value); });
			QVERIFY(id & flag); QVERIFY(!saved.contains(id & ~flag));
			reserved.insert(id); saved.insert(id & ~flag);
		}
		QCOMPARE(reserved.size(), static_cast<int>(count)); QCOMPARE(probes, 2 * count);
	}

	void actualJournalReservesBothAliasesForLargeLayouts()
	{
		ProjectJournal journal;
		journal.reallocID(1, nullptr); journal.reallocID(ProjectJournal::idFromSave(2), nullptr);
		QSet<jo_id_t> saved;
		for (int index = 0; index < 65537; ++index)
		{
			const auto live = journal.allocID(nullptr), serialized = ProjectJournal::idToSave(live);
			QVERIFY(serialized != 1 && serialized != 2); QVERIFY(!saved.contains(serialized)); saved.insert(serialized);
			QCOMPARE(ProjectJournal::idFromSave(serialized), live);
		}
		QCOMPARE(saved.size(), 65537);
		journal.clearJournal(); // Retired reservations may be released without resetting the allocation cursor.
		const auto next = ProjectJournal::idToSave(journal.allocID(nullptr)); QVERIFY(!saved.contains(next));
	}

	void wrapSkipsLiveAndSavedReservations()
	{
		std::uint32_t cursor = 7;
		const QSet<std::uint32_t> reserved{15, 0, 9};
		const auto id = nextJournalId(cursor, 8, [&](std::uint32_t value) { return reserved.contains(value); });
		QCOMPARE(id, std::uint32_t{10}); QCOMPARE(cursor, std::uint32_t{3});
	}

	void exhaustionFailsAfterOneNamespaceTraversal_data()
	{
		QTest::addColumn<unsigned int>("flag");
		QTest::newRow("one-slot") << 1u; QTest::newRow("two-slots") << 2u;
		QTest::newRow("eight-slots") << 8u; QTest::newRow("256-slots") << 256u;
	}
	void exhaustionFailsAfterOneNamespaceTraversal()
	{
		QFETCH(unsigned int, flag); std::uint32_t cursor = flag - 1, probes = 0;
		const auto occupied = [&](std::uint32_t id) { ++probes; return id < flag; };
		QVERIFY_EXCEPTION_THROWN(nextJournalId(cursor, flag, occupied), std::overflow_error);
		QCOMPARE(probes, 2 * flag); QCOMPARE(cursor, flag - 1);
		const auto available = nextJournalId(cursor, flag, [&](std::uint32_t id) { return id < flag && id != flag - 1; });
		QCOMPARE(available, (flag - 1) | flag); QCOMPARE(cursor, std::uint32_t{0});
	}

	void invalidNamespaceOrCursorIsRefused_data()
	{
		QTest::addColumn<unsigned int>("flag"); QTest::addColumn<unsigned int>("position");
		QTest::newRow("empty") << 0u << 0u; QTest::newRow("non-power-of-two") << 3u << 0u;
		QTest::newRow("cursor-at-end") << 8u << 8u; QTest::newRow("cursor-outside") << 8u << 99u;
	}
	void invalidNamespaceOrCursorIsRefused()
	{
		QFETCH(unsigned int, flag); QFETCH(unsigned int, position); std::uint32_t cursor = position; int probes = 0;
		QVERIFY_EXCEPTION_THROWN(nextJournalId(cursor, flag, [&](std::uint32_t) { ++probes; return false; }), std::invalid_argument);
		QCOMPARE(probes, 0); QCOMPARE(cursor, position);
	}
};

QTEST_GUILESS_MAIN(JournalIdAllocatorTest)
#include "JournalIdAllocatorTest.moc"
