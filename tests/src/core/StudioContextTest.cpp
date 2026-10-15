/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <memory>
#include <stdexcept>
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
class StudioContextTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	static QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
	{
		return {{"id", id}, {"op", op}, {"args", args}};
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
	static QJsonObject job(const QTemporaryDir& directory) { return manifest(directory).value("agent").toObject().value("job").toObject(); }
	static QString hash(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
	static QJsonObject provider(const QString& protocol)
	{
		return {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol},
			{"endpoint", "http://127.0.0.1:12345/test"}, {"model", "context-fixture"}}}};
	}
	static QJsonObject call(const QString& id, const QString& name, const QJsonObject& args)
	{
		return {{"id", id}, {"name", name}, {"args", args}};
	}
	static QJsonObject decision(const QString& protocol, const QJsonArray& calls, const QString& text = {})
	{
		QJsonArray output;
		if (protocol == "responses" && !text.isEmpty())
		{
			output.append(QJsonObject{{"type", "message"}, {"role", "assistant"}, {"content", QJsonArray{
				QJsonObject{{"type", "output_text"}, {"text", text}}}}});
		}
		for (const auto& value : calls)
		{
			const auto item = value.toObject(); const auto arguments = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact));
			if (protocol == "responses")
			{
				output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", arguments}});
			}
			else
			{
				output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", arguments}}}});
			}
		}
		if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; }
		return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{
			{"role", "assistant"}, {"content", text.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(text)}, {"tool_calls", output}}}}}}};
	}
	static bool start(StudioRuntime& studio, const QString& protocol, QString& error)
	{
		return studio.open(error)
			&& studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()
			&& studio.process(request("chat", "session.chat", {{"text", "Preserve the original task and set tempo to 91 BPM"}})).value("ok").toBool();
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
	void compactionPreservesProtocolEvidenceAndContinuesAfterRestart_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("restart");
		for (const auto protocol : {"chat", "responses"})
		{
			QTest::newRow(qPrintable(QString(protocol) + "-same-process")) << QString(protocol) << false;
			QTest::newRow(qPrintable(QString(protocol) + "-restart")) << QString(protocol) << true;
		}
	}
	void compactionPreservesProtocolEvidenceAndContinuesAfterRestart()
	{
		QFETCH(QString, protocol); QFETCH(bool, restart); QTemporaryDir directory; int rounds = 0; QByteArray continuedInput;
		const QString summary = "Keep the original task. Tempo is 91 BPM; successful evidence call is tempo. Verify and finish.";
		const QString largeText = "OLD-LARGE-EXCHANGE:" + QString(300000, 'x');
		const auto transport = [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			if (++rounds == 1) { return decision(protocol, {call("tempo", "song_set", {{"tempo", 91}})}); }
			if (rounds == 2) { return decision(protocol, {call("compact", "agent_context_compact", {{"summary", summary}})}, largeText); }
			continuedInput = QJsonDocument(body.value(protocol == "chat" ? "messages" : "input").toArray()).toJson(QJsonDocument::Compact);
			return decision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Native tempo is 91 BPM."}, {"evidence", QJsonArray{"tempo"}}})});
		};
		auto studio = std::make_unique<StudioRuntime>(directory.path(), transport); QString error;
		QVERIFY2(start(*studio, protocol, error), qPrintable(error));
		for (int i = 0; i < 3; ++i) { QVERIFY(!studio->advanceAgent().isEmpty()); }
		const auto before = manifest(directory); QVERIFY(!studio->advanceAgent().isEmpty());
		const auto after = manifest(directory); const auto compacted = job(directory);
		QCOMPARE(after.value("revision"), before.value("revision")); QCOMPARE(after.value("history"), before.value("history"));
		QVERIFY(compacted.value("input").toArray().isEmpty()); QVERIFY(compacted.value("pending").toArray().isEmpty());
		QCOMPARE(compacted.value("contextSummary").toString(), summary); QCOMPARE(compacted.value("operations").toInt(), 2);
		QVERIFY(StudioAgent::validState(after.value("agent").toObject())); QCOMPARE(compacted.value("pastInputs").toArray().size(), 1);
		const auto reference = compacted.value("pastInputs").toArray().at(0).toObject(); QVERIFY(!reference.contains("input"));
		const auto bytes = read(directory.filePath(reference.value("path").toString())); QCOMPARE(hash(bytes), reference.value("sha256").toString());
		const auto archived = QJsonDocument::fromJson(bytes).object(); QCOMPARE(archived.value("protocol").toString(), protocol);
		QVERIFY(bytes.contains("OLD-LARGE-EXCHANGE:")); QCOMPARE(archived.value("input").toArray().size(), reference.value("items").toInt());
		const auto last = archived.value("input").toArray().last().toObject();
		QCOMPARE(last.value(protocol == "chat" ? "tool_call_id" : "call_id").toString(), QString("compact"));
		QVERIFY(QJsonDocument::fromJson(last.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object().value("ok").toBool());
		if (restart)
		{
			studio.reset(); studio = std::make_unique<StudioRuntime>(directory.path(), transport);
			QVERIFY2(studio->open(error), qPrintable(error));
		}
		QVERIFY(!studio->advanceAgent().isEmpty()); QVERIFY(!studio->advanceAgent().isEmpty());
		QVERIFY(continuedInput.contains(summary.toUtf8())); QVERIFY(continuedInput.contains("original task"));
		QVERIFY(!continuedInput.contains("OLD-LARGE-EXCHANGE:")); QVERIFY(continuedInput.size() < 75000);
		QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(Engine::getSong()->getTempo(), 91);
		QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject())); QCOMPARE(rounds, 3);
	}
	void invalidContextArchivesRejectOpenBeforeNativeReplacement_data()
	{
		QTest::addColumn<bool>("archivedJob"); QTest::addColumn<int>("fault");
		for (const auto archived : {false, true})
		{
			for (int fault = 0; fault < 3; ++fault)
			{
				QTest::newRow(qPrintable(QString("%1-%2").arg(archived ? "past-job" : "current-job").arg(fault))) << archived << fault;
			}
		}
	}
	void invalidContextArchivesRejectOpenBeforeNativeReplacement()
	{
		QFETCH(bool, archivedJob); QFETCH(int, fault); QTemporaryDir directory; QJsonObject reference;
		{
			StudioRuntime studio(directory.path(), [](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				return decision("chat", {call("tempo", "song_set", {{"tempo", 91}})});
			});
			QString error; QVERIFY2(start(studio, "chat", error), qPrintable(error));
			studio.advanceAgent(); studio.advanceAgent();
			QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Tempo is 91; retain evidence tempo."}})).value("ok").toBool());
			reference = job(directory).value("pastInputs").toArray().at(0).toObject();
			if (archivedJob)
			{
				QVERIFY(studio.process(request("cancel", "agent.cancel")).value("ok").toBool());
				QVERIFY(studio.process(request("next", "session.chat", {{"text", "Continue tomorrow"}})).value("ok").toBool());
			}
		}
		const auto path = directory.filePath(reference.value("path").toString()); const auto saved = read(path);
		auto state = manifest(directory);
		if (fault == 0) { QVERIFY(QFile::remove(path)); }
		if (fault == 1) { QVERIFY(write(path, saved + "corruption")); }
		if (fault == 2)
		{
			auto agent = state.value("agent").toObject(); auto contextJob = agent.value("job").toObject(); QJsonArray archives;
			if (archivedJob)
			{
				archives = agent.value("archives").toArray();
				contextJob = QJsonDocument::fromJson(read(directory.filePath(archives.at(0).toObject().value("path").toString()))).object();
			}
			auto contexts = contextJob.value("pastInputs").toArray(); auto wrong = contexts.at(0).toObject();
			wrong.insert("items", wrong.value("items").toInt() + 1); contexts[0] = wrong; contextJob.insert("pastInputs", contexts);
			if (archivedJob)
			{
				auto archive = archives.at(0).toObject(); const auto bytes = QJsonDocument(contextJob).toJson(QJsonDocument::Compact);
				QVERIFY(write(directory.filePath(archive.value("path").toString()), bytes)); archive.insert("sha256", hash(bytes));
				archives[0] = archive; agent.insert("archives", archives);
			}
			else { agent.insert("job", contextJob); }
			state.insert("agent", agent); QVERIFY(write(directory.filePath("session.json"), QJsonDocument(state).toJson(QJsonDocument::Compact)));
		}
		const auto manifestBefore = read(directory.filePath("session.json")); Engine::getSong()->setTempo(77);
		StudioRuntime failed(directory.path()); QString error; QVERIFY(!failed.open(error)); QVERIFY(error.contains("context", Qt::CaseInsensitive));
		QCOMPARE(Engine::getSong()->getTempo(), 77); QCOMPARE(read(directory.filePath("session.json")), manifestBefore);
	}
	void providerReplanningPreservesReferencedAndLegacyInlineContexts_data()
	{
		QTest::addColumn<QString>("firstProtocol"); QTest::newRow("chat-to-responses") << QString("chat");
		QTest::newRow("responses-to-chat") << QString("responses");
	}
	void providerReplanningPreservesReferencedAndLegacyInlineContexts()
	{
		QFETCH(QString, firstProtocol); const auto secondProtocol = firstProtocol == "chat" ? QString("responses") : QString("chat");
		QTemporaryDir directory;
		{
			int rounds = 0;
			StudioRuntime studio(directory.path(), [&](const QJsonObject& config, const QJsonObject&, const StudioInference::Cancelled&) {
				++rounds; return decision(config.value("protocol").toString(), {call(QString::number(rounds), "song_set", {{"tempo", rounds == 1 ? 91 : 97}})});
			});
			QString error; QVERIFY2(start(studio, firstProtocol, error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
			QVERIFY(studio.process(request("switch", "session.provider.set", provider(secondProtocol))).value("ok").toBool());
			QVERIFY(studio.process(request("resume", "agent.resume")).value("ok").toBool());
			QCOMPARE(job(directory).value("pastInputs").toArray().at(0).toObject().value("protocol").toString(), firstProtocol);
			studio.advanceAgent(); studio.advanceAgent();
			const auto compact = request("compact", "agent.context.compact", {{"summary", "Tempo is 97; preserve the original musical task."}});
			QVERIFY(studio.process(compact).value("ok").toBool()); const auto after = manifest(directory);
			QVERIFY(studio.process(compact).value("replayed").toBool()); QCOMPARE(manifest(directory), after);
			QCOMPARE(job(directory).value("pastInputs").toArray().size(), 2); QVERIFY(StudioAgent::validState(after.value("agent").toObject()));
		}
		auto state = manifest(directory); auto agent = state.value("agent").toObject(); auto current = agent.value("job").toObject();
		auto contexts = current.value("pastInputs").toArray();
		contexts[0] = QJsonDocument::fromJson(read(directory.filePath(contexts.at(0).toObject().value("path").toString()))).object();
		current.insert("pastInputs", contexts); agent.insert("job", current); state.insert("agent", agent);
		QVERIFY(StudioAgent::validState(agent)); QVERIFY(write(directory.filePath("session.json"), QJsonDocument(state).toJson(QJsonDocument::Compact)));
		StudioRuntime recovered(directory.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->getTempo(), 97); QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
		QVERIFY(recovered.process(request("finish-multiple-contexts", "agent.finish", {{"status", "complete"},
			{"summary", "Both provider contexts retain their successful tempo changes."}, {"evidence", QJsonArray{"1", "2", "1"}}})).value("ok").toBool());
		QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void failedContextWriteRetainsPendingExchangeAndManifest()
	{
		QTemporaryDir directory;
		{
			int rounds = 0;
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				if (++rounds == 1) { return decision("chat", {call("tempo", "song_set", {{"tempo", 91}})}); }
				return decision("chat", {call("compact", "agent_context_compact", {{"summary", "Keep the original task and the tempo edit."}})});
			});
			QString error; QVERIFY2(start(studio, "chat", error), qPrintable(error));
			studio.advanceAgent(); studio.advanceAgent(); studio.advanceAgent();
			const auto bytes = read(directory.filePath("session.json")); QVERIFY(write(directory.filePath("contexts"), "not a directory"));
			const auto response = studio.advanceAgent(); QVERIFY(!response.value("ok").toBool()); QVERIFY(studio.stopping());
			QCOMPARE(read(directory.filePath("session.json")), bytes); QCOMPARE(job(directory).value("pending").toArray().size(), 1);
			QCOMPARE(job(directory).value("operations").toInt(), 1);
			QCOMPARE(job(directory).value("records").toArray().at(1).toObject().value("kind").toString(), QString("tool"));
			QVERIFY(QFile::remove(directory.filePath("contexts")));
		}
		StudioRuntime recovered(directory.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		QVERIFY(job(directory).value("pending").toArray().isEmpty()); QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
		QCOMPARE(Engine::getSong()->getTempo(), 91);
	}
	void compactedToolPayloadRemainsRetrievableAndSupportsEvidence_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("restart");
		for (const auto protocol : {"chat", "responses"})
		{
			QTest::newRow(qPrintable(QString(protocol) + "-same-process")) << QString(protocol) << false;
			QTest::newRow(qPrintable(QString(protocol) + "-restart")) << QString(protocol) << true;
		}
	}
	void compactedToolPayloadRemainsRetrievableAndSupportsEvidence()
	{
		QFETCH(QString, protocol); QFETCH(bool, restart); QTemporaryDir directory; int rounds = 0; QString error;
		const QString payload = QString(180000, 'p') + QString::fromUtf8("\xe9\x9f\xb3");
		const auto transport = [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (++rounds == 1) { return decision(protocol, {call("memory", "session_memory_set", {{"memory", QJsonObject{{"payload", payload}}}})}); }
			if (rounds == 2) { return decision(protocol, {call("compact", "agent_context_compact", {{"summary", "The exact memory payload is retained; cite memory as evidence."}})}); }
			return decision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Preserved the exact payload."}, {"evidence", QJsonArray{"memory"}}})});
		};
		auto studio = std::make_unique<StudioRuntime>(directory.path(), transport);
		QVERIFY2(start(*studio, protocol, error), qPrintable(error)); studio->advanceAgent(); studio->advanceAgent();
		const auto before = manifest(directory); const auto previousRecords = job(directory).value("records").toArray();
		const auto recordBytes = QJsonDocument(previousRecords.last().toObject()).toJson(QJsonDocument::Compact);
		QVERIFY(recordBytes.size() > 360000); studio->advanceAgent(); studio->advanceAgent();
		const auto current = job(directory); const auto records = current.value("records").toArray();
		const auto inspected = StudioAgent::inspect(manifest(directory).value("agent").toObject()).value("job").toObject();
		QCOMPARE(inspected.value("unarchivedToolRecordBytes").toInt(), 2);
		QCOMPARE(inspected.value("retainedRecordBytes").toInt(), int(QJsonDocument(records).toJson(QJsonDocument::Compact).size()));
		QVERIFY(QJsonDocument(records).toJson(QJsonDocument::Compact).size() < 2048);
		QCOMPARE(records.at(1).toObject().value("kind").toString(), QString("archivedTool"));
		QCOMPARE(records.at(1).toObject().value("id").toString(), QString("memory"));
		QCOMPARE(records.last().toObject().value("id").toString(), QString("compact"));
		QCOMPARE(current.value("operations").toInt(), 2); QCOMPARE(current.value("rounds").toInt(), 2);
		QCOMPARE(manifest(directory).value("history"), before.value("history")); QCOMPARE(manifest(directory).value("revision"), before.value("revision"));
		QCOMPARE(manifest(directory).value("memory"), before.value("memory"));
		const auto reference = current.value("pastInputs").toArray().at(0).toObject(); QCOMPARE(reference.value("recordItems").toInt(), 2);
		const auto archive = QJsonDocument::fromJson(read(directory.filePath(reference.value("path").toString()))).object();
		QCOMPARE(QJsonDocument(archive.value("records").toArray().at(0).toObject()).toJson(QJsonDocument::Compact), recordBytes);
		if (restart) { studio.reset(); studio = std::make_unique<StudioRuntime>(directory.path(), transport); QVERIFY2(studio->open(error), qPrintable(error)); }
		const auto recovered = manifest(directory); const auto id = current.value("id").toString();
		auto response = studio->process(request("record-list", "agent.history.list", {{"job", id}})); QVERIFY(response.value("ok").toBool());
		QCOMPARE(response.value("result").toObject().value("contexts").toArray().at(0).toObject().value("recordItems").toInt(), 2);
		QByteArray assembled; int offset = 0; QString digest;
		do
		{
			QJsonObject args{{"job", id}, {"context", 0}, {"record", 0}, {"offset", offset}, {"limit", 65536}};
			if (!digest.isEmpty()) { args.insert("sha256", digest); }
			response = studio->process(request("record-page-" + QString::number(offset), "agent.history.read", args));
			QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject();
			QCOMPARE(result.value("record").toInt(), 0); QCOMPARE(result.value("context").toInt(), 0);
			digest = result.value("sha256").toString(); QCOMPARE(digest, hash(recordBytes));
			assembled += result.value("text").toString().toUtf8(); QVERIFY(result.value("nextOffset").toInt() > offset); offset = result.value("nextOffset").toInt();
			QCOMPARE(offset, int(assembled.size())); QCOMPARE(result.value("complete").toBool(), offset == recordBytes.size());
		} while (offset < recordBytes.size());
		QCOMPARE(assembled, recordBytes);
		QVERIFY(!studio->process(request("no-context", "agent.history.read", {{"job", id}, {"record", 0}})).value("ok").toBool());
		QVERIFY(!studio->process(request("unknown-record", "agent.history.read", {{"job", id}, {"context", 0}, {"record", 2}})).value("ok").toBool());
		QCOMPARE(manifest(directory).value("revision"), recovered.value("revision")); QCOMPARE(manifest(directory).value("history"), recovered.value("history"));
		studio->advanceAgent(); studio->advanceAgent(); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
		QCOMPARE(rounds, 3); QCOMPARE(job(directory).value("operations").toInt(), 3);
		QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
	}
	void inconsistentArchivedToolIndexesRejectOpen_data()
	{
		QTest::addColumn<bool>("pastJob"); QTest::addColumn<int>("fault");
		for (const auto past : {false, true})
		{
			for (int fault = 0; fault < 4; ++fault)
			{
				QTest::newRow(qPrintable(QString(past ? "past-job-" : "current-job-") + QString::number(fault))) << past << fault;
			}
		}
	}
	void inconsistentArchivedToolIndexesRejectOpen()
	{
		QFETCH(bool, pastJob); QFETCH(int, fault); QTemporaryDir directory; QString error;
		{
			StudioRuntime studio(directory.path(), [](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				return decision("chat", {call("tempo", "song_set", {{"tempo", 91}}), call("observe", "project_inspect", {})});
			});
			QVERIFY2(start(studio, "chat", error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent(); studio.advanceAgent();
			QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Keep the tempo edit and project observation."}})).value("ok").toBool());
			if (pastJob)
			{
				QVERIFY(studio.process(request("cancel", "agent.cancel")).value("ok").toBool());
				QVERIFY(studio.process(request("new-job", "session.chat", {{"text", "Continue the composition."}})).value("ok").toBool());
			}
		}
		auto state = manifest(directory); auto agent = state.value("agent").toObject(); auto current = agent.value("job").toObject();
		auto archives = agent.value("archives").toArray(); QJsonObject parent;
		if (pastJob) { parent = archives.at(0).toObject(); current = QJsonDocument::fromJson(read(directory.filePath(parent.value("path").toString()))).object(); }
		auto records = current.value("records").toArray(); auto index = records.at(1).toObject();
		auto contexts = current.value("pastInputs").toArray(); auto context = contexts.at(0).toObject();
		if (fault == 0) { index.insert("ok", false); records[1] = index; } // Valid shape, false evidence metadata.
		if (fault == 1) { index.insert("record", 1); records[1] = index; } // Valid ordinal, another native call.
		if (fault == 2) { context.insert("recordItems", 3); }
		if (fault == 3)
		{
			auto document = QJsonDocument::fromJson(read(directory.filePath(context.value("path").toString()))).object();
			auto payload = document.value("records").toArray(); auto invalid = payload.at(0).toObject(); invalid.insert("response", QJsonObject{{"ok", "true"}});
			payload[0] = invalid; document.insert("records", payload); const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
			QVERIFY(write(directory.filePath(context.value("path").toString()), bytes)); context.insert("sha256", hash(bytes));
		}
		contexts[0] = context; current.insert("pastInputs", contexts); current.insert("records", records);
		if (pastJob)
		{
			const auto bytes = QJsonDocument(current).toJson(QJsonDocument::Compact); QVERIFY(write(directory.filePath(parent.value("path").toString()), bytes));
			parent.insert("sha256", hash(bytes)); archives[0] = parent; agent.insert("archives", archives);
		}
		else { agent.insert("job", current); }
		state.insert("agent", agent); const auto before = QJsonDocument(state).toJson(QJsonDocument::Compact);
		QVERIFY(write(directory.filePath("session.json"), before)); Engine::getSong()->setTempo(77);
		StudioRuntime recovered(directory.path()); QVERIFY(!recovered.open(error)); QVERIFY(error.contains("context", Qt::CaseInsensitive));
		QCOMPARE(Engine::getSong()->getTempo(), 77); QCOMPARE(read(directory.filePath("session.json")), before);
	}
	void archivedEvidenceMustStillExistWhenFinishing_data()
	{
		QTest::addColumn<bool>("remove"); QTest::newRow("missing") << true; QTest::newRow("altered") << false;
	}
	void archivedEvidenceMustStillExistWhenFinishing()
	{
		QFETCH(bool, remove); QTemporaryDir directory;
		StudioRuntime studio(directory.path(), [](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision("chat", {call("tempo", "song_set", {{"tempo", 91}}), call("failed", "unavailable_native_fixture", {})});
		});
		QString error; QVERIFY2(start(studio, "chat", error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent(); studio.advanceAgent();
		QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Retain tempo as evidence."}})).value("ok").toBool());
		QVERIFY(!studio.process(request("failed-evidence", "agent.finish", {{"status", "complete"}, {"summary", "Failed evidence must be rejected."}, {"evidence", QJsonArray{"failed"}}})).value("ok").toBool());
		const auto reference = job(directory).value("pastInputs").toArray().at(0).toObject(); const auto path = directory.filePath(reference.value("path").toString());
		const auto original = read(path); if (remove) { QVERIFY(QFile::remove(path)); } else { QVERIFY(write(path, original + " ")); }
		const auto before = manifest(directory); const QJsonObject args{{"status", "complete"}, {"summary", "Tempo was changed."}, {"evidence", QJsonArray{"tempo"}}};
		const auto response = studio.process(request("finish-missing", "agent.finish", args)); QVERIFY(!response.value("ok").toBool());
		QCOMPARE(job(directory), before.value("agent").toObject().value("job").toObject()); QVERIFY(!studio.stopping());
		QCOMPARE(manifest(directory).value("revision"), before.value("revision")); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QVERIFY(write(path, original)); QVERIFY(studio.process(request("finish-restored", "agent.finish", args)).value("ok").toBool());
		QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(Engine::getSong()->getTempo(), 91);
	}
	void compactedCallIdsCannotBeReusedAfterRestart_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses");
	}
	void compactedCallIdsCannotBeReusedAfterRestart()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				return decision(protocol, {call("tempo", "song_set", {{"tempo", 91}})});
			});
			QVERIFY2(start(studio, protocol, error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
			QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Do not repeat the tempo call."}})).value("ok").toBool());
		}
		StudioRuntime recovered(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision(protocol, {call("tempo", "song_set", {{"tempo", 97}})});
		});
		QVERIFY2(recovered.open(error), qPrintable(error)); const auto before = manifest(directory); recovered.advanceAgent();
		QCOMPARE(job(directory).value("status").toString(), QString("paused")); QVERIFY(job(directory).value("lastError").toString().contains("reused"));
		QVERIFY(job(directory).value("pending").toArray().isEmpty()); QCOMPARE(job(directory).value("operations").toInt(), 1);
		QCOMPARE(manifest(directory).value("revision"), before.value("revision")); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QCOMPARE(Engine::getSong()->getTempo(), 91); QVERIFY(!recovered.stopping());
	}
	void legacyContextsWithoutRecordArchivesRemainUsable_data()
	{
		QTest::addColumn<bool>("inlineContext"); QTest::newRow("legacy-file") << false; QTest::newRow("legacy-inline") << true;
	}
	void legacyContextsWithoutRecordArchivesRemainUsable()
	{
		QFETCH(bool, inlineContext); QTemporaryDir directory; QString error;
		{
			StudioRuntime studio(directory.path(), [](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				return decision("chat", {call("tempo", "song_set", {{"tempo", 91}})});
			});
			QVERIFY2(start(studio, "chat", error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
			QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Legacy evidence remains tempo."}})).value("ok").toBool());
		}
		auto state = manifest(directory); auto agent = state.value("agent").toObject(); auto current = agent.value("job").toObject();
		auto contexts = current.value("pastInputs").toArray(); auto reference = contexts.at(0).toObject();
		auto context = QJsonDocument::fromJson(read(directory.filePath(reference.value("path").toString()))).object();
		auto records = current.value("records").toArray(); records[1] = context.value("records").toArray().at(0); current.insert("records", records);
		context.remove("records"); const auto bytes = QJsonDocument(context).toJson(QJsonDocument::Compact);
		QVERIFY(write(directory.filePath(reference.value("path").toString()), bytes)); reference.remove("recordItems"); reference.insert("sha256", hash(bytes));
		contexts[0] = inlineContext ? context : reference; current.insert("pastInputs", contexts); agent.insert("job", current); state.insert("agent", agent);
		QVERIFY(StudioAgent::validState(agent)); QVERIFY(write(directory.filePath("session.json"), QJsonDocument(state).toJson(QJsonDocument::Compact)));
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		const auto response = recovered.process(request("legacy-context", "agent.history.read", {{"job", current.value("id")}, {"context", 0}, {"limit", 65536}}));
		QVERIFY(response.value("ok").toBool()); QCOMPARE(response.value("result").toObject().value("text").toString().toUtf8(), bytes);
		QVERIFY(!recovered.process(request("no-legacy-record", "agent.history.read", {{"job", current.value("id")}, {"context", 0}, {"record", 0}})).value("ok").toBool());
		QVERIFY(recovered.process(request("finish", "agent.finish", {{"status", "complete"}, {"summary", "Legacy tempo is retained."}, {"evidence", QJsonArray{"tempo"}}})).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getTempo(), 91); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void historyPagingPreservesExactContextAndJobBytes_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("inlineContext");
		for (const auto protocol : {"chat", "responses"})
		{
			QTest::newRow(qPrintable(QString(protocol) + "-file")) << QString(protocol) << false;
			QTest::newRow(qPrintable(QString(protocol) + "-legacy-inline")) << QString(protocol) << true;
		}
	}
	void historyPagingPreservesExactContextAndJobBytes()
	{
		QFETCH(QString, protocol); QFETCH(bool, inlineContext); QTemporaryDir directory; QString error, oldId; QByteArray original;
		const auto unicode = QString::fromUtf8("\xf0\x9f\x8e\xb5\xc3\xa9\xe9\x9f\xb3");
		{
			int rounds = 0;
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				++rounds; return decision(protocol, {call(QString::number(rounds), "song_set", {{"tempo", rounds == 1 ? 91 : 97}})}, unicode + QString(2000, 'x'));
			});
			QVERIFY2(start(studio, protocol, error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
			QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Recover exact notes from the archived exchange."}})).value("ok").toBool());
			oldId = job(directory).value("id").toString();
			const auto reference = job(directory).value("pastInputs").toArray().at(0).toObject();
			original = read(directory.filePath(reference.value("path").toString())); QVERIFY(original.contains(unicode.toUtf8()));
			// Keep a second, mutable exchange: it must be hidden until the job is archived.
			studio.advanceAgent(); studio.advanceAgent(); QCOMPARE(Engine::getSong()->getTempo(), 97);
		}
		if (inlineContext)
		{
			auto state = manifest(directory); auto agent = state.value("agent").toObject(); auto current = agent.value("job").toObject();
			current.insert("pastInputs", QJsonArray{QJsonDocument::fromJson(original).object()}); agent.insert("job", current); state.insert("agent", agent);
			QVERIFY(write(directory.filePath("session.json"), QJsonDocument(state).toJson(QJsonDocument::Compact)));
		}
		auto studio = std::make_unique<StudioRuntime>(directory.path()); QVERIFY2(studio->open(error), qPrintable(error));
		const auto before = manifest(directory);
		auto response = studio->process(request("jobs", "agent.history.list")); QVERIFY(response.value("ok").toBool());
		auto result = response.value("result").toObject(); QCOMPARE(result.value("total").toInt(), 1);
		QCOMPARE(result.value("jobs").toArray().at(0).toObject().value("id").toString(), oldId);
		response = studio->process(request("contexts", "agent.history.list", {{"job", oldId}})); QVERIFY(response.value("ok").toBool());
		result = response.value("result").toObject(); QCOMPARE(result.value("total").toInt(), 1);
		QCOMPARE(result.value("contexts").toArray().at(0).toObject().value("storage").toString(), inlineContext ? QString("inline") : QString("contextArchive"));
		QVERIFY(!studio->process(request("live-whole", "agent.history.read", {{"job", oldId}})).value("ok").toBool());
		QVERIFY(!studio->process(request("live-input", "agent.history.read", {{"job", oldId}, {"context", 1}})).value("ok").toBool());
		QByteArray assembled; int offset = 0, pages = 0; QString digest;
		do
		{
			QJsonObject args{{"job", oldId}, {"context", 0}, {"offset", offset}, {"limit", 1021}};
			if (!digest.isEmpty()) { args.insert("sha256", digest); }
			response = studio->process(request("page-" + QString::number(pages++), "agent.history.read", args));
			QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); result = response.value("result").toObject();
			QCOMPARE(result.value("offset").toInt(), offset); QCOMPARE(result.value("totalBytes").toInt(), int(original.size()));
			QCOMPARE(result.value("protocol").toString(), protocol); QCOMPARE(result.value("encoding").toString(), QString("UTF-8 JSON"));
			digest = result.value("sha256").toString(); QCOMPARE(digest, hash(original));
			const auto chunk = result.value("text").toString().toUtf8(); QVERIFY(chunk.size() <= 1021); assembled += chunk;
			QVERIFY(result.value("nextOffset").toInt() > offset); offset = result.value("nextOffset").toInt();
			QCOMPARE(offset, int(assembled.size())); QCOMPARE(result.value("complete").toBool(), offset == original.size());
		} while (offset < original.size());
		QCOMPARE(assembled, original); QVERIFY(pages > 1);
		const int unicodeOffset = original.indexOf(unicode.toUtf8()); QVERIFY(unicodeOffset >= 0);
		response = studio->process(request("before-character", "agent.history.read", {{"job", oldId}, {"context", 0}, {"offset", unicodeOffset - 1}, {"limit", 4}}));
		QVERIFY(response.value("ok").toBool()); QCOMPARE(response.value("result").toObject().value("nextOffset").toInt(), unicodeOffset);
		QCOMPARE(response.value("result").toObject().value("text").toString().toUtf8(), original.mid(unicodeOffset - 1, 1));
		response = studio->process(request("character", "agent.history.read", {{"job", oldId}, {"context", 0}, {"offset", unicodeOffset}, {"limit", 4}}));
		QVERIFY(response.value("ok").toBool()); QCOMPARE(response.value("result").toObject().value("text").toString().toUtf8(), unicode.toUtf8().left(4));
		QVERIFY(!studio->process(request("split-character", "agent.history.read", {{"job", oldId}, {"context", 0}, {"offset", unicodeOffset + 1}})).value("ok").toBool());
		response = studio->process(request("at-end", "agent.history.read", {{"job", oldId}, {"context", 0}, {"offset", int(original.size())}, {"sha256", digest}}));
		QVERIFY(response.value("ok").toBool()); QVERIFY(response.value("result").toObject().value("complete").toBool());
		QVERIFY(response.value("result").toObject().value("text").toString().isEmpty());
		QCOMPARE(manifest(directory).value("revision"), before.value("revision")); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		const auto finalContext = QJsonDocument(QJsonObject{{"protocol", protocol}, {"input", job(directory).value("input")}}).toJson(QJsonDocument::Compact);
		QVERIFY(studio->process(request("cancel", "agent.cancel")).value("ok").toBool());
		QVERIFY(studio->process(request("new-job", "session.chat", {{"text", QString(1100, 'x') + unicode}})).value("ok").toBool());
		const auto currentId = job(directory).value("id").toString(); QVERIFY(currentId != oldId);
		response = studio->process(request("current-page", "agent.history.list", {{"limit", 1}})); result = response.value("result").toObject();
		QVERIFY(response.value("ok").toBool()); QCOMPARE(result.value("total").toInt(), 2); QVERIFY(!result.value("complete").toBool());
		QCOMPARE(result.value("nextOffset").toInt(), 1);
		QCOMPARE(result.value("jobs").toArray().at(0).toObject().value("id").toString(), currentId);
		QVERIFY(result.value("jobs").toArray().at(0).toObject().value("intentPreview").toString().toUtf8().size() <= 1024);
		response = studio->process(request("old-page", "agent.history.list", {{"offset", 1}, {"limit", 1}})); result = response.value("result").toObject();
		QVERIFY(response.value("ok").toBool()); QVERIFY(result.value("complete").toBool());
		QCOMPARE(result.value("jobs").toArray().at(0).toObject().value("id").toString(), oldId);
		QVERIFY(result.value("jobs").toArray().at(0).toObject().value("archived").toBool());
		response = studio->process(request("final-context-list", "agent.history.list", {{"job", oldId}, {"offset", 1}, {"limit", 1}})); result = response.value("result").toObject();
		QVERIFY(response.value("ok").toBool()); QCOMPARE(result.value("total").toInt(), 2); QVERIFY(result.value("complete").toBool());
		QCOMPARE(result.value("contexts").toArray().at(0).toObject().value("storage").toString(), QString("jobArchive"));
		response = studio->process(request("final-context", "agent.history.read", {{"job", oldId}, {"context", 1}, {"limit", 65536}}));
		QVERIFY(response.value("ok").toBool()); QCOMPARE(response.value("result").toObject().value("text").toString().toUtf8(), finalContext);
		const auto archive = manifest(directory).value("agent").toObject().value("archives").toArray().at(0).toObject();
		const auto archiveBytes = read(directory.filePath(archive.value("path").toString()));
		const auto wholeRequest = request("whole-job", "agent.history.read", {{"job", oldId}, {"limit", 65536}});
		const auto wholeResponse = studio->process(wholeRequest); QVERIFY(wholeResponse.value("ok").toBool());
		QCOMPARE(wholeResponse.value("result").toObject().value("text").toString().toUtf8(), archiveBytes);
		QCOMPARE(wholeResponse.value("result").toObject().value("sha256"), archive.value("sha256"));
		QCOMPARE(wholeResponse.value("result").toObject().value("context").toInt(), -1);
		studio.reset(); studio = std::make_unique<StudioRuntime>(directory.path()); QVERIFY2(studio->open(error), qPrintable(error));
		const auto recovered = manifest(directory); response = studio->process(wholeRequest); QVERIFY(response.value("replayed").toBool());
		response.remove("replayed"); QCOMPARE(response, wholeResponse); QCOMPARE(manifest(directory), recovered);
		response = studio->process(request("old-after-restart", "agent.history.read", {{"job", oldId}, {"context", 0}, {"limit", 65536}, {"sha256", digest}}));
		QVERIFY(response.value("ok").toBool()); QCOMPARE(response.value("result").toObject().value("text").toString().toUtf8(), original);
		QCOMPARE(manifest(directory).value("revision"), recovered.value("revision")); QCOMPARE(manifest(directory).value("history"), recovered.value("history"));
		QCOMPARE(Engine::getSong()->getTempo(), 97); QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
	}
	void freshHistoryReadsRejectChangedArchives_data()
	{
		QTest::addColumn<bool>("parentJob"); QTest::addColumn<bool>("remove");
		for (const auto parent : {false, true})
		{
			QTest::newRow(parent ? "job-altered" : "context-altered") << parent << false;
			QTest::newRow(parent ? "job-missing" : "context-missing") << parent << true;
		}
	}
	void freshHistoryReadsRejectChangedArchives()
	{
		QFETCH(bool, parentJob); QFETCH(bool, remove); QTemporaryDir directory;
		StudioRuntime studio(directory.path(), [](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision("chat", {call("tempo", "song_set", {{"tempo", 91}})});
		});
		QString error; QVERIFY2(start(studio, "chat", error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
		QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Tempo was changed."}})).value("ok").toBool());
		const auto oldId = job(directory).value("id").toString();
		auto reference = job(directory).value("pastInputs").toArray().at(0).toObject();
		QVERIFY(studio.process(request("cancel", "agent.cancel")).value("ok").toBool());
		QVERIFY(studio.process(request("new-job", "session.chat", {{"text", "Continue the composition."}})).value("ok").toBool());
		if (parentJob) { reference = manifest(directory).value("agent").toObject().value("archives").toArray().at(0).toObject(); }
		const auto path = directory.filePath(reference.value("path").toString()); const auto original = read(path);
		const auto query = request("initial-read", "agent.history.read", {{"job", oldId}, {"context", 0}, {"limit", 65536}});
		const auto first = studio.process(query); QVERIFY(first.value("ok").toBool());
		if (remove) { QVERIFY(QFile::remove(path)); }
		else { QVERIFY(write(path, original + " ")); } // Still JSON, but not the committed digest.
		const auto before = manifest(directory); auto response = studio.process(query); QVERIFY(response.value("replayed").toBool());
		response.remove("replayed"); QCOMPARE(response, first); QCOMPARE(manifest(directory), before);
		auto fresh = query; fresh.insert("id", "fresh-read"); response = studio.process(fresh);
		QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QVERIFY(!studio.stopping());
		QCOMPARE(manifest(directory).value("revision"), before.value("revision")); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QCOMPARE(job(directory), before.value("agent").toObject().value("job").toObject()); QCOMPARE(Engine::getSong()->getTempo(), 91);
		QVERIFY(write(path, original)); fresh.insert("id", "restored-read"); response = studio.process(fresh); QVERIFY(response.value("ok").toBool());
		QCOMPARE(response.value("result"), first.value("result"));
	}
	void historyRejectsInvalidSelectionsWithoutNativeChanges()
	{
		QTemporaryDir directory;
		StudioRuntime studio(directory.path(), [](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision("chat", {call("tempo", "song_set", {{"tempo", 91}})});
		});
		QString error; QVERIFY2(start(studio, "chat", error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
		QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Keep the tempo edit."}})).value("ok").toBool());
		const auto before = manifest(directory); const auto id = job(directory).value("id").toString();
		const QJsonObject valid{{"job", id}, {"context", 0}};
		const QJsonArray invalid{
			QJsonObject{{"job", "../../session.json"}}, QJsonObject{{"job", ""}}, QJsonObject{{"job", 1}}, QJsonObject{{"path", "session.json"}},
			QJsonObject{{"context", -1}}, QJsonObject{{"context", 0.5}}, QJsonObject{{"context", 1}}, QJsonObject{{"offset", -1}},
			QJsonObject{{"record", -1}}, QJsonObject{{"record", 0.5}}, QJsonObject{{"record", 1}},
			QJsonObject{{"offset", 0.5}}, QJsonObject{{"offset", 16777217}}, QJsonObject{{"limit", 3}}, QJsonObject{{"limit", 65537}},
			QJsonObject{{"sha256", QString(64, '0')}}, QJsonObject{{"sha256", 1}}};
		int count = 0;
		for (const auto& value : invalid)
		{
			auto args = valid; const auto bad = value.toObject();
			for (auto it = bad.begin(); it != bad.end(); ++it) { args.insert(it.key(), it.value()); }
			const auto response = studio.process(request("bad-" + QString::number(count++), "agent.history.read", args));
			QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result"));
		}
		QVERIFY(!studio.process(request("missing-job", "agent.history.read")).value("ok").toBool());
		QVERIFY(!studio.process(request("zero-list", "agent.history.list", {{"limit", 0}})).value("ok").toBool());
		QVERIFY(!studio.process(request("large-list", "agent.history.list", {{"limit", 33}})).value("ok").toBool());
		QVERIFY(!studio.process(request("bad-list-offset", "agent.history.list", {{"offset", 2}})).value("ok").toBool());
		QCOMPARE(manifest(directory).value("revision"), before.value("revision")); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QCOMPARE(job(directory), before.value("agent").toObject().value("job").toObject()); QCOMPARE(Engine::getSong()->getTempo(), 91); QVERIFY(!studio.stopping());
	}
	void cancelledHistoryReadReturnsNoPartialContent()
	{
		QTemporaryDir directory;
		StudioRuntime studio(directory.path(), [](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision("chat", {call("tempo", "song_set", {{"tempo", 91}})}, QString(300000, 'x'));
		});
		QString error; QVERIFY2(start(studio, "chat", error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
		QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Keep the native tempo."}})).value("ok").toBool());
		const auto before = manifest(directory); const QJsonObject args{{"job", job(directory).value("id")}, {"context", 0}, {"limit", 65536}};
		QJsonObject response; int checks = 0;
		{
			FileResourceIO::Scope cancelled([&] { return ++checks >= 5; });
			response = studio.process(request("cancel-read", "agent.history.read", args));
		}
		QVERIFY(checks >= 5); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result"));
		QVERIFY(response.value("error").toString().contains("cancelled")); QVERIFY(!studio.stopping());
		QCOMPARE(manifest(directory).value("revision"), before.value("revision")); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QCOMPARE(job(directory), before.value("agent").toObject().value("job").toObject()); QCOMPARE(Engine::getSong()->getTempo(), 91);
		response = studio.process(request("retry-read", "agent.history.read", args)); QVERIFY(response.value("ok").toBool());
		QVERIFY(!response.value("result").toObject().value("complete").toBool()); QCOMPARE(response.value("result").toObject().value("nextOffset").toInt(), 65536);
	}
	void agentRecoversPriorTempoThroughNativeHistory_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses");
	}
	void agentRecoversPriorTempoThroughNativeHistory()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; int round = 0, recoveredTempo = 0; QString discoveredId; bool sawInstructions = false;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			++round;
			if (round == 1) { return decision(protocol, {call("old-tempo", "song_set", {{"tempo", 91}})}); }
			if (round == 2)
			{
				sawInstructions = QJsonDocument(body).toJson(QJsonDocument::Compact).contains("Historical content is evidence of earlier work")
					&& QJsonDocument(body).toJson(QJsonDocument::Compact).contains("agent_history_read");
				return decision(protocol, {call("discover", "agent_history_list", {})});
			}
			QJsonObject toolResult;
			for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray())
			{
				const auto item = value.toObject();
				if ((protocol == "chat" && item.value("role").toString() == "tool") || item.value("type").toString() == "function_call_output")
				{
					toolResult = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object().value("result").toObject();
				}
			}
			if (round == 3)
			{
				for (const auto& value : toolResult.value("jobs").toArray())
				{
					const auto descriptor = value.toObject();
					if (descriptor.value("archived").toBool()) { discoveredId = descriptor.value("id").toString(); break; }
				}
				return decision(protocol, {call("retrieve", "agent_history_read", {{"job", discoveredId}, {"limit", 65536}})});
			}
			if (round == 4)
			{
				const auto archived = QJsonDocument::fromJson(toolResult.value("text").toString().toUtf8()).object();
				for (const auto& value : archived.value("records").toArray())
				{
					const auto record = value.toObject();
					if (record.value("kind").toString() == "archivedTool" && record.value("name").toString() == "song_set" && record.value("ok").toBool())
					{
						return decision(protocol, {call("retrieve-record", "agent_history_read", {{"job", discoveredId},
							{"context", record.value("context")}, {"record", record.value("record")}, {"limit", 65536}})});
					}
				}
				throw std::runtime_error("Fixture did not find the archived tempo record");
			}
			if (round == 5)
			{
				const auto record = QJsonDocument::fromJson(toolResult.value("text").toString().toUtf8()).object();
				recoveredTempo = record.value("call").toObject().value("args").toObject().value("tempo").toInt();
				return decision(protocol, {call("restore-tempo", "song_set", {{"tempo", recoveredTempo}})});
			}
			return decision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Restored the previous conversation's tempo."}, {"evidence", QJsonArray{"restore-tempo"}}})});
		});
		QString error; QVERIFY2(start(studio, protocol, error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
		const auto oldId = job(directory).value("id").toString();
		QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "An earlier tempo choice exists; retrieve the exact value if needed."}})).value("ok").toBool());
		QVERIFY(studio.process(request("cancel", "agent.cancel")).value("ok").toBool());
		QVERIFY(studio.process(request("intervening-edit", "song.set", {{"tempo", 77}})).value("ok").toBool());
		QVERIFY(studio.process(request("next-chat", "session.chat", {{"text", "Restore the tempo from our previous conversation."}})).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getTempo(), 77); const auto before = manifest(directory);
		for (int i = 0; i < 4; ++i) { QVERIFY(!studio.advanceAgent().isEmpty()); }
		QCOMPARE(discoveredId, oldId); QCOMPARE(Engine::getSong()->getTempo(), 77);
		QCOMPARE(manifest(directory).value("revision"), before.value("revision")); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		for (int i = 0; i < 6; ++i) { QVERIFY(!studio.advanceAgent().isEmpty()); }
		QVERIFY(sawInstructions); QCOMPARE(round, 6); QCOMPARE(recoveredTempo, 91); QCOMPARE(Engine::getSong()->getTempo(), 91);
		QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(job(directory).value("operations").toInt(), 5);
		QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
	}
	void compactionCannotDiscardOutstandingCalls_data()
	{
		QTest::addColumn<bool>("manual"); QTest::newRow("manual-with-pending-call") << true; QTest::newRow("not-last-in-batch") << false;
	}
	void compactionCannotDiscardOutstandingCalls()
	{
		QFETCH(bool, manual); QTemporaryDir directory;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			QJsonArray calls;
			if (!manual) { calls.append(call("compact", "agent_context_compact", {{"summary", "Do not discard the tempo edit."}})); }
			calls.append(call("tempo", "song_set", {{"tempo", 91}})); return decision("chat", calls);
		});
		QString error; QVERIFY2(start(studio, "chat", error), qPrintable(error)); studio.advanceAgent(); const auto before = job(directory);
		if (manual)
		{
			QVERIFY(!studio.process(request("manual", "agent.context.compact", {{"summary", "Pending edits must remain."}})).value("ok").toBool());
			QCOMPARE(job(directory), before); studio.advanceAgent(); QCOMPARE(Engine::getSong()->getTempo(), 91);
		}
		else
		{
			studio.advanceAgent(); QCOMPARE(Engine::getSong()->getTempo(), DefaultTempo);
			QVERIFY(job(directory).value("pending").toArray().isEmpty());
			QVERIFY(!job(directory).value("records").toArray().last().toObject().value("response").toObject().value("ok").toBool());
		}
		QVERIFY(job(directory).value("pastInputs").toArray().isEmpty()); QVERIFY(!QDir(directory.filePath("contexts")).exists());
		QVERIFY(!studio.stopping()); QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::StudioContextTest)
#include "StudioContextTest.moc"
