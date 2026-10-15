/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioResources.h"
#include "StudioRuntime.h"

namespace lmms
{
class StudioResourceBudgetTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	struct Counters
	{
		double cpu = 0, resident = 1024. * 1024., wall = 0;
		bool missingCpu = false, missingMemory = false, throws = false;
		QString instance = "11111111-2222-3333-4444-555555555555";
		QJsonObject sample()
		{
			if (throws) { throw std::runtime_error("resource sampler test failure"); }
			return {{"format", 1}, {"scope", "lmms-process"}, {"process", "123"}, {"instance", instance}, {"at", "2026-10-13T12:00:00.000Z"},
				{"monotonicMs", ++wall}, {"cpuUserMs", missingCpu ? QJsonValue(QJsonValue::Null) : QJsonValue(cpu)}, {"cpuSystemMs", 0},
				{"residentBytes", missingMemory ? QJsonValue(QJsonValue::Null) : QJsonValue(resident)}, {"peakResidentBytes", resident},
				{"sources", QJsonObject{{"cpu", "fixture"}, {"resident", "fixture"}, {"peakResident", "fixture"}}}, {"errors", QJsonArray{}}};
		}
	};
	static QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
	static QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
	static QJsonObject manifest(const QTemporaryDir& directory) { return QJsonDocument::fromJson(read(directory.filePath("session.json"))).object(); }
	static QJsonObject job(const QTemporaryDir& directory) { return manifest(directory).value("agent").toObject().value("job").toObject(); }
	static QJsonObject tool(const QString& id, const QString& name, const QJsonObject& args)
	{
		return {{"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name}, {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}};
	}
	static QJsonObject decision(int round, bool batch = false)
	{
		QJsonArray calls{tool("tempo-" + QString::number(round), "song_set", {{"tempo", 97}})};
		if (batch) { calls.append(tool("dependent-" + QString::number(round), "track_add", {{"type", "automation"}})); }
		return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{
			{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", calls}}}}}}};
	}
	static bool setup(StudioRuntime& studio, QString& error)
	{
		if (!studio.open(error)) { return false; }
		for (const auto& command : {request("baseline", "song.set", {{"tempo", 91}}), request("provider", "session.provider.set",
			{{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", "chat"}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "budget-fixture"}}}})})
		{
			const auto response = studio.process(command); if (!response.value("ok").toBool()) { error = response.value("error").toString(); return false; }
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
	void invalidBudgetsPreserveJobAndProject_data()
	{
		QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value");
		for (const auto& field : {QString("maximumCpuTimeMs"), QString("maximumResidentMiB")})
		{
			QTest::newRow(qPrintable(field + "-negative")) << field << QJsonValue(-1);
			QTest::newRow(qPrintable(field + "-fraction")) << field << QJsonValue(1.5);
			QTest::newRow(qPrintable(field + "-boolean")) << field << QJsonValue(true);
			QTest::newRow(qPrintable(field + "-large")) << field << QJsonValue(field == "maximumCpuTimeMs" ? 86400001 : 1048577);
		}
	}
	void invalidBudgetsPreserveJobAndProject()
	{
		QFETCH(QString, field); QFETCH(QJsonValue, value); QTemporaryDir directory; QString error; StudioRuntime studio(directory.path()); QVERIFY2(setup(studio, error), qPrintable(error));
		QVERIFY(!studio.process(request("bad-chat", "session.chat", {{"text", "Revise"}, {field, value}})).value("ok").toBool()); QVERIFY(job(directory).isEmpty());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Revise"}})).value("ok").toBool());
		QVERIFY(studio.process(request("cancel", "agent.cancel")).value("ok").toBool()); const auto before = job(directory);
		QVERIFY(!studio.process(request("bad-resume", "agent.resume", {{field, value}})).value("ok").toBool()); QCOMPARE(job(directory), before); QCOMPARE(Engine::getSong()->getTempo(), 91);
	}
	void unavailableCountersPauseBeforeInference_data()
	{
		QTest::addColumn<int>("fault"); QTest::newRow("cpu") << 0; QTest::newRow("resident") << 1; QTest::newRow("sampler-exception") << 2;
	}
	void unavailableCountersPauseBeforeInference()
	{
		QFETCH(int, fault); QTemporaryDir directory; QString error; Counters counters; int calls = 0;
		counters.missingCpu = fault == 0; counters.missingMemory = fault == 1; counters.throws = fault == 2;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(++calls); },
			stdout, {}, [&] { return counters.sample(); }); QVERIFY2(setup(studio, error), qPrintable(error));
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Revise"}, {"maximumCpuTimeMs", 100}, {"maximumResidentMiB", 2}})).value("ok").toBool());
		QVERIFY(!studio.advanceAgent().isEmpty()); QCOMPARE(calls, 0); QCOMPARE(job(directory).value("status").toString(), QString("paused"));
		QVERIFY(job(directory).value("lastError").toString().contains("cannot be checked")); QCOMPARE(job(directory).value("rounds").toInt(), 0);
		QCOMPARE(Engine::getSong()->getTempo(), 91);
		// Explicitly disabling budgets permits native work even without telemetry.
		QVERIFY(studio.process(request("resume", "agent.resume", {{"maximumCpuTimeMs", 0}, {"maximumResidentMiB", 0}})).value("ok").toBool());
		QVERIFY(!studio.advanceAgent().isEmpty()); QVERIFY(!studio.advanceAgent().isEmpty()); QCOMPARE(calls, 1); QCOMPARE(Engine::getSong()->getTempo(), 97);
	}
	void inferenceLimitDiscardsLateDecisionsAndResumeRetainsUsage_data()
	{
		QTest::addColumn<bool>("memory"); QTest::addColumn<bool>("periodic");
		for (const auto memory : {false, true})
		{
			QTest::newRow(memory ? "memory-boundary" : "cpu-boundary") << memory << false;
			QTest::newRow(memory ? "memory-periodic" : "cpu-periodic") << memory << true;
		}
	}
	void inferenceLimitDiscardsLateDecisionsAndResumeRetainsUsage()
	{
		QFETCH(bool, memory); QFETCH(bool, periodic); QTemporaryDir directory; QString error; Counters counters; int calls = 0; bool cancelledDuringTransport = false;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled& cancelled) {
			++calls; if (calls == 1) { counters.cpu = 150; counters.resident = 4 * 1024. * 1024.; if (periodic) { QTest::qSleep(60); cancelledDuringTransport = cancelled(); } }
			return decision(calls);
		}, stdout, {}, [&] { return counters.sample(); }); QVERIFY2(setup(studio, error), qPrintable(error));
		const auto field = memory ? QString("maximumResidentMiB") : QString("maximumCpuTimeMs");
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Revise"}, {field, memory ? 2 : 100}})).value("ok").toBool());
		QVERIFY(!studio.advanceAgent().isEmpty()); QCOMPARE(calls, 1); QCOMPARE(cancelledDuringTransport, periodic);
		auto stopped = job(directory); QCOMPARE(stopped.value("status").toString(), QString("paused")); QVERIFY(stopped.value("pending").toArray().isEmpty());
		QCOMPARE(stopped.value("operations").toInt(), 0); QCOMPARE(Engine::getSong()->getTempo(), 91);
		QVERIFY(stopped.value("lastError").toString().contains(memory ? "resident-memory budget" : "CPU-time budget"));
		QCOMPARE(stopped.value("resourceUsage").toObject().value("cpuMs").toDouble(), 150.);
		QVERIFY(studio.process(request("same-limit", "agent.resume")).value("ok").toBool()); QVERIFY(!studio.advanceAgent().isEmpty()); QCOMPARE(calls, 1);
		QVERIFY(studio.process(request("larger-limit", "agent.resume", {{field, memory ? 8 : 300}})).value("ok").toBool());
		QVERIFY(!studio.advanceAgent().isEmpty()); QVERIFY(!studio.advanceAgent().isEmpty()); QCOMPARE(calls, 2); QCOMPARE(Engine::getSong()->getTempo(), 97);
		QCOMPARE(job(directory).value("resourceUsage").toObject().value("cpuMs").toDouble(), 150.);
	}
	void nativeLimitRollsBackAndDiscardsDependentCalls_data()
	{
		QTest::addColumn<bool>("memory"); QTest::newRow("cpu") << false; QTest::newRow("resident") << true;
	}
	void nativeLimitRollsBackAndDiscardsDependentCalls()
	{
		QFETCH(bool, memory); QTemporaryDir directory; QString error; Counters counters; int calls = 0; bool edited = false;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(++calls, true); },
			stdout, {}, [&] { return counters.sample(); }); QVERIFY2(setup(studio, error), qPrintable(error));
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Revise"}, {memory ? "maximumResidentMiB" : "maximumCpuTimeMs", memory ? 2 : 100}})).value("ok").toBool());
		QVERIFY(!studio.advanceAgent().isEmpty()); const auto before = manifest(directory);
		const auto connection = connect(Engine::getSong(), &Song::tempoChanged, this, [&](bpm_t tempo) {
			if (tempo == 97 && !edited) { edited = true; counters.cpu = 150; counters.resident = 4 * 1024. * 1024.; }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!studio.advanceAgent().isEmpty()); QVERIFY(edited); QCOMPARE(calls, 1); QCOMPARE(Engine::getSong()->getTempo(), 91); QVERIFY(Engine::getSong()->tracks().isEmpty());
		const auto stopped = job(directory); QCOMPARE(stopped.value("status").toString(), QString("paused")); QVERIFY(stopped.value("pending").toArray().isEmpty());
		QCOMPARE(stopped.value("operations").toInt(), 1); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QCOMPARE(manifest(directory).value("revision").toInt(), before.value("revision").toInt() + 1);
		int failures = 0; for (const auto& value : stopped.value("records").toArray())
		{
			const auto record = value.toObject(); if (record.value("kind").toString() == "tool" && !record.value("response").toObject().value("ok").toBool()) { ++failures; }
		}
		QCOMPARE(failures, 2);
		QVERIFY(studio.process(request("manual", "song.set", {{"tempo", 105}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->getTempo(), 105); // Budgets apply to autonomous execution.
	}
	void nestedCancellationRemainsAvailableOverBudget()
	{
		QTemporaryDir directory; QString error; Counters counters; QJsonObject cancellation, observation;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(1, true); },
			stdout, {}, [&] { return counters.sample(); }); QVERIFY2(setup(studio, error), qPrintable(error));
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Revise"}, {"maximumCpuTimeMs", 100}})).value("ok").toBool()); QVERIFY(!studio.advanceAgent().isEmpty());
		const auto connection = connect(Engine::getSong(), &Song::tempoChanged, this, [&](bpm_t tempo) {
			if (tempo == 97)
			{
				counters.cpu = 150; observation = studio.process(request("resources", "studio.resources.inspect"));
				cancellation = studio.process(request("cancel", "agent.cancel"));
			}
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!studio.advanceAgent().isEmpty()); QVERIFY(cancellation.value("ok").toBool()); QCOMPARE(job(directory).value("status").toString(), QString("cancelled"));
		QVERIFY(observation.value("ok").toBool()); QVERIFY(StudioResources::validSample(observation.value("result").toObject()));
		QCOMPARE(Engine::getSong()->getTempo(), 91); QVERIFY(!studio.stopping());
	}
	void incompleteHistoricalCpuCannotEvadeAnEnabledBudget_data()
	{
		QTest::addColumn<int>("fault"); QTest::newRow("legacy-unmeasured") << 0; QTest::newRow("missing-counter") << 1; QTest::newRow("interrupted-round") << 2;
	}
	void incompleteHistoricalCpuCannotEvadeAnEnabledBudget()
	{
		QFETCH(int, fault); Counters counters; const auto before = counters.sample(); counters.cpu = 50; const auto after = counters.sample();
		QJsonObject job{{"maximumCpuTimeMs", 100}, {"rounds", 1}, {"operations", 0}};
		if (fault)
		{
			auto usage = StudioResources::accumulate({}, before, after);
			if (fault == 1) { usage.insert("cpuMeasuredTransitions", 0); } else { job.insert("rounds", 2); }
			job.insert("resourceUsage", usage);
		}
		QVERIFY(StudioResources::budgetError(job, after, after).contains("incomplete CPU accounting"));
		job.insert("maximumCpuTimeMs", 0); QVERIFY(StudioResources::budgetError(job, after, after).isEmpty());
	}
	void cpuBudgetAndUsageSurviveRestart()
	{
		QTemporaryDir directory; QString error; Counters first;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { first.cpu = 150; return decision(1); },
				stdout, {}, [&] { return first.sample(); }); QVERIFY2(setup(studio, error), qPrintable(error));
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Revise"}, {"maximumCpuTimeMs", 100}})).value("ok").toBool());
			QVERIFY(!studio.advanceAgent().isEmpty()); QCOMPARE(job(directory).value("status").toString(), QString("paused"));
		}
		Counters second; second.instance = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"; int calls = 0;
		StudioRuntime recovered(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { ++calls; second.cpu = 50; return decision(2); },
			stdout, {}, [&] { return second.sample(); }); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(job(directory).value("maximumCpuTimeMs").toInt(), 100);
		QVERIFY(recovered.process(request("same", "agent.resume")).value("ok").toBool()); QVERIFY(!recovered.advanceAgent().isEmpty()); QCOMPARE(calls, 0);
		QVERIFY(recovered.process(request("larger", "agent.resume", {{"maximumCpuTimeMs", 250}})).value("ok").toBool());
		QVERIFY(!recovered.advanceAgent().isEmpty()); QVERIFY(!recovered.advanceAgent().isEmpty()); QCOMPARE(calls, 1); QCOMPARE(Engine::getSong()->getTempo(), 97);
		QCOMPARE(job(directory).value("resourceUsage").toObject().value("cpuMs").toDouble(), 200.);
	}
	void invalidPersistedBudgetsRejectStartup_data()
	{
		QTest::addColumn<QString>("field"); QTest::newRow("cpu") << QString("maximumCpuTimeMs"); QTest::newRow("resident") << QString("maximumResidentMiB");
	}
	void invalidPersistedBudgetsRejectStartup()
	{
		QFETCH(QString, field); QTemporaryDir directory; QString error;
		{ StudioRuntime studio(directory.path()); QVERIFY2(setup(studio, error), qPrintable(error)); QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Revise"}})).value("ok").toBool()); }
		auto state = manifest(directory), agent = state.value("agent").toObject(), current = agent.value("job").toObject();
		current.insert(field, -1); agent.insert("job", current); state.insert("agent", agent); const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
		const auto path = directory.filePath("session.json"); { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(bytes), qint64(bytes.size())); }
		Engine::getSong()->setTempo(77); StudioRuntime failed(directory.path()); QVERIFY(!failed.open(error)); QVERIFY(!error.isEmpty());
		QCOMPARE(read(path), bytes); QCOMPARE(Engine::getSong()->getTempo(), 77);
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::StudioResourceBudgetTest)
#include "StudioResourceBudgetTest.moc"
