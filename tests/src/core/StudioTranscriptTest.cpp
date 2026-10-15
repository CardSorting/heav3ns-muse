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
#include "StudioAgent.h"
#include "StudioRuntime.h"

namespace lmms
{
class StudioTranscriptTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	static QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
	static QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
	static bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
	static QJsonObject manifest(const QTemporaryDir& directory) { return QJsonDocument::fromJson(read(directory.filePath("session.json"))).object(); }
	static QJsonObject agent(const QTemporaryDir& directory) { return manifest(directory).value("agent").toObject(); }
	static QString hash(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
	static QJsonArray dialogue(int count, int width = 0)
	{
		QJsonArray messages;
		for (int i = 0; i < count; ++i)
		{
			messages.append(QJsonObject{{"role", i % 2 ? "assistant" : "user"}, {"content", QString::number(i) + QString::fromUtf8("\xc3\xa9\xe9\x9f\xb3") + QString(width, 'x')},
				{"job", "legacy-fixture"}, {"at", "2026-10-13T12:00:00Z"}});
		}
		return messages;
	}
	static bool seed(const QTemporaryDir& directory, const QJsonArray& messages, QString& error)
	{
		{ StudioRuntime studio(directory.path()); if (!studio.open(error)) { return false; } }
		auto state = manifest(directory); auto stored = state.value("agent").toObject(); stored.insert("transcript", messages); state.insert("agent", stored);
		return StudioAgent::validState(stored) && write(directory.filePath("session.json"), QJsonDocument(state).toJson(QJsonDocument::Compact));
	}
	static QJsonObject decision(const QString& protocol, const QString& id, const QString& name, const QJsonObject& args)
	{
		const auto encoded = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
		if (protocol == "responses") { return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", encoded}}}}}; }
		const QJsonObject tool{{"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name}, {"arguments", encoded}}}};
		const QJsonObject message{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{tool}}};
		return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", message}}}}};
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
	void archivedDialoguePagesPreserveExactOrder_data()
	{
		QTest::addColumn<bool>("large"); QTest::addColumn<bool>("restart");
		for (const auto large : {false, true})
		{
			QTest::newRow(large ? "bytes-live" : "count-live") << large << false;
			QTest::newRow(large ? "bytes-restart" : "count-restart") << large << true;
		}
	}
	void archivedDialoguePagesPreserveExactOrder()
	{
		QFETCH(bool, large); QFETCH(bool, restart); QTemporaryDir directory; QString error;
		const auto expected = dialogue(large ? 40 : 140, large ? 60000 : 0); QVERIFY2(seed(directory, expected, error), qPrintable(error));
		auto studio = std::make_unique<StudioRuntime>(directory.path()); QVERIFY2(studio->open(error), qPrintable(error));
		const auto stored = agent(directory); QVERIFY(stored.value("transcriptOffset").toInt() > 0); QVERIFY(stored.value("transcript").toArray().size() <= 64);
		QVERIFY(QJsonDocument(stored.value("transcript").toArray()).toJson(QJsonDocument::Compact).size() <= 256 * 1024);
		QCOMPARE(StudioAgent::inspect(stored).value("conversationMessages").toInt(), int(expected.size()));
		if (restart) { studio.reset(); studio = std::make_unique<StudioRuntime>(directory.path()); QVERIFY2(studio->open(error), qPrintable(error)); }
		const auto before = manifest(directory); QJsonArray assembled; int offset = 0, pages = 0; QJsonObject firstRequest, firstResponse;
		do
		{
			const auto query = request("page-" + QString::number(pages), "conversation.history", {{"offset", offset}, {"limit", 256}});
			const auto response = studio->process(query); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
			if (!pages) { firstRequest = query; firstResponse = response; } ++pages;
			const auto result = response.value("result").toObject(); QCOMPARE(result.value("total").toInt(), int(expected.size()));
			for (const auto& message : result.value("messages").toArray()) { assembled.append(message); }
			QVERIFY(result.value("nextOffset").toInt() > offset); offset = result.value("nextOffset").toInt(); QCOMPARE(offset, int(assembled.size()));
			QCOMPARE(result.value("complete").toBool(), offset == expected.size());
		} while (offset < expected.size());
		QCOMPARE(assembled, expected); if (large) { QVERIFY(pages > 1); }
		const auto done = studio->process(request("past-end", "conversation.history", {{"offset", int(expected.size()) + 1}}));
		QVERIFY(done.value("ok").toBool()); QVERIFY(done.value("result").toObject().value("messages").toArray().isEmpty()); QVERIFY(done.value("result").toObject().value("complete").toBool());
		QCOMPARE(manifest(directory).value("history"), before.value("history")); QCOMPARE(manifest(directory).value("revision"), before.value("revision"));
		QVERIFY(studio->process(request("new-chat", "session.chat", {{"text", "Continue the old composition"}})).value("ok").toBool());
		const auto newer = manifest(directory); auto replay = studio->process(firstRequest); QVERIFY(replay.value("replayed").toBool()); replay.remove("replayed");
		QCOMPARE(replay, firstResponse); QCOMPARE(manifest(directory), newer);
		QCOMPARE(StudioAgent::inspect(agent(directory)).value("conversationMessages").toInt(), int(expected.size()) + 1);
	}
	void invalidDialogueArchivesRejectStartup_data()
	{
		QTest::addColumn<int>("fault"); for (int i = 0; i < 6; ++i) { QTest::newRow(qPrintable(QString::number(i))) << i; }
	}
	void invalidDialogueArchivesRejectStartup()
	{
		QFETCH(int, fault); QTemporaryDir directory; QString error; QVERIFY2(seed(directory, dialogue(140), error), qPrintable(error));
		{ StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error)); }
		auto state = manifest(directory); auto stored = state.value("agent").toObject(); auto archives = stored.value("transcriptArchives").toArray(); auto reference = archives.at(0).toObject();
		const auto path = directory.filePath(reference.value("path").toString()); const auto original = read(path);
		if (fault == 0) { QVERIFY(QFile::remove(path)); }
		if (fault == 1) { QVERIFY(write(path, original + " ")); }
		if (fault >= 2 && fault <= 4)
		{
			auto document = QJsonDocument::fromJson(original).object(); auto messages = document.value("messages").toArray();
			if (fault == 2) { document.insert("offset", 1); }
			if (fault == 3) { auto message = messages.at(0).toObject(); message.insert("content", 17); messages[0] = message; }
			if (fault == 4) { messages.removeLast(); }
			document.insert("messages", messages); const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
			QVERIFY(write(path, bytes)); reference.insert("sha256", hash(bytes));
		}
		if (fault == 5) { reference.insert("offset", 1); }
		archives[0] = reference; stored.insert("transcriptArchives", archives); state.insert("agent", stored);
		const auto before = QJsonDocument(state).toJson(QJsonDocument::Compact); QVERIFY(write(directory.filePath("session.json"), before));
		Engine::getSong()->setTempo(77); StudioRuntime failed(directory.path()); QVERIFY(!failed.open(error)); QVERIFY(!error.isEmpty());
		QCOMPARE(Engine::getSong()->getTempo(), 77); QCOMPARE(read(directory.filePath("session.json")), before);
	}
	void freshDialogueReadsRevalidateSelectedArchives_data()
	{
		QTest::addColumn<bool>("remove"); QTest::newRow("missing") << true; QTest::newRow("altered") << false;
	}
	void freshDialogueReadsRevalidateSelectedArchives()
	{
		QFETCH(bool, remove); QTemporaryDir directory; QString error; QVERIFY2(seed(directory, dialogue(140), error), qPrintable(error));
		StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error));
		const auto query = request("first", "conversation.history", {{"limit", 10}}); const auto first = studio.process(query); QVERIFY(first.value("ok").toBool());
		const auto reference = agent(directory).value("transcriptArchives").toArray().at(0).toObject(); const auto path = directory.filePath(reference.value("path").toString()); const auto original = read(path);
		if (remove) { QVERIFY(QFile::remove(path)); } else { QVERIFY(write(path, original + " ")); }
		const auto before = manifest(directory); auto replay = studio.process(query); QVERIFY(replay.value("replayed").toBool()); replay.remove("replayed"); QCOMPARE(replay, first);
		const auto failed = studio.process(request("fresh", "conversation.history", {{"limit", 10}})); QVERIFY(!failed.value("ok").toBool()); QVERIFY(!failed.contains("result"));
		QCOMPARE(manifest(directory).value("history"), before.value("history")); QCOMPARE(manifest(directory).value("revision"), before.value("revision")); QVERIFY(!studio.stopping());
		QVERIFY(write(path, original)); const auto restored = studio.process(request("restored", "conversation.history", {{"limit", 10}}));
		QVERIFY(restored.value("ok").toBool()); QCOMPARE(restored.value("result"), first.value("result"));
	}
	void archiveWriteFailurePreservesDialogueAndNativeState_data()
	{
		QTest::addColumn<bool>("migration"); QTest::newRow("startup-migration") << true; QTest::newRow("new-message") << false;
	}
	void archiveWriteFailurePreservesDialogueAndNativeState()
	{
		QFETCH(bool, migration); QTemporaryDir directory; QString error; QVERIFY2(seed(directory, dialogue(migration ? 140 : 128), error), qPrintable(error));
		{
			StudioRuntime studio(directory.path());
			if (!migration) { QVERIFY2(studio.open(error), qPrintable(error)); }
			QVERIFY(write(directory.filePath("conversations"), "directory obstruction")); const auto before = read(directory.filePath("session.json"));
			Engine::getSong()->setTempo(77);
			if (migration) { QVERIFY(!studio.open(error)); QVERIFY(error.contains("conversation")); }
			else
			{
				const auto result = studio.process(request("new-chat", "session.chat", {{"text", "Continue composition"}}));
				QVERIFY(!result.value("ok").toBool()); QVERIFY(studio.stopping());
			}
			QCOMPARE(Engine::getSong()->getTempo(), 77); QCOMPARE(read(directory.filePath("session.json")), before);
			QVERIFY(QFile::remove(directory.filePath("conversations")));
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(StudioAgent::inspect(agent(directory)).value("conversationMessages").toInt(), migration ? 140 : 128);
	}
	void uncommittedDialogueArchivesCannotReplaceTheTranscript()
	{
		QTemporaryDir directory; QString error; const auto expected = dialogue(128); QVERIFY2(seed(directory, expected, error), qPrintable(error));
		const auto chat = request("new-chat", "session.chat", {{"text", "Continue the composition"}});
		{
			StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error));
			const auto path = directory.filePath("session.json"), retained = directory.filePath("last-session.json"); const auto before = read(path);
			QVERIFY(QFile::rename(path, retained));
			const auto restoreManifest = qScopeGuard([&] { QDir(directory.path()).rmdir("session.json"); QFile::rename(retained, path); });
			QVERIFY(QDir(directory.path()).mkdir("session.json")); const auto failed = studio.process(chat);
			QVERIFY(!failed.value("ok").toBool()); QVERIFY(studio.stopping()); QCOMPARE(read(retained), before);
			const auto files = QDir(directory.filePath("conversations")).entryList({"*.json"}, QDir::Files); QVERIFY(!files.isEmpty());
			QVERIFY(write(directory.filePath("conversations/" + files.at(0)), "unreferenced damaged archive"));
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		const auto history = recovered.process(request("original", "conversation.history", {{"limit", 256}})); QVERIFY(history.value("ok").toBool());
		QCOMPARE(history.value("result").toObject().value("messages").toArray(), expected);
		const auto response = recovered.process(chat); QVERIFY(response.value("ok").toBool()); QVERIFY(!response.value("replayed").toBool());
		QCOMPARE(StudioAgent::inspect(agent(directory)).value("conversationMessages").toInt(), 129);
	}
	void agentRetrievesArchivedDialogueBeforeNativeEdit_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses");
	}
	void agentRetrievesArchivedDialogueBeforeNativeEdit()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; auto messages = dialogue(140);
		auto first = messages.at(0).toObject(); first.insert("content", "Set tempo to 91 BPM."); messages[0] = first;
		QVERIFY2(seed(directory, messages, error), qPrintable(error)); int rounds = 0, recalled = 0; bool recentOnly = false;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			if (++rounds == 1)
			{
				const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact); recentOnly = !bytes.contains("Set tempo to 91 BPM.") && bytes.contains("earlier messages are archived");
				return decision(protocol, "recall", "conversation_history", {{"offset", 0}, {"limit", 1}});
			}
			if (rounds == 2)
			{
				for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray())
				{
					const auto item = value.toObject(); if (item.value(protocol == "chat" ? "tool_call_id" : "call_id").toString() != "recall") { continue; }
					const auto result = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object().value("result").toObject();
					const auto retrieved = result.value("messages").toArray();
					if (!retrieved.isEmpty()) { recalled = retrieved.at(0).toObject().value("content").toString().section(' ', 3, 3).toInt(); }
				}
				return decision(protocol, "tempo", "song_set", {{"tempo", recalled}});
			}
			return decision(protocol, "finish", "agent_finish", {{"status", "complete"}, {"summary", "Restored the earlier tempo preference."}, {"evidence", QJsonArray{"tempo"}}});
		});
		QVERIFY2(studio.open(error), qPrintable(error)); QVERIFY(studio.process(request("tempo-before", "song.set", {{"tempo", 77}})).value("ok").toBool());
		QVERIFY(studio.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "transcript-fixture"}}}})).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Apply my earliest tempo preference"}})).value("ok").toBool());
		const auto before = manifest(directory); studio.advanceAgent(); studio.advanceAgent();
		QCOMPARE(Engine::getSong()->getTempo(), 77); QCOMPARE(manifest(directory).value("revision"), before.value("revision"));
		for (int i = 0; i < 4; ++i) { QVERIFY(!studio.advanceAgent().isEmpty()); }
		QVERIFY(recentOnly); QCOMPARE(recalled, 91); QCOMPARE(rounds, 3); QCOMPARE(Engine::getSong()->getTempo(), 91);
		QCOMPARE(agent(directory).value("job").toObject().value("status").toString(), QString("complete"));
	}
	void cancelledDialogueReadReturnsNoPartialPage()
	{
		QTemporaryDir directory; QString error; QVERIFY2(seed(directory, dialogue(40, 60000), error), qPrintable(error));
		StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error)); const auto before = manifest(directory); int checks = 0; QJsonObject response;
		{ FileResourceIO::Scope scope([&] { return ++checks >= 5; }); response = studio.process(request("cancelled", "conversation.history")); }
		QVERIFY(checks >= 5); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QVERIFY(!studio.stopping());
		QCOMPARE(agent(directory), before.value("agent").toObject()); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QCOMPARE(manifest(directory).value("revision"), before.value("revision"));
		QVERIFY(studio.process(request("retry", "conversation.history", {{"limit", 1}})).value("ok").toBool());
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::StudioTranscriptTest)
#include "StudioTranscriptTest.moc"
