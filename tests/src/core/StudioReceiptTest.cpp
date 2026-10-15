/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <memory>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioRuntime.h"
#include "Track.h"

namespace lmms
{
class StudioReceiptTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	static QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
	{
		return {{"id", id}, {"op", op}, {"args", args}};
	}
	static QJsonObject largeEdit(const QString& id = "large-edit")
	{
		return request(id, "track.add", {{"type", "automation"}, {"name", QString(20000, 'n')}});
	}
	static QByteArray read(const QString& path)
	{
		QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
	}
	static bool write(const QString& path, const QByteArray& bytes)
	{
		QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
	}
	static QJsonObject manifest(const QTemporaryDir& directory)
	{
		return QJsonDocument::fromJson(read(directory.filePath("session.json"))).object();
	}
	static QString hash(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
	static QJsonObject receipt(const QTemporaryDir& directory, const QString& id = "large-edit")
	{
		for (const auto& entry : manifest(directory).value("requests").toArray())
		{
			if (entry.toObject().value("id").toString() == id) { return entry.toObject(); }
		}
		return {};
	}
	static QJsonObject nativeJob(const QTemporaryDir& directory) { return manifest(directory).value("agent").toObject().value("job").toObject(); }
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
	void largeReceiptsReplayWithoutRepeatingNativeEdits_data()
	{
		QTest::addColumn<bool>("restart"); QTest::addColumn<bool>("largeResponse");
		for (const auto restart : {false, true})
		{
			QTest::newRow(restart ? "request-restart" : "request-same-process") << restart << false;
			QTest::newRow(restart ? "response-restart" : "response-same-process") << restart << true;
		}
	}
	void largeReceiptsReplayWithoutRepeatingNativeEdits()
	{
		QFETCH(bool, restart); QFETCH(bool, largeResponse); QTemporaryDir directory; QString error;
		auto studio = std::make_unique<StudioRuntime>(directory.path()); QVERIFY2(studio->open(error), qPrintable(error));
		if (largeResponse)
		{
			QVERIFY(studio->process(request("memory", "session.memory.set", {{"memory", QJsonObject{{"payload", QString(20000, 'm')}}}})).value("ok").toBool());
		}
		const auto count = Engine::getSong()->tracks().size(); const auto edit = largeResponse ? request("large-edit", "session.inspect") : largeEdit();
		const auto response = studio->process(edit); const int added = largeResponse ? 0 : 1;
		QVERIFY(response.value("ok").toBool()); QCOMPARE(Engine::getSong()->tracks().size(), count + added);
		if (largeResponse) { QVERIFY(QJsonDocument(response).toJson(QJsonDocument::Compact).size() > 20000); }
		const auto saved = receipt(directory); const auto archive = saved.value("archive").toObject(); QVERIFY(!archive.isEmpty());
		QVERIFY(!saved.contains("request")); QVERIFY(!saved.contains("response"));
		const auto bytes = read(directory.filePath(archive.value("path").toString())); QCOMPARE(bytes.size(), archive.value("bytes").toInt());
		QCOMPARE(hash(bytes), archive.value("sha256").toString()); const auto content = QJsonDocument::fromJson(bytes).object();
		QCOMPARE(content.value("request").toObject(), edit); QCOMPARE(content.value("response").toObject(), response);
		QVERIFY(QJsonDocument(manifest(directory).value("requests").toArray()).toJson(QJsonDocument::Compact).size() < 8192);
		if (largeResponse) { QVERIFY(studio->process(request("clear-memory", "session.memory.set", {{"memory", QJsonObject{}}})).value("ok").toBool()); }
		QVERIFY(studio->process(request("tempo", "song.set", {{"tempo", 97}})).value("ok").toBool()); QVERIFY(receipt(directory, "tempo").contains("response"));
		if (restart) { studio.reset(); studio = std::make_unique<StudioRuntime>(directory.path()); QVERIFY2(studio->open(error), qPrintable(error)); }
		const auto before = manifest(directory); auto replay = studio->process(edit); QVERIFY(replay.value("replayed").toBool());
		replay.remove("replayed"); QCOMPARE(replay, response); QCOMPARE(manifest(directory), before);
		QCOMPARE(Engine::getSong()->tracks().size(), count + added); QCOMPARE(Engine::getSong()->getTempo(), 97);
		QVERIFY(!studio->process(request("large-edit", "song.set", {{"tempo", 101}})).value("ok").toBool());
		QCOMPARE(manifest(directory), before); QCOMPARE(Engine::getSong()->getTempo(), 97);
	}
	void invalidReceiptsRejectOpenBeforeNativeReplacement_data()
	{
		QTest::addColumn<int>("fault");
		for (int i = 0; i < 7; ++i) { QTest::newRow(qPrintable(QString::number(i))) << i; }
	}
	void invalidReceiptsRejectOpenBeforeNativeReplacement()
	{
		QFETCH(int, fault); QTemporaryDir directory; QString error;
		{
			StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(largeEdit()).value("ok").toBool());
		}
		auto state = manifest(directory); auto entries = state.value("requests").toArray(); auto entry = entries.at(0).toObject();
		auto archive = entry.value("archive").toObject(); const auto path = directory.filePath(archive.value("path").toString()); const auto original = read(path);
		if (fault == 0) { QVERIFY(QFile::remove(path)); }
		if (fault == 1) { QVERIFY(write(path, original + " ")); }
		if (fault == 2) { archive.insert("bytes", archive.value("bytes").toInt() + 1); }
		if (fault >= 3 && fault <= 5)
		{
			auto content = QJsonDocument::fromJson(original).object(); auto req = content.value("request").toObject(); auto response = content.value("response").toObject();
			if (fault == 3)
			{
				content.insert("id", "different-id"); req.insert("id", "different-id"); response.insert("id", "different-id");
				content.insert("sha256", hash(QJsonDocument(req).toJson(QJsonDocument::Compact)));
			}
			if (fault == 4) { response.insert("revision", state.value("revision").toInt() + 1); }
			if (fault == 5) { req.insert("op", "song.set"); }
			content.insert("request", req); content.insert("response", response); const auto bytes = QJsonDocument(content).toJson(QJsonDocument::Compact);
			QVERIFY(write(path, bytes)); archive.insert("sha256", hash(bytes)); archive.insert("bytes", int(bytes.size()));
		}
		if (fault == 6) { archive.insert("path", "../session.json"); }
		entry.insert("archive", archive); entries[0] = entry; state.insert("requests", entries);
		const auto before = QJsonDocument(state).toJson(QJsonDocument::Compact); QVERIFY(write(directory.filePath("session.json"), before));
		Engine::getSong()->setTempo(77); StudioRuntime failed(directory.path()); QVERIFY(!failed.open(error));
		QVERIFY(error.contains("receipt", Qt::CaseInsensitive) || error.contains("manifest", Qt::CaseInsensitive));
		QCOMPARE(Engine::getSong()->getTempo(), 77); QCOMPARE(read(directory.filePath("session.json")), before);
	}
	void failedFreshReplayNeverRepeatsTheNativeEdit_data()
	{
		QTest::addColumn<bool>("remove"); QTest::newRow("missing") << true; QTest::newRow("altered") << false;
	}
	void failedFreshReplayNeverRepeatsTheNativeEdit()
	{
		QFETCH(bool, remove); QTemporaryDir directory; QString error; StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error));
		const auto edit = largeEdit(); const auto first = studio.process(edit); QVERIFY(first.value("ok").toBool());
		const auto archive = receipt(directory).value("archive").toObject(); const auto path = directory.filePath(archive.value("path").toString()); const auto original = read(path);
		if (remove) { QVERIFY(QFile::remove(path)); } else { QVERIFY(write(path, original + " ")); }
		const auto before = manifest(directory); const auto count = Engine::getSong()->tracks().size();
		const auto failed = studio.process(edit); QVERIFY(!failed.value("ok").toBool()); QVERIFY(!failed.contains("result")); QVERIFY(!failed.value("replayed").toBool());
		QCOMPARE(manifest(directory), before); QCOMPARE(Engine::getSong()->tracks().size(), count); QVERIFY(!studio.stopping());
		QVERIFY(write(path, original)); auto replay = studio.process(edit); QVERIFY(replay.value("replayed").toBool()); replay.remove("replayed"); QCOMPARE(replay, first);
		QCOMPARE(manifest(directory), before); QCOMPARE(Engine::getSong()->tracks().size(), count);
	}
	void receiptPublicationFailureRollsBackNativeEdit_data()
	{
		QTest::addColumn<bool>("agent"); QTest::newRow("direct") << false; QTest::newRow("agent") << true;
	}
	void receiptPublicationFailureRollsBackNativeEdit()
	{
		QFETCH(bool, agent); QTemporaryDir directory; QString error; int count = 0;
		{
			StudioRuntime studio(directory.path(), [](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				const QJsonObject function{{"name", "track_add"}, {"arguments", QString::fromUtf8(QJsonDocument(largeEdit().value("args").toObject()).toJson(QJsonDocument::Compact))}};
				const QJsonObject tool{{"id", "add"}, {"type", "function"}, {"function", function}};
				const QJsonObject message{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{tool}}};
				return QJsonObject{{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", message}}}}};
			});
			QVERIFY2(studio.open(error), qPrintable(error)); count = Engine::getSong()->tracks().size();
			if (agent)
			{
				QVERIFY(studio.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{
					{"protocol", "chat"}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "receipt-fixture"}}}})).value("ok").toBool());
				QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Add a native automation track"}})).value("ok").toBool()); studio.advanceAgent();
				QCOMPARE(nativeJob(directory).value("pending").toArray().size(), 1);
			}
			QVERIFY(write(directory.filePath("receipts"), "directory obstruction")); const auto before = read(directory.filePath("session.json"));
			const auto result = agent ? studio.advanceAgent() : studio.process(largeEdit());
			QVERIFY(!result.value("ok").toBool()); QVERIFY(result.value("error").toString().contains("receipt")); QVERIFY(studio.stopping());
			QCOMPARE(read(directory.filePath("session.json")), before); QCOMPARE(Engine::getSong()->tracks().size(), count);
			if (agent) { QCOMPARE(nativeJob(directory).value("operations").toInt(), 0); QCOMPARE(nativeJob(directory).value("pending").toArray().size(), 1); }
			QVERIFY(QFile::remove(directory.filePath("receipts")));
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(Engine::getSong()->tracks().size(), count);
		if (agent) { QVERIFY(nativeJob(directory).value("pending").toArray().isEmpty()); QCOMPARE(nativeJob(directory).value("operations").toInt(), 0); }
	}
	void publishedReceiptIsNotAcknowledgedWithoutManifestCommit()
	{
		QTemporaryDir directory; QString error; int count = 0; QJsonObject before;
		{
			StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error)); count = Engine::getSong()->tracks().size();
			before = manifest(directory); const auto path = directory.filePath("session.json"), retained = directory.filePath("last-session.json");
			const auto original = read(path); QVERIFY(QFile::rename(path, retained));
			const auto restoreManifest = qScopeGuard([&] { QDir(directory.path()).rmdir("session.json"); QFile::rename(retained, path); });
			QVERIFY(QDir(directory.path()).mkdir("session.json"));
			const auto response = studio.process(largeEdit()); QVERIFY(!response.value("ok").toBool()); QVERIFY(studio.stopping());
			QVERIFY(response.value("error").toString().contains("Session commit failed")); QCOMPARE(read(retained), original);
			QCOMPARE(Engine::getSong()->tracks().size(), count);
			const auto files = QDir(directory.filePath("receipts")).entryList({"*.json"}, QDir::Files); QCOMPARE(files.size(), 1);
			const auto orphan = directory.filePath("receipts/" + files.at(0));
			QCOMPARE(QJsonDocument::fromJson(read(orphan)).object().value("request").toObject(), largeEdit());
			QVERIFY(write(orphan, "invalid orphan must not be replayed"));
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(Engine::getSong()->tracks().size(), count);
		QCOMPARE(manifest(directory).value("history"), before.value("history")); QVERIFY(receipt(directory).isEmpty());
		const auto result = recovered.process(largeEdit()); QVERIFY(result.value("ok").toBool()); QVERIFY(!result.value("replayed").toBool());
		QCOMPARE(Engine::getSong()->tracks().size(), count + 1);
	}
	void legacyLargeReceiptsMigrateBeforeNativeReplacement_data()
	{
		QTest::addColumn<bool>("blockMigration"); QTest::newRow("success") << false; QTest::newRow("write-failure") << true;
	}
	void legacyLargeReceiptsMigrateBeforeNativeReplacement()
	{
		QFETCH(bool, blockMigration); QTemporaryDir directory; QString error; QJsonObject first;
		{
			StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error)); first = studio.process(largeEdit()); QVERIFY(first.value("ok").toBool());
		}
		auto state = manifest(directory); auto entries = state.value("requests").toArray(); const auto reference = entries.at(0).toObject();
		entries[0] = QJsonDocument::fromJson(read(directory.filePath(reference.value("archive").toObject().value("path").toString()))).object();
		state.insert("requests", entries); const auto before = QJsonDocument(state).toJson(QJsonDocument::Compact); QVERIFY(write(directory.filePath("session.json"), before));
		if (blockMigration)
		{
			QVERIFY(QDir(directory.path()).rename("receipts", "saved-receipts")); QVERIFY(write(directory.filePath("receipts"), "directory obstruction"));
			Engine::getSong()->setTempo(77);
			{ StudioRuntime failed(directory.path()); QVERIFY(!failed.open(error)); QVERIFY(error.contains("receipt")); }
			QCOMPARE(Engine::getSong()->getTempo(), 77); QCOMPARE(read(directory.filePath("session.json")), before);
			QVERIFY(QFile::remove(directory.filePath("receipts"))); QVERIFY(QDir(directory.path()).rename("saved-receipts", "receipts"));
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QVERIFY(receipt(directory).contains("archive"));
		const auto recoveredState = manifest(directory); auto replay = recovered.process(largeEdit()); QVERIFY(replay.value("replayed").toBool());
		replay.remove("replayed"); QCOMPARE(replay, first); QCOMPARE(manifest(directory), recoveredState);
	}
	void cooperativeReplayReservesIdsAndPreservesNestedControl_data()
	{
		QTest::addColumn<bool>("shutdown"); QTest::newRow("cancel-job") << false; QTest::newRow("shutdown") << true;
	}
	void cooperativeReplayReservesIdsAndPreservesNestedControl()
	{
		QFETCH(bool, shutdown); QTemporaryDir directory; QString error; StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error));
		const auto edit = largeEdit(); const auto first = studio.process(edit); QVERIFY(first.value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Continue the current project"}})).value("ok").toBool());
		const auto before = manifest(directory); const auto count = Engine::getSong()->tracks().size(); QJsonObject collision, control, replay; bool entered = false;
		{
			FileResourceIO::Scope nested([&] {
				if (!entered)
				{
					entered = true; collision = studio.process(request("large-edit", "session.inspect"));
					control = studio.process(request("distinct-control", shutdown ? "session.shutdown" : "agent.cancel"));
				}
				return false;
			});
			replay = studio.process(edit);
		}
		QVERIFY(entered); QVERIFY(!collision.value("ok").toBool()); QVERIFY(collision.value("error").toString().contains("still executing"));
		QVERIFY(control.value("ok").toBool()); QCOMPARE(studio.stopping(), shutdown); QCOMPARE(Engine::getSong()->tracks().size(), count);
		QCOMPARE(nativeJob(directory).value("status").toString(), shutdown ? QString("paused") : QString("cancelled"));
		QCOMPARE(manifest(directory).value("revision"), before.value("revision")); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QCOMPARE(manifest(directory).value("requests").toArray().size(), before.value("requests").toArray().size() + 1);
		if (shutdown) { QVERIFY(!replay.value("ok").toBool()); QVERIFY(!replay.value("replayed").toBool()); }
		else { QVERIFY(replay.value("replayed").toBool()); replay.remove("replayed"); QCOMPARE(replay, first); }
	}
	void cancelledReceiptReadCanRetryWithoutNewExecution()
	{
		QTemporaryDir directory; QString error; StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error));
		const auto first = studio.process(largeEdit()); QVERIFY(first.value("ok").toBool()); const auto before = manifest(directory); int checks = 0; QJsonObject cancelled;
		const auto count = Engine::getSong()->tracks().size();
		{ FileResourceIO::Scope scope([&] { return ++checks >= 5; }); cancelled = studio.process(largeEdit()); }
		QVERIFY(checks >= 5); QVERIFY(!cancelled.value("ok").toBool()); QVERIFY(cancelled.value("error").toString().contains("cancelled"));
		QVERIFY(!cancelled.contains("result")); QCOMPARE(manifest(directory), before); QVERIFY(!studio.stopping());
		auto replay = studio.process(largeEdit()); QVERIFY(replay.value("replayed").toBool()); replay.remove("replayed"); QCOMPARE(replay, first);
		QCOMPARE(manifest(directory), before); QCOMPARE(Engine::getSong()->tracks().size(), count);
	}
	void expiredReceiptsCannotBecomeCurrentFromOrphanFiles()
	{
		QTemporaryDir directory; QString error; int count = 0;
		{
			StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error)); QVERIFY(studio.process(largeEdit()).value("ok").toBool());
			count = Engine::getSong()->tracks().size(); const auto archive = receipt(directory).value("archive").toObject();
			for (int i = 0; i < 128; ++i) { QVERIFY(studio.process(request("small-" + QString::number(i), "session.inspect")).value("ok").toBool()); }
			QCOMPARE(manifest(directory).value("requests").toArray().size(), 128); QVERIFY(receipt(directory).isEmpty());
			QVERIFY(write(directory.filePath(archive.value("path").toString()), "corrupt unreferenced receipt"));
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(Engine::getSong()->tracks().size(), count);
		// Request-ID deduplication has always been limited to the retained 128 receipts.
		const auto response = recovered.process(largeEdit()); QVERIFY(response.value("ok").toBool()); QVERIFY(!response.value("replayed").toBool());
		QCOMPARE(Engine::getSong()->tracks().size(), count + 1); QVERIFY(receipt(directory).contains("archive"));
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::StudioReceiptTest)
#include "StudioReceiptTest.moc"
