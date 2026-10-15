/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <thread>
#include "AudioEngine.h"
#include "ModelValueNotificationTestUtils.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioRuntime.h"

namespace lmms
{
class StudioSessionReentryTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	static QJsonObject request(const QString& id, const QString& operation, const QJsonObject& args = {})
	{
		return {{"id", id}, {"op", operation}, {"args", args}};
	}
	static QByteArray manifestBytes(const QString& directory)
	{
		QFile file(QDir(directory).filePath("session.json"));
		return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
	}
	static QJsonObject manifest(const QString& directory)
	{
		return QJsonDocument::fromJson(manifestBytes(directory)).object();
	}
	static bool uniqueReceipts(const QJsonObject& state)
	{
		QSet<QString> ids;
		for (const auto& value : state.value("requests").toArray())
		{
			const auto id = value.toObject().value("id").toString();
			if (id.isEmpty() || ids.contains(id)) { return false; }
			ids.insert(id);
		}
		return true;
	}
	static QJsonObject provider()
	{
		return {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", "chat"},
			{"endpoint", "http://127.0.0.1:12345/test"}, {"model", "session-reentry-fixture"}}}};
	}
	static QJsonObject tool(const QString& id, const QString& name, const QJsonObject& args)
	{
		return {{"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name},
			{"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}};
	}
	static QJsonObject decision(const QJsonArray& calls)
	{
		return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{
			{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", calls}}}}}}};
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
	void nestedControlCannotClaimAnExecutingRequestId_data()
	{
		QTest::addColumn<QString>("operation"); QTest::addColumn<bool>("failOuter");
		for (const auto operation : {"session.inspect", "agent.inspect", "agent.cancel", "session.shutdown"})
		{
			QTest::newRow(qPrintable(QString(operation) + "-success")) << QString(operation) << false;
			QTest::newRow(qPrintable(QString(operation) + "-failure")) << QString(operation) << true;
		}
	}
	void nestedControlCannotClaimAnExecutingRequestId()
	{
		QFETCH(QString, operation); QFETCH(bool, failOuter); QTemporaryDir directory; QJsonObject response;
		const auto edit = request("executing", "song.set", {{"tempo", 123}});
		{
			StudioRuntime studio(directory.path()); QString error; QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool());
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Continue this project"}})).value("ok").toBool());
			const auto before = manifest(directory.path()); bool entered = false; QJsonObject collision, observation;
			ModelValueNotificationProbe fault(&Engine::getSong()->tempoModel(), [&] { const auto tempo = Engine::getSong()->getTempo();
				if (tempo != 123 || entered) { return; }
				entered = true;
				collision = studio.process(request("executing", operation));
				observation = studio.process(request("distinct-observer", "session.inspect"));
				if (failOuter) { throw std::runtime_error("Fail after nested request rejection"); }
			});
			response = studio.process(edit); QVERIFY(entered); QCOMPARE(response.value("ok").toBool(), !failOuter);
			QVERIFY(!collision.value("ok").toBool()); QVERIFY(collision.value("error").toString().contains("still executing"));
			QVERIFY(observation.value("ok").toBool()); QCOMPARE(observation.value("revision"), before.value("revision"));
			QVERIFY(!studio.stopping()); QCOMPARE(Engine::getSong()->getTempo(), failOuter ? 91 : 123);
			const auto after = manifest(directory.path()); QVERIFY(uniqueReceipts(after));
			QCOMPARE(after.value("requests").toArray().size(), before.value("requests").toArray().size() + 2);
			QCOMPARE(after.value("agent").toObject().value("job").toObject().value("status").toString(), QString("queued"));
			QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1);
			const auto replay = studio.process(edit); QVERIFY(replay.value("replayed").toBool());
			QCOMPARE(replay.value("ok"), response.value("ok")); QCOMPARE(manifest(directory.path()), after);
			const auto conflict = studio.process(request("executing", operation));
			QVERIFY(!conflict.value("ok").toBool()); QVERIFY(conflict.value("error").toString().contains("different content"));
			QVERIFY(studio.process(request("after", "session.inspect")).value("ok").toBool());
		}
		StudioRuntime recovered(directory.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		const auto replay = recovered.process(edit); QVERIFY(replay.value("replayed").toBool());
		QCOMPARE(replay.value("ok"), response.value("ok")); QVERIFY(uniqueReceipts(manifest(directory.path())));
		QCOMPARE(Engine::getSong()->getTempo(), failOuter ? 91 : 123);
	}
	void distinctNestedControlSurvivesAgentRollbackAndRestart_data()
	{
		QTest::addColumn<bool>("shutdown"); QTest::newRow("cancel") << false; QTest::newRow("shutdown") << true;
	}
	void distinctNestedControlSurvivesAgentRollbackAndRestart()
	{
		QFETCH(bool, shutdown); QTemporaryDir directory; int rounds = 0;
		const auto transport = [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (++rounds == 1)
			{
				return decision({tool("interrupted", "song_set", {{"tempo", 123}}),
					tool("dependent", "track_add", {{"type", "automation"}})});
			}
			if (rounds == 2) { return decision({tool("revised", "song_set", {{"tempo", 97}})}); }
			return decision({tool("finish", "agent_finish", {{"status", "complete"}, {"summary", "Set the tempo to 97 BPM."},
				{"evidence", QJsonArray{"revised"}}})});
		};
		const auto control = request("control", shutdown ? "session.shutdown" : "agent.cancel");
		{
			StudioRuntime studio(directory.path(), transport); QString error; QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool());
			QVERIFY(studio.process(request("provider", "session.provider.set", provider())).value("ok").toBool());
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Revise the project"}})).value("ok").toBool());
			QVERIFY(!studio.advanceAgent().isEmpty()); const auto before = manifest(directory.path()); QJsonObject controlled;
			bool entered = false;
			const auto connection = connect(Engine::getSong(), &Song::tempoChanged, this, [&](bpm_t tempo) {
				if (tempo == 123 && !entered) { entered = true; controlled = studio.process(control); }
			}, Qt::DirectConnection);
			const auto disconnectFault = qScopeGuard([&] { QObject::disconnect(connection); });
			QVERIFY(!studio.advanceAgent().isEmpty()); QVERIFY(entered); QVERIFY(controlled.value("ok").toBool());
			QCOMPARE(studio.stopping(), shutdown); QCOMPARE(Engine::getSong()->getTempo(), 91);
			QVERIFY(Engine::getSong()->tracks().empty()); const auto after = manifest(directory.path());
			QVERIFY(uniqueReceipts(after)); QCOMPARE(after.value("history"), before.value("history"));
			const auto job = after.value("agent").toObject().value("job").toObject();
			QCOMPARE(job.value("status").toString(), shutdown ? QString("paused") : QString("cancelled"));
			QCOMPARE(job.value("operations").toInt(), 1); QVERIFY(job.value("pending").toArray().isEmpty());
			QCOMPARE(job.value("expectedRevision"), after.value("revision"));
			QVERIFY(StudioAgent::validState(after.value("agent").toObject())); QVERIFY(studio.advanceAgent().isEmpty());
		}
		StudioRuntime recovered(directory.path(), transport); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		QVERIFY(recovered.process(control).value("replayed").toBool()); QVERIFY(!recovered.stopping());
		QVERIFY(recovered.process(request("resume", "agent.resume")).value("ok").toBool());
		for (int i = 0; i < 4; ++i) { QVERIFY(!recovered.advanceAgent().isEmpty()); }
		const auto after = manifest(directory.path()); QVERIFY(uniqueReceipts(after));
		QVERIFY(StudioAgent::validState(after.value("agent").toObject()));
		QCOMPARE(after.value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete"));
		QCOMPARE(Engine::getSong()->getTempo(), 97); QVERIFY(Engine::getSong()->tracks().empty()); QCOMPARE(rounds, 3);
	}
	void foreignThreadEntryCannotReadOrChangeSessionState()
	{
		QTemporaryDir directory; int rounds = 0;
		auto studio = std::make_unique<StudioRuntime>(directory.filePath("first"),
			[&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				++rounds; return decision({tool("tempo", "song_set", {{"tempo", 97}})});
			});
		QString error; QVERIFY2(studio->open(error), qPrintable(error));
		QVERIFY(studio->process(request("provider", "session.provider.set", provider())).value("ok").toBool());
		QVERIFY(studio->process(request("chat", "session.chat", {{"text", "Set tempo"}})).value("ok").toBool());
		const auto before = manifestBytes(directory.filePath("first")); StudioRuntime candidate(directory.filePath("second"));
		QJsonArray rejected; QString openError, reopenError; bool opened = true, reopened = true; int runResult = EXIT_SUCCESS;
		std::thread worker([&] {
			opened = candidate.open(openError); reopened = studio->open(reopenError);
			for (const auto operation : {"session.inspect", "agent.inspect", "agent.cancel", "session.shutdown", "session.memory.set", "song.set"})
			{
				const auto args = QString(operation) == "song.set" ? QJsonObject{{"tempo", 123}}
					: QString(operation) == "session.memory.set" ? QJsonObject{{"memory", QJsonObject{{"unexpected", true}}}} : QJsonObject{};
				rejected.append(studio->process(request("foreign", operation, args)));
			}
			rejected.append(studio->advanceAgent()); runResult = studio->run();
		});
		worker.join(); QVERIFY(!opened); QVERIFY(!reopened); QCOMPARE(runResult, EXIT_FAILURE);
		QVERIFY(openError.contains("application thread")); QVERIFY(reopenError.contains("application thread"));
		for (const auto& value : rejected)
		{
			QVERIFY(!value.toObject().value("ok").toBool());
			QVERIFY(value.toObject().value("error").toString().contains("application thread"));
			QVERIFY(!value.toObject().contains("revision"));
		}
		QCOMPARE(rounds, 0); QVERIFY(!studio->stopping()); QCOMPARE(manifestBytes(directory.filePath("first")), before);
		QVERIFY(!QDir(directory.filePath("second")).exists()); QCOMPARE(Engine::getSong()->getTempo(), DefaultTempo);
		QVERIFY(!studio->advanceAgent().isEmpty()); QVERIFY(!studio->advanceAgent().isEmpty());
		QCOMPARE(rounds, 1); QCOMPARE(Engine::getSong()->getTempo(), 97);
		studio.reset(); QVERIFY2(candidate.open(error), qPrintable(error));
	}
	void oneEngineRejectsAnotherSessionThroughoutItsOwnerLifetime_data()
	{
		QTest::addColumn<int>("phase");
		QTest::newRow("opening") << 0; QTest::newRow("running") << 1; QTest::newRow("shutdown") << 2;
	}
	void oneEngineRejectsAnotherSessionThroughoutItsOwnerLifetime()
	{
		QFETCH(int, phase); QTemporaryDir directory;
		auto first = std::make_unique<StudioRuntime>(directory.filePath("first"));
		auto second = std::make_unique<StudioRuntime>(directory.filePath("second"));
		bool attempted = false, accepted = true; QString secondError;
		const auto connection = connect(Engine::getSong(), &Song::tempoChanged, this, [&](bpm_t) {
			if (phase == 0 && !attempted) { attempted = true; accepted = second->open(secondError); }
		}, Qt::DirectConnection);
		const auto disconnectFault = qScopeGuard([&] { QObject::disconnect(connection); });
		QString error; QVERIFY2(first->open(error), qPrintable(error)); QObject::disconnect(connection);
		QVERIFY(first->process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool());
		if (phase == 2) { QVERIFY(first->process(request("shutdown", "session.shutdown")).value("ok").toBool()); }
		const auto bytes = manifestBytes(directory.filePath("first"));
		if (phase != 0) { attempted = true; accepted = second->open(secondError); }
		QVERIFY(attempted); QVERIFY(!accepted); QVERIFY(secondError.contains("already owned"));
		QVERIFY(!QDir(directory.filePath("second")).exists()); QCOMPARE(Engine::getSong()->getTempo(), 91);
		QCOMPARE(manifestBytes(directory.filePath("first")), bytes);
		QVERIFY(!second->process(request("closed", "song.set", {{"tempo", 123}})).value("ok").toBool());
		first.reset(); QVERIFY2(second->open(error), qPrintable(error));
		QVERIFY(second->process(request("second-edit", "song.set", {{"tempo", 77}})).value("ok").toBool());
		second.reset(); StudioRuntime recovered(directory.filePath("first"));
		QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(Engine::getSong()->getTempo(), 91);
	}
	void failedOpenReleasesOwnershipWithoutStealingALaterOwner()
	{
		QTemporaryDir directory; auto failed = std::make_unique<StudioRuntime>(directory.filePath("failed"));
		ModelValueNotificationProbe fault(&Engine::getSong()->tempoModel(), [] { throw 17; }, true);
		QString error; QVERIFY(!failed->open(error)); fault.reset();
		auto second = std::make_unique<StudioRuntime>(directory.filePath("second"));
		QVERIFY2(second->open(error), qPrintable(error));
		QVERIFY(second->process(request("tempo", "song.set", {{"tempo", 91}})).value("ok").toBool());
		const auto bytes = manifestBytes(directory.filePath("second"));
		QVERIFY(!failed->open(error)); failed.reset(); StudioRuntime third(directory.filePath("third"));
		QVERIFY(!third.open(error)); QVERIFY(error.contains("already owned")); QCOMPARE(Engine::getSong()->getTempo(), 91);
		QCOMPARE(manifestBytes(directory.filePath("second")), bytes);
		second.reset(); QVERIFY2(third.open(error), qPrintable(error));
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::StudioSessionReentryTest)
#include "StudioSessionReentryTest.moc"
