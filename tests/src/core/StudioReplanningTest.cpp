/* Durable dispositions for abandoned native decisions. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <memory>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "Track.h"

using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject call(const QString& id, const QString& name, const QJsonObject& args = {}) { return {{"id", id}, {"name", name}, {"args", args}}; }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QString hash(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
QJsonObject manifest(const QTemporaryDir& dir) { return QJsonDocument::fromJson(read(dir.filePath("session.json"))).object(); }
QJsonObject job(const QTemporaryDir& dir) { return manifest(dir).value("agent").toObject().value("job").toObject(); }
QStringList names() { QStringList result; for (auto track : Engine::getSong()->tracks()) { result.append(track->name()); } return result; }
QString other(const QString& protocol) { return protocol == "chat" ? "responses" : "chat"; }
QJsonObject provider(const QString& protocol)
{ return {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-replanning-fixture"}}}}; }
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output;
	for (const auto& value : calls)
	{
		const auto item = value.toObject(); const auto args = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact));
		if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", args}}); }
		else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", args}}}}); }
	}
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
QJsonArray proposed()
{
	return {call("kept", "song_set", {{"tempo", 91}}), call("stale", "track_edit", {{"track", 0}, {"name", "Wrong"}}),
		call("memory", "session_memory_set", {{"memory", QJsonObject{{"mustNotPublish", "unexecuted musical decision"}}}}),
		call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Unobserved"}, {"evidence", QJsonArray{"kept"}}})};
}
QJsonArray continued()
{
	return {call("fresh-read", "project_inspect", {{"section", "summary"}}), call("fresh-edit", "song_set", {{"tempo", 123}}),
		call("fresh-finish", "agent_finish", {{"status", "complete"}, {"summary", "Continued the native composition"}, {"evidence", QJsonArray{"fresh-read", "fresh-edit"}}})};
}
bool setup(StudioRuntime& studio, const QString& protocol, QString& error)
{
	if (!studio.open(error)) { return false; }
	for (const auto name : {"Alpha", "Beta"})
	{ if (!studio.process(request(name, "track.add", {{"type", "instrument"}, {"name", name}})).value("ok").toBool()) { return false; } }
	return studio.process(request("tempo", "song.set", {{"tempo", 101}})).value("ok").toBool()
		&& studio.process(request("memory", "session.memory.set", {{"memory", QJsonObject{{"preference", "Preserve the arrangement"}}}})).value("ok").toBool()
		&& studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()
		&& studio.process(request("chat", "session.chat", {{"text", "Continue this editable composition"}})).value("ok").toBool()
		&& studio.process(request("plan", "agent.plan.set", {{"goal", "Preserve the user's composition"}, {"steps", QJsonArray{"Observe", "Revise"}}, {"decisions", QJsonArray{"Keep existing tracks"}}})).value("ok").toBool();
}
QJsonObject readRecord(StudioRuntime& studio, const QString& jobId, const QString& callId, const QString& prefix)
{
	const auto response = studio.process(request(prefix + callId, "agent.history.read", {{"job", jobId}, {"call", callId}, {"limit", 65536}}));
	if (!response.value("ok").toBool() || !response.value("result").toObject().value("complete").toBool()) { return {}; }
	return QJsonDocument::fromJson(response.value("result").toObject().value("text").toString().toUtf8()).object();
}
bool paired(const QJsonArray& input, const QString& protocol, int executed)
{
	QJsonArray outputs;
	for (const auto& value : input)
	{
		const auto item = value.toObject();
		if (item.value("type").toString() == "function_call_output" || item.value("role").toString() == "tool") { outputs.append(item); }
	}
	if (outputs.size() != proposed().size()) { return false; }
	for (int i = 0; i < outputs.size(); ++i)
	{
		const auto output = outputs.at(i).toObject();
		if (output.value(protocol == "chat" ? "tool_call_id" : "call_id") != proposed().at(i).toObject().value("id")) { return false; }
		const auto result = QJsonDocument::fromJson(output.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object();
		if (result.value("ok").toBool() != (i < executed) || (i >= executed && !result.value("error").toString().contains("queued call was not executed"))) { return false; }
	}
	return true;
}
void protocols() { QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
}

class StudioReplanningTest : public QObject
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

	void replanningClosesTheOldProtocolAndPreservesNativeContinuation_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("reason"); QTest::addColumn<int>("executed"); QTest::addColumn<bool>("restart");
		for (const auto protocol : {QString("chat"), QString("responses")})
			for (const auto reason : {QString("revision"), QString("provider"), QString("both")})
				for (int executed : {0, 1}) for (bool restart : {false, true})
				{ QTest::newRow(qPrintable(protocol + reason + QString::number(executed) + QString::number(restart))) << protocol << reason << executed << restart; }
	}
	void replanningClosesTheOldProtocolAndPreservesNativeContinuation()
	{
		QFETCH(QString, protocol); QFETCH(QString, reason); QFETCH(int, executed); QFETCH(bool, restart);
		QTemporaryDir directory; QString error; int rounds = 0; bool fresh = false;
		const auto transport = [&](const QJsonObject& config, const QJsonObject& body, const StudioInference::Cancelled&) {
			if (++rounds == 1) { return decision(protocol, proposed()); }
			const auto currentProtocol = config.value("protocol").toString();
			for (const auto& value : body.value(currentProtocol == "chat" ? "messages" : "input").toArray())
			{
				const auto text = value.toObject().value("content").toString();
				if (text.startsWith("Native runtime observation (data):\n"))
				{
					const auto observation = QJsonDocument::fromJson(text.mid(text.indexOf('\n') + 1).toUtf8()).object();
					fresh = observation.value("projectSummary").toObject() == StudioCapabilities::inspectSummary()
						&& observation.value("revision") == manifest(directory).value("revision");
				}
			}
			return decision(currentProtocol, continued());
		};
		auto studio = std::make_unique<StudioRuntime>(directory.path(), transport); QVERIFY2(setup(*studio, protocol, error), qPrintable(error));
		studio->advanceAgent(); if (executed) { studio->advanceAgent(); }
		const auto before = job(directory); const auto memory = manifest(directory).value("memory"); const auto id = before.value("id").toString();
		QVERIFY(studio->process(request("cancel", "agent.cancel")).value("ok").toBool());
		if (reason != "provider") { QVERIFY(studio->process(request("outside", "track.remove", {{"track", 0}})).value("ok").toBool()); }
		if (reason != "revision") { QVERIFY(studio->process(request("switch", "session.provider.set", provider(other(protocol)))).value("ok").toBool()); }
		const auto revision = manifest(directory).value("revision"), history = manifest(directory).value("history");
		const auto resume = request("resume", "agent.resume"); QVERIFY(studio->process(resume).value("ok").toBool());
		const auto after = job(directory); QCOMPARE(after.value("id"), before.value("id")); QCOMPARE(after.value("intent"), before.value("intent")); QCOMPARE(after.value("plan"), before.value("plan"));
		QCOMPARE(after.value("operations").toInt(), executed); QCOMPARE(after.value("rounds"), before.value("rounds")); QCOMPARE(after.value("elapsedMs"), before.value("elapsedMs"));
		QCOMPARE(after.value("expectedRevision"), revision); QCOMPARE(manifest(directory).value("history"), history); QCOMPARE(manifest(directory).value("memory"), memory);
		QVERIFY(after.value("pending").toArray().isEmpty()); QVERIFY(after.value("input").toArray().isEmpty()); QCOMPARE(after.value("pastInputs").toArray().size(), 1);
		const auto reference = after.value("pastInputs").toArray().at(0).toObject(); const auto bytes = read(directory.filePath(reference.value("path").toString()));
		QCOMPARE(hash(bytes), reference.value("sha256").toString()); const auto frozen = QJsonDocument::fromJson(bytes).object();
		QCOMPARE(frozen.value("protocol").toString(), protocol); QVERIFY(paired(frozen.value("input").toArray(), protocol, executed)); QCOMPARE(frozen.value("records").toArray().size(), 4);
		const auto acknowledged = manifest(directory); QVERIFY(studio->process(resume).value("replayed").toBool()); QCOMPARE(manifest(directory), acknowledged);
		if (restart) { studio.reset(); studio = std::make_unique<StudioRuntime>(directory.path(), transport); QVERIFY2(studio->open(error), qPrintable(error)); }
		for (int i = 0; i < proposed().size(); ++i)
		{
			const auto expected = proposed().at(i).toObject(); const auto record = readRecord(*studio, id, expected.value("id").toString(), "read-");
			QCOMPARE(record.value("call").toObject(), expected); QCOMPARE(record.value("response").toObject().value("ok").toBool(), i < executed);
		}
		QVERIFY(!studio->process(request("invalid-evidence", "agent.finish", {{"status", "complete"}, {"summary", "Not executed"}, {"evidence", QJsonArray{"stale"}}})).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getTempo(), executed ? 91 : 101); QVERIFY(!names().contains("Wrong"));
		for (int step = 0; step < 4; ++step) { studio->advanceAgent(); }
		QVERIFY(fresh); QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(job(directory).value("operations").toInt(), executed + 3);
		QCOMPARE(Engine::getSong()->getTempo(), 123); QCOMPARE(manifest(directory).value("memory"), memory); QVERIFY(!studio->stopping());
		QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
	}

	void replannedCallIdsCannotBeReused_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("restart");
		for (const auto protocol : {QString("chat"), QString("responses")}) for (bool restart : {false, true})
		{ QTest::newRow(qPrintable(protocol + QString::number(restart))) << protocol << restart; }
	}
	void replannedCallIdsCannotBeReused()
	{
		QFETCH(QString, protocol); QFETCH(bool, restart); QTemporaryDir directory; QString error; int round = 0;
		const auto transport = [&](const QJsonObject& config, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision(config.value("protocol").toString(), ++round == 1 ? proposed() : QJsonArray{call("stale", "song_set", {{"tempo", 211}})});
		};
		auto studio = std::make_unique<StudioRuntime>(directory.path(), transport); QVERIFY(setup(*studio, protocol, error)); studio->advanceAgent();
		QVERIFY(studio->process(request("switch", "session.provider.set", provider(other(protocol)))).value("ok").toBool());
		QVERIFY(studio->process(request("resume", "agent.resume")).value("ok").toBool());
		if (restart) { studio.reset(); studio = std::make_unique<StudioRuntime>(directory.path(), transport); QVERIFY(studio->open(error)); }
		const auto records = job(directory).value("records"); studio->advanceAgent();
		QCOMPARE(job(directory).value("status").toString(), QString("paused")); QVERIFY(job(directory).value("lastError").toString().contains("reused"));
		QCOMPARE(job(directory).value("records"), records); QCOMPARE(job(directory).value("operations").toInt(), 0); QCOMPARE(Engine::getSong()->getTempo(), 101);
	}

	void newIntentArchivesExplicitSkippedOutcomes_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("paused"); QTest::addColumn<int>("executed"); QTest::addColumn<bool>("restart");
		for (const auto protocol : {QString("chat"), QString("responses")}) for (bool paused : {false, true})
			for (int executed : {0, 1}) for (bool restart : {false, true})
			{ QTest::newRow(qPrintable(protocol + QString::number(paused) + QString::number(executed) + QString::number(restart))) << protocol << paused << executed << restart; }
	}
	void newIntentArchivesExplicitSkippedOutcomes()
	{
		QFETCH(QString, protocol); QFETCH(bool, paused); QFETCH(int, executed); QFETCH(bool, restart); QTemporaryDir directory; QString error; int round = 0;
		const auto transport = [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, ++round == 1 ? proposed() : continued()); };
		auto studio = std::make_unique<StudioRuntime>(directory.path(), transport); QVERIFY(setup(*studio, protocol, error)); studio->advanceAgent(); if (executed) { studio->advanceAgent(); }
		if (paused) { QVERIFY(studio->process(request("outside", "track.remove", {{"track", 1}})).value("ok").toBool()); studio->advanceAgent(); }
		else { QVERIFY(studio->process(request("cancel", "agent.cancel")).value("ok").toBool()); }
		const auto before = job(directory); const auto id = before.value("id").toString(); const auto memory = manifest(directory).value("memory");
		QCOMPARE(before.value("status").toString(), paused ? QString("paused") : QString("cancelled"));
		const auto next = request("new-intent", "session.chat", {{"text", "Keep the project and start a new revision task"}});
		QVERIFY(studio->process(next).value("ok").toBool()); const auto state = manifest(directory);
		const auto reference = state.value("agent").toObject().value("archives").toArray().last().toObject(); const auto bytes = read(directory.filePath(reference.value("path").toString()));
		QCOMPARE(hash(bytes), reference.value("sha256").toString()); const auto archived = QJsonDocument::fromJson(bytes).object();
		QCOMPARE(archived.value("id"), before.value("id")); QCOMPARE(archived.value("status"), before.value("status")); QCOMPARE(archived.value("plan"), before.value("plan"));
		QCOMPARE(archived.value("operations").toInt(), executed); QVERIFY(archived.value("pending").toArray().isEmpty()); QVERIFY(paired(archived.value("input").toArray(), protocol, executed));
		QVERIFY(job(directory).value("id") != before.value("id")); QCOMPARE(job(directory).value("operations").toInt(), 0); QVERIFY(job(directory).value("records").toArray().isEmpty());
		QVERIFY(studio->process(next).value("replayed").toBool()); QCOMPARE(manifest(directory), state);
		if (restart) { studio.reset(); studio = std::make_unique<StudioRuntime>(directory.path(), transport); QVERIFY2(studio->open(error), qPrintable(error)); }
		for (int i = 0; i < proposed().size(); ++i)
		{
			const auto expected = proposed().at(i).toObject(); const auto record = readRecord(*studio, id, expected.value("id").toString(), "old-");
			QCOMPARE(record.value("call").toObject(), expected); QCOMPARE(record.value("response").toObject().value("ok").toBool(), i < executed);
		}
		QVERIFY(!names().contains("Wrong")); QCOMPARE(Engine::getSong()->getTempo(), executed ? 91 : 101); QCOMPARE(manifest(directory).value("memory"), memory);
		for (int step = 0; step < 4; ++step) { studio->advanceAgent(); }
		QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(Engine::getSong()->getTempo(), 123);
		QCOMPARE(read(directory.filePath(reference.value("path").toString())), bytes); QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
	}

	void failedPublicationPreservesUnfinishedDecisions_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("newIntent"); QTest::addColumn<bool>("manifestFailure");
		for (const auto protocol : {QString("chat"), QString("responses")}) for (bool newIntent : {false, true}) for (bool manifestFailure : {false, true})
		{ QTest::newRow(qPrintable(protocol + QString::number(newIntent) + QString::number(manifestFailure))) << protocol << newIntent << manifestFailure; }
	}
	void failedPublicationPreservesUnfinishedDecisions()
	{
		QFETCH(QString, protocol); QFETCH(bool, newIntent); QFETCH(bool, manifestFailure); QTemporaryDir directory; QString error; QJsonObject before;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, proposed()); });
			QVERIFY(setup(studio, protocol, error)); studio.advanceAgent(); studio.advanceAgent();
			QVERIFY(studio.process(request("cancel", "agent.cancel")).value("ok").toBool());
			if (!newIntent) { QVERIFY(studio.process(request("switch", "session.provider.set", provider(other(protocol)))).value("ok").toBool()); }
			before = job(directory); const auto path = directory.filePath("session.json"), retained = directory.filePath("acknowledged.json"); const auto acknowledged = read(path);
			const auto obstacle = directory.filePath(newIntent ? "jobs" : "contexts");
			if (manifestFailure) { QVERIFY(QFile::rename(path, retained)); QVERIFY(QDir(directory.path()).mkdir("session.json")); }
			else { QVERIFY(write(obstacle, "block archive directory creation")); }
			const auto cleanup = qScopeGuard([&] {
				if (manifestFailure) { QDir(directory.path()).rmdir("session.json"); QFile::rename(retained, path); }
				else { QFile::remove(obstacle); }
			});
			const auto result = studio.process(newIntent ? request("transition", "session.chat", {{"text", "New task"}}) : request("transition", "agent.resume"));
			QVERIFY(!result.value("ok").toBool()); QCOMPARE(studio.stopping(), manifestFailure); QCOMPARE(Engine::getSong()->getTempo(), 91); QVERIFY(!names().contains("Wrong"));
			if (manifestFailure) { QCOMPARE(read(retained), acknowledged); }
			else { QCOMPARE(job(directory), before); QVERIFY(manifest(directory).value("agent").toObject().value("archives").toArray().isEmpty()); }
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(job(directory).value("pending"), before.value("pending")); QCOMPARE(job(directory).value("records"), before.value("records"));
		QCOMPARE(job(directory).value("operations").toInt(), 1); QCOMPARE(Engine::getSong()->getTempo(), 91);
		QVERIFY(recovered.process(request("retry", "agent.resume")).value("ok").toBool()); QVERIFY(job(directory).value("pending").toArray().isEmpty());
		const auto stale = readRecord(recovered, before.value("id").toString(), "stale", "retry-");
		QVERIFY(!stale.isEmpty()); QVERIFY(!stale.value("response").toObject().value("ok").toBool());
	}

	void unchangedResumeKeepsValidQueuedWork_data() { protocols(); }
	void unchangedResumeKeepsValidQueuedWork()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision(protocol, {call("one", "song_set", {{"tempo", 91}}), call("two", "song_set", {{"tempo", 97}}),
				call("done", "agent_finish", {{"status", "complete"}, {"summary", "Resumed queued work"}, {"evidence", QJsonArray{"one", "two"}}})});
		});
		QVERIFY(setup(studio, protocol, error)); studio.advanceAgent(); studio.advanceAgent(); const auto before = job(directory);
		QVERIFY(studio.process(request("cancel", "agent.cancel")).value("ok").toBool()); QVERIFY(studio.process(request("resume", "agent.resume")).value("ok").toBool());
		QCOMPARE(job(directory).value("pending"), before.value("pending")); QCOMPARE(job(directory).value("input"), before.value("input"));
		QCOMPARE(job(directory).value("records"), before.value("records")); QVERIFY(job(directory).value("pastInputs").toArray().isEmpty());
		studio.advanceAgent(); studio.advanceAgent(); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
		QCOMPARE(job(directory).value("operations").toInt(), 3); QCOMPARE(Engine::getSong()->getTempo(), 97);
	}
};
QTEST_GUILESS_MAIN(StudioReplanningTest)
#include "StudioReplanningTest.moc"
