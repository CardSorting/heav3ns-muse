/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioRuntime.h"
#include "StudioSignals.h"
#include "lmmsconfig.h"
#ifndef LMMS_BUILD_WIN32
#include <signal.h>
#endif

namespace lmms
{
class StudioShutdownTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	static QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
	static QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
	static QJsonObject manifest(const QTemporaryDir& directory) { return QJsonDocument::fromJson(read(directory.filePath("session.json"))).object(); }
	static QJsonObject job(const QTemporaryDir& directory) { return manifest(directory).value("agent").toObject().value("job").toObject(); }
	static bool seed(const QTemporaryDir& directory, bool queued, QString& error, const QString& protocol = "chat")
	{
		StudioRuntime studio(directory.path()); if (!studio.open(error)) { return false; }
		for (const auto& command : {
			request("baseline", "song.set", {{"tempo", 91}}),
			request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{
				{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "shutdown-fixture"}}}})})
		{
			const auto response = studio.process(command);
			if (!response.value("ok").toBool()) { error = response.value("error").toString(); return false; }
		}
		if (queued)
		{
			const auto response = studio.process(request("chat", "session.chat", {{"text", "Revise this project"}}));
			if (!response.value("ok").toBool()) { error = response.value("error").toString(); return false; }
		}
		return true;
	}
	static QJsonObject decision(const QString& protocol)
	{
		const auto args = QStringLiteral("{\"tempo\":97}");
		if (protocol == "responses")
		{
			return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{
				{"type", "function_call"}, {"call_id", "tempo"}, {"name", "song_set"}, {"arguments", args}}}}};
		}
		const QJsonObject tool{{"id", "tempo"}, {"type", "function"}, {"function", QJsonObject{{"name", "song_set"}, {"arguments", args}}}};
		return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{
			{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{tool}}}}}}}};
	}
	static QJsonArray output(std::FILE* stream)
	{
		std::rewind(stream); QByteArray bytes; char buffer[4096];
		while (const auto count = std::fread(buffer, 1, sizeof(buffer), stream)) { bytes.append(buffer, static_cast<int>(count)); }
		QJsonArray events;
		for (const auto& line : bytes.split('\n')) { if (!line.trimmed().isEmpty()) { events.append(QJsonDocument::fromJson(line).object()); } }
		return events;
	}
	static QJsonObject shutdownEvent(const QJsonArray& events)
	{
		QJsonObject result;
		for (const auto& value : events)
		{
			const auto event = value.toObject(); if (event.value("event").toString() != "shutdown") { continue; }
			if (!result.isEmpty()) { return {}; } result = event;
		}
		return result;
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
	void scopedHandlersLatchAndRestore_data()
	{
		QTest::addColumn<int>("signalNumber");
#ifdef LMMS_BUILD_WIN32
		QTest::newRow("console-handler-lifetime") << 0;
#else
		QTest::newRow("interrupt") << SIGINT; QTest::newRow("terminate") << SIGTERM;
#endif
	}
	void scopedHandlersLatchAndRestore()
	{
		QFETCH(int, signalNumber); QString error;
#ifndef LMMS_BUILD_WIN32
		struct sigaction original{}, sentinel{}; sentinel.sa_handler = SIG_IGN; sigemptyset(&sentinel.sa_mask);
		QVERIFY(::sigaction(signalNumber, &sentinel, &original) == 0);
		const auto restore = qScopeGuard([&] { ::sigaction(signalNumber, &original, nullptr); });
#else
		Q_UNUSED(signalNumber); // Actual console events are sent only to a child process by StudioProcessTest.
#endif
		{
			StudioSignals guard; QVERIFY2(guard.install(error), qPrintable(error)); QVERIFY(!guard.requested());
			QVERIFY(!guard.install(error)); QVERIFY(!error.isEmpty());
			StudioSignals other; QVERIFY(!other.install(error)); QVERIFY(!other.requested());
#ifndef LMMS_BUILD_WIN32
			QVERIFY(::raise(signalNumber) == 0); QVERIFY(guard.requested());
			QVERIFY(::raise(signalNumber) == 0); QVERIFY(guard.requested());
#endif
		}
#ifndef LMMS_BUILD_WIN32
		struct sigaction restored{}; QVERIFY(::sigaction(signalNumber, nullptr, &restored) == 0);
		QVERIFY(restored.sa_handler == SIG_IGN); QVERIFY(::raise(signalNumber) == 0);
#endif
		StudioSignals fresh; QVERIFY2(fresh.install(error), qPrintable(error)); QVERIFY(!fresh.requested());
	}
	void interruptedStartupPreservesManifestAndNativeProject()
	{
		QTemporaryDir directory; QString error; QVERIFY2(seed(directory, true, error), qPrintable(error));
		const auto before = read(directory.filePath("session.json")); Engine::getSong()->setTempo(77);
		auto stream = std::tmpfile(); QVERIFY(stream); const auto close = qScopeGuard([&] { std::fclose(stream); });
		{
			StudioRuntime studio(directory.path(), {}, stream, [] { return true; });
			QCOMPARE(studio.run(), EXIT_FAILURE); QCOMPARE(Engine::getSong()->getTempo(), 77);
			QCOMPARE(read(directory.filePath("session.json")), before);
		}
		const auto events = output(stream); QCOMPARE(events.size(), 1); QVERIFY(!events.at(0).toObject().value("ok").toBool());
		QVERIFY(events.at(0).toObject().value("error").toString().contains("interrupted"));
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(Engine::getSong()->getTempo(), 91);
	}
	void idleInterruptCommitsOneShutdownBeforeDispatch_data()
	{
		QTest::addColumn<bool>("queued"); QTest::newRow("idle") << false; QTest::newRow("queued") << true;
	}
	void idleInterruptCommitsOneShutdownBeforeDispatch()
	{
		QFETCH(bool, queued); QTemporaryDir directory; QString error; QVERIFY2(seed(directory, queued, error), qPrintable(error));
		const auto before = manifest(directory); std::atomic<bool> interrupted{false}; int calls = 0;
		auto stream = std::tmpfile(); QVERIFY(stream); const auto close = qScopeGuard([&] { std::fclose(stream); });
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				++calls; return decision("chat");
			}, stream, [&] { return interrupted.load(); });
			QObject timerLifetime; QTimer::singleShot(0, &timerLifetime, [&] { interrupted.store(true); });
			QCOMPARE(studio.run(), EXIT_SUCCESS); QVERIFY(studio.stopping()); QCOMPARE(calls, 0);
			QVERIFY(!studio.process(request("too-late", "song.set", {{"tempo", 200}})).value("ok").toBool());
		}
		const auto events = output(stream); QCOMPARE(events.first().toObject().value("event").toString(), QString("ready"));
		const auto shutdown = shutdownEvent(events); QVERIFY(shutdown.value("ok").toBool());
		QCOMPARE(shutdown.value("reason").toString(), QString("process interrupt"));
		QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QCOMPARE(manifest(directory).value("revision").toInt(), before.value("revision").toInt() + 1); // Opening replaces native handles.
		QCOMPARE(manifest(directory).value("requests").toArray().size(), before.value("requests").toArray().size() + 1);
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(Engine::getSong()->getTempo(), 91);
		if (queued) { QCOMPARE(job(directory).value("status").toString(), QString("paused")); }
	}
	void inferenceInterruptDiscardsLateDecision_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses");
	}
	void inferenceInterruptDiscardsLateDecision()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; QVERIFY2(seed(directory, true, error, protocol), qPrintable(error));
		const auto before = manifest(directory); std::atomic<bool> interrupted{false}; int calls = 0; bool cancellationObserved = false;
		auto stream = std::tmpfile(); QVERIFY(stream); const auto close = qScopeGuard([&] { std::fclose(stream); });
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled& cancelled) {
				++calls; interrupted.store(true); cancellationObserved = cancelled(); return decision(protocol);
			}, stream, [&] { return interrupted.load(); });
			QCOMPARE(studio.run(), EXIT_SUCCESS); QCOMPARE(calls, 1); QVERIFY(cancellationObserved);
		}
		QVERIFY(shutdownEvent(output(stream)).value("ok").toBool());
		QCOMPARE(job(directory).value("status").toString(), QString("paused")); QVERIFY(job(directory).value("pending").toArray().isEmpty());
		QCOMPARE(job(directory).value("operations").toInt(), 0); QCOMPARE(job(directory).value("rounds").toInt(), 1);
		QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QCOMPARE(manifest(directory).value("revision").toInt(), before.value("revision").toInt() + 1);
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(Engine::getSong()->getTempo(), 91);
	}
	void nativeInterruptRollsBackBeforeExit_data()
	{
		QTest::addColumn<bool>("nested"); QTest::newRow("next-boundary") << false; QTest::newRow("nested-shutdown") << true;
	}
	void nativeInterruptRollsBackBeforeExit()
	{
		QFETCH(bool, nested); QTemporaryDir directory; QString error; QVERIFY2(seed(directory, true, error), qPrintable(error));
		const auto before = manifest(directory); std::atomic<bool> interrupted{false}; bool edited = false, nestedShutdown = false; int calls = 0;
		auto stream = std::tmpfile(); QVERIFY(stream); const auto close = qScopeGuard([&] { std::fclose(stream); });
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				++calls; return decision("chat");
			}, stream, [&] { return interrupted.load(); });
			const auto connection = connect(Engine::getSong(), &Song::tempoChanged, this, [&](bpm_t tempo) {
				if (tempo != 97 || edited) { return; } edited = true; interrupted.store(true);
				if (nested) { QTest::qWait(30); nestedShutdown = studio.stopping(); }
			}, Qt::DirectConnection);
			const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
			QCOMPARE(studio.run(), EXIT_SUCCESS); QVERIFY(edited); QCOMPARE(calls, 1); QCOMPARE(Engine::getSong()->getTempo(), 91);
			QCOMPARE(nestedShutdown, nested);
		}
		QVERIFY(shutdownEvent(output(stream)).value("ok").toBool());
		QCOMPARE(job(directory).value("status").toString(), QString("paused")); QVERIFY(job(directory).value("pending").toArray().isEmpty());
		QCOMPARE(job(directory).value("operations").toInt(), 1); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QCOMPARE(manifest(directory).value("revision").toInt(), before.value("revision").toInt() + 2); // Open and rollback each replace handles.
		bool failedTool = false;
		for (const auto& value : job(directory).value("records").toArray())
		{
			const auto record = value.toObject();
			if (record.value("kind").toString() == "tool") { failedTool = !record.value("response").toObject().value("ok").toBool(); }
		}
		QVERIFY(failedTool); StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->getTempo(), 91); QCOMPARE(job(directory).value("status").toString(), QString("paused"));
	}
	void failedShutdownCommitReturnsFailureWithoutAcknowledgement()
	{
		QTemporaryDir directory; QString error; QVERIFY2(seed(directory, true, error), qPrintable(error));
		const auto path = directory.filePath("session.json"); const auto backup = directory.filePath("session-before-interrupt.json");
		QByteArray acknowledged; std::atomic<bool> interrupted{false}; bool obstructed = false; int calls = 0;
		auto stream = std::tmpfile(); QVERIFY(stream); const auto close = qScopeGuard([&] { std::fclose(stream); });
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				++calls; return decision("chat");
			}, stream, [&] { return interrupted.load(); });
			QObject timerLifetime; QTimer::singleShot(0, &timerLifetime, [&] {
				acknowledged = read(path); // run() has completed native recovery and emitted ready.
				obstructed = QFile::rename(path, backup) && QDir().mkdir(path); interrupted.store(true);
			});
			QCOMPARE(studio.run(), EXIT_FAILURE); QVERIFY(obstructed); QCOMPARE(calls, 0); QVERIFY(studio.stopping());
			QVERIFY(!studio.process(request("too-late", "song.set", {{"tempo", 200}})).value("ok").toBool());
		}
		const auto shutdown = shutdownEvent(output(stream)); QVERIFY(!shutdown.isEmpty()); QVERIFY(!shutdown.value("ok").toBool());
		QVERIFY(shutdown.value("error").toString().contains("commit failed")); QVERIFY(!acknowledged.isEmpty()); QCOMPARE(read(backup), acknowledged);
		QVERIFY(QDir().rmdir(path)); QVERIFY(QFile::rename(backup, path));
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(Engine::getSong()->getTempo(), 91);
		const auto before = QJsonDocument::fromJson(acknowledged).object();
		QCOMPARE(manifest(directory).value("history"), before.value("history"));
		QCOMPARE(manifest(directory).value("revision").toInt(), before.value("revision").toInt() + 1);
		QCOMPARE(job(directory).value("status").toString(), QString("queued"));
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::StudioShutdownTest)
#include "StudioShutdownTest.moc"
