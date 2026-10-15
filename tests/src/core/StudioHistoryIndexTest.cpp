/* Bounded discovery of preserved native decisions. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSet>
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

using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject call(const QString& id, const QString& name, const QJsonObject& args = {}) { return {{"id", id}, {"name", name}, {"args", args}}; }
QByteArray bytes(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& value) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(value) == value.size(); }
QString hash(const QByteArray& value) { return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex()); }
QJsonObject manifest(const QTemporaryDir& dir) { return QJsonDocument::fromJson(read(dir.filePath("session.json"))).object(); }
QJsonObject job(const QTemporaryDir& dir) { return manifest(dir).value("agent").toObject().value("job").toObject(); }
QJsonObject provider(const QString& protocol)
{ return {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-history-index-fixture"}}}}; }
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output;
	for (const auto& value : calls)
	{
		const auto item = value.toObject(); const auto arguments = QString::fromUtf8(bytes(item.value("args").toObject()));
		if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", arguments}}); }
		else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", arguments}}}}); }
	}
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
QJsonArray tempoCalls()
{ QJsonArray calls; for (int i = 0; i < 17; ++i) { calls.append(call("tempo-" + QString::number(i), "song_set", {{"tempo", 60 + i}})); } return calls; }
bool start(StudioRuntime& studio, const QString& protocol, QString& error)
{
	return studio.open(error) && studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()
		&& studio.process(request("chat", "session.chat", {{"text", "Retain these native tempo decisions"}})).value("ok").toBool();
}
QJsonObject list(StudioRuntime& studio, const QString& requestId, const QString& jobId, int offset = 0, int limit = 16)
{ return studio.process(request(requestId, "agent.history.list", {{"job", jobId}, {"context", 0}, {"offset", offset}, {"limit", limit}})); }
QByteArray retrieve(StudioRuntime& studio, const QJsonObject& selector, const QString& prefix)
{
	QByteArray result; QString digest;
	for (int page = 0; page < 256; ++page)
	{
		auto args = selector; args.insert("offset", result.size()); args.insert("limit", 65536); if (!digest.isEmpty()) { args.insert("sha256", digest); }
		const auto response = studio.process(request(prefix + QString::number(page), "agent.history.read", args));
		if (!response.value("ok").toBool()) { return {}; }
		const auto data = response.value("result").toObject(); digest = data.value("sha256").toString(); result += data.value("text").toString().toUtf8();
		if (data.value("nextOffset").toInt() != result.size()) { return {}; }
		if (data.value("complete").toBool()) { return hash(result) == digest ? result : QByteArray{}; }
	}
	return {};
}
QJsonObject lastOutput(const QJsonObject& body, const QString& protocol)
{
	QJsonObject output;
	for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray())
	{
		const auto item = value.toObject();
		if (item.value("role").toString() == "tool" || item.value("type").toString() == "function_call_output")
		{ output = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); }
	}
	return output;
}
void protocols() { QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
}

class StudioHistoryIndexTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
		Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->clearProject(); }

	void nativeRecordsAreDiscoverableInEveryFrozenStore_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("storage"); QTest::addColumn<bool>("restart");
		for (const auto protocol : {QString("chat"), QString("responses")})
			for (const auto storage : {QString("file"), QString("inline"), QString("final")}) for (bool restart : {false, true})
			{ if (storage == "inline" && !restart) { continue; } QTest::newRow(qPrintable(protocol + storage + QString::number(restart))) << protocol << storage << restart; }
	}
	void nativeRecordsAreDiscoverableInEveryFrozenStore()
	{
		QFETCH(QString, protocol); QFETCH(QString, storage); QFETCH(bool, restart); QTemporaryDir directory; QString error;
		auto studio = std::make_unique<StudioRuntime>(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, tempoCalls()); });
		QVERIFY2(start(*studio, protocol, error), qPrintable(error)); for (int i = 0; i < 18; ++i) { studio->advanceAgent(); }
		QCOMPARE(job(directory).value("operations").toInt(), 17); const auto id = job(directory).value("id").toString();
		const auto oldInput = job(directory).value("input"); const auto finalBytes = bytes({{"protocol", protocol}, {"input", oldInput}});
		if (storage == "final")
		{
			QVERIFY(studio->process(request("cancel", "agent.cancel")).value("ok").toBool());
			QVERIFY(studio->process(request("next", "session.chat", {{"text", "Continue the composition"}})).value("ok").toBool());
		}
		else { QVERIFY(studio->process(request("compact", "agent.context.compact", {{"summary", "Tempo is 76; earlier decisions remain archived."}})).value("ok").toBool()); }
		if (restart)
		{
			studio.reset();
			if (storage == "inline")
			{
				auto state = manifest(directory), agent = state.value("agent").toObject(), current = agent.value("job").toObject(); auto contexts = current.value("pastInputs").toArray();
				contexts[0] = QJsonDocument::fromJson(read(directory.filePath(contexts.at(0).toObject().value("path").toString()))).object();
				current.insert("pastInputs", contexts); agent.insert("job", current); state.insert("agent", agent); QVERIFY(write(directory.filePath("session.json"), bytes(state)));
			}
			studio = std::make_unique<StudioRuntime>(directory.path()); QVERIFY2(studio->open(error), qPrintable(error));
		}
		const auto before = manifest(directory); auto response = studio->process(request("contexts", "agent.history.list", {{"job", id}}));
		QVERIFY(response.value("ok").toBool()); QCOMPARE(response.value("result").toObject().value("contexts").toArray().at(0).toObject().value("recordItems").toInt(), 17);
		const auto firstRequest = request("default-page", "agent.history.list", {{"job", id}, {"context", 0}});
		response = studio->process(firstRequest); QVERIFY(response.value("ok").toBool()); QCOMPARE(response.value("result").toObject().value("records").toArray().size(), 16);
		QVERIFY(studio->process(firstRequest).value("replayed").toBool()); int offset = 0; QSet<QString> seen;
		while (offset < 17)
		{
			response = list(*studio, "page-" + QString::number(offset), id, offset, 7); QVERIFY(response.value("ok").toBool()); const auto page = response.value("result").toObject();
			QVERIFY(!page.value("records").toArray().isEmpty());
			QCOMPARE(page.value("protocol").toString(), protocol); QCOMPARE(page.value("total").toInt(), 17); QCOMPARE(page.value("offset").toInt(), offset);
			for (const auto& value : page.value("records").toArray())
			{
				const auto record = value.toObject(); const auto callId = record.value("call").toString(); QVERIFY(!seen.contains(callId)); seen.insert(callId);
				QCOMPARE(record.value("record").toInt(), offset++); QCOMPARE(callId, "tempo-" + QString::number(offset - 1));
				QCOMPARE(record.value("name").toString(), QString("song_set")); QVERIFY(record.value("ok").toBool()); QVERIFY(!record.contains("args")); QVERIFY(!record.contains("result"));
			}
			QCOMPARE(page.value("nextOffset").toInt(), offset); QCOMPARE(page.value("complete").toBool(), offset == 17);
		}
		response = list(*studio, "end", id, 17); QVERIFY(response.value("ok").toBool()); QVERIFY(response.value("result").toObject().value("records").toArray().isEmpty());
		const auto byIndex = retrieve(*studio, {{"job", id}, {"context", 0}, {"record", 13}}, "record-"); QVERIFY(!byIndex.isEmpty());
		QCOMPARE(byIndex, retrieve(*studio, {{"job", id}, {"call", "tempo-13"}}, "call-"));
		QCOMPARE(QJsonDocument::fromJson(byIndex).object().value("call").toObject().value("args").toObject().value("tempo").toInt(), 73);
		if (storage == "final") { QCOMPARE(retrieve(*studio, {{"job", id}, {"context", 0}}, "whole-"), finalBytes); }
		QCOMPARE(Engine::getSong()->getTempo(), 76); QCOMPARE(manifest(directory).value("revision"), before.value("revision")); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QCOMPARE(job(directory), before.value("agent").toObject().value("job").toObject());
	}

	void finalStoredPayloadAndSkippedCallsRemainAccessible_data() { protocols(); }
	void finalStoredPayloadAndSkippedCallsRemainAccessible()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision(protocol, {call("large", "project_inspect"), call("restore", "session_undo", {{"summary", true}}),
				call("skipped", "track_edit", {{"track", 0}, {"name", "Wrong"}})});
		});
		QVERIFY(start(studio, protocol, error)); const QString name(300000, 'n');
		QVERIFY(studio.process(request("native-track", "track.add", {{"type", "instrument"}, {"name", name}})).value("ok").toBool());
		QVERIFY(studio.process(request("replan", "agent.resume")).value("ok").toBool()); for (int i = 0; i < 3; ++i) { studio.advanceAgent(); }
		const auto id = job(directory).value("id").toString();
		QVERIFY(studio.process(request("cancel", "agent.cancel")).value("ok").toBool()); QVERIFY(studio.process(request("next", "session.chat", {{"text", "Use the prior evidence"}})).value("ok").toBool());
		const auto response = list(studio, "metadata", id); QVERIFY(response.value("ok").toBool()); const auto records = response.value("result").toObject().value("records").toArray();
		QCOMPARE(records.size(), 3); QCOMPARE(records.at(0).toObject().value("responseStorage").toString(), QString("toolResultArchive"));
		QVERIFY(records.at(0).toObject().value("payloadBytes").toInt() > 256 * 1024); QVERIFY(!records.at(2).toObject().value("ok").toBool());
		QVERIFY(records.at(2).toObject().value("errorPreview").toString().contains("restored")); QVERIFY(QJsonDocument(records).toJson(QJsonDocument::Compact).size() < 4096);
		const auto full = retrieve(studio, {{"job", id}, {"context", 0}, {"record", 0}}, "payload-"); QVERIFY(full.contains(name.toUtf8()));
		QCOMPARE(full, retrieve(studio, {{"job", id}, {"call", "large"}}, "same-"));
		const auto skipped = QJsonDocument::fromJson(retrieve(studio, {{"job", id}, {"context", 0}, {"record", 2}}, "skipped-")).object();
		QCOMPARE(skipped.value("call").toObject().value("args").toObject().value("name").toString(), QString("Wrong")); QVERIFY(!skipped.value("response").toObject().value("ok").toBool());
	}

	void metadataPagesBoundEscapedPreviewsAndCancelWithoutPartialResults_data() { protocols(); }
	void metadataPagesBoundEscapedPreviewsAndCancelWithoutPartialResults()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, tempoCalls()); });
			QVERIFY(start(studio, protocol, error)); for (int i = 0; i < 18; ++i) { studio.advanceAgent(); }
			QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Synthetic failure-metadata fixture follows"}})).value("ok").toBool());
		}
		auto state = manifest(directory), agent = state.value("agent").toObject(), current = agent.value("job").toObject(); auto contexts = current.value("pastInputs").toArray();
		auto context = QJsonDocument::fromJson(read(directory.filePath(contexts.at(0).toObject().value("path").toString()))).object();
		auto records = context.value("records").toArray(), input = context.value("input").toArray(), index = current.value("records").toArray();
		const auto music = QString::fromUtf8("\xf0\x9f\x8e\xb5");
		for (int i = 0; i < records.size(); ++i)
		{
			const auto message = (i == 0 ? QString(1022, 'a') + music : QString(1023, QChar(1)) + music) + QString(100000, 'x');
			auto record = records.at(i).toObject(); const auto id = record.value("call").toObject().value("id"); const QJsonObject response{{"ok", false}, {"error", message}};
			record.insert("response", response); records[i] = record;
			for (int j = 0; j < input.size(); ++j)
			{
				auto item = input.at(j).toObject(); if (item.value(protocol == "chat" ? "tool_call_id" : "call_id") != id) { continue; }
				if (item.value("role").toString() == "tool" || item.value("type").toString() == "function_call_output")
				{ item.insert(protocol == "chat" ? "content" : "output", QString::fromUtf8(bytes(response))); input[j] = item; }
			}
			for (int j = 0; j < index.size(); ++j) { auto item = index.at(j).toObject(); if (item.value("id") == id) { item.insert("ok", false); index[j] = item; } }
		}
		context.insert("records", records); context.insert("input", input); contexts[0] = context; current.insert("pastInputs", contexts); current.insert("records", index);
		agent.insert("job", current); state.insert("agent", agent); QVERIFY(StudioAgent::validState(agent)); QVERIFY(write(directory.filePath("session.json"), bytes(state)));
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); const auto before = job(directory); const auto id = current.value("id").toString();
		int offset = 0, pages = 0;
		while (offset < 17)
		{
			const auto response = list(recovered, "bounded-" + QString::number(pages++), id, offset, 32); QVERIFY(response.value("ok").toBool()); const auto page = response.value("result").toObject();
			const auto entries = page.value("records").toArray(); QVERIFY(!entries.isEmpty()); QVERIFY(QJsonDocument(entries).toJson(QJsonDocument::Compact).size() <= 64 * 1024);
			for (const auto& value : entries)
			{
				const auto entry = value.toObject(); const auto preview = entry.value("errorPreview").toString(); QVERIFY(preview.toUtf8().size() <= 1024); QVERIFY(!preview.contains(QChar(0xfffd)));
				QCOMPARE(entry.value("record").toInt(), offset++); QVERIFY(!entry.value("ok").toBool()); QVERIFY(!preview.contains('x'));
			}
			QCOMPARE(page.value("nextOffset").toInt(), offset);
		}
		QVERIFY(pages > 1); int checks = 0; QJsonObject cancelled;
		{ FileResourceIO::Scope scope([&] { return ++checks >= 4; }); cancelled = list(recovered, "cancelled", id); }
		QVERIFY(!cancelled.value("ok").toBool()); QVERIFY(!cancelled.contains("result")); QCOMPARE(job(directory), before); QCOMPARE(Engine::getSong()->getTempo(), 76);
	}

	void invalidSelectorsAndLegacyMissingRecordsRemainExplicit()
	{
		QTemporaryDir directory; QString error;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision("chat", tempoCalls()); });
			QVERIFY(start(studio, "chat", error)); for (int i = 0; i < 18; ++i) { studio.advanceAgent(); }
			QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Legacy input-only history"}})).value("ok").toBool());
		}
		auto state = manifest(directory), agent = state.value("agent").toObject(), current = agent.value("job").toObject(); auto contexts = current.value("pastInputs").toArray();
		auto context = QJsonDocument::fromJson(read(directory.filePath(contexts.at(0).toObject().value("path").toString()))).object(); context.remove("records"); contexts[0] = context;
		QJsonArray remaining; for (const auto& item : current.value("records").toArray()) { if (item.toObject().value("kind").toString() != "archivedTool") { remaining.append(item); } }
		current.insert("records", remaining); current.insert("pastInputs", contexts); agent.insert("job", current); state.insert("agent", agent); QVERIFY(write(directory.filePath("session.json"), bytes(state)));
		StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error)); const auto id = current.value("id").toString(); const auto before = job(directory);
		const auto response = list(studio, "empty", id); QVERIFY(response.value("ok").toBool()); QCOMPARE(response.value("result").toObject().value("total").toInt(), 0); QVERIFY(response.value("result").toObject().value("complete").toBool());
		QCOMPARE(retrieve(studio, {{"job", id}, {"context", 0}}, "legacy-"), bytes(context));
		QVERIFY(!studio.process(request("no-record", "agent.history.read", {{"job", id}, {"context", 0}, {"record", 0}})).value("ok").toBool());
		int counter = 0;
		for (const auto& invalid : QJsonArray{QJsonObject{{"context", -1}}, QJsonObject{{"context", 0.5}}, QJsonObject{{"context", "0"}}, QJsonObject{{"context", 1}},
			QJsonObject{{"offset", 1}}, QJsonObject{{"offset", -1}}, QJsonObject{{"offset", 0.5}}, QJsonObject{{"limit", 0}}, QJsonObject{{"limit", 33}},
			QJsonObject{{"record", 0}}, QJsonObject{{"call", "tempo-0"}}, QJsonObject{{"sha256", "x"}}})
		{
			QJsonObject args{{"job", id}, {"context", 0}}; const auto fields = invalid.toObject(); for (auto it = fields.begin(); it != fields.end(); ++it) { args.insert(it.key(), it.value()); }
			const auto rejected = studio.process(request("invalid-" + QString::number(counter++), "agent.history.list", args)); QVERIFY(!rejected.value("ok").toBool()); QVERIFY(!rejected.contains("result"));
		}
		QVERIFY(!studio.process(request("missing-job", "agent.history.list", {{"context", 0}})).value("ok").toBool()); QCOMPARE(job(directory), before); QCOMPARE(Engine::getSong()->getTempo(), 76);
	}

	void freshIndexReadsRejectAlteredArchivesButReceiptsRemainReplayable_data() { protocols(); }
	void freshIndexReadsRejectAlteredArchivesButReceiptsRemainReplayable()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, tempoCalls()); });
		QVERIFY(start(studio, protocol, error)); for (int i = 0; i < 18; ++i) { studio.advanceAgent(); }
		QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Preserve the native tempo"}})).value("ok").toBool());
		const auto id = job(directory).value("id").toString(); const auto accepted = list(studio, "page", id); QVERIFY(accepted.value("ok").toBool());
		const auto reference = job(directory).value("pastInputs").toArray().at(0).toObject(); const auto path = directory.filePath(reference.value("path").toString()); const auto original = read(path);
		const auto cleanup = qScopeGuard([&] { write(path, original); }); QVERIFY(write(path, "altered archive"));
		QVERIFY(list(studio, "page", id).value("replayed").toBool()); const auto fresh = list(studio, "fresh", id); QVERIFY(!fresh.value("ok").toBool()); QVERIFY(!fresh.contains("result"));
		QCOMPARE(Engine::getSong()->getTempo(), 76);
	}

	void agentFindsAnEarlierDecisionAndContinuesNativeEditing_data() { protocols(); }
	void agentFindsAnEarlierDecisionAndContinuesNativeEditing()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error, oldId; int round = 0; bool discovered = false;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			if (++round == 1) { return decision(protocol, tempoCalls()); }
			if (round == 2) { return decision(protocol, {call("index", "agent_history_list", {{"job", oldId}, {"context", 0}})}); }
			const auto result = lastOutput(body, protocol).value("result").toObject();
			if (round == 3)
			{
				int record = -1; for (const auto& item : result.value("records").toArray()) { if (item.toObject().value("call").toString() == "tempo-13") { record = item.toObject().value("record").toInt(); } }
				discovered = record == 13; return decision(protocol, {call("read", "agent_history_read", {{"job", oldId}, {"context", 0}, {"record", record}})});
			}
			const auto historical = QJsonDocument::fromJson(result.value("text").toString().toUtf8()).object();
			return decision(protocol, {call("revise", "song_set", {{"tempo", historical.value("call").toObject().value("args").toObject().value("tempo")}}),
				call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Recovered the earlier native tempo"}, {"evidence", QJsonArray{"index", "read", "revise"}}})});
		});
		QVERIFY(start(studio, protocol, error)); for (int i = 0; i < 18; ++i) { studio.advanceAgent(); } oldId = job(directory).value("id").toString();
		QVERIFY(studio.process(request("cancel", "agent.cancel")).value("ok").toBool()); QVERIFY(studio.process(request("new", "session.chat", {{"text", "Restore the tempo decision named tempo-13."}})).value("ok").toBool());
		for (int i = 0; i < 7; ++i) { studio.advanceAgent(); }
		QVERIFY(discovered); QCOMPARE(Engine::getSong()->getTempo(), 73); QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(job(directory).value("operations").toInt(), 4);
	}
};
QTEST_GUILESS_MAIN(StudioHistoryIndexTest)
#include "StudioHistoryIndexTest.moc"
