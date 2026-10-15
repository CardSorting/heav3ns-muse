/* Native capability preflight, late-field rejection and agent correction.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <algorithm>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <stdexcept>
#include <cmath>
#include <limits>
#include <thread>
#include "FileResourceIO.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "MidiController.h"
#include "Mixer.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "ProjectJournal.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject arguments() { return {{"track", 0}, {"clip", 0}, {"items", QJsonArray{QJsonObject{{"position", 144}, {"length", 24}, {"key", 65}}, QJsonObject{{"position", 120}, {"length", 36}, {"key", 67}, {"volume", 110.9}, {"panning", -20.9}}}}}; }
std::vector<MidiClip::NoteSpec> inputs() { return {{144, 24, 65, 85, -20, Note::Type::Regular}, {120, 36, 67, 86, -19, Note::Type::Regular}}; }
MidiClip* midi(int track = 0, int clip = 0) { return static_cast<MidiClip*>(Engine::getSong()->tracks().at(track)->getClip(clip)); }
void populate(MidiClip* clip)
{
	clip->clearNotes(); for (int index = 0; index < 2; ++index) { auto note = clip->addNote(Note(48, 24 + index * 48, 60, 85 + index, -20 + index), false); note->createDetuning(); note->detuning()->automationClip()->putValue(24, 3.1415927f, false); }
	clip->notes()[0]->detuning()->linkToModel(clip->notes()[1]->detuning().get()); clip->notes()[0]->detuning()->automationClip()->mutedModel()->linkToModel(clip->notes()[1]->detuning()->automationClip()->soloModel());
}
void seed()
{
	auto first = new InstrumentTrack(Engine::getSong()), second = new InstrumentTrack(Engine::getSong()); populate(static_cast<MidiClip*>(first->createClip(0))); second->createClip(0);
}
bool linked(const Note* a, const Note* b)
{
	if (!a->detuning() || !b->detuning() || !a->detuning()->isLinkedToModel(b->detuning().get())) { return false; }
	const auto left = a->detuning()->automationClip(), right = b->detuning()->automationClip();
	return left->mutedModel()->isLinkedToModel(right->soloModel()) || right->mutedModel()->isLinkedToModel(left->soloModel());
}
QJsonObject call(const QString& id, const QString& op, const QJsonObject& args, bool catalog = false)
{ if (catalog) { return {{"id", id}, {"name", "studio_execute"}, {"args", QJsonObject{{"op", op}, {"args", args}}}}; } auto name = op; name.replace('.', '_'); return {{"id", id}, {"name", name}, {"args", args}}; }
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output; for (const auto& value : calls) { const auto item = value.toObject(); const auto args = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact)); if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", args}}); } else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", args}}}}); } }
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; } return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-capability-preflight-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioCapabilityPreflightTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing(); journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::projectJournal()->clearJournal(); Engine::projectJournal()->setJournalling(false); }
	void cleanup() { Engine::projectJournal()->setJournalling(false); Engine::projectJournal()->clearJournal(); Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void lateSongFieldsRefuseBeforeTempoChanges_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("master-volume") << 0; QTest::newRow("master-pitch") << 1; QTest::newRow("fractional-volume") << 2; }
	void lateSongFieldsRefuseBeforeTempoChanges()
	{
		QFETCH(int, failure); auto song = Engine::getSong(); song->setTempo(120); const auto volume = song->masterVolume(); const auto pitch = song->masterPitch(); QJsonObject args{{"tempo", 140}}; args.insert(failure == 1 ? "masterPitch" : "masterVolume", failure == 0 ? 201. : failure == 1 ? 13. : 100.5); const auto response = StudioCapabilities::execute("song.set", args); QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("args.master")); QCOMPARE(song->getTempo(), 120); QCOMPARE(song->masterVolume(), volume); QCOMPARE(song->masterPitch(), pitch); QVERIFY(StudioCapabilities::execute("song.set", {{"tempo", 125}, {"masterVolume", 90}, {"masterPitch", 2}}).value("ok").toBool()); QCOMPARE(song->getTempo(), 125); QCOMPARE(song->masterVolume(), 90); QCOMPARE(song->masterPitch(), 2);
	}
	void invalidCreationNamesDoNotPublishObjects_data()
	{ QTest::addColumn<int>("mode"); QTest::newRow("track") << 0; QTest::newRow("clip") << 1; QTest::newRow("controller") << 2; QTest::newRow("mixer") << 3; }
	void invalidCreationNamesDoNotPublishObjects()
	{
		QFETCH(int, mode); seed(); const auto tracks = Engine::getSong()->tracks().size(); const auto controllers = Engine::getSong()->controllers().size(); const int clips = midi()->getTrack()->numOfClips(), channels = Engine::mixer()->numChannels(); QString operation; QJsonObject args{{"name", ""}}; if (mode == 0) { operation = "track.add"; args.insert("type", "instrument"); } if (mode == 1) { operation = "clip.add"; args.insert("track", 0); args.insert("position", 192); } if (mode == 2) { operation = "controller.add"; args.insert("type", "lfo"); } if (mode == 3) { operation = "mixer.add"; } const auto response = StudioCapabilities::execute(operation, args); QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("args.name")); QCOMPARE(Engine::getSong()->tracks().size(), tracks); QCOMPARE(Engine::getSong()->controllers().size(), controllers); QCOMPARE(midi()->getTrack()->numOfClips(), clips); QCOMPARE(int(Engine::mixer()->numChannels()), channels);
	}
	void invalidLateClipNameDoesNotMoveOrResizeTheClip()
	{
		seed(); auto clip = midi(); const auto position = clip->startPosition(), length = clip->length(); const auto response = StudioCapabilities::execute("clip.edit", {{"track", 0}, {"clip", 0}, {"position", 384}, {"length", 96}, {"name", ""}}); QVERIFY(!response.value("ok").toBool()); QCOMPARE(clip->startPosition(), position); QCOMPARE(clip->length(), length);
	}
	void invalidNestedAttributesRefuseBeforePluginLookup()
	{
		seed(); const auto response = StudioCapabilities::execute("effect.add", {{"track", 0}, {"plugin", "deliberately-missing"}, {"attributes", QJsonObject{{"vendor", 1}}}}); QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("args.attributes.vendor"));
	}
	void runtimeCachesPreflightFailureAndPreservesSongState()
	{
		QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); const auto command = request("reject", "song.set", {{"tempo", 140}, {"masterVolume", 201}}); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QCOMPARE(Engine::getSong()->getTempo(), 123); QCOMPARE(runtime.process(command), response); QCOMPARE(Engine::getSong()->getTempo(), 123); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsCorrectRejectedFieldsThroughBothRoutes_data() { protocols(); }
	void scriptedAgentsCorrectRejectedFieldsThroughBothRoutes()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false, unchangedAtCorrection = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Change tempo and gain"}, {"steps", QJsonArray{"Set native song values"}}}), call("bad", "song.set", {{"tempo", 140}, {"masterVolume", 201}}, catalog), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Rejected values"}, {"evidence", QJsonArray{"bad"}}})}); } unchangedAtCorrection = Engine::getSong()->getTempo() == 123; return decision(protocol, {call("good", "song.set", {{"tempo", 130}, {"masterVolume", 90}}, catalog), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native fields corrected"}, {"evidence", QJsonArray{"good"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Set this song's tempo and gain."}})).value("ok").toBool()); for (int index = 0; index < 8; ++index) { runtime.advanceAgent(); } QVERIFY(!record(directory, "bad").value("ok").toBool()); QVERIFY(record(directory, "good").value("ok").toBool()); QVERIFY(unchangedAtCorrection); QCOMPARE(Engine::getSong()->getTempo(), 130); QCOMPARE(Engine::getSong()->masterVolume(), 90); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void cancelledPreflightDoesNotPublishNativeNotes()
	{
		seed(); const auto roster = midi()->notes(); int checks = 0; const auto response = StudioCapabilities::execute("note.batch.add", arguments(), [&] { ++checks; return true; }); QVERIFY(!response.value("ok").toBool()); QVERIFY(checks > 0); QVERIFY(response.value("error").toString().contains("cancelled")); QCOMPARE(midi()->notes(), roster);
	}
};
QTEST_GUILESS_MAIN(StudioCapabilityPreflightTest)
#include "StudioCapabilityPreflightTest.moc"
