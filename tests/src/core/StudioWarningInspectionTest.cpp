/* Paged native diagnostics and live/historical agent observations.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;
namespace
{
QByteArray encoded(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject inspect(QJsonObject args = {}) { args.insert("section", "warnings"); return StudioCapabilities::execute("project.inspect", args); }
QJsonObject summary() { return StudioCapabilities::execute("project.inspect", {{"section", "summary"}}).value("result").toObject(); }
QJsonObject manifest(const QTemporaryDir& directory)
{ QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
QJsonObject decision(const QString& protocol, const QString& id, const QString& name, const QJsonObject& args)
{
	const auto arguments = QString::fromUtf8(encoded(args));
	if (protocol == "responses") { return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{
		{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", arguments}}}}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{
		{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{QJsonObject{{"id", id}, {"type", "function"},
			{"function", QJsonObject{{"name", name}, {"arguments", arguments}}}}}}}}}}}};
}
}

class StudioWarningInspectionTest : public QObject
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
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }

	void nativeCountsAndGenerationDescribeCurrentWarnings()
	{
		auto song = Engine::getSong(); const auto empty = summary(); QVERIFY(!empty.value("nativeWarningsPresent").toBool()); QCOMPARE(empty.value("nativeWarningCount").toInt(), 0);
		song->clearErrors(); QCOMPARE(summary().value("diagnosticGeneration"), empty.value("diagnosticGeneration"));
		song->collectError("Missing sample"); song->collectError("Missing sample"); song->collectError("Unavailable controller");
		const auto current = summary(); QVERIFY(current.value("nativeWarningsPresent").toBool()); QCOMPARE(current.value("nativeWarningCount").toInt(), 2);
		QVERIFY(current.value("diagnosticGeneration") != empty.value("diagnosticGeneration"));
		const auto response = inspect({{"diagnosticGeneration", current.value("diagnosticGeneration")}}); QVERIFY(response.value("ok").toBool());
		const auto result = response.value("result").toObject(); QCOMPARE(result.value("total").toInt(), 2); QVERIFY(!result.value("hasMore").toBool());
		QHash<QString, int> counts;
		for (const auto& entry : result.value("warnings").toArray())
		{ const auto item = entry.toObject(); counts.insert(item.value("message").toString(), item.value("occurrences").toInt()); QVERIFY(!item.value("occurrencesSaturated").toBool()); }
		QCOMPARE(counts, song->nativeErrors()); song->clearErrors(); const auto cleared = summary();
		QVERIFY(cleared.value("diagnosticGeneration") != current.value("diagnosticGeneration")); QVERIFY(!cleared.value("nativeWarningsPresent").toBool());
		QVERIFY(inspect().value("result").toObject().value("warnings").toArray().isEmpty());
	}

	void escapedPagesAreBoundedReadOnlyAndReplayHistoricalData()
	{
		QTemporaryDir directory; StudioRuntime studio(directory.path()); QString error; QVERIFY(studio.open(error)); auto song = Engine::getSong();
		for (int index = 0; index < 160; ++index) { song->collectError(QString(8192, QChar(1)) + QString::number(index)); }
		const auto original = song->nativeErrors(); const auto before = manifest(directory); const auto generation = summary().value("diagnosticGeneration");
		int offset = 0; bool byteLimited = false; QJsonObject firstRequest, firstResponse; QSet<int> indices;
		while (offset < 160)
		{
			const auto query = request("warnings-" + QString::number(offset), "project.inspect", {{"section", "warnings"}, {"offset", offset},
				{"limit", 128}, {"messageLimit", 8192}, {"diagnosticGeneration", generation}});
			const auto response = studio.process(query); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
			if (!offset) { firstRequest = query; firstResponse = response; }
			const auto page = response.value("result").toObject(); const auto items = page.value("warnings").toArray(); QVERIFY(!items.isEmpty());
			QVERIFY(QJsonDocument(items).toJson(QJsonDocument::Compact).size() <= 512 * 1024); byteLimited = byteLimited || items.size() < 128;
			for (const auto& entry : items)
			{
				const auto item = entry.toObject(); QCOMPARE(item.value("index").toInt(), offset); QVERIFY(!indices.contains(offset)); indices.insert(offset++);
				QCOMPARE(item.value("message").toString(), QString(8192, QChar(1))); QVERIFY(item.value("messagePage").toObject().value("hasMore").toBool());
			}
			QCOMPARE(page.value("nextOffset").toInt(), offset); QCOMPARE(page.value("diagnosticGeneration"), generation);
		}
		QVERIFY(byteLimited); QCOMPARE(indices.size(), 160); QCOMPARE(song->nativeErrors(), original);
		const auto after = manifest(directory); QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(after.value("revision"), before.value("revision"));
		song->clearErrors(); const auto replay = studio.process(firstRequest); QVERIFY(replay.value("replayed").toBool()); QCOMPARE(replay.value("result"), firstResponse.value("result"));
		QVERIFY(inspect().value("result").toObject().value("warnings").toArray().isEmpty());
	}

	void longUnicodeMessagesRoundTripThroughIndependentChunks_data()
	{
		QTest::addColumn<int>("limit"); QTest::newRow("two-units") << 2; QTest::newRow("three-units") << 3;
		QTest::newRow("default-size") << 1024; QTest::newRow("maximum-size") << 8192;
	}
	void longUnicodeMessagesRoundTripThroughIndependentChunks()
	{
		QFETCH(int, limit); const auto emoji = QString::fromUtf8("\xf0\x9f\x8e\xb5");
		const QString message = "x" + (emoji + "\"\n").repeated(1200) + "end"; auto song = Engine::getSong(); song->collectError(message);
		const auto generation = summary().value("diagnosticGeneration"); QString restored; int offset = 0;
		while (offset < message.size())
		{
			const auto response = inspect({{"limit", 1}, {"messageOffset", offset}, {"messageLimit", limit}, {"diagnosticGeneration", generation}}); QVERIFY(response.value("ok").toBool());
			const auto transported = QJsonDocument::fromJson(encoded(response)).object();
			const auto item = transported.value("result").toObject().value("warnings").toArray().at(0).toObject(); const auto text = item.value("message").toString();
			QVERIFY(!text.isEmpty()); QVERIFY(text.size() <= limit); restored += text; const auto page = item.value("messagePage").toObject();
			QCOMPARE(page.value("total").toInt(), message.size()); QCOMPARE(page.value("offset").toInt(), offset);
			offset = page.value("nextOffset").toInt(); QCOMPARE(offset, restored.size()); QCOMPARE(page.value("hasMore").toBool(), offset < message.size());
		}
		QCOMPARE(restored, message); QCOMPARE(song->nativeErrors().value(message), 1);
		const auto beyond = inspect({{"messageOffset", 2147483647}}).value("result").toObject().value("warnings").toArray().at(0).toObject();
		QVERIFY(beyond.value("message").toString().isEmpty()); QVERIFY(!beyond.value("messagePage").toObject().value("hasMore").toBool());
	}

	void changedGenerationRejectsStaleEntryAndMessageOffsets_data()
	{
		QTest::addColumn<QString>("change"); QTest::newRow("new-warning") << QString("new");
		QTest::newRow("repetition") << QString("repeat"); QTest::newRow("clear") << QString("clear");
	}
	void changedGenerationRejectsStaleEntryAndMessageOffsets()
	{
		QFETCH(QString, change); auto song = Engine::getSong(); song->collectError("Initial warning"); const auto generation = summary().value("diagnosticGeneration");
		if (change == "new") { song->collectError("Second warning"); } else if (change == "repeat") { song->collectError("Initial warning"); } else { song->clearErrors(); }
		const auto current = song->nativeErrors(); const auto response = inspect({{"offset", 1}, {"messageOffset", 2}, {"diagnosticGeneration", generation}});
		QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QVERIFY(response.value("error").toString().contains("diagnostics changed"));
		QCOMPARE(song->nativeErrors(), current); QVERIFY(inspect({{"diagnosticGeneration", summary().value("diagnosticGeneration")}}).value("ok").toBool());
	}

	void invalidWarningSelectorsDoNotChangeNativeDiagnostics_data()
	{
		QTest::addColumn<QJsonObject>("args"); QTest::newRow("negative-offset") << QJsonObject{{"offset", -1}};
		QTest::newRow("fractional-offset") << QJsonObject{{"offset", .5}}; QTest::newRow("zero-limit") << QJsonObject{{"limit", 0}};
		QTest::newRow("excessive-limit") << QJsonObject{{"limit", 1025}}; QTest::newRow("negative-message-offset") << QJsonObject{{"messageOffset", -1}};
		QTest::newRow("fractional-message-offset") << QJsonObject{{"messageOffset", .5}}; QTest::newRow("surrogate-offset") << QJsonObject{{"messageOffset", 1}};
		QTest::newRow("one-unit") << QJsonObject{{"messageLimit", 1}}; QTest::newRow("excessive-message-limit") << QJsonObject{{"messageLimit", 8193}};
		QTest::newRow("numeric-generation") << QJsonObject{{"diagnosticGeneration", 0}}; QTest::newRow("empty-generation") << QJsonObject{{"diagnosticGeneration", ""}};
		QTest::newRow("track-selector") << QJsonObject{{"track", 0}}; QTest::newRow("names-selector") << QJsonObject{{"names", false}};
	}
	void invalidWarningSelectorsDoNotChangeNativeDiagnostics()
	{
		QFETCH(QJsonObject, args); auto song = Engine::getSong(); song->collectError(QString::fromUtf8("\xf0\x9f\x8e\xb5") + "warning");
		const auto before = song->nativeErrors(); const auto generation = song->errorGeneration(); const auto response = inspect(args);
		QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QCOMPARE(song->nativeErrors(), before); QCOMPARE(song->errorGeneration(), generation);
	}

	void cancellationWhileSkippingWarningsCannotPublishOrDispatchEvents()
	{
		auto song = Engine::getSong(); for (int index = 0; index < 100; ++index) { song->collectError("Warning " + QString::number(index)); }
		const auto before = song->nativeErrors(); QObject owner; bool eventRan = false; QTimer::singleShot(0, &owner, [&] { eventRan = true; }); int checks = 0;
		{
			FileResourceIO::Scope work([&] { return ++checks >= 8; }, true); const auto response = inspect({{"offset", 90}});
			QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QVERIFY(response.value("error").toString().contains("cancelled")); QVERIFY(!eventRan);
		}
		QCOMPARE(song->nativeErrors(), before); QCoreApplication::processEvents(); QVERIFY(eventRan); QVERIFY(inspect().value("ok").toBool());
	}

	void nativeAgentDiscoversWarningPagesFromItsCompactObservation_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void nativeAgentDiscoversWarningPagesFromItsCompactObservation()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; int round = 0; bool compact = true; QSet<QString> observedMessages; QString generation;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			const auto input = body.value(protocol == "chat" ? "messages" : "input").toArray(); QJsonObject lastOutput;
			for (const auto& value : input)
			{
				const auto item = value.toObject(); const auto text = item.value("content").toString();
				if (text.startsWith("Native runtime observation (data):\n"))
				{
					const auto project = QJsonDocument::fromJson(text.mid(text.indexOf('\n') + 1).toUtf8()).object().value("projectSummary").toObject();
					compact = compact && project.value("nativeWarningsPresent").toBool() && project.value("nativeWarningCount").toInt() == 2 && !project.contains("nativeWarnings");
					generation = project.value("diagnosticGeneration").toString();
				}
				if (item.value("role").toString() == "tool" || item.value("type").toString() == "function_call_output")
				{ lastOutput = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); }
			}
			if (++round == 1) { return decision(protocol, "warning-first", "project_inspect", {{"section", "warnings"}, {"limit", 1}, {"diagnosticGeneration", generation}}); }
			if (round <= 3)
			{
				if (!lastOutput.value("ok").toBool()) { throw std::runtime_error("Native warning page failed"); }
				const auto page = lastOutput.value("result").toObject(); observedMessages.insert(page.value("warnings").toArray().at(0).toObject().value("message").toString());
				if (round == 2) { return decision(protocol, "warning-second", "project_inspect", {{"section", "warnings"}, {"offset", page.value("nextOffset")}, {"limit", 1}, {"diagnosticGeneration", generation}}); }
			}
			return decision(protocol, "finish", "agent_finish", {{"status", "complete"}, {"summary", "Read native diagnostics; no compatibility or musical judgment claimed."}, {"evidence", QJsonArray{"warning-first", "warning-second"}}});
		});
		QVERIFY(studio.open(error)); QVERIFY(studio.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{
			{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-warning-fixture"}}}})).value("ok").toBool());
		Engine::getSong()->collectError("Native diagnostic A"); Engine::getSong()->collectError("Native diagnostic B");
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Read current native diagnostics and preserve their evidence."}})).value("ok").toBool());
		for (int step = 0; step < 6; ++step) { studio.advanceAgent(); }
		QVERIFY(compact); QCOMPARE(observedMessages, (QSet<QString>{"Native diagnostic A", "Native diagnostic B"}));
		QCOMPARE(manifest(directory).value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete")); QVERIFY(!studio.stopping());
	}

	void failedProjectLoadKeepsHistoricalWarningsDistinctFromRestoredLiveState()
	{
		QTemporaryDir directory; StudioRuntime studio(directory.path()); QString error; QVERIFY(studio.open(error));
		QVERIFY(studio.process(request("tempo", "song.set", {{"tempo", 123}})).value("ok").toBool());
		QVERIFY(studio.process(request("controller", "controller.add", {{"type", "lfo"}, {"name", "Original"}})).value("ok").toBool());
		const auto path = directory.filePath("unavailable.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(path)); QDomDocument document;
		{ QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(document.setContent(file.readAll())); }
		auto element = document.documentElement().firstChildElement("song").firstChildElement("controllers").firstChildElement(); QVERIFY(!element.isNull()); element.setAttribute("type", "unavailable");
		{ QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); const auto bytes = document.toByteArray(); QCOMPARE(file.write(bytes), qint64(bytes.size())); }
		const auto before = manifest(directory); const auto query = request("failed-load", "project.load", {{"path", path}, {"summary", true}}); const auto response = studio.process(query);
		QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("nativeWarnings").toString().contains("Unavailable project controller"));
		QCOMPARE(Engine::getSong()->getTempo(), 123); QCOMPARE(Engine::getSong()->controllers().size(), std::size_t{1}); QCOMPARE(Engine::getSong()->controllers().front()->name(), QString("Original"));
		QVERIFY(!summary().value("nativeWarningsPresent").toBool()); QVERIFY(inspect().value("result").toObject().value("warnings").toArray().isEmpty());
		QCOMPARE(manifest(directory).value("history"), before.value("history")); Engine::getSong()->collectError("New live diagnostic");
		const auto replay = studio.process(query); QVERIFY(replay.value("replayed").toBool()); QCOMPARE(replay.value("nativeWarnings"), response.value("nativeWarnings"));
		QCOMPARE(inspect().value("result").toObject().value("warnings").toArray().at(0).toObject().value("message").toString(), QString("New live diagnostic")); QVERIFY(!studio.stopping());
	}

	void diagnosticMutationFromWorkCallbackCannotPublishMixedPages()
	{
		auto song = Engine::getSong(); for (int index = 0; index < 10; ++index) { song->collectError("Warning " + QString::number(index)); }
		int checks = 0; bool changed = false;
		{
			FileResourceIO::Scope work([&] {
				++checks; changed = true; song->collectError("New warning during inspection"); return false;
			});
			const auto response = inspect(); QVERIFY(changed); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result"));
			QVERIFY(response.value("error").toString().contains("diagnostics changed"));
		}
		QCOMPARE(song->nativeErrors().size(), 11); QVERIFY(inspect().value("ok").toBool());
	}
};
QTEST_GUILESS_MAIN(StudioWarningInspectionTest)
#include "StudioWarningInspectionTest.moc"
