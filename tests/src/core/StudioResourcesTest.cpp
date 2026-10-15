/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioResources.h"
#include "StudioRuntime.h"

namespace lmms
{
class StudioResourcesTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	static QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
	static QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
	static QJsonObject manifest(const QTemporaryDir& directory) { return QJsonDocument::fromJson(read(directory.filePath("session.json"))).object(); }
	static QJsonObject usage(const QTemporaryDir& directory) { return manifest(directory).value("agent").toObject().value("job").toObject().value("resourceUsage").toObject(); }
	static QJsonObject sample(double wall, double user, double system, double resident)
	{
		return {{"format", 1}, {"scope", "lmms-process"}, {"process", "123"}, {"instance", "11111111-2222-3333-4444-555555555555"}, {"at", "2026-10-13T12:00:00.000Z"},
			{"monotonicMs", wall}, {"cpuUserMs", user}, {"cpuSystemMs", system}, {"residentBytes", resident}, {"peakResidentBytes", resident + 1000},
			{"sources", QJsonObject{{"cpu", "fixture"}, {"resident", "fixture"}, {"peakResident", "fixture"}}}, {"errors", QJsonArray{}}};
	}
	static QJsonObject decision(bool failNative)
	{
		const QJsonObject args = failNative ? QJsonObject{{"tempo", 97}, {"masterPitch", 100}} : QJsonObject{{"tempo", 97}};
		const QJsonObject tool{{"id", "tempo"}, {"type", "function"}, {"function", QJsonObject{{"name", "song_set"},
			{"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}};
		return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{
			{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{tool}}}}}}}};
	}
private slots:
	void initTestCase()
	{
		QVERIFY(StudioResources::validSample(StudioResources::sample())); // No native engine is required to sample the process.
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
	void nativeCountersObserveRealCpuWork()
	{
		const auto before = StudioResources::sample(); QVERIFY(StudioResources::validSample(before));
		const QByteArray allocated(16 * 1024 * 1024, 'x'); QByteArray digest; QElapsedTimer elapsed; elapsed.start();
		do { digest = QCryptographicHash::hash(allocated, QCryptographicHash::Sha256); } while (elapsed.elapsed() < 80);
		QVERIFY(!digest.isEmpty()); const auto after = StudioResources::sample(); QVERIFY(StudioResources::validSample(after));
		QCOMPARE(after.value("process").toString(), QString::number(QCoreApplication::applicationPid()));
		const auto measured = StudioResources::interval(before, after); QVERIFY(measured.value("wallMs").toDouble() > 0);
		QVERIFY(measured.value("cpuMs").isDouble()); QVERIFY(measured.value("cpuMs").toDouble() > 0);
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
		QVERIFY(after.value("residentBytes").isDouble()); QVERIFY(after.value("residentBytes").toDouble() > 0);
		QVERIFY(after.value("peakResidentBytes").isDouble()); QVERIFY(after.value("peakResidentBytes").toDouble() >= before.value("peakResidentBytes").toDouble());
#endif
	}
	void intervalsRejectResetsAndMissingCounters_data()
	{
		QTest::addColumn<int>("fault"); for (int i = 0; i < 9; ++i) { QTest::newRow(qPrintable(QString::number(i))) << i; }
	}
	void intervalsRejectResetsAndMissingCounters()
	{
		QFETCH(int, fault); const auto before = sample(100, 20, 10, 4096); auto after = sample(200, 50, 30, 8192);
		if (fault == 1) { after.insert("cpuUserMs", 250); } // Multi-core CPU can exceed one wall-clock interval.
		if (fault == 2) { after.insert("monotonicMs", 100); }
		if (fault == 3) { after.insert("process", "456"); }
		if (fault == 4) { after.insert("instance", "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"); }
		if (fault == 5) { after.insert("monotonicMs", 99); }
		if (fault == 6) { after.insert("cpuUserMs", 19); }
		if (fault == 7) { after.insert("cpuSystemMs", QJsonValue::Null); }
		if (fault == 8) { after.remove("residentBytes"); }
		const auto measured = StudioResources::interval(before, after);
		if ((fault >= 3 && fault <= 5) || fault == 8) { QVERIFY(measured.value("wallMs").isNull()); QVERIFY(measured.value("cpuMs").isNull()); }
		else if (fault == 6 || fault == 7) { QCOMPARE(measured.value("wallMs").toDouble(), 100.); QVERIFY(measured.value("cpuMs").isNull()); }
		else
		{
			QCOMPARE(measured.value("cpuMs").toDouble(), fault == 1 ? 250. : 50.);
			if (fault == 2) { QVERIFY(measured.value("cpuPercentOfOneCore").isNull()); }
			else { QCOMPARE(measured.value("cpuPercentOfOneCore").toDouble(), fault == 1 ? 250. : 50.); }
		}
	}
	void usageAccumulatesIntervalsAcrossRestartWithoutInventingMissingData()
	{
		auto first = sample(100, 20, 10, 4096), second = sample(200, 50, 30, 8192);
		auto accumulated = StudioResources::accumulate({}, first, second); QVERIFY(StudioResources::validUsage(accumulated));
		QCOMPARE(accumulated.value("cpuMs").toDouble(), 50.); QCOMPARE(accumulated.value("wallMs").toDouble(), 100.);
		QCOMPARE(accumulated.value("maximumObservedResidentBytes").toDouble(), 8192.);
		first = sample(1, 1, 0, 2048); second = sample(21, 6, 1, 4096);
		for (auto object : {&first, &second}) { object->insert("instance", "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"); }
		accumulated = StudioResources::accumulate(accumulated, first, second);
		QCOMPARE(accumulated.value("cpuMs").toDouble(), 56.); QCOMPARE(accumulated.value("wallMs").toDouble(), 120.);
		second.insert("cpuSystemMs", QJsonValue::Null); second.insert("residentBytes", QJsonValue::Null);
		accumulated = StudioResources::accumulate(accumulated, first, second);
		QCOMPARE(accumulated.value("transitions").toInt(), 3); QCOMPARE(accumulated.value("cpuMeasuredTransitions").toInt(), 2);
		QCOMPARE(accumulated.value("cpuMs").toDouble(), 56.); QCOMPARE(accumulated.value("maximumObservedResidentBytes").toDouble(), 8192.);
		QVERIFY(StudioResources::validUsage(accumulated)); QCOMPARE(StudioResources::accumulate(accumulated, {}, second), accumulated);
		const auto overflow = StudioResources::accumulate({}, sample(0, 0, 0, 0), sample(9007199254740991., 9007199254740991., 9007199254740991., 0));
		QVERIFY(StudioResources::validUsage(overflow)); QCOMPARE(overflow.value("cpuMeasuredTransitions").toInt(), 0);
		QVERIFY(overflow.value("lastInterval").toObject().value("cpuPercentOfOneCore").isNull());
	}
	void resourceRequestsReplayTheirOriginalSample()
	{
		QTemporaryDir directory; QString error; StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error));
		const auto before = manifest(directory); const auto query = request("sample", "studio.resources.inspect");
		const auto first = studio.process(query); QVERIFY(first.value("ok").toBool()); QVERIFY(StudioResources::validSample(first.value("result").toObject()));
		QTest::qSleep(2); auto replay = studio.process(query); QVERIFY(replay.value("replayed").toBool()); replay.remove("replayed"); QCOMPARE(replay, first);
		const auto fresh = studio.process(request("fresh", "studio.resources.inspect")); QVERIFY(fresh.value("ok").toBool());
		QVERIFY(fresh.value("result").toObject().value("monotonicMs").toDouble() > first.value("result").toObject().value("monotonicMs").toDouble());
		QCOMPARE(manifest(directory).value("history"), before.value("history")); QCOMPARE(manifest(directory).value("revision"), before.value("revision"));
		QVERIFY(!studio.process(request("invalid", "studio.resources.inspect", {{"pid", 123}})).value("ok").toBool());
		QCOMPARE(StudioAgent::operation("studio_resources_inspect"), QString("studio.resources.inspect"));
	}
	void agentMeasurementsPersistWithNativeOutcomes_data()
	{
		QTest::addColumn<int>("mode");
		QTest::newRow("native-success") << 0; QTest::newRow("native-rollback") << 1; QTest::newRow("provider-failure") << 2;
		QTest::newRow("nested-cancel") << 3; QTest::newRow("nested-shutdown") << 4;
	}
	void agentMeasurementsPersistWithNativeOutcomes()
	{
		QFETCH(int, mode); QTemporaryDir directory; QString error; StudioRuntime* active = nullptr; bool observed = false; QJsonObject control, persisted;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) -> QJsonObject {
				const auto content = body.value("messages").toArray().last().toObject().value("content").toString();
				const auto observation = QJsonDocument::fromJson(content.mid(content.indexOf('\n') + 1).toUtf8()).object();
				observed = StudioResources::validSample(observation.value("processResources").toObject());
				if (mode == 2) { throw std::runtime_error("Provider test failure"); }
				if (mode >= 3) { control = active->process(request("control", mode == 3 ? "agent.cancel" : "session.shutdown")); }
				return decision(mode == 1);
			}); active = &studio; QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool());
			QVERIFY(studio.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{
				{"protocol", "chat"}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "resource-fixture"}}}})).value("ok").toBool());
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Revise this project"}})).value("ok").toBool());
			QVERIFY(!studio.advanceAgent().isEmpty()); QVERIFY(observed);
			if (mode < 2) { QVERIFY(!studio.advanceAgent().isEmpty()); }
			if (mode >= 3) { QVERIFY(control.value("ok").toBool()); }
			persisted = usage(directory);
			if (mode == 4) { QVERIFY(studio.stopping()); QVERIFY(persisted.isEmpty()); }
			else { QVERIFY(StudioResources::validUsage(persisted)); QCOMPARE(persisted.value("transitions").toInt(), mode < 2 ? 2 : 1); }
			QCOMPARE(Engine::getSong()->getTempo(), mode == 0 ? 97 : 91);
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(usage(directory), persisted); QCOMPARE(Engine::getSong()->getTempo(), mode == 0 ? 97 : 91);
	}
	void malformedPersistedMetricsRejectOpen_data()
	{
		QTest::addColumn<int>("fault"); for (int i = 0; i < 7; ++i) { QTest::newRow(qPrintable(QString::number(i))) << i; }
	}
	void malformedPersistedMetricsRejectOpen()
	{
		QFETCH(int, fault); QTemporaryDir directory; QString error;
		{ StudioRuntime studio(directory.path()); QVERIFY2(studio.open(error), qPrintable(error)); QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Retain this task"}})).value("ok").toBool()); }
		auto state = manifest(directory), agent = state.value("agent").toObject(), job = agent.value("job").toObject();
		auto measured = StudioResources::accumulate({}, sample(100, 10, 1, 4096), sample(200, 20, 2, 8192));
		if (fault == 0) { measured.insert("cpuMs", -1); }
		if (fault == 1) { measured.insert("transitions", 1.5); }
		if (fault == 2) { measured.insert("cpuMeasuredTransitions", 2); }
		if (fault == 3) { measured.insert("maximumObservedResidentBytes", "unknown"); }
		if (fault == 4) { auto last = measured.value("lastSample").toObject(); last.insert("instance", "bad"); measured.insert("lastSample", last); }
		if (fault == 5) { auto last = measured.value("lastInterval").toObject(); last.insert("wallMs", QJsonValue::Null); measured.insert("lastInterval", last); }
		if (fault == 6) { measured.insert("cpuMs", 1e100); }
		job.insert("resourceUsage", measured); agent.insert("job", job); state.insert("agent", agent);
		const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact); const auto path = directory.filePath("session.json");
		{ QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(bytes), qint64(bytes.size())); }
		Engine::getSong()->setTempo(77); StudioRuntime failed(directory.path()); QVERIFY(!failed.open(error)); QVERIFY(!error.isEmpty());
		QCOMPARE(Engine::getSong()->getTempo(), 77); QCOMPARE(read(path), bytes);
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::StudioResourcesTest)
#include "StudioResourcesTest.moc"
