/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioRuntime.h"
#include "StudioStorage.h"

namespace lmms
{
class StudioStorageTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	static QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
	static QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
	static bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
	static QJsonObject manifest(const QTemporaryDir& directory) { return QJsonDocument::fromJson(read(directory.filePath("session.json"))).object(); }
	static bool save(const QTemporaryDir& directory, const QJsonObject& state) { return write(directory.filePath("session.json"), QJsonDocument(state).toJson(QJsonDocument::Compact)); }
	static QJsonObject reference(const QByteArray& bytes, bool receipt = false)
	{
		return {{"path", QString(receipt ? "receipts/" : "checkpoints/") + QUuid::createUuid().toString(QUuid::WithoutBraces) + (receipt ? ".json" : ".mmp")},
			{"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
	}
	static bool seed(const QTemporaryDir& directory, QString& error)
	{
		StudioRuntime studio(directory.path()); if (!studio.open(error)) { return false; }
		for (const auto tempo : {91, 123})
		{
			const auto response = studio.process(request("tempo-" + QString::number(tempo), "song.set", {{"tempo", tempo}}));
			if (!response.value("ok").toBool()) { error = response.value("error").toString(); return false; }
		}
		return true;
	}
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->stop(); Engine::getSong()->clearProject();
		Engine::getSong()->tempoModel().setInitValue(DefaultTempo); Engine::getSong()->clearErrors();
	}
	void branchRetirementPreservesRetainedUndoAndUnownedFiles()
	{
		QTemporaryDir directory; QString error; QVERIFY2(seed(directory, error), qPrintable(error));
		const auto orphan = reference("unowned checkpoint").value("path").toString(); QVERIFY(write(directory.filePath(orphan), "unowned checkpoint"));
		QVERIFY(QDir().mkpath(directory.filePath("checkpoints/assets"))); QVERIFY(write(directory.filePath("checkpoints/assets/keep.wav"), "retained media"));
		QVERIFY(write(directory.filePath("export.mmp"), "user export"));
		QString retired;
		{
			StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error));
			const auto before = manifest(directory); retired = before.value("history").toArray().last().toObject().value("path").toString();
			QVERIFY(studio.process(request("undo", "session.undo")).value("ok").toBool()); QCOMPARE(Engine::getSong()->getTempo(), 91);
			QVERIFY(QFileInfo::exists(directory.filePath(retired))); // Redo remains reachable until the branch changes.
			QVERIFY(studio.process(request("branch", "song.set", {{"tempo", 97}})).value("ok").toBool());
			QVERIFY(!QFileInfo::exists(directory.filePath(retired)));
			const auto retiredEntries = manifest(directory).value("retiredFiles").toArray(); QCOMPARE(retiredEntries.size(), 1);
			QCOMPARE(retiredEntries.first().toObject().value("path").toString(), retired);
			for (const auto& entry : manifest(directory).value("history").toArray()) { QVERIFY(QFileInfo::exists(directory.filePath(entry.toObject().value("path").toString()))); }
			const auto info = studio.process(request("storage", "session.inspect")); QVERIFY(info.value("ok").toBool());
			QCOMPARE(info.value("result").toObject().value("storage").toObject().value("lastCleanup").toObject().value("removed").toInt(), 1);
			QVERIFY(manifest(directory).value("retiredFiles").toArray().isEmpty());
			QVERIFY(studio.process(request("undo-branch", "session.undo")).value("ok").toBool()); QCOMPARE(Engine::getSong()->getTempo(), 91);
			QVERIFY(studio.process(request("redo-branch", "session.redo")).value("ok").toBool()); QCOMPARE(Engine::getSong()->getTempo(), 97);
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(Engine::getSong()->getTempo(), 97);
		QCOMPARE(read(directory.filePath(orphan)), QByteArray("unowned checkpoint")); QCOMPARE(read(directory.filePath("export.mmp")), QByteArray("user export"));
		QCOMPARE(read(directory.filePath("checkpoints/assets/keep.wav")), QByteArray("retained media"));
	}
	void historyWindowReclaimsOnlyExpiredCheckpoints()
	{
		QTemporaryDir directory; QString error;
		{
			StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error));
			const auto initial = manifest(directory).value("history").toArray().first().toObject().value("path").toString();
			for (int i = 0; i < 258; ++i) { QVERIFY(studio.process(request("edit-" + QString::number(i), "song.set", {{"tempo", 91 + i % 2}})).value("ok").toBool()); }
			const auto history = manifest(directory).value("history").toArray(); QCOMPARE(history.size(), 256);
			QVERIFY(!QFileInfo::exists(directory.filePath(initial))); QCOMPARE(QDir(directory.filePath("checkpoints")).entryList({"*.mmp"}, QDir::Files).size(), 256);
			QVERIFY(studio.process(request("undo", "session.undo")).value("ok").toBool()); QCOMPARE(Engine::getSong()->getTempo(), 91);
			QVERIFY(studio.process(request("redo", "session.redo")).value("ok").toBool()); QCOMPARE(Engine::getSong()->getTempo(), 92);
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(Engine::getSong()->getTempo(), 92);
	}
	void expiredReceiptIsRemovedWithoutChangingReplaySemantics()
	{
		QTemporaryDir directory; QString error; StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error));
		const auto large = request("large", "session.memory.set", {{"memory", QJsonObject{{"preference", QString(20000, 'm')}}}});
		const auto first = studio.process(large); QVERIFY(first.value("ok").toBool());
		const auto archive = manifest(directory).value("requests").toArray().last().toObject().value("archive").toObject(); QVERIFY(!archive.isEmpty());
		QVERIFY(studio.process(large).value("replayed").toBool());
		for (int i = 0; i < 128; ++i) { QVERIFY(studio.process(request("small-" + QString::number(i), "transport.inspect")).value("ok").toBool()); }
		QVERIFY(!QFileInfo::exists(directory.filePath(archive.value("path").toString())));
		const auto current = studio.process(request("small-127", "transport.inspect")); QVERIFY(current.value("replayed").toBool());
		const auto fresh = studio.process(large); QVERIFY(fresh.value("ok").toBool()); QVERIFY(!fresh.value("replayed").toBool());
		QCOMPARE(fresh.value("result"), first.value("result"));
	}
	void failedManifestCommitCannotRetireRedo()
	{
		QTemporaryDir directory; QString error; QVERIFY2(seed(directory, error), qPrintable(error));
		const auto path = directory.filePath("session.json"), backup = directory.filePath("saved-session.json");
		QString redo; QByteArray acknowledged;
		{
			StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error));
			redo = manifest(directory).value("history").toArray().last().toObject().value("path").toString();
			QVERIFY(studio.process(request("undo", "session.undo")).value("ok").toBool()); acknowledged = read(path); bool obstructed = false;
			const auto connection = connect(Engine::getSong(), &Song::tempoChanged, this, [&](bpm_t tempo) {
				if (tempo == 97 && !obstructed) { obstructed = QFile::rename(path, backup) && QDir().mkdir(path); }
			}, Qt::DirectConnection);
			const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
			const auto failed = studio.process(request("branch", "song.set", {{"tempo", 97}}));
			QVERIFY(obstructed); QVERIFY(!failed.value("ok").toBool()); QVERIFY(studio.stopping()); QCOMPARE(Engine::getSong()->getTempo(), 91);
			QVERIFY(QFileInfo::exists(directory.filePath(redo))); QCOMPARE(read(backup), acknowledged);
		}
		QVERIFY(QDir().rmdir(path)); QVERIFY(QFile::rename(backup, path));
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		QVERIFY(recovered.process(request("redo", "session.redo")).value("ok").toBool()); QCOMPARE(Engine::getSong()->getTempo(), 123);
	}
	void interruptedCleanupRetriesAfterRestart_data()
	{
		QTest::addColumn<bool>("alreadyRemoved"); QTest::newRow("before-removal") << false; QTest::newRow("after-removal-before-queue-prune") << true;
	}
	void interruptedCleanupRetriesAfterRestart()
	{
		QFETCH(bool, alreadyRemoved); QTemporaryDir directory; QString error; QVERIFY2(seed(directory, error), qPrintable(error));
		const QByteArray bytes("a formerly committed file"); const auto retired = reference(bytes); const auto path = directory.filePath(retired.value("path").toString());
		if (!alreadyRemoved) { QVERIFY(write(path, bytes)); }
		auto state = manifest(directory); state.insert("retiredFiles", QJsonArray{retired}); QVERIFY(save(directory, state));
		{
			StudioRuntime cancelled(directory.path(), {}, stdout, [] { return true; }); QVERIFY2(cancelled.open(error), qPrintable(error));
			QCOMPARE(QFileInfo::exists(path), !alreadyRemoved); QCOMPARE(manifest(directory).value("retiredFiles").toArray().size(), 1);
		}
		{
			StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QVERIFY(!QFileInfo::exists(path)); QCOMPARE(Engine::getSong()->getTempo(), 123);
			const auto response = recovered.process(request("storage", "session.inspect")); const auto report = response.value("result").toObject().value("storage").toObject().value("lastCleanup").toObject();
			QCOMPARE(report.value(alreadyRemoved ? "alreadyAbsent" : "removed").toInt(), 1); QVERIFY(manifest(directory).value("retiredFiles").toArray().isEmpty());
		}
	}
	void invalidRetirementQueueRejectsStartup_data()
	{
		QTest::addColumn<int>("fault"); for (int i = 0; i < 8; ++i) { QTest::newRow(qPrintable(QString::number(i))) << i; }
	}
	void invalidRetirementQueueRejectsStartup()
	{
		QFETCH(int, fault); QTemporaryDir directory; QString error; QVERIFY2(seed(directory, error), qPrintable(error));
		auto state = manifest(directory); auto retired = reference("orphan"); QJsonArray entries;
		if (fault == 0) { retired.insert("path", "../outside.mmp"); }
		if (fault == 1) { retired.insert("path", "checkpoints/assets/" + QString(64, 'a') + ".wav"); }
		if (fault == 2) { retired.insert("sha256", "invalid"); }
		if (fault == 3) { retired.insert("unrecognized", true); }
		if (fault == 4)
		{
			const auto current = state.value("history").toArray().last().toObject();
			retired = {{"path", current.value("path")}, {"sha256", current.value("sha256")}};
		}
		entries.append(retired); if (fault == 5) { entries.append(retired); }
		if (fault == 6) { for (int i = 0; i < StudioStorage::MaximumRetiredFiles; ++i) { entries.append(reference("extra")); } }
		state.insert("retiredFiles", fault == 7 ? QJsonValue("not an array") : QJsonValue(entries)); QVERIFY(save(directory, state));
		const auto before = read(directory.filePath("session.json")); Engine::getSong()->setTempo(77);
		StudioRuntime failed(directory.path()); QVERIFY(!failed.open(error)); QVERIFY(!error.isEmpty());
		QCOMPARE(Engine::getSong()->getTempo(), 77); QCOMPARE(read(directory.filePath("session.json")), before);
	}
	void changedFilesAreDeferredAndCleanupRotates()
	{
		QTemporaryDir directory; QVERIFY(QDir().mkpath(directory.filePath("receipts"))); QJsonArray entries; QString altered;
		for (int i = 0; i < 20; ++i)
		{
			const QByteArray bytes = QByteArray::number(i); const auto entry = reference(bytes, true); const auto path = directory.filePath(entry.value("path").toString());
			QVERIFY(write(path, i == 0 ? QByteArray("changed") : bytes)); entries.append(entry); if (i == 0) { altered = path; }
		}
		const QJsonObject state{{"retiredFiles", entries}}; QSet<QString> collected; int cursor = 0;
		const QDir root(directory.path()); const auto first = StudioStorage::collect(root, root.canonicalPath(), state, collected, cursor);
		QCOMPARE(first.value("attempted").toInt(), 16); QCOMPARE(first.value("removed").toInt(), 15); QCOMPARE(first.value("pending").toInt(), 5);
		const auto second = StudioStorage::collect(root, root.canonicalPath(), state, collected, cursor);
		QCOMPARE(second.value("removed").toInt(), 4); QCOMPARE(second.value("pending").toInt(), 1); QCOMPARE(read(altered), QByteArray("changed"));
		QVERIFY(!second.value("errors").toArray().isEmpty());
	}
	void linksAndReplacedRootsAreNeverCollected_data()
	{
		QTest::addColumn<int>("fault"); QTest::newRow("wrong-root") << 0; QTest::newRow("directory-instead-of-file") << 1;
#ifndef Q_OS_WIN
		QTest::newRow("file-symlink") << 2; QTest::newRow("directory-symlink") << 3;
#endif
	}
	void linksAndReplacedRootsAreNeverCollected()
	{
		QFETCH(int, fault); QTemporaryDir directory, outside; const QByteArray bytes("owned before replacement"); const auto entry = reference(bytes, true);
		const auto relative = entry.value("path").toString(), path = directory.filePath(relative), target = outside.filePath(QFileInfo(path).fileName());
		QVERIFY(write(target, bytes)); const QDir root(directory.path()); QVERIFY(QDir().mkpath(directory.filePath("receipts")));
		if (fault == 0) { QVERIFY(write(path, bytes)); }
		if (fault == 1) { QVERIFY(QDir().mkdir(path)); }
#ifndef Q_OS_WIN
		if (fault == 2) { QVERIFY(QFile::link(target, path)); }
		if (fault == 3) { QVERIFY(QDir().rmdir(directory.filePath("receipts"))); QVERIFY(QFile::link(outside.path(), directory.filePath("receipts"))); }
#endif
		QSet<QString> collected; int cursor = 0;
		const auto report = StudioStorage::collect(root, fault == 0 ? outside.path() : root.canonicalPath(), {{"retiredFiles", QJsonArray{entry}}}, collected, cursor);
		QCOMPARE(report.value("removed").toInt(), 0); QCOMPARE(report.value("pending").toInt(), 1); QVERIFY(collected.isEmpty());
		QCOMPARE(read(target), bytes); QVERIFY(QFileInfo::exists(path)); QVERIFY(!report.value("errors").toArray().isEmpty());
	}
	void retirementQueueLimitPreservesTheCommittedState()
	{
		QJsonArray retired;
		for (int i = 0; i < StudioStorage::MaximumRetiredFiles; ++i) { retired.append(reference("pending")); }
		const auto current = reference("current");
		const QJsonObject previous{{"retiredFiles", retired}, {"history", QJsonArray{current}}};
		QJsonObject next{{"history", QJsonArray{reference("replacement")}}}; const auto before = next; QString error;
		QVERIFY(StudioStorage::validRetirements(previous)); QVERIFY(!StudioStorage::prepare(previous, next, {}, error));
		QVERIFY(error.contains("4096")); QCOMPARE(next, before);
		QSet<QString> collected{retired.first().toObject().value("path").toString()};
		QVERIFY2(StudioStorage::prepare(previous, next, collected, error), qPrintable(error));
		QCOMPARE(next.value("retiredFiles").toArray().size(), StudioStorage::MaximumRetiredFiles);
		QVERIFY(next.value("retiredFiles").toArray().contains(current));
	}
	void cancellationDuringHashLeavesTheFileForRetry()
	{
		QTemporaryDir directory; QVERIFY(QDir().mkpath(directory.filePath("receipts"))); const QByteArray bytes(2 * 1024 * 1024, 'x');
		const auto entry = reference(bytes, true); const auto path = directory.filePath(entry.value("path").toString()); QVERIFY(write(path, bytes));
		QSet<QString> collected; int cursor = 0, checks = 0; const QDir root(directory.path());
		const QJsonObject state{{"retiredFiles", QJsonArray{entry}}};
		const auto cancelled = StudioStorage::collect(root, root.canonicalPath(), state, collected, cursor, [&] { return ++checks > 8; });
		QVERIFY(checks > 8); QCOMPARE(cancelled.value("removed").toInt(), 0); QCOMPARE(read(path), bytes); QVERIFY(collected.isEmpty());
		const auto retried = StudioStorage::collect(root, root.canonicalPath(), state, collected, cursor);
		QCOMPARE(retried.value("removed").toInt(), 1); QVERIFY(!QFileInfo::exists(path));
	}
	void cleanupCannotDispatchNestedQtWork()
	{
		QTemporaryDir directory; QVERIFY(QDir().mkpath(directory.filePath("receipts"))); const QByteArray bytes(2 * 1024 * 1024, 'x');
		const auto entry = reference(bytes, true); const auto path = directory.filePath(entry.value("path").toString()); QVERIFY(write(path, bytes));
		bool dispatched = false; QObject lifetime; QTimer::singleShot(0, &lifetime, [&] { dispatched = true; });
		FileResourceIO::Scope outer([] { return false; }, true); QTest::qSleep(15);
		QSet<QString> collected; int cursor = 0; const QDir root(directory.path());
		const auto report = StudioStorage::collect(root, root.canonicalPath(), {{"retiredFiles", QJsonArray{entry}}}, collected, cursor);
		QVERIFY(!dispatched); QCOMPARE(report.value("removed").toInt(), 1); QVERIFY(!QFileInfo::exists(path));
		QCoreApplication::processEvents(); QVERIFY(dispatched);
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::StudioStorageTest)
#include "StudioStorageTest.moc"
