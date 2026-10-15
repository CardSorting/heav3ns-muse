/* Native agent decisions across project restoration. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PluginFactory.h"
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
QByteArray read(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& data) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size(); }
QJsonObject manifest(const QTemporaryDir& dir) { return QJsonDocument::fromJson(read(dir.filePath("session.json"))).object(); }
QJsonObject job(const QTemporaryDir& dir) { return manifest(dir).value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& dir, const QString& id)
{
	for (const auto& item : job(dir).value("records").toArray())
	{ const auto object = item.toObject(); if (object.value("call").toObject().value("id").toString() == id) { return object; } }
	return {};
}
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
QJsonObject provider(const QString& protocol)
{ return {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-restore-fixture"}}}}; }
QJsonObject namedCall(const QString& id, const QString& operation, const QJsonObject& args, bool catalog)
{
	if (catalog) { return call(id, "studio_execute", {{"op", operation}, {"args", args}}); }
	auto name = operation; name.replace('.', '_'); return call(id, name, args);
}
QJsonArray outputs(const QJsonObject& body, const QString& protocol)
{
	QJsonArray result;
	for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray())
	{
		const auto item = value.toObject();
		if (item.value("role").toString() == "tool" || item.value("type").toString() == "function_call_output")
		{
			result.append(QJsonObject{{"id", item.value(protocol == "chat" ? "tool_call_id" : "call_id")},
				{"response", QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object()}});
		}
	}
	return result;
}
QJsonObject observation(const QJsonObject& body, const QString& protocol)
{
	QJsonObject result;
	for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray())
	{
		const auto text = value.toObject().value("content").toString();
		if (text.startsWith("Native runtime observation (data):\n")) { result = QJsonDocument::fromJson(text.mid(text.indexOf('\n') + 1).toUtf8()).object(); }
	}
	return result;
}
QStringList names()
{ QStringList result; for (auto track : Engine::getSong()->tracks()) { result.append(track->name()); } return result; }
bool accepted(StudioRuntime& studio, const QString& id, const QString& op, const QJsonObject& args, QString& error)
{
	const auto response = studio.process(request(id, op, args)); error = response.value("error").toString(); return response.value("ok").toBool();
}
bool setup(StudioRuntime& studio, const QTemporaryDir& dir, const QString& operation, QJsonObject& arguments, QString& error)
{
	if (!studio.open(error)) { return false; }
	if (!accepted(studio, "alpha", "track.add", {{"type", "instrument"}, {"name", "Alpha"}}, error)
		|| !accepted(studio, "beta", "track.add", {{"type", "instrument"}, {"name", "Beta"}}, error)) { return false; }
	if (operation == "session.undo" || operation == "session.redo")
	{
		if (!accepted(studio, "remove-alpha", "track.remove", {{"track", 0}}, error)) { return false; }
		if (operation == "session.redo" && !accepted(studio, "prepare-redo", "session.undo", {{"summary", true}}, error)) { return false; }
	}
	if (operation == "project.load")
	{
		if (!accepted(studio, "loaded-name", "track.edit", {{"track", 0}, {"name", "Loaded Alpha"}}, error)
			|| !accepted(studio, "save", "project.save", {{"path", dir.filePath("saved.mmp")}}, error)
			|| !accepted(studio, "current-name", "track.edit", {{"track", 0}, {"name", "Alpha"}}, error)) { return false; }
		arguments.insert("path", dir.filePath("saved.mmp"));
	}
	if (operation == "project.new") { arguments.insert("empty", true); }
	if (operation == "project.import")
	{
		const auto midi = QByteArray::fromHex("4d546864000000060000000100304d54726b0000000c00903c6060803c0000ff2f00");
		if (!write(dir.filePath("fixture.mid"), midi)) { error = "Could not write native MIDI-import fixture"; return false; }
		arguments.insert("path", dir.filePath("fixture.mid"));
	}
	arguments.insert("summary", true); return true;
}
void protocols()
{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
}

class StudioAgentRestoreTest : public QObject
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

	void successfulProjectChangeSkipsStaleCallsAndContinuesFromObservation_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("operation"); QTest::addColumn<bool>("catalog");
		for (const auto protocol : {QString("chat"), QString("responses")})
			for (const auto operation : {QString("session.undo"), QString("session.redo"), QString("project.new"), QString("project.load"), QString("project.import"), QString("project.import.pattern")})
				for (bool catalog : {false, true})
				{
					if (catalog && operation.startsWith("session.")) { continue; }
					QTest::newRow(qPrintable(protocol + operation + (catalog ? "-catalog" : "-direct"))) << protocol << operation << catalog;
				}
	}
	void successfulProjectChangeSkipsStaleCallsAndContinuesFromObservation()
	{
		QFETCH(QString, protocol); QFETCH(QString, operation); QFETCH(bool, catalog);
		const bool patternImport = operation == "project.import.pattern";
		if (patternImport) { operation = "project.import"; }
		if (operation == "project.import" && getPluginFactory()->pluginInfo("midiimport").isNull()) { QSKIP("Requires the built native MIDI import plugin"); }
		QTemporaryDir directory; QString error; QJsonObject boundaryArgs; int round = 0, priorParameter = -1; bool paired = false, freshObservation = false;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			if (++round == 1)
			{
				return decision(protocol, {call("before", "project_inspect", {{"section", "summary"}}), namedCall("boundary", operation, boundaryArgs, catalog),
					call("wrong-track", "track_edit", {{"track", 0}, {"name", "Stale edit must not run"}}),
					call("wrong-parameter", "parameter_set", {{"parameter", priorParameter}, {"value", 37}}),
					call("wrong-tempo", "song_set", {{"tempo", 211}}), call("wrong-render", "audio_render", {{"path", directory.filePath("must-not-exist.wav")}}),
					call("wrong-finish", "agent_finish", {{"status", "complete"}, {"summary", "Unobserved result"}, {"evidence", QJsonArray{"boundary"}}})});
			}
			const auto replies = outputs(body, protocol);
			if (round == 2)
			{
				const QStringList ids{"before", "boundary", "wrong-track", "wrong-parameter", "wrong-tempo", "wrong-render", "wrong-finish"};
				paired = replies.size() == ids.size();
				for (int i = 0; i < replies.size() && i < ids.size(); ++i)
				{ const auto reply = replies.at(i).toObject(); paired = paired && reply.value("id").toString() == ids.at(i) && reply.value("response").toObject().value("ok").toBool() == (i < 2); }
				const auto data = observation(body, protocol); freshObservation = data.value("revision") == manifest(directory).value("revision")
					&& data.value("projectSummary").toObject().value("inspection").toString() == "summary"
					&& data.value("projectSummary").toObject().value("tracks").toArray().size() == int(Engine::getSong()->tracks().size());
				const auto headers = data.value("projectSummary").toObject().value("tracks").toArray(); const auto liveNames = names();
				for (int i = 0; i < headers.size() && i < liveNames.size(); ++i)
				{ freshObservation = freshObservation && headers.at(i).toObject().value("namePreview").toString() == liveNames.at(i); }
				return decision(protocol, {call("rediscover", "project_inspect", {{"section", "tracks"}, {"names", false}})});
			}
			if (round == 3)
			{
				const auto response = replies.last().toObject().value("response").toObject(); if (!response.value("ok").toBool()) { throw std::runtime_error("Fresh native discovery failed"); }
				const auto tracks = response.value("result").toObject().value("tracks").toArray();
				return decision(protocol, {tracks.isEmpty() ? call("fresh-edit", "track_add", {{"type", "instrument"}, {"name", "Fresh observed composition"}})
					: call("fresh-edit", "track_edit", {{"track", tracks.at(0).toObject().value("index")}, {"name", "Fresh observed composition"}})});
			}
			return decision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Observed and continued the native composition."}, {"evidence", QJsonArray{"boundary", "rediscover", "fresh-edit"}}})});
		});
		QVERIFY2(setup(studio, directory, operation, boundaryArgs, error), qPrintable(error));
		if (patternImport)
		{
			for (int i = 0; i < 3; ++i)
			{ QVERIFY(accepted(studio, QString("pattern-%1").arg(i), "track.add", {{"type", "pattern"}, {"name", QString("Pattern %1").arg(i)}}, error)); }
			Engine::patternStore()->setCurrentPattern(1);
			QVERIFY(accepted(studio, "pattern-selection", "song.set", {{"tempo", 101}}, error));
			boundaryArgs.insert("container", "pattern"); boundaryArgs.insert("pattern", 2);
		}
		priorParameter = int(static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->pitchModel()->id());
		QVERIFY(studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Restore or import the project, inspect it, and continue editing."}})).value("ok").toBool());
		const auto before = manifest(directory); studio.advanceAgent(); studio.advanceAgent(); studio.advanceAgent();
		const auto state = manifest(directory); const auto current = job(directory); QVERIFY(current.value("pending").toArray().isEmpty());
		QCOMPARE(current.value("operations").toInt(), 2); QCOMPARE(state.value("revision").toInt(), before.value("revision").toInt() + 1);
		QCOMPARE(current.value("expectedRevision"), state.value("revision")); QVERIFY(StudioAgent::validState(state.value("agent").toObject()));
		QVERIFY(record(directory, "boundary").value("response").toObject().value("ok").toBool());
		for (const auto id : {"wrong-track", "wrong-parameter", "wrong-tempo", "wrong-render", "wrong-finish"})
		{
			const auto response = record(directory, id).value("response").toObject(); QVERIFY(response.contains("ok")); QVERIFY(!response.value("ok").toBool());
			QVERIFY(response.value("error").toString().contains("rediscover"));
		}
		QVERIFY(!QFile::exists(directory.filePath("must-not-exist.wav"))); QVERIFY(Engine::getSong()->getTempo() != 211);
		if (operation == "session.undo") { QCOMPARE(names(), (QStringList{"Alpha", "Beta"})); QCOMPARE(state.value("cursor").toInt(), before.value("cursor").toInt() - 1); }
		else if (operation == "session.redo") { QCOMPARE(names(), (QStringList{"Beta"})); QCOMPARE(state.value("cursor").toInt(), before.value("cursor").toInt() + 1); }
		else if (operation == "project.new") { QVERIFY(names().isEmpty()); }
		else if (operation == "project.load") { QCOMPARE(names(), (QStringList{"Loaded Alpha", "Beta"})); }
		else
		{
			QVERIFY(names().size() > 2); QCOMPARE(names().at(0), QString("Alpha")); QCOMPARE(names().at(1), QString("Beta")); bool importedNotes = false;
			auto destination = patternImport ? static_cast<TrackContainer*>(Engine::patternStore()) : Engine::getSong();
			for (auto track : destination->tracks()) { for (auto clip : track->getClips()) { if (auto midi = dynamic_cast<MidiClip*>(clip)) { importedNotes = importedNotes || !midi->notes().empty(); } } }
			QVERIFY(importedNotes);
			if (patternImport)
			{
				QCOMPARE(Engine::patternStore()->currentPattern(), 1); QCOMPARE(names().size(), 5);
				for (auto row : Engine::patternStore()->tracks())
				{
					QCOMPARE(row->numOfClips(), 3);
					if (auto midi = dynamic_cast<MidiClip*>(row->getClips()[2]))
					{ QCOMPARE(midi->notes().size(), std::size_t(1)); QCOMPARE(midi->notes()[0]->key(), 60); }
				}
			}
		}
		if (operation.startsWith("session.")) { QCOMPARE(state.value("history"), before.value("history")); }
		else { QCOMPARE(state.value("history").toArray().size(), before.value("history").toArray().size() + 1); }
		QVERIFY(!studio.process(request("invalid-evidence", "agent.finish", {{"status", "complete"}, {"summary", "No stale work was executed"}, {"evidence", QJsonArray{"wrong-track"}}})).value("ok").toBool());
		for (int step = 0; step < 6; ++step) { studio.advanceAgent(); }
		QVERIFY(paired); QVERIFY(freshObservation); QCOMPARE(names().first(), QString("Fresh observed composition"));
		QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(job(directory).value("operations").toInt(), 5); QVERIFY(!studio.stopping());
	}

	void archivedReplacementResultsKeepTheInvalidationDurable_data() { protocols(); }
	void archivedReplacementResultsKeepTheInvalidationDurable()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; QJsonObject arguments, committed; const QString largeName(300000, 'x');
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				return decision(protocol, {namedCall("restore", "project.load", arguments, true), call("tail-read", "project_inspect", {{"section", "summary"}}),
					call("tail-finish", "agent_finish", {{"status", "complete"}, {"summary", "Unobserved load"}, {"evidence", QJsonArray{"restore"}}})});
			});
			QVERIFY2(setup(studio, directory, "project.load", arguments, error), qPrintable(error)); arguments.remove("summary");
			QVERIFY(accepted(studio, "large-name", "track.edit", {{"track", 0}, {"name", largeName}}, error));
			QVERIFY(accepted(studio, "large-save", "project.save", {{"path", directory.filePath("saved.mmp")}}, error));
			QVERIFY(accepted(studio, "short-name", "track.edit", {{"track", 0}, {"name", "Alpha"}}, error));
			QVERIFY(studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Load the large native project and inspect it."}})).value("ok").toBool());
			studio.advanceAgent(); studio.advanceAgent(); const auto restored = record(directory, "restore");
			QVERIFY(restored.value("response").toObject().value("ok").toBool()); QVERIFY(restored.value("response").toObject().value("storedResult").isObject());
			const auto original = QJsonDocument::fromJson(read(directory.filePath(restored.value("archive").toObject().value("path").toString()))).object();
			QCOMPARE(original.value("call"), restored.value("call"));
			QCOMPARE(original.value("response").toObject().value("result").toObject().value("tracks").toArray().at(0).toObject().value("name").toString(), largeName);
			for (const auto id : {"tail-read", "tail-finish"}) { QVERIFY(!record(directory, id).value("response").toObject().value("ok").toBool()); }
			committed = job(directory); QCOMPARE(committed.value("operations").toInt(), 1); QVERIFY(committed.value("pending").toArray().isEmpty());
			QVERIFY(StudioAgent::active(committed)); QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(names().first(), largeName);
		QCOMPARE(job(directory).value("records"), committed.value("records")); QVERIFY(job(directory).value("pending").toArray().isEmpty());
		QVERIFY(recovered.process(request("usable", "project.inspect", {{"section", "summary"}})).value("ok").toBool()); QVERIFY(!recovered.stopping());
	}

	void committedSkippedDecisionsSurviveRestartAndCannotBeReused_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("operation");
		for (const auto protocol : {QString("chat"), QString("responses")})
			for (const auto operation : {QString("session.undo"), QString("project.load")}) { QTest::newRow(qPrintable(protocol + operation)) << protocol << operation; }
	}
	void committedSkippedDecisionsSurviveRestartAndCannotBeReused()
	{
		QFETCH(QString, protocol); QFETCH(QString, operation); QTemporaryDir directory; QString error; QJsonObject arguments, committed; QStringList restoredNames;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				return decision(protocol, {namedCall("restore", operation, arguments, operation == "project.load"), call("stale", "track_edit", {{"track", 0}, {"name", "Wrong"}})});
			});
			QVERIFY2(setup(studio, directory, operation, arguments, error), qPrintable(error));
			QVERIFY(studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Restore and continue."}})).value("ok").toBool()); studio.advanceAgent(); studio.advanceAgent();
			committed = job(directory); restoredNames = names(); QVERIFY(committed.value("pending").toArray().isEmpty());
			QVERIFY(!record(directory, "stale").value("response").toObject().value("ok").toBool());
		}
		bool observedSkipped = false;
		StudioRuntime recovered(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			const auto responses = outputs(body, protocol); observedSkipped = responses.size() == 2 && !responses.last().toObject().value("response").toObject().value("ok").toBool();
			return decision(protocol, {call("stale", "track_edit", {{"track", 0}, {"name", "Reused skipped call"}})});
		});
		QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(names(), restoredNames); QCOMPARE(job(directory).value("records"), committed.value("records"));
		recovered.advanceAgent(); QVERIFY(observedSkipped); QCOMPARE(names(), restoredNames); QCOMPARE(job(directory).value("operations").toInt(), 1);
		QCOMPARE(job(directory).value("status").toString(), QString("paused")); QVERIFY(job(directory).value("lastError").toString().contains("reused")); QVERIFY(!recovered.stopping());
	}

	void failedManifestCommitCannotPublishReplacementOrSkippedOutcomes_data() { protocols(); }
	void failedManifestCommitCannotPublishReplacementOrSkippedOutcomes()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; QJsonObject arguments, before; QByteArray acknowledged;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				return decision(protocol, {namedCall("restore", "project.load", arguments, true), call("stale", "track_edit", {{"track", 0}, {"name", "Wrong"}})});
			});
			QVERIFY2(setup(studio, directory, "project.load", arguments, error), qPrintable(error));
			QVERIFY(studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Load the saved project."}})).value("ok").toBool()); studio.advanceAgent(); before = manifest(directory);
			const auto path = directory.filePath("session.json"), retained = directory.filePath("acknowledged-session.json"); acknowledged = read(path);
			QVERIFY(QFile::rename(path, retained)); const auto restoreFile = qScopeGuard([&] { QDir(directory.path()).rmdir("session.json"); QFile::rename(retained, path); });
			QVERIFY(QDir(directory.path()).mkdir("session.json")); const auto result = studio.advanceAgent();
			QVERIFY(!result.value("ok").toBool()); QVERIFY(studio.stopping()); QCOMPARE(read(retained), acknowledged); QCOMPARE(names(), (QStringList{"Alpha", "Beta"}));
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(names(), (QStringList{"Alpha", "Beta"}));
		QCOMPARE(manifest(directory).value("history"), before.value("history")); QCOMPARE(job(directory).value("operations").toInt(), 0);
		QVERIFY(job(directory).value("pending").toArray().isEmpty());
		for (const auto id : {"restore", "stale"}) { QVERIFY(!record(directory, id).value("response").toObject().value("ok").toBool()); }
		QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
	}

	void ordinaryCompositionBatchesStillExecuteAllNativeSteps_data() { protocols(); }
	void ordinaryCompositionBatchesStillExecuteAllNativeSteps()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; int round = 0;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (++round == 1) { return decision(protocol, {call("track", "track_add", {{"type", "instrument"}, {"name", "Phrase"}}),
				call("clip", "clip_add", {{"track", 0}, {"position", 0}}), call("note-one", "note_add", {{"track", 0}, {"clip", 0}, {"position", 0}, {"length", 24}, {"key", 60}}),
				call("note-two", "note_add", {{"track", 0}, {"clip", 0}, {"position", 48}, {"length", 24}, {"key", 64}}), call("tempo", "song_set", {{"tempo", 99}})}); }
			return decision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Native editable phrase created."}, {"evidence", QJsonArray{"track", "clip", "note-one", "note-two", "tempo"}}})});
		});
		QVERIFY(studio.open(error)); QVERIFY(studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Create an editable phrase."}})).value("ok").toBool());
		for (int step = 0; step < 8; ++step) { studio.advanceAgent(); }
		QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(job(directory).value("operations").toInt(), 6);
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(static_cast<MidiClip*>(Engine::getSong()->tracks().at(0)->getClip(0))->notes().size(), std::size_t(2));
		QCOMPARE(Engine::getSong()->getTempo(), 99); QVERIFY(!studio.stopping());
	}

	void failedProjectChangeKeepsFailureAndSkipsDependentCalls_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("invalidUndo");
		for (const auto protocol : {QString("chat"), QString("responses")})
			for (bool invalid : {false, true}) { QTest::newRow(qPrintable(protocol + (invalid ? "-undo" : "-load"))) << protocol << invalid; }
	}
	void failedProjectChangeKeepsFailureAndSkipsDependentCalls()
	{
		QFETCH(QString, protocol); QFETCH(bool, invalidUndo); QTemporaryDir directory; QString error;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision(protocol, {invalidUndo ? call("failed", "session_undo", {{"summary", 1}}) : call("failed", "project_load", {{"path", directory.filePath("missing.mmp")}, {"summary", true}}),
				call("dependent", "track_edit", {{"track", 0}, {"name", "Wrong"}})});
		});
		QVERIFY(studio.open(error)); QVERIFY(studio.process(request("track", "track.add", {{"type", "instrument"}, {"name", "Preserved"}})).value("ok").toBool());
		QVERIFY(studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Restore before revising."}})).value("ok").toBool()); const auto before = manifest(directory);
		studio.advanceAgent(); studio.advanceAgent(); QCOMPARE(names(), (QStringList{"Preserved"})); QCOMPARE(job(directory).value("operations").toInt(), 1);
		QCOMPARE(manifest(directory).value("history"), before.value("history")); QVERIFY(job(directory).value("pending").toArray().isEmpty());
		QVERIFY(!record(directory, "failed").value("response").toObject().value("ok").toBool());
		QVERIFY(record(directory, "dependent").value("response").toObject().value("error").toString().contains("Previous native call failed")); QVERIFY(!studio.stopping());
	}
};
QTEST_GUILESS_MAIN(StudioAgentRestoreTest)
#include "StudioAgentRestoreTest.moc"
