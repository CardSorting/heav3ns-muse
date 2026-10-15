/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <stdexcept>
#include "AudioEngine.h"
#include "ModelValueNotificationTestUtils.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioRuntime.h"

namespace lmms
{
class StudioRecoveryTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;

	static QJsonObject request(const QString& id, const QString& operation, const QJsonObject& args = {})
	{
		return {{"id", id}, {"op", operation}, {"args", args}};
	}
	static QByteArray manifestBytes(const QTemporaryDir& directory)
	{
		QFile file(directory.filePath("session.json"));
		return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
	}
	static QJsonObject manifest(const QTemporaryDir& directory)
	{
		return QJsonDocument::fromJson(manifestBytes(directory)).object();
	}
	static QJsonObject provider()
	{
		return {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", "chat"},
			{"endpoint", "http://127.0.0.1:12345/test"}, {"model", "recovery-fixture"}}}};
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
	static QJsonObject finish()
	{
		return decision({tool("finish", "agent_finish", {{"status", "complete"},
			{"summary", "Changed the native tempo to 97 BPM."}, {"evidence", QJsonArray{"revised-tempo"}}})});
	}
	[[noreturn]] static void raise(bool nonstandard)
	{
		if (nonstandard) { throw 17; }
		throw std::runtime_error("Native recovery fixture exception");
	}
	static void exceptionTypes()
	{
		QTest::addColumn<bool>("nonstandard");
		QTest::newRow("standard") << false;
		QTest::newRow("nonstandard") << true;
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
		Engine::getSong()->tempoModel().setInitValue(DefaultTempo);
		Engine::getSong()->clearErrors();
	}
	void callbackExceptionsRollbackAndCommitReplayableFailure_data() { exceptionTypes(); }
	void callbackExceptionsRollbackAndCommitReplayableFailure()
	{
		QFETCH(bool, nonstandard); QTemporaryDir directory; QJsonObject rejected;
		const auto edit = request("throwing-edit", "song.set", {{"tempo", 123}});
		{
			StudioRuntime studio(directory.path()); QString error; QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool());
			QVERIFY(studio.process(request("track", "track.add", {{"type", "automation"}})).value("ok").toBool());
			const auto before = manifest(directory); const bool journalling = Engine::projectJournal()->isJournalling();
			bool armed = true;
			ModelValueNotificationProbe fault(&Engine::getSong()->tempoModel(), [&] { const auto tempo = Engine::getSong()->getTempo();
				if (armed && tempo == 123) { armed = false; raise(nonstandard); }
			});
			rejected = studio.process(edit); QVERIFY(!rejected.value("ok").toBool()); QVERIFY(!studio.stopping());
			QVERIFY(!rejected.value("error").toString().isEmpty()); QVERIFY(!armed);
			QCOMPARE(Engine::getSong()->getTempo(), 91); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1));
			QVERIFY(!Engine::getSong()->isLoadingProject()); QCOMPARE(Engine::projectJournal()->isJournalling(), journalling);
			const auto after = manifest(directory);
			QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(after.value("cursor"), before.value("cursor"));
			QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1);
			QCOMPARE(rejected.value("revision"), after.value("revision"));
			const auto replay = studio.process(edit); QVERIFY(replay.value("replayed").toBool());
			QCOMPARE(replay.value("error"), rejected.value("error")); QCOMPARE(manifest(directory), after);
			QVERIFY(studio.process(request("later-edit", "song.set", {{"tempo", 97}})).value("ok").toBool());
		}
		StudioRuntime recovered(directory.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		const auto replay = recovered.process(edit); QVERIFY(replay.value("replayed").toBool());
		QCOMPARE(replay.value("error"), rejected.value("error")); QCOMPARE(Engine::getSong()->getTempo(), 97);
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1));
	}
	void rollbackExceptionsStopWithoutAcknowledgingTheMutation_data()
	{
		QTest::addColumn<bool>("nonstandard"); QTest::addColumn<bool>("agentWork");
		QTest::newRow("request-standard") << false << false;
		QTest::newRow("request-nonstandard") << true << false;
		QTest::newRow("agent-standard") << false << true;
		QTest::newRow("agent-nonstandard") << true << true;
	}
	void rollbackExceptionsStopWithoutAcknowledgingTheMutation()
	{
		QFETCH(bool, nonstandard); QFETCH(bool, agentWork); QTemporaryDir directory; QJsonObject before;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				return decision({tool("bad-tempo", "song_set", {{"tempo", 123}}),
					tool("dependent", "track_add", {{"type", "automation"}})});
			});
			QString error; QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool());
			QVERIFY(studio.process(request("track", "track.add", {{"type", "automation"}})).value("ok").toBool());
			if (agentWork)
			{
				QVERIFY(studio.process(request("provider", "session.provider.set", provider())).value("ok").toBool());
				QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Revise the tempo and add a track"}})).value("ok").toBool());
				QVERIFY(!studio.advanceAgent().isEmpty());
			}
			before = manifest(directory); const auto bytes = manifestBytes(directory);
			const bool journalling = Engine::projectJournal()->isJournalling(); int callbacks = 0;
			ModelValueNotificationProbe fault(&Engine::getSong()->tempoModel(), [&] {
				if (++callbacks == 1) { throw std::runtime_error("Original native edit failure"); }
				raise(nonstandard);
			});
			const auto response = agentWork ? studio.advanceAgent() : studio.process(request("failed", "song.set", {{"tempo", 123}}));
			QVERIFY(!response.value("ok").toBool()); QVERIFY(studio.stopping()); QCOMPARE(callbacks, 2);
			QVERIFY(response.value("error").toString().contains("Original native edit failure"));
			QVERIFY(response.value("error").toString().contains("rollback failed"));
			QVERIFY(!Engine::getSong()->isLoadingProject()); QCOMPARE(Engine::projectJournal()->isJournalling(), journalling);
			QCOMPARE(manifestBytes(directory), bytes); QVERIFY(studio.advanceAgent().isEmpty());
			QVERIFY(!studio.process(request("blocked", "song.set", {{"tempo", 80}})).value("ok").toBool());
			QCOMPARE(manifestBytes(directory), bytes);
		}
		StudioRuntime recovered(directory.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->getTempo(), 91); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1));
		const auto after = manifest(directory); QCOMPARE(after.value("history"), before.value("history"));
		QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1);
		if (agentWork)
		{
			const auto job = after.value("agent").toObject().value("job").toObject();
			QCOMPARE(job.value("operations").toInt(), 0); QVERIFY(job.value("pending").toArray().isEmpty());
			QVERIFY(StudioAgent::validState(after.value("agent").toObject()));
		}
	}
	void commitFailureStopsBeforeRollbackCallbacks_data()
	{
		QTest::addColumn<int>("rollbackFailure");
		QTest::newRow("restored") << 0;
		QTest::newRow("standard-exception") << 1;
		QTest::newRow("nonstandard-exception") << 2;
	}
	void commitFailureStopsBeforeRollbackCallbacks()
	{
		QFETCH(int, rollbackFailure); QTemporaryDir directory; QJsonObject before;
		{
			StudioRuntime studio(directory.path()); QString error; QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool());
			before = manifest(directory); const auto bytes = manifestBytes(directory);
			const auto path = directory.filePath("session.json"), retained = directory.filePath("last-session.json");
			QVERIFY(QFile::rename(path, retained));
			const auto restoreManifest = qScopeGuard([&] { QDir(directory.path()).rmdir("session.json"); QFile::rename(retained, path); });
			QVERIFY(QDir(directory.path()).mkdir("session.json"));
			int callbacks = 0; bool rejectedNestedRequest = true;
			ModelValueNotificationProbe fault(&Engine::getSong()->tempoModel(), [&] {
				if (!Engine::getSong()->isLoadingProject()) { return; }
				++callbacks;
				const auto nested = studio.process(request("during-rollback", "session.inspect"));
				rejectedNestedRequest = rejectedNestedRequest && !nested.value("ok").toBool();
				if (rollbackFailure) { raise(rollbackFailure == 2); }
			});
			const auto response = studio.process(request("uncommitted", "song.set", {{"tempo", 123}}));
			QVERIFY(!response.value("ok").toBool()); QVERIFY(studio.stopping());
			QVERIFY(response.value("error").toString().contains("Session commit failed"));
			QCOMPARE(response.value("revision"), before.value("revision"));
			QVERIFY(callbacks > 0); QVERIFY(rejectedNestedRequest);
			QCOMPARE(response.value("error").toString().contains("; rollback:"), rollbackFailure != 0);
			if (!rollbackFailure) { QCOMPARE(Engine::getSong()->getTempo(), 91); }
			QFile retainedFile(retained); QVERIFY(retainedFile.open(QIODevice::ReadOnly)); QCOMPARE(retainedFile.readAll(), bytes);
			QVERIFY(!studio.process(request("blocked", "song.set", {{"tempo", 80}})).value("ok").toBool());
		}
		StudioRuntime recovered(directory.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->getTempo(), 91); const auto after = manifest(directory);
		QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(after.value("cursor"), before.value("cursor"));
		QCOMPARE(after.value("requests"), before.value("requests"));
	}
	void undoLoadExceptionsPreserveTheCurrentCursorAndModelFlags_data() { exceptionTypes(); }
	void undoLoadExceptionsPreserveTheCurrentCursorAndModelFlags()
	{
		QFETCH(bool, nonstandard); QTemporaryDir directory; StudioRuntime studio(directory.path()); QString error;
		QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(request("first", "song.set", {{"tempo", 91}})).value("ok").toBool());
		QVERIFY(studio.process(request("second", "song.set", {{"tempo", 105}})).value("ok").toBool());
		const auto before = manifest(directory); const bool journalling = Engine::getSong()->tempoModel().isJournalling();
		bool armed = true;
		ModelValueNotificationProbe fault(&Engine::getSong()->tempoModel(), [&] { const auto tempo = Engine::getSong()->getTempo();
			if (armed && tempo == 91) { armed = false; raise(nonstandard); }
		});
		const auto undo = request("throwing-undo", "session.undo"); const auto response = studio.process(undo);
		QVERIFY(!response.value("ok").toBool()); QVERIFY(!armed); QVERIFY(!studio.stopping());
		QCOMPARE(Engine::getSong()->getTempo(), 105); QVERIFY(!Engine::getSong()->isLoadingProject());
		QCOMPARE(Engine::getSong()->tempoModel().isJournalling(), journalling);
		const auto after = manifest(directory); QCOMPARE(after.value("history"), before.value("history"));
		QCOMPARE(after.value("cursor"), before.value("cursor"));
		QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1);
		QVERIFY(studio.process(undo).value("replayed").toBool()); QCOMPARE(Engine::getSong()->getTempo(), 105);
		QVERIFY(studio.process(request("retry-undo", "session.undo")).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getTempo(), 91);
	}
	void initializationExceptionsPreserveSubsequentNativeUndo_data()
	{
		QTest::addColumn<bool>("nonstandard"); QTest::addColumn<bool>("enabled");
		QTest::newRow("enabled-standard") << false << true;
		QTest::newRow("enabled-nonstandard") << true << true;
		QTest::newRow("disabled-standard") << false << false;
		QTest::newRow("disabled-nonstandard") << true << false;
	}
	void initializationExceptionsPreserveSubsequentNativeUndo()
	{
		QFETCH(bool, nonstandard); QFETCH(bool, enabled); IntModel model(10, 0, 100);
		const auto journal = Engine::projectJournal(); const bool previous = journal->isJournalling();
		const auto restoreJournal = qScopeGuard([&] { journal->clearJournal(); journal->setJournalling(previous); });
		journal->clearJournal(); journal->setJournalling(true); model.setJournalling(enabled);
		bool caught = false;
		ModelValueNotificationProbe fault(&model, [&] { raise(nonstandard); });
		try { model.setInitValue(20); } catch (...) { caught = true; }
		QVERIFY(caught); QCOMPARE(model.isJournalling(), enabled); QVERIFY(!journal->canUndo());
		fault.reset();
		model.setValue(30); QCOMPARE(journal->canUndo(), enabled);
		if (enabled)
		{
			journal->undo(); QCOMPARE(model.value(), 20); journal->redo(); QCOMPARE(model.value(), 30);
		}
		else
		{
			model.setJournalling(true); model.setValue(40); QVERIFY(journal->canUndo());
			journal->undo(); QCOMPARE(model.value(), 30);
		}
		QVERIFY(!Engine::getSong()->hasErrors());
	}
	void startupExceptionsPreserveTheLastManifest_data()
	{
		QTest::addColumn<bool>("nonstandard"); QTest::addColumn<bool>("existing");
		QTest::newRow("new-standard") << false << false;
		QTest::newRow("new-nonstandard") << true << false;
		QTest::newRow("existing-standard") << false << true;
		QTest::newRow("existing-nonstandard") << true << true;
	}
	void startupExceptionsPreserveTheLastManifest()
	{
		QFETCH(bool, nonstandard); QFETCH(bool, existing); QTemporaryDir directory;
		if (existing)
		{
			StudioRuntime initial(directory.path()); QString error; QVERIFY2(initial.open(error), qPrintable(error));
			QVERIFY(initial.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool());
		}
		const auto bytes = manifestBytes(directory); int callbacks = 0;
		{
			ModelValueNotificationProbe fault(&Engine::getSong()->tempoModel(), [&] {
				++callbacks; raise(nonstandard);
			}, true);
			StudioRuntime failed(directory.path()); QString error;
			QVERIFY(!failed.open(error)); QVERIFY(!error.isEmpty()); QCOMPARE(callbacks, 1);
			QVERIFY(!failed.process(request("closed", "song.set", {{"tempo", 99}})).value("ok").toBool());
			QCOMPARE(manifestBytes(directory), bytes); QVERIFY(!Engine::getSong()->isLoadingProject());
		}
		StudioRuntime recovered(directory.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->getTempo(), existing ? 91 : DefaultTempo);
	}
	void inferenceExceptionsPreserveCancellationAndAllowResume_data()
	{
		QTest::addColumn<bool>("nonstandard"); QTest::addColumn<bool>("cancelDuring");
		QTest::newRow("pause-standard") << false << false;
		QTest::newRow("pause-nonstandard") << true << false;
		QTest::newRow("cancel-standard") << false << true;
		QTest::newRow("cancel-nonstandard") << true << true;
	}
	void inferenceExceptionsPreserveCancellationAndAllowResume()
	{
		QFETCH(bool, nonstandard); QFETCH(bool, cancelDuring); QTemporaryDir directory; int rounds = 0;
		StudioRuntime* current = nullptr; bool cancelled = false;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (++rounds == 1)
			{
				if (cancelDuring) { cancelled = current->process(request("cancel", "agent.cancel")).value("ok").toBool(); }
				raise(nonstandard);
			}
			if (rounds == 2) { return decision({tool("revised-tempo", "song_set", {{"tempo", 97}})}); }
			return finish();
		});
		current = &studio; QString error; QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(request("provider", "session.provider.set", provider())).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Set the tempo to 97 BPM"}})).value("ok").toBool());
		const auto before = manifest(directory); QVERIFY(!studio.advanceAgent().isEmpty()); QVERIFY(!studio.stopping());
		const auto after = manifest(directory); const auto job = after.value("agent").toObject().value("job").toObject();
		QCOMPARE(job.value("status").toString(), cancelDuring ? QString("cancelled") : QString("paused"));
		QCOMPARE(cancelled, cancelDuring); QCOMPARE(job.value("rounds").toInt(), 1); QCOMPARE(job.value("operations").toInt(), 0);
		QVERIFY(job.value("pending").toArray().isEmpty()); QVERIFY(StudioAgent::validState(after.value("agent").toObject()));
		QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(after.value("revision"), before.value("revision"));
		if (!cancelDuring) { QVERIFY(!job.value("lastError").toString().isEmpty()); }
		QVERIFY(studio.advanceAgent().isEmpty());
		QVERIFY(studio.process(request("resume", "agent.resume")).value("ok").toBool());
		for (int i = 0; i < 4; ++i) { QVERIFY(!studio.advanceAgent().isEmpty()); }
		const auto finished = manifest(directory).value("agent").toObject();
		QVERIFY(StudioAgent::validState(finished)); QCOMPARE(finished.value("job").toObject().value("status").toString(), QString("complete"));
		QCOMPARE(Engine::getSong()->getTempo(), 97); QCOMPARE(rounds, 3);
	}
	void nativeExceptionsDiscardDependentCallsAndReplan_data() { exceptionTypes(); }
	void nativeExceptionsDiscardDependentCallsAndReplan()
	{
		QFETCH(bool, nonstandard); QTemporaryDir directory; int rounds = 0;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (++rounds == 1)
			{
				return decision({tool("bad-tempo", "song_set", {{"tempo", 123}}),
					tool("dependent", "track_add", {{"type", "automation"}})});
			}
			if (rounds == 2) { return decision({tool("revised-tempo", "song_set", {{"tempo", 97}})}); }
			return finish();
		});
		QString error; QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool());
		QVERIFY(studio.process(request("provider", "session.provider.set", provider())).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Revise the project"}})).value("ok").toBool());
		bool armed = true;
		ModelValueNotificationProbe fault(&Engine::getSong()->tempoModel(), [&] { const auto tempo = Engine::getSong()->getTempo();
			if (armed && tempo == 123) { armed = false; raise(nonstandard); }
		});
		QVERIFY(!studio.advanceAgent().isEmpty()); const auto before = manifest(directory);
		QVERIFY(!studio.advanceAgent().isEmpty()); QVERIFY(!studio.stopping()); QVERIFY(!armed);
		const auto after = manifest(directory); const auto job = after.value("agent").toObject().value("job").toObject();
		QVERIFY(job.value("pending").toArray().isEmpty()); QCOMPARE(job.value("operations").toInt(), 1);
		QCOMPARE(job.value("expectedRevision"), after.value("revision"));
		QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1);
		QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(Engine::getSong()->getTempo(), 91);
		QVERIFY(Engine::getSong()->tracks().empty()); int failures = 0;
		for (const auto& value : job.value("records").toArray())
		{
			const auto record = value.toObject(); if (record.value("kind").toString() != "tool") { continue; }
			QVERIFY(!record.value("response").toObject().value("ok").toBool()); ++failures;
		}
		QCOMPARE(failures, 2); QVERIFY(StudioAgent::validState(after.value("agent").toObject()));
		for (int i = 0; i < 4; ++i) { QVERIFY(!studio.advanceAgent().isEmpty()); }
		const auto finished = manifest(directory).value("agent").toObject();
		QVERIFY(StudioAgent::validState(finished)); QCOMPARE(finished.value("job").toObject().value("status").toString(), QString("complete"));
		QCOMPARE(Engine::getSong()->getTempo(), 97); QVERIFY(Engine::getSong()->tracks().empty()); QCOMPARE(rounds, 3);
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::StudioRecoveryTest)
#include "StudioRecoveryTest.moc"
