/* Shared native file search and Qt browser adapter. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <stdexcept>
#include "Engine.h"
#include "FileSearch.h"
#include "FileSearchJob.h"
using namespace lmms;
namespace
{
bool write(const QString& path) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write("candidate") == 9; }
FileSearch::Task task(const QString& path) { FileSearch::Task value; value.paths = QStringList{path}; value.extensions = QStringList{"*"}; value.includeDirectories = false; return value; }
}
class FileSearchTest : public QObject
{
	Q_OBJECT
private slots:
	void tokenAndSuffixSemanticsRemainNative_data()
	{
		QTest::addColumn<QString>("query"); QTest::addColumn<QString>("extension"); QTest::addColumn<bool>("directory"); QTest::addColumn<bool>("expected");
		QTest::newRow("all") << QString() << QString("*") << false << true;
		QTest::newRow("tokens") << QString("warm bass") << QString("*.wav") << false << true;
		QTest::newRow("phrase") << QString("\"WARM BASS\"") << QString("*.wav") << false << true;
		QTest::newRow("wrong-phrase") << QString("\"warm take\"") << QString("*.wav") << false << false;
		QTest::newRow("missing-token") << QString("warm absent") << QString("*.wav") << false << false;
		QTest::newRow("compound-suffix") << QString("bass") << QString("*.take.wav") << false << true;
		QTest::newRow("different-suffix") << QString("bass") << QString("*.ogg") << false << false;
		QTest::newRow("directory-ignores-extension") << QString("warm bass") << QString("*.ogg") << true << true;
		QTest::newRow("literal-query") << QString("[missing]") << QString("*.wav") << false << false;
	}
	void tokenAndSuffixSemanticsRemainNative()
	{
		QFETCH(QString, query); QFETCH(QString, extension); QFETCH(bool, directory); QFETCH(bool, expected); QTemporaryDir root; const auto path = root.filePath("Warm Bass.Take.WAV"); if (directory) { QVERIFY(QDir().mkdir(path)); } else { QVERIFY(write(path)); } auto search = task(root.path()); search.filter = query; search.extensions = QStringList{extension}; search.includeDirectories = true; QCOMPARE(FileSearch::matches(search, FileSearch::tokens(query), QFileInfo(path)), expected); QVERIFY(!Engine::getSong());
	}
	void recursionAndHiddenFlagsFollowNativeMetadata_data()
	{ QTest::addColumn<bool>("recursive"); QTest::addColumn<bool>("hidden"); QTest::newRow("shallow") << false << false; QTest::newRow("recursive") << true << false; QTest::newRow("shallow-hidden") << false << true; QTest::newRow("recursive-hidden") << true << true; }
	void recursionAndHiddenFlagsFollowNativeMetadata()
	{
		QFETCH(bool, recursive); QFETCH(bool, hidden); QTemporaryDir root; QVERIFY(QDir().mkdir(root.filePath("nested"))); QVERIFY(write(root.filePath("one.wav"))); QVERIFY(write(root.filePath("nested/two.wav"))); const auto special = root.filePath(".hidden.wav"); QVERIFY(write(special)); auto search = task(root.path()); search.recursive = recursive; if (hidden) { search.dirFilters |= QDir::Hidden; } const auto result = FileSearch::scan(search, {}); const qint64 expected = 1 + (recursive ? 1 : 0) + (hidden || !QFileInfo(special).isHidden() ? 1 : 0); QCOMPARE(result.matches, expected); QVERIFY(!result.cancelled); QVERIFY(!result.limited);
	}
	void entryBudgetDistinguishesExactEndFromTruncation_data()
	{ QTest::addColumn<int>("limit"); QTest::newRow("limited") << 2; QTest::newRow("exact-end") << 3; }
	void entryBudgetDistinguishesExactEndFromTruncation()
	{
		QFETCH(int, limit); QTemporaryDir root; for (int i = 0; i < 3; ++i) { QVERIFY(write(root.filePath(QString::number(i) + ".wav"))); } auto search = task(root.path()); search.maximumEntries = limit; const auto result = FileSearch::scan(search, {}); QCOMPARE(result.entries, qint64(limit)); QCOMPARE(result.matches, qint64(limit)); QCOMPARE(result.limited, limit < 3);
	}
	void multipleRootsShareOneEntryBudget()
	{
		QTemporaryDir first, second; QVERIFY(write(first.filePath("one.wav"))); QVERIFY(write(second.filePath("two.wav"))); QVERIFY(write(second.filePath("three.wav"))); auto search = task(first.path()); search.paths.append(second.path()); search.maximumEntries = 2; const auto result = FileSearch::scan(search, {}); QCOMPARE(result.entries, qint64(2)); QCOMPARE(result.matches, qint64(2)); QVERIFY(result.limited);
	}
	void unavailableRootsAreNotReportedAsSuccessfulCoverage()
	{
		QTemporaryDir root; auto search = task(root.filePath("missing")); const auto result = FileSearch::scan(search, {}); QCOMPARE(result.entries, qint64(0)); QCOMPARE(result.unavailableRoots, qint64(1));
	}
	void cancellationStopsBeforeAnotherEntryOrRoot()
	{
		QTemporaryDir first, second; QVERIFY(write(first.filePath("one.wav"))); QVERIFY(write(second.filePath("two.wav"))); auto search = task(first.path()); search.paths.append(second.path()); bool stop = false; FileSearch::Callbacks callbacks; callbacks.cancelled = [&] { return stop; }; callbacks.entry = [&](const QFileInfo&, bool) { stop = true; }; const auto result = FileSearch::scan(search, callbacks); QVERIFY(result.cancelled); QCOMPARE(result.entries, qint64(1));
	}
	void taskAndCallbacksAreOwnedAcrossReturnedEntries()
	{
		QTemporaryDir root; QVERIFY(write(root.filePath("one.wav"))); QVERIFY(write(root.filePath("two.wav"))); auto search = task(root.path()); FileSearch::Callbacks callbacks; int matches = 0; callbacks.match = [&](const QString&) { ++matches; }; callbacks.entry = [&](const QFileInfo&, bool) { search.extensions.clear(); callbacks.match = [&](const QString&) { throw std::runtime_error("Borrowed callback was reused"); }; }; const auto result = FileSearch::scan(search, callbacks); QCOMPARE(result.matches, qint64(2)); QCOMPARE(matches, 2);
	}
	void callbackExceptionsReleaseTheIterator_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void callbackExceptionsReleaseTheIterator()
	{
		QFETCH(bool, nonstandard); QTemporaryDir root; QVERIFY(write(root.filePath("one.wav"))); FileSearch::Callbacks callbacks; callbacks.entry = [&](const QFileInfo&, bool) { if (nonstandard) { throw 83; } throw std::runtime_error("Native search callback failure"); }; bool failed = false; try { FileSearch::scan(task(root.path()), callbacks); } catch (...) { failed = true; } QVERIFY(failed); QCOMPARE(FileSearch::scan(task(root.path()), {}).matches, qint64(1));
	}
	void browserWorkerUsesSharedMatchingWithoutAnEngineOrWindow()
	{
		QTemporaryDir root; QVERIFY(write(root.filePath("take.one.wav"))); QVERIFY(write(root.filePath("take.two.wav"))); gui::FileSearchJob job; QSignalSpy started(&job, &gui::FileSearchJob::started), finished(&job, &gui::FileSearchJob::finished), found(&job, &gui::FileSearchJob::foundMatch); auto search = task(root.path()); search.filter = "take"; search.extensions = QStringList{"*.wav"}; job.search(search); QTRY_COMPARE(finished.count(), 1); QCOMPARE(started.count(), 1); QCOMPARE(found.count(), 2); search.filter = "absent"; job.search(search); QTRY_COMPARE(finished.count(), 2); QCOMPARE(found.count(), 2); QVERIFY(!Engine::getSong());
	}
	void negativeEntryBudgetsAreRefused()
	{ auto search = task(QString{}); search.maximumEntries = -1; QVERIFY_EXCEPTION_THROWN(FileSearch::scan(search, {}), std::invalid_argument); }
};
QTEST_GUILESS_MAIN(FileSearchTest)
#include "FileSearchTest.moc"
