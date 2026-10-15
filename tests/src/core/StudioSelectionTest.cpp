/* Native queued-call target lifetimes. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Controller.h"
#include "DummyEffect.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Mixer.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "Track.h"

using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
{ return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject call(const QString& id, const QString& op, const QJsonObject& args = {}, bool catalog = false)
{
	if (catalog) { return {{"id", id}, {"name", "studio_execute"}, {"args", QJsonObject{{"op", op}, {"args", args}}}}; }
	auto name = op; name.replace('.', '_'); return {{"id", id}, {"name", name}, {"args", args}};
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
QJsonObject manifest(const QTemporaryDir& dir) { return QJsonDocument::fromJson(read(dir.filePath("session.json"))).object(); }
QJsonObject job(const QTemporaryDir& dir) { return manifest(dir).value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& dir, const QString& id)
{
	for (const auto& value : job(dir).value("records").toArray())
	{ const auto item = value.toObject(); if (item.value("call").toObject().value("id").toString() == id) { return item.value("response").toObject(); } }
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
{ return {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-selection-fixture"}}}}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
void native(const QString& op, const QJsonObject& args)
{
	const auto response = StudioCapabilities::execute(op, args);
	if (!response.value("ok").toBool()) { throw std::runtime_error(response.value("error").toString().toStdString()); }
}
struct Fixture { QJsonObject change, stale; };
Fixture seed(const QString& kind, bool catalog)
{
	const QJsonObject owner{{"track", 0}, {"clip", 0}};
	for (const auto name : {"Alpha", "Beta"}) { native("track.add", {{"type", "instrument"}, {"name", name}}); }
	for (int track = 0; track < 2; ++track)
		for (int clip = 0; clip < 2; ++clip) { native("clip.add", {{"track", track}, {"position", clip * 192}, {"name", QString("Clip %1").arg(clip)}}); }
	native("note.clear", owner);
	for (int index = 0; index < 2; ++index) { native("note.add", {{"track", 0}, {"clip", 0}, {"position", 48 + index * 48}, {"length", 24}, {"key", 60 + index * 4}}); }
	const auto pair = [&](const QString& op, const QJsonObject& args, const QString& target, const QJsonObject& targetArgs) {
		return Fixture{call("change", op, args, catalog), call("stale", target, targetArgs, catalog)};
	};
	const QJsonObject staleNote{{"track", 0}, {"clip", 0}, {"note", 0}, {"key", 99}};
	if (kind == "track-remove") { return pair("track.remove", {{"track", 0}}, "track.edit", {{"track", 0}, {"name", "Wrong"}}); }
	if (kind == "track-move") { return pair("track.edit", {{"track", 0}, {"to", 1}}, "clip.edit", {{"track", 0}, {"clip", 0}, {"name", "Wrong"}}); }
	if (kind == "clip-remove") { return pair("clip.remove", owner, "clip.edit", {{"track", 0}, {"clip", 0}, {"name", "Wrong"}}); }
	if (kind == "note-add") { return pair("note.add", {{"track", 0}, {"clip", 0}, {"position", 0}, {"length", 24}, {"key", 48}}, "note.edit", staleNote); }
	if (kind == "note-remove") { return pair("note.remove", {{"track", 0}, {"clip", 0}, {"note", 0}}, "note.edit", staleNote); }
	if (kind == "note-move") { return pair("note.edit", {{"track", 0}, {"clip", 0}, {"note", 0}, {"position", 144}}, "note.edit", staleNote); }
	if (kind == "note-clear") { return pair("note.clear", owner, "note.edit", staleNote); }
	if (kind == "parameter-delete")
	{
		const auto instrument = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		return pair("track.remove", {{"track", 0}}, "parameter.set", {{"parameter", static_cast<int>(instrument->pitchModel()->id())}, {"value", 37}});
	}
	if (kind == "controller-remove")
	{
		for (const auto name : {"First", "Second"}) { native("controller.add", {{"type", "lfo"}, {"name", name}, {"summary", true}}); }
		return pair("controller.remove", {{"controller", 0}}, "controller.edit", {{"controller", 0}, {"name", "Wrong"}});
	}
	if (kind.startsWith("pattern-"))
	{
		for (const auto name : {"Pattern A", "Pattern B"}) { native("track.add", {{"container", "pattern"}, {"type", "instrument"}, {"name", name}}); }
		for (const auto name : {"Beat A", "Beat B"}) { native("track.add", {{"type", "pattern"}, {"name", name}}); }
		if (kind == "pattern-row") { return pair("track.edit", {{"container", "pattern"}, {"track", 0}, {"to", 1}}, "track.edit", {{"container", "pattern"}, {"track", 0}, {"name", "Wrong"}}); }
		return pair("track.remove", {{"track", 2}}, "clip.edit", {{"container", "pattern"}, {"track", 0}, {"clip", 0}, {"name", "Wrong"}});
	}
	for (const auto name : {"Bus A", "Bus B"}) { native("mixer.add", {{"name", name}}); }
	if (kind == "mixer-remove") { return pair("mixer.remove", {{"channel", 1}}, "mixer.edit", {{"channel", 1}, {"name", "Wrong"}}); }
	if (kind == "mixer-move") { return pair("mixer.edit", {{"channel", 1}, {"to", 2}}, "mixer.edit", {{"channel", 1}, {"name", "Wrong"}}); }
	if (kind == "route-from") { return pair("mixer.edit", {{"channel", 1}, {"to", 2}}, "parameter.list", {{"from", 1}, {"choices", false}}); }
	if (kind == "route-to") { return pair("mixer.edit", {{"channel", 1}, {"to", 2}}, "parameter.list", {{"to", 1}, {"choices", false}}); }
	if (kind == "route-reset") { return pair("mixer.reset", {{"channel", 1}}, "mixer.route", {{"from", 1}, {"to", 0}, {"amount", 0.25}}); }
	const auto chain = &Engine::mixer()->mixerChannel(0)->m_fxChain;
	for (int index = 0; index < 2; ++index)
	{
		QDomDocument document; auto element = document.createElement("effect"); element.setAttribute("name", QString("missing-selection-fixture-%1").arg(index));
		std::unique_ptr<Effect> effect = std::make_unique<DummyEffect>(chain, element);
		if (!chain->appendEffect(effect)) { throw std::runtime_error("Fixture chain rejected an effect"); }
	}
	if (kind == "effect-remove") { return pair("effect.remove", {{"channel", 0}, {"effect", 0}}, "effect.remove", {{"channel", 0}, {"effect", 0}}); }
	if (kind == "effect-move") { return pair("effect.move", {{"channel", 0}, {"effect", 0}, {"to", 1}}, "effect.remove", {{"channel", 0}, {"effect", 0}}); }
	return pair("effect.clear", {{"channel", 0}}, "effect.inspect", {{"channel", 0}, {"effect", 0}, {"details", false}});
}
}

class StudioSelectionTest : public QObject
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

	void structuralChangesSkipTheBatchAndContinueFromObservation_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("kind");
		for (const auto protocol : {QString("chat"), QString("responses")})
			for (const auto kind : {"track-remove", "track-move", "clip-remove", "note-add", "note-remove", "note-move", "note-clear",
				"mixer-remove", "mixer-move", "route-from", "route-to", "route-reset", "effect-remove", "effect-move", "effect-clear",
				"controller-remove", "parameter-delete", "pattern-cell", "pattern-row"})
			{ QTest::newRow(qPrintable(protocol + "-" + kind)) << protocol << QString(kind); }
	}
	void structuralChangesSkipTheBatchAndContinueFromObservation()
	{
		QFETCH(QString, protocol); QFETCH(QString, kind); QTemporaryDir directory; QString error; Fixture fixture; int round = 0; bool observed = false, paired = false;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			if (++round == 1) { return decision(protocol, {fixture.change, fixture.stale, call("unrelated", "song.set", {{"tempo", 211}}),
				call("premature", "agent.finish", {{"status", "complete"}, {"summary", "Unobserved"}, {"evidence", QJsonArray{"change"}}})}); }
			QJsonArray replies;
			for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray())
			{
				const auto item = value.toObject(); const auto content = item.value("content").toString();
				if (content.startsWith("Native runtime observation (data):\n"))
				{
					const auto data = QJsonDocument::fromJson(content.mid(content.indexOf('\n') + 1).toUtf8()).object();
					observed = data.value("revision") == manifest(directory).value("revision")
						&& data.value("projectSummary").toObject() == StudioCapabilities::inspectSummary();
				}
				if (item.value("role").toString() == "tool" || item.value("type").toString() == "function_call_output")
				{ replies.append(QJsonObject{{"id", item.value(protocol == "chat" ? "tool_call_id" : "call_id")}, {"result", QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object()}}); }
			}
			const QStringList ids{"change", "stale", "unrelated", "premature"}; paired = replies.size() == ids.size();
			for (int i = 0; i < replies.size() && i < ids.size(); ++i)
			{ paired = paired && replies.at(i).toObject().value("id").toString() == ids.at(i) && replies.at(i).toObject().value("result").toObject().value("ok").toBool() == (i == 0); }
			return decision(protocol, {call("fresh", "song.set", {{"tempo", 123}}),
				call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Continued from native observation"}, {"evidence", QJsonArray{"change", "fresh"}}})});
		});
		QVERIFY2(studio.open(error), qPrintable(error)); fixture = seed(kind, protocol == "responses");
		QVERIFY(studio.process(request("seed", "song.set", {{"tempo", 101}})).value("ok").toBool());
		QVERIFY(studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Revise the native arrangement."}})).value("ok").toBool());
		const auto revision = manifest(directory).value("revision").toInt(); studio.advanceAgent(); studio.advanceAgent();
		QVERIFY2(record(directory, "change").value("ok").toBool(), qPrintable(record(directory, "change").value("error").toString()));
		QCOMPARE(manifest(directory).value("revision").toInt(), revision + 1); QCOMPARE(job(directory).value("operations").toInt(), 1);
		QVERIFY(job(directory).value("pending").toArray().isEmpty()); QVERIFY(StudioAgent::active(job(directory))); QCOMPARE(Engine::getSong()->getTempo(), 101);
		for (const auto id : {"stale", "unrelated", "premature"})
		{ QVERIFY(!record(directory, id).value("ok").toBool()); QVERIFY(record(directory, id).value("error").toString().contains("changed a queued native selection")); }
		for (const auto track : Engine::getSong()->tracks())
		{
			QVERIFY(track->name() != "Wrong");
			for (const auto clip : track->getClips())
			{ QVERIFY(clip->name() != "Wrong"); if (const auto midi = dynamic_cast<MidiClip*>(clip)) { for (const auto note : midi->notes()) { QVERIFY(note->key() != 99); } } }
		}
		for (const auto controller : Engine::getSong()->controllers()) { QVERIFY(controller->name() != "Wrong"); }
		for (int i = 0; i < Engine::mixer()->numChannels(); ++i) { QVERIFY(Engine::mixer()->mixerChannel(i)->m_name != "Wrong"); }
		if (kind == "effect-remove") { QCOMPARE(Engine::mixer()->mixerChannel(0)->m_fxChain.effects().size(), std::size_t(1)); }
		if (kind == "effect-move") { QCOMPARE(Engine::mixer()->mixerChannel(0)->m_fxChain.effects().size(), std::size_t(2)); }
		if (kind == "route-reset") { QCOMPARE(Engine::mixer()->channelSendModel(1, 0)->value(), 1.f); }
		for (int step = 0; step < 3; ++step) { studio.advanceAgent(); }
		QVERIFY(paired); QVERIFY(observed); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
		QCOMPARE(job(directory).value("operations").toInt(), 3); QCOMPARE(Engine::getSong()->getTempo(), 123); QVERIFY(!studio.stopping());
	}

	void noteIdentitySurvivesAssignmentButNeverCopiesOrAddressReuse()
	{
		alignas(Note) unsigned char storage[sizeof(Note)];
		auto first = std::construct_at(reinterpret_cast<Note*>(storage)); Note* live = first;
		const auto cleanup = qScopeGuard([&] { if (live) { std::destroy_at(live); } });
		const auto identity = first->selectionIdentity();
		Note copy(*first); QVERIFY(copy.selectionIdentity() != identity);
		std::unique_ptr<Note> clone(first->clone()); QVERIFY(clone->selectionIdentity() != identity);
		*first = copy; QVERIFY(first->selectionIdentity() == identity);
		std::destroy_at(first); live = nullptr; live = std::construct_at(reinterpret_cast<Note*>(storage));
		QVERIFY(live->selectionIdentity() != identity);
	}

	void globalAndPatternSelectionFollowNativeLifetime()
	{
		seed("pattern-row", false);
		const QJsonObject explicitPattern{{"playMode", "pattern"}, {"pattern", 0}}, selectedPattern{{"playMode", "pattern"}};
		const auto explicitBefore = StudioCapabilities::selection("transport.play", explicitPattern);
		const auto selectedBefore = StudioCapabilities::selection("transport.play", selectedPattern);
		native("track.remove", {{"track", 2}});
		QVERIFY(explicitBefore.changed(StudioCapabilities::selection("transport.play", explicitPattern)));
		QVERIFY(!selectedBefore.changed(StudioCapabilities::selection("transport.play", selectedPattern)));
		native("track.add", {{"type", "pattern"}});
		QVERIFY(selectedBefore.changed(StudioCapabilities::selection("transport.play", selectedPattern)));
		const auto globals = Engine::getSong()->globalAutomationTrack();
		globals->createClip(TimePos(0)); globals->createClip(TimePos(0));
		const auto curve = StudioCapabilities::selection("automation.global.inspect", {{"global", 0}});
		delete globals->getClips().front();
		QVERIFY(curve.changed(StudioCapabilities::selection("automation.global.inspect", {{"global", 0}})));
	}

	void destinationSelectorsAndInvalidFutureIndices()
	{
		seed("track-remove", false);
		const QJsonObject clone{{"track", 0}, {"clip", 0}, {"targetTrack", 1}, {"position", 0}};
		const QJsonObject slices{{"track", 0}, {"clip", 0}, {"targetTrack", 1}};
		const auto source = StudioCapabilities::selection("clip.clone", clone), target = StudioCapabilities::selection("clip.fromSlices", slices);
		native("clip.remove", {{"track", 1}, {"clip", 0}});
		QVERIFY(!source.changed(StudioCapabilities::selection("clip.clone", clone)));
		QVERIFY(target.changed(StudioCapabilities::selection("clip.fromSlices", slices)));
		native("track.remove", {{"track", 1}}); QVERIFY(source.changed(StudioCapabilities::selection("clip.clone", clone)));
		const auto missing = StudioCapabilities::selection("track.edit", {{"track", 1}});
		native("track.add", {{"type", "instrument"}});
		QVERIFY(!missing.changed(StudioCapabilities::selection("track.edit", {{"track", 1}})));
		for (const auto value : {QJsonValue(-1), QJsonValue(0.5), QJsonValue("0"), QJsonValue(2147483648.)})
		{
			const auto invalid = StudioCapabilities::selection("note.edit", {{"track", value}, {"clip", value}, {"note", value}});
			QVERIFY(!invalid.changed(StudioCapabilities::selection("note.edit", {{"track", value}, {"clip", value}, {"note", value}})));
		}
	}

	void stableTargetsAndNewObjectsKeepExecuting_data() { protocols(); }
	void stableTargetsAndNewObjectsKeepExecuting()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision(protocol, {call("remove-last", "track.remove", {{"track", 1}}), call("retained", "track.edit", {{"track", 0}, {"name", "Retained"}}),
				call("append", "note.add", {{"track", 0}, {"clip", 0}, {"position", 192}, {"length", 24}, {"key", 67}}),
				call("old-note", "note.edit", {{"track", 0}, {"clip", 0}, {"note", 0}, {"key", 61}}),
				call("new-clip", "clip.add", {{"track", 0}, {"position", 384}}), call("edit-new", "clip.edit", {{"track", 0}, {"clip", 2}, {"name", "New clip"}}),
				call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Stable selections preserved"}, {"evidence", QJsonArray{"retained", "old-note", "edit-new"}}})});
		});
		QVERIFY(studio.open(error)); seed("track-remove", false); QVERIFY(studio.process(request("seed", "song.set", {{"tempo", 101}})).value("ok").toBool());
		QVERIFY(studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Continue editing unchanged targets."}})).value("ok").toBool());
		for (int i = 0; i < 8; ++i) { studio.advanceAgent(); }
		QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(job(directory).value("operations").toInt(), 7);
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); const auto track = Engine::getSong()->tracks().at(0);
		QCOMPARE(track->name(), QString("Retained")); QCOMPARE(track->getClip(2)->name(), QString("New clip"));
		QCOMPARE(static_cast<MidiClip*>(track->getClip(0))->notes().at(0)->key(), 61);
		for (const auto value : job(directory).value("records").toArray()) { QVERIFY(value.toObject().value("response").toObject().value("ok").toBool()); }
	}

	void skippedStructuralCallsRemainCommittedAcrossRestart_data() { protocols(); }
	void skippedStructuralCallsRemainCommittedAcrossRestart()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; QJsonValue records;
		{
			Fixture fixture; StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {fixture.change, fixture.stale}); });
			QVERIFY(studio.open(error)); fixture = seed("track-remove", true); QVERIFY(studio.process(request("seed", "song.set", {{"tempo", 101}})).value("ok").toBool());
			QVERIFY(studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Remove Alpha."}})).value("ok").toBool());
			studio.advanceAgent(); studio.advanceAgent(); records = job(directory).value("records");
			QVERIFY(record(directory, "change").value("ok").toBool()); QVERIFY(!record(directory, "stale").value("ok").toBool());
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(job(directory).value("records"), records); QCOMPARE(job(directory).value("operations").toInt(), 1);
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks().at(0)->name(), QString("Beta"));
		QVERIFY(job(directory).value("pending").toArray().isEmpty()); QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
	}

	void failedCommitRollsBackTheStructuralChangeAndSkippedOutputs_data() { protocols(); }
	void failedCommitRollsBackTheStructuralChangeAndSkippedOutputs()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; QByteArray acknowledged;
		{
			Fixture fixture; StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {fixture.change, fixture.stale}); });
			QVERIFY(studio.open(error)); fixture = seed("track-remove", true); QVERIFY(studio.process(request("seed", "song.set", {{"tempo", 101}})).value("ok").toBool());
			QVERIFY(studio.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool());
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Remove Alpha."}})).value("ok").toBool()); studio.advanceAgent();
			const auto path = directory.filePath("session.json"), retained = directory.filePath("acknowledged.json"); acknowledged = read(path);
			QVERIFY(QFile::rename(path, retained)); const auto restoreFile = qScopeGuard([&] { QDir(directory.path()).rmdir("session.json"); QFile::rename(retained, path); });
			QVERIFY(QDir(directory.path()).mkdir("session.json")); QVERIFY(!studio.advanceAgent().value("ok").toBool()); QVERIFY(studio.stopping());
			QCOMPARE(read(retained), acknowledged); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(2)); QCOMPARE(Engine::getSong()->tracks().at(0)->name(), QString("Alpha"));
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(job(directory).value("operations").toInt(), 0); QCOMPARE(Engine::getSong()->tracks().at(0)->name(), QString("Alpha"));
		for (const auto id : {"change", "stale"}) { QVERIFY(!record(directory, id).value("ok").toBool()); }
		QVERIFY(job(directory).value("pending").toArray().isEmpty());
	}
};
QTEST_GUILESS_MAIN(StudioSelectionTest)
#include "StudioSelectionTest.moc"
