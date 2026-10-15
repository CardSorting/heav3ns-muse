/* Durable native tool payloads and provider projections. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioRuntime.h"
#include "Track.h"

using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
{ return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject call(const QString& id, const QString& name, const QJsonObject& args = {})
{ return {{"id", id}, {"name", name}, {"args", args}}; }
QByteArray read(const QString& path)
{ QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes)
{ QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
QByteArray encoded(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QString hash(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
QJsonObject manifest(const QTemporaryDir& dir) { return QJsonDocument::fromJson(read(dir.filePath("session.json"))).object(); }
QJsonObject job(const QTemporaryDir& dir) { return manifest(dir).value("agent").toObject().value("job").toObject(); }
QJsonObject tool(const QTemporaryDir& dir, const QString& id)
{
	for (const auto& value : job(dir).value("records").toArray())
	{
		const auto record = value.toObject(); if (record.value("call").toObject().value("id") == QJsonValue(id)) { return record; }
	}
	return {};
}
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output;
	for (const auto& value : calls)
	{
		const auto item = value.toObject(); const auto args = QString::fromUtf8(encoded(item.value("args").toObject()));
		if (protocol == "responses")
		{ output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", args}}); }
		else
		{ output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", args}}}}); }
	}
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{
		{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
QJsonObject provider(const QString& protocol)
{ return {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "tool-result-fixture"}}}}; }
QString nativeName(int length = 300000) { return QString(length, 'p') + QString::fromUtf8("\xf0\x9f\x8e\xb5\xe9\x9f\xb3"); }
bool start(StudioRuntime& studio, const QString& protocol, QString& error, int nameLength = 300000)
{
	return studio.open(error)
		&& studio.process(request("native-track", "track.add", {{"type", "sample"}, {"name", nativeName(nameLength)}})).value("ok").toBool()
		&& studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()
		&& studio.process(request("chat", "session.chat", {{"text", "Inspect the native composition, preserve its exact state and continue."}})).value("ok").toBool();
}
QByteArray retrieve(StudioRuntime& studio, const QString& jobId, const QString& callId, const QString& digest)
{
	static int query = 0; const auto prefix = "read-" + QString::number(++query) + "-";
	QByteArray result; int offset = 0;
	for (int page = 0; page < 128; ++page)
	{
		const auto response = studio.process(request(prefix + QString::number(offset), "agent.history.read",
			{{"job", jobId}, {"call", callId}, {"offset", offset}, {"limit", 65536}, {"sha256", digest}}));
		if (!response.value("ok").toBool()) { return {}; }
		const auto item = response.value("result").toObject();
		if (item.value("sha256") != QJsonValue(digest) || item.value("offset").toInt() != offset || item.value("nextOffset").toInt() <= offset) { return {}; }
		result += item.value("text").toString().toUtf8(); offset = item.value("nextOffset").toInt();
		if (offset != result.size()) { return {}; }
		if (item.value("complete").toBool()) { return result; }
	}
	return {};
}
void protocols()
{
	QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses");
}
}

class StudioToolResultTest : public QObject
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
	void init()
	{
		Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->tempoModel().setInitValue(DefaultTempo); Engine::getSong()->clearErrors();
	}
	void cleanup() { Engine::getSong()->clearProject(); }

	void errorPreviewsPreserveExactUnicodePrefixes_data()
	{
		QTest::addColumn<QString>("errorText"); QTest::addColumn<QString>("expected");
		const auto emoji = QString::fromUtf8("\xf0\x9f\x8e\xb5");
		const auto euro = QString::fromUtf8("\xe2\x82\xac");
		const auto accented = QString::fromUtf8("\xc3\xa1");
		QTest::newRow("short") << QString("Native failure") << QString("Native failure");
		QTest::newRow("exact-byte-boundary") << QString(1024, 'a') << QString(1024, 'a');
		QTest::newRow("large-ascii") << QString(3 * 1024 * 1024, 'z') << QString(1024, 'z');
		QTest::newRow("surrogate-crosses-character-cap") << (QString(1023, 'a') + emoji + QString(300000, 'z')) << QString(1023, 'a');
		QTest::newRow("surrogate-after-byte-cap") << (QString(1024, 'a') + emoji + QString(300000, 'z')) << QString(1024, 'a');
		QTest::newRow("three-byte-character-crosses-byte-cap") << (QString(1023, 'a') + euro + QString(300000, 'z')) << QString(1023, 'a');
		QTest::newRow("mixed-encoding-boundary") << (QString(511, accented.at(0)) + euro + QString(300000, 'z')) << QString(511, accented.at(0));
	}
	void errorPreviewsPreserveExactUnicodePrefixes()
	{
		QFETCH(QString, errorText); QFETCH(QString, expected);
		const auto command = call("failure", "project_inspect");
		const QJsonObject response{{"id", "native-failure"}, {"revision", 7}, {"ok", false}, {"error", errorText}};
		const QJsonObject original{{"kind", "tool"}, {"call", command}, {"response", response}};
		const auto bytes = encoded(original);
		const QJsonObject archive{{"path", "tool-results/12345678-1234-1234-1234-123456789abc.json"}, {"sha256", hash(bytes)}, {"bytes", static_cast<int>(bytes.size())}};
		const auto projected = StudioAgent::storedToolResponse(command, response, archive, "12345678-1234-1234-1234-123456789abc");
		QCOMPARE(projected.value("errorPreview").toString(), expected);
		QVERIFY(projected.value("errorPreview").toString().toUtf8().size() <= 1024);
		QCOMPARE(projected.value("ok"), QJsonValue(false)); QCOMPARE(projected.value("id"), response.value("id"));
		QCOMPARE(projected.value("revision"), response.value("revision"));
		QCOMPARE(projected.value("storedResult").toObject().value("sha256"), archive.value("sha256"));
		QCOMPARE(projected.value("storedResult").toObject().value("bytes"), archive.value("bytes"));
		QCOMPARE(original.value("response").toObject().value("error").toString(), errorText);
		QVERIFY(StudioAgent::validToolRecord({{"kind", "tool"}, {"call", command}, {"response", projected}, {"archive", archive}}));
		for (const auto invalid : {QJsonValue(QString(3 * 1024 * 1024, 'z')), QJsonValue(QString(512, euro.at(0))), QJsonValue(42)})
		{
			auto rejected = projected; rejected.insert("errorPreview", invalid);
			QVERIFY(!StudioAgent::validToolRecord({{"kind", "tool"}, {"call", command}, {"response", rejected}, {"archive", archive}}));
		}
	}

	void largeBatchPreservesNativeResultsAndProviderPairs_data() { protocols(); }
	void largeBatchPreservesNativeResultsAndProviderPairs()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; int round = 0; QJsonObject providerBody;
		const auto transport = [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			if (++round == 1)
			{
				QJsonArray calls; for (int index = 0; index < 32; ++index) { calls.append(call("scan-" + QString::number(index), "project_inspect")); }
				return decision(protocol, calls);
			}
			providerBody = body;
			return decision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Native inventory preserved."}, {"evidence", QJsonArray{"scan-0", "scan-31"}}})});
		};
		StudioRuntime studio(directory.path(), transport); QVERIFY2(start(studio, protocol, error), qPrintable(error));
		const auto before = manifest(directory); studio.advanceAgent(); qint64 retainedBytes = 0;
		for (int index = 0; index < 32; ++index)
		{
			const auto response = studio.advanceAgent(); QCOMPARE(response.value("event").toString(), QString("agent"));
			QVERIFY(!studio.stopping()); const auto current = job(directory);
			QCOMPARE(current.value("pending").toArray().size(), 31 - index); QCOMPARE(current.value("operations").toInt(), index + 1);
			const auto record = tool(directory, "scan-" + QString::number(index)); QVERIFY(StudioAgent::validToolRecord(record));
			const auto archive = record.value("archive").toObject(); QVERIFY(!archive.isEmpty());
			const auto bytes = read(directory.filePath(archive.value("path").toString())); retainedBytes += bytes.size();
			QCOMPARE(bytes.size(), archive.value("bytes").toInt()); QCOMPARE(hash(bytes), archive.value("sha256").toString());
			const auto original = QJsonDocument::fromJson(bytes).object(); QVERIFY(!original.contains("archive"));
			QCOMPARE(original.value("response").toObject().value("result").toObject().value("tracks").toArray().at(0).toObject().value("name").toString(), nativeName());
			QVERIFY(read(directory.filePath("session.json")).size() < 2 * 1024 * 1024);
		}
		QVERIFY(retainedBytes * 2 > 16 * 1024 * 1024);
		QCOMPARE(manifest(directory).value("history"), before.value("history")); QCOMPARE(manifest(directory).value("revision"), before.value("revision"));
		studio.advanceAgent(); const auto input = providerBody.value(protocol == "chat" ? "messages" : "input").toArray(); QSet<QString> outputs, calls;
		for (const auto& value : input)
		{
			const auto item = value.toObject(); const auto id = item.value(protocol == "chat" ? "tool_call_id" : "call_id").toString();
			if (item.value("type").toString() == "function_call") { QVERIFY(!calls.contains(id)); calls.insert(id); }
			for (const auto& entry : item.value("tool_calls").toArray())
			{ const auto called = entry.toObject().value("id").toString(); QVERIFY(!calls.contains(called)); calls.insert(called); }
			if ((protocol == "chat" && item.value("role").toString() != "tool")
				|| (protocol == "responses" && item.value("type").toString() != "function_call_output")) { continue; }
			QVERIFY(!outputs.contains(id)); outputs.insert(id);
			const auto projected = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object();
			QCOMPARE(projected, tool(directory, id).value("response").toObject()); QVERIFY(projected.contains("storedResult")); QVERIFY(!projected.contains("result"));
		}
		QCOMPARE(outputs.size(), 32); QCOMPARE(calls, outputs); studio.advanceAgent(); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
		QCOMPARE(Engine::getSong()->tracks().at(0)->name(), nativeName());
	}

	void exactRetrievalSurvivesCompactionRestartAndJobArchiving_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("compact");
		QTest::newRow("chat-live") << QString("chat") << false; QTest::newRow("chat-compacted") << QString("chat") << true;
		QTest::newRow("responses-live") << QString("responses") << false; QTest::newRow("responses-compacted") << QString("responses") << true;
	}
	void exactRetrievalSurvivesCompactionRestartAndJobArchiving()
	{
		QFETCH(QString, protocol); QFETCH(bool, compact); QTemporaryDir directory; QString error, id, digest; QByteArray original;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("observe", "project_inspect")}); });
			QVERIFY2(start(studio, protocol, error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
			id = job(directory).value("id").toString(); const auto archive = tool(directory, "observe").value("archive").toObject();
			original = read(directory.filePath(archive.value("path").toString())); digest = archive.value("sha256").toString(); QVERIFY(!original.isEmpty());
			QCOMPARE(retrieve(studio, id, "observe", digest), original);
			if (compact)
			{
				QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Keep the original composition; observe retains the exact inventory."}})).value("ok").toBool());
				QCOMPARE(job(directory).value("records").toArray().last().toObject().value("kind").toString(), QString("archivedTool"));
				const auto selected = studio.process(request("record-in-context", "agent.history.read", {{"job", id}, {"context", 0}, {"record", 0}, {"limit", 65536}}));
				QVERIFY(selected.value("ok").toBool()); const auto page = selected.value("result").toObject();
				QCOMPARE(page.value("sha256").toString(), digest); QCOMPARE(page.value("totalBytes").toInt(), int(original.size()));
				QCOMPARE(page.value("text").toString().toUtf8(), original.left(page.value("nextOffset").toInt()));
			}
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(retrieve(recovered, id, "observe", digest), original);
		// Fresh reads must validate selectors rather than succeeding through old receipts.
		for (const auto& invalid : QJsonArray{QJsonObject{{"call", "observe"}, {"context", 0}}, QJsonObject{{"call", "observe"}, {"record", 0}},
			QJsonObject{{"call", 1}}, QJsonObject{{"call", ""}}, QJsonObject{{"call", "unknown"}}, QJsonObject{{"call", "observe"}, {"sha256", QString(64, '0')}}})
		{
			auto args = invalid.toObject(); args.insert("job", id);
			QVERIFY(!recovered.process(request("invalid-" + QString::number(args.size()) + hash(encoded(args)), "agent.history.read", args)).value("ok").toBool());
		}
		QVERIFY(recovered.process(request("finish", "agent.finish", {{"status", "complete"}, {"summary", "Preserved the native inventory."}, {"evidence", QJsonArray{"observe"}}})).value("ok").toBool());
		QVERIFY(recovered.process(request("next", "session.chat", {{"text", "Continue editing the existing composition."}})).value("ok").toBool());
		QVERIFY(job(directory).value("id").toString() != id);
		QCOMPARE(retrieve(recovered, id, "observe", digest), original);
		QCOMPARE(Engine::getSong()->tracks().at(0)->name(), nativeName());
		QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
	}

	void invalidArchivesRefuseStartupBeforeNativeReplacement_data()
	{
		QTest::addColumn<int>("location"); QTest::addColumn<bool>("missing");
		QTest::newRow("live-missing") << 0 << true; QTest::newRow("live-altered") << 0 << false;
		QTest::newRow("context-missing") << 1 << true; QTest::newRow("context-altered") << 1 << false;
		QTest::newRow("old-job-missing") << 2 << true; QTest::newRow("old-job-altered") << 2 << false;
	}
	void invalidArchivesRefuseStartupBeforeNativeReplacement()
	{
		QFETCH(int, location); QFETCH(bool, missing); QTemporaryDir directory; QString error, path;
		{
			StudioRuntime studio(directory.path(), [](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision("chat", {call("observe", "project_inspect")}); });
			QVERIFY2(start(studio, "chat", error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
			path = directory.filePath(tool(directory, "observe").value("archive").toObject().value("path").toString());
			if (location) { QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "Retain the exact inventory."}})).value("ok").toBool()); }
			if (location == 2)
			{
				QVERIFY(studio.process(request("finish", "agent.finish", {{"status", "complete"}, {"summary", "Inventory preserved."}, {"evidence", QJsonArray{"observe"}}})).value("ok").toBool());
				QVERIFY(studio.process(request("next", "session.chat", {{"text", "Continue."}})).value("ok").toBool());
			}
		}
		if (missing) { QVERIFY(QFile::remove(path)); } else { QVERIFY(write(path, "{}")); }
		const auto before = read(directory.filePath("session.json")); Engine::getSong()->setTempo(77);
		StudioRuntime failed(directory.path()); QVERIFY(!failed.open(error)); QVERIFY(!error.isEmpty());
		QCOMPARE(Engine::getSong()->getTempo(), 77); QCOMPARE(read(directory.filePath("session.json")), before);
	}

	void failedResultPublicationRollsBackNativeLoad_data() { protocols(); }
	void failedResultPublicationRollsBackNativeLoad()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; const auto project = directory.filePath("large.mmp");
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("load", "project_load", {{"path", project}})}); });
			QVERIFY(studio.open(error)); QVERIFY(studio.process(request("track", "track.add", {{"type", "sample"}, {"name", nativeName()}})).value("ok").toBool());
			QVERIFY(studio.process(request("save", "project.save", {{"path", project}})).value("ok").toBool());
			QVERIFY(studio.process(request("empty", "project.new", {{"empty", true}})).value("ok").toBool());
			QVERIFY(studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Load the saved native composition."}})).value("ok").toBool());
			studio.advanceAgent(); const auto before = read(directory.filePath("session.json"));
			QVERIFY(write(directory.filePath("tool-results"), "not a directory")); const auto result = studio.advanceAgent();
			QVERIFY(!result.value("ok").toBool()); QVERIFY(studio.stopping()); QCOMPARE(read(directory.filePath("session.json")), before);
			QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(0)); QCOMPARE(job(directory).value("pending").toArray().size(), 1);
			QCOMPARE(job(directory).value("operations").toInt(), 0); QVERIFY(QFile::remove(directory.filePath("tool-results")));
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		QVERIFY(job(directory).value("pending").toArray().isEmpty()); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(0));
		QVERIFY(!tool(directory, "load").value("response").toObject().value("ok").toBool());
	}

	void changedPayloadRejectsNewReadsAndEvidence_data()
	{ QTest::addColumn<bool>("compact"); QTest::newRow("current") << false; QTest::newRow("context") << true; }
	void changedPayloadRejectsNewReadsAndEvidence()
	{
		QFETCH(bool, compact); QTemporaryDir directory; QString error;
		StudioRuntime studio(directory.path(), [](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision("chat", {call("observe", "project_inspect")}); });
		QVERIFY2(start(studio, "chat", error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
		const auto id = job(directory).value("id").toString(); const auto archive = tool(directory, "observe").value("archive").toObject();
		const auto query = request("read-before", "agent.history.read", {{"job", id}, {"call", "observe"}, {"limit", 8192}});
		const auto accepted = studio.process(query); QVERIFY(accepted.value("ok").toBool());
		if (compact) { QVERIFY(studio.process(request("compact", "agent.context.compact", {{"summary", "The inventory is retained."}})).value("ok").toBool()); }
		QVERIFY(write(directory.filePath(archive.value("path").toString()), "{}"));
		const auto replay = studio.process(query); QVERIFY(replay.value("replayed").toBool()); QCOMPARE(replay.value("result"), accepted.value("result"));
		QVERIFY(!studio.process(request("read-after", "agent.history.read", query.value("args").toObject())).value("ok").toBool());
		QVERIFY(!studio.process(request("finish", "agent.finish", {{"status", "complete"}, {"summary", "Must not accept changed evidence."}, {"evidence", QJsonArray{"observe"}}})).value("ok").toBool());
		QVERIFY(StudioAgent::active(job(directory))); QVERIFY(!studio.stopping());
	}

	void accumulatedModerateResultsKeepLatestOutputUsable_data() { protocols(); }
	void accumulatedModerateResultsKeepLatestOutputUsable()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			QJsonArray calls; for (int index = 0; index < 64; ++index) { calls.append(call("scan-" + QString::number(index), "project_inspect")); }
			return decision(protocol, calls);
		});
		QVERIFY2(start(studio, protocol, error, 180000), qPrintable(error)); studio.advanceAgent();
		for (int index = 0; index < 64; ++index)
		{
			QCOMPARE(studio.advanceAgent().value("event").toString(), QString("agent")); QVERIFY(!studio.stopping());
			const auto latest = tool(directory, "scan-" + QString::number(index)); QVERIFY(!latest.contains("archive"));
			QCOMPARE(latest.value("response").toObject().value("result").toObject().value("tracks").toArray().at(0).toObject().value("name").toString(), nativeName(180000));
			QVERIFY(read(directory.filePath("session.json")).size() < 6 * 1024 * 1024);
		}
		int stored = 0;
		for (const auto& record : job(directory).value("records").toArray()) { if (record.toObject().contains("archive")) { ++stored; } }
		QVERIFY(stored > 32); QVERIFY(tool(directory, "scan-0").contains("archive")); QCOMPARE(job(directory).value("operations").toInt(), 64);
	}

	void invalidStoredReferenceIsRejected_data()
	{
		QTest::addColumn<int>("fault"); QTest::newRow("outside-path") << 0; QTest::newRow("missing-digest") << 1;
		QTest::newRow("zero-bytes") << 2; QTest::newRow("oversized") << 3; QTest::newRow("wrong-call") << 4;
		QTest::newRow("wrong-job") << 5; QTest::newRow("invented-result") << 6; QTest::newRow("unbounded-preview") << 7;
	}
	void invalidStoredReferenceIsRejected()
	{
		QFETCH(int, fault); auto nativeJob = StudioAgent::job("Keep the original task", {}, {}, 0);
		const auto nativeCall = call("observe", "project_inspect"); const QJsonObject response{{"ok", true}, {"id", "receipt"}, {"revision", 0}};
		QJsonObject archive{{"path", "tool-results/00000000-0000-0000-0000-000000000001.json"}, {"sha256", QString(64, 'a')}, {"bytes", 4096}};
		auto projection = StudioAgent::storedToolResponse(nativeCall, response, archive, nativeJob.value("id").toString());
		QJsonObject record{{"kind", "tool"}, {"call", nativeCall}, {"response", projection}, {"archive", archive}}; QVERIFY(StudioAgent::validToolRecord(record));
		if (fault == 0) { archive.insert("path", "../outside.json"); }
		if (fault == 1) { archive.remove("sha256"); }
		if (fault == 2) { archive.insert("bytes", 0); }
		if (fault == 3) { archive.insert("bytes", 16 * 1024 * 1024 + 1); }
		if (fault == 4 || fault == 5)
		{
			auto stored = projection.value("storedResult").toObject(); stored.insert(fault == 4 ? "call" : "job", "wrong"); projection.insert("storedResult", stored);
		}
		if (fault == 6) { projection.insert("result", QJsonObject{{"invented", true}}); }
		if (fault == 7) { projection.insert("errorPreview", QString(1025, 'x')); }
		record.insert("archive", archive); record.insert("response", projection); QVERIFY(!StudioAgent::validToolRecord(record));
		nativeJob.insert("records", QJsonArray{record}); auto state = StudioAgent::initialState(); state.insert("job", nativeJob); QVERIFY(!StudioAgent::validState(state));
	}

	void providerRetrievesStoredBytesBeforeNativeRevision_data() { protocols(); }
	void providerRetrievesStoredBytesBeforeNativeRevision()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; int round = 0, offset = 0, targetTempo = -1;
		QByteArray assembled; QJsonObject reference; bool continued = false;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			if (++round == 1) { return decision(protocol, {call("observe", "project_inspect")}); }
			const auto input = body.value(protocol == "chat" ? "messages" : "input").toArray(); QJsonObject output; QString id;
			for (int index = input.size() - 1; index >= 0; --index)
			{
				const auto item = input.at(index).toObject();
				if ((protocol == "chat" && item.value("role").toString() == "tool")
					|| (protocol == "responses" && item.value("type").toString() == "function_call_output"))
				{
					id = item.value(protocol == "chat" ? "tool_call_id" : "call_id").toString();
					output = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); break;
				}
			}
			if (!output.value("ok").toBool()) { throw std::runtime_error("Scripted provider did not receive a successful native output"); }
			if (id == "apply-tempo")
			{
				continued = output.value("result").toObject().value("tempo").toInt() == targetTempo;
				return decision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Retrieved native state and revised its tempo."}, {"evidence", QJsonArray{"observe", "apply-tempo"}}})});
			}
			if (id == "observe") { reference = output.value("storedResult").toObject(); }
			else
			{
				const auto page = output.value("result").toObject();
				if (page.value("offset").toInt() != offset || page.value("sha256") != reference.value("sha256")) { throw std::runtime_error("Scripted history continuation differs"); }
				assembled += page.value("text").toString().toUtf8(); offset = page.value("nextOffset").toInt();
				if (page.value("complete").toBool())
				{
					const auto record = QJsonDocument::fromJson(assembled).object();
					if (hash(assembled) != reference.value("sha256").toString() || record.value("call").toObject().value("id").toString() != "observe")
					{ throw std::runtime_error("Scripted provider did not recover the exact native record"); }
					targetTempo = record.value("response").toObject().value("result").toObject().value("tempo").toInt() - 7;
					return decision(protocol, {call("apply-tempo", "song_set", {{"tempo", targetTempo}})});
				}
			}
			return decision(protocol, {call("read-" + QString::number(offset), "agent_history_read", {{"job", reference.value("job")},
				{"call", reference.value("call")}, {"sha256", reference.value("sha256")}, {"offset", offset}, {"limit", 65536}})});
		});
		QVERIFY2(start(studio, protocol, error), qPrintable(error)); const auto oldTempo = Engine::getSong()->getTempo();
		for (int step = 0; step < 40 && StudioAgent::active(job(directory)); ++step) { studio.advanceAgent(); }
		QVERIFY2(continued, qPrintable(job(directory).value("lastError").toString())); QVERIFY(round >= 7);
		QCOMPARE(targetTempo, oldTempo - 7); QCOMPARE(Engine::getSong()->getTempo(), targetTempo);
		QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(!studio.stopping());
	}

	void legacyInlineResultsMigrateBeforeContinuedWork_data() { protocols(); }
	void legacyInlineResultsMigrateBeforeContinuedWork()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error, relative; QJsonObject original;
		QDateTime modified;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("observe", "project_inspect")}); });
			QVERIFY2(start(studio, protocol, error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
			const auto record = tool(directory, "observe"); relative = record.value("archive").toObject().value("path").toString();
			original = QJsonDocument::fromJson(read(directory.filePath(relative))).object();
		}
		QFile immutable(directory.filePath(relative)); QVERIFY(immutable.open(QIODevice::ReadWrite));
		QVERIFY(immutable.setFileTime(QDateTime::fromMSecsSinceEpoch(946684800000ll), QFileDevice::FileModificationTime)); immutable.close();
		modified = QFileInfo(directory.filePath(relative)).lastModified();
		const int fileCount = QDir(directory.filePath("tool-results")).entryList({"*.json"}, QDir::Files).size();
		auto state = manifest(directory), agent = state.value("agent").toObject(), nativeJob = agent.value("job").toObject();
		auto records = nativeJob.value("records").toArray(); records[records.size() - 1] = original;
		auto input = nativeJob.value("input").toArray(); auto last = input.last().toObject();
		// Older JSON formatting may differ while the exact structured result agrees.
		last.insert(protocol == "chat" ? "content" : "output", QString::fromUtf8(QJsonDocument(original.value("response").toObject()).toJson(QJsonDocument::Indented)));
		input[input.size() - 1] = last;
		nativeJob.insert("records", records); nativeJob.insert("input", input); agent.insert("job", nativeJob); state.insert("agent", agent);
		QVERIFY(StudioAgent::validState(agent)); QVERIFY(write(directory.filePath("session.json"), encoded(state)));
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); const auto migrated = tool(directory, "observe");
		QVERIFY(migrated.contains("archive")); QCOMPARE(migrated.value("call"), original.value("call"));
		QCOMPARE(migrated.value("archive").toObject().value("path").toString(), relative);
		QCOMPARE(QFileInfo(directory.filePath(relative)).lastModified(), modified);
		QCOMPARE(QDir(directory.filePath("tool-results")).entryList({"*.json"}, QDir::Files).size(), fileCount);
		QCOMPARE(retrieve(recovered, job(directory).value("id").toString(), "observe", migrated.value("archive").toObject().value("sha256").toString()), encoded(original));
		QCOMPARE(Engine::getSong()->tracks().at(0)->name(), nativeName());
	}

	void inconsistentInlineBatchesFailBeforeAnyArchivePublication_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<int>("fault");
		QTest::newRow("chat-late-value-mismatch") << QString("chat") << 0;
		QTest::newRow("responses-late-value-mismatch") << QString("responses") << 0;
		QTest::newRow("chat-late-invalid-json") << QString("chat") << 1;
		QTest::newRow("responses-late-invalid-json") << QString("responses") << 1;
		QTest::newRow("chat-conflicting-duplicate-output") << QString("chat") << 2;
		QTest::newRow("responses-conflicting-duplicate-output") << QString("responses") << 2;
	}
	void inconsistentInlineBatchesFailBeforeAnyArchivePublication()
	{
		QFETCH(QString, protocol); QFETCH(int, fault); QTemporaryDir directory; QString error;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&)
			{ return decision(protocol, {call("first", "project_inspect"), call("second", "project_inspect")}); });
			QVERIFY2(start(studio, protocol, error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent(); studio.advanceAgent();
		}
		auto state = manifest(directory), agent = state.value("agent").toObject(), nativeJob = agent.value("job").toObject();
		auto records = nativeJob.value("records").toArray(), input = nativeJob.value("input").toArray(); QStringList paths;
		for (int index = 0; index < records.size(); ++index)
		{
			const auto record = records.at(index).toObject();
			if (record.value("kind").toString() != "tool" || !record.contains("archive")) { continue; }
			const auto relative = record.value("archive").toObject().value("path").toString(); paths.append(relative);
			const auto original = QJsonDocument::fromJson(read(directory.filePath(relative))).object(); QVERIFY(!original.isEmpty()); records[index] = original;
			const auto id = original.value("call").toObject().value("id");
			for (int output = 0; output < input.size(); ++output)
			{
				auto item = input.at(output).toObject(); const auto key = protocol == "chat" ? "tool_call_id" : "call_id";
				if ((protocol == "chat" && item.value("role").toString() != "tool")
					|| (protocol == "responses" && item.value("type").toString() != "function_call_output")) { continue; }
				if (item.value(key) != id) { continue; }
				item.insert(protocol == "chat" ? "content" : "output", QString::fromUtf8(encoded(original.value("response").toObject()))); input[output] = item;
			}
		}
		QCOMPARE(paths.size(), 2);
		for (int output = 0; output < input.size(); ++output)
		{
			auto item = input.at(output).toObject(); const auto key = protocol == "chat" ? "tool_call_id" : "call_id";
			if ((protocol == "chat" && item.value("role").toString() != "tool")
				|| (protocol == "responses" && item.value("type").toString() != "function_call_output")) { continue; }
			if (item.value(key).toString() != "second") { continue; }
			const auto field = protocol == "chat" ? "content" : "output";
			if (fault == 1) { item.insert(field, "not JSON"); }
			else
			{
				auto response = QJsonDocument::fromJson(item.value(field).toString().toUtf8()).object(); auto result = response.value("result").toObject();
				result.insert("tempo", 31); response.insert("result", result); item.insert(field, QString::fromUtf8(encoded(response)));
			}
			if (fault == 2) { input.append(item); } else { input[output] = item; }
			break;
		}
		nativeJob.insert("records", records); nativeJob.insert("input", input); agent.insert("job", nativeJob); state.insert("agent", agent);
		QVERIFY(StudioAgent::validState(agent)); const auto before = encoded(state); QVERIFY(write(directory.filePath("session.json"), before));
		// These are fixture-owned payloads whose references were replaced above
		// by full inline records. Production code never scans/deletes this directory.
		for (const auto& relative : paths) { QVERIFY(QFile::remove(directory.filePath(relative))); }
		QCOMPARE(QDir(directory.filePath("tool-results")).entryList({"*.json"}, QDir::Files).size(), 0);
		Engine::getSong()->clearProject(); Track::create(Track::Type::Sample, Engine::getSong())->setName("Current local composition");
		Engine::getSong()->setTempo(103); StudioRuntime recovered(directory.path()); QVERIFY(!recovered.open(error));
		QVERIFY(error.contains("differs from its durable record")); QCOMPARE(read(directory.filePath("session.json")), before);
		QCOMPARE(QDir(directory.filePath("tool-results")).entryList({"*.json"}, QDir::Files).size(), 0);
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks()[0]->name(), QString("Current local composition"));
		QCOMPARE(Engine::getSong()->getTempo(), 103);
	}

	void conflictingContentPathsArePreservedAndRefused_data()
	{ protocols(); }
	void conflictingContentPathsArePreservedAndRefused()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error, relative; QJsonObject original;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&)
			{ return decision(protocol, {call("observe", "project_inspect")}); });
			QVERIFY2(start(studio, protocol, error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
			const auto record = tool(directory, "observe"); relative = record.value("archive").toObject().value("path").toString();
			original = QJsonDocument::fromJson(read(directory.filePath(relative))).object();
		}
		auto state = manifest(directory), agent = state.value("agent").toObject(), nativeJob = agent.value("job").toObject();
		auto records = nativeJob.value("records").toArray(), input = nativeJob.value("input").toArray(); records[records.size() - 1] = original;
		auto item = input.last().toObject(); item.insert(protocol == "chat" ? "content" : "output", QString::fromUtf8(encoded(original.value("response").toObject()))); input[input.size() - 1] = item;
		nativeJob.insert("records", records); nativeJob.insert("input", input); agent.insert("job", nativeJob); state.insert("agent", agent);
		QVERIFY(StudioAgent::validState(agent)); const auto before = encoded(state); QVERIFY(write(directory.filePath("session.json"), before));
		QVERIFY(write(directory.filePath(relative), "Conflicting archive fixture"));
		Engine::getSong()->clearProject(); Track::create(Track::Type::Sample, Engine::getSong())->setName("Current local composition");
		StudioRuntime recovered(directory.path()); QVERIFY(!recovered.open(error)); QVERIFY(error.contains("cannot be reused safely"));
		QCOMPARE(read(directory.filePath(relative)), QByteArray("Conflicting archive fixture")); QCOMPARE(read(directory.filePath("session.json")), before);
		QCOMPARE(QDir(directory.filePath("tool-results")).entryList({"*.json"}, QDir::Files).size(), 1);
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks()[0]->name(), QString("Current local composition"));
	}

	void rehashedPayloadStillMustMatchItsCommittedCallAndOutcome_data()
	{
		QTest::addColumn<int>("fault"); QTest::newRow("different-arguments") << 0; QTest::newRow("different-outcome") << 1;
		QTest::newRow("nested-reference") << 2;
	}
	void rehashedPayloadStillMustMatchItsCommittedCallAndOutcome()
	{
		QFETCH(int, fault); QTemporaryDir directory; QString error;
		{
			StudioRuntime studio(directory.path(), [](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision("chat", {call("observe", "project_inspect")}); });
			QVERIFY2(start(studio, "chat", error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
		}
		auto state = manifest(directory), agent = state.value("agent").toObject(), nativeJob = agent.value("job").toObject();
		auto records = nativeJob.value("records").toArray(); auto record = records.last().toObject(), archive = record.value("archive").toObject();
		const auto path = directory.filePath(archive.value("path").toString()); auto original = QJsonDocument::fromJson(read(path)).object();
		if (fault == 0) { auto called = original.value("call").toObject(); called.insert("args", QJsonObject{{"changed", true}}); original.insert("call", called); }
		if (fault == 1) { auto response = original.value("response").toObject(); response.insert("ok", false); original.insert("response", response); }
		if (fault == 2) { original = record; }
		const auto bytes = encoded(original); QVERIFY(write(path, bytes)); archive.insert("bytes", bytes.size()); archive.insert("sha256", hash(bytes));
		auto projection = record.value("response").toObject(), stored = projection.value("storedResult").toObject();
		stored.insert("bytes", bytes.size()); stored.insert("sha256", hash(bytes)); projection.insert("storedResult", stored);
		record.insert("archive", archive); record.insert("response", projection); records[records.size() - 1] = record; nativeJob.insert("records", records);
		agent.insert("job", nativeJob); state.insert("agent", agent); QVERIFY(StudioAgent::validState(agent)); QVERIFY(write(directory.filePath("session.json"), encoded(state)));
		const auto before = read(directory.filePath("session.json")); Engine::getSong()->setTempo(77);
		StudioRuntime failed(directory.path()); QVERIFY(!failed.open(error)); QCOMPARE(Engine::getSong()->getTempo(), 77);
		QCOMPARE(read(directory.filePath("session.json")), before);
	}

	void restartKeepsStoredResultsAndInvalidatesDependentCalls_data() { protocols(); }
	void restartKeepsStoredResultsAndInvalidatesDependentCalls()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; const auto originalTempo = Engine::getSong()->getTempo();
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				return decision(protocol, {call("observe", "project_inspect"), call("dependent", "song_set", {{"tempo", 77}})});
			});
			QVERIFY2(start(studio, protocol, error), qPrintable(error)); studio.advanceAgent(); studio.advanceAgent();
			QVERIFY(tool(directory, "observe").contains("archive")); QCOMPARE(job(directory).value("pending").toArray().size(), 1);
			QCOMPARE(Engine::getSong()->getTempo(), originalTempo);
		}
		StudioRuntime recovered(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision(protocol, {call("observe", "song_set", {{"tempo", 79}})}); // Reusing even an archived output ID is refused.
		});
		QVERIFY2(recovered.open(error), qPrintable(error)); QVERIFY(job(directory).value("pending").toArray().isEmpty());
		QVERIFY(!tool(directory, "dependent").value("response").toObject().value("ok").toBool());
		const auto archive = tool(directory, "observe").value("archive").toObject();
		QCOMPARE(retrieve(recovered, job(directory).value("id").toString(), "observe", archive.value("sha256").toString()), read(directory.filePath(archive.value("path").toString())));
		recovered.advanceAgent(); QCOMPARE(job(directory).value("status").toString(), QString("paused"));
		QVERIFY(job(directory).value("lastError").toString().contains("reused")); QCOMPARE(Engine::getSong()->getTempo(), originalTempo);
		QVERIFY(!recovered.stopping());
	}
};

QTEST_GUILESS_MAIN(StudioToolResultTest)
#include "StudioToolResultTest.moc"
