/* Checked native clip properties, virtual offset behavior, history and recovery.
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
#include "AutomationClip.h"
#include "PatternClip.h"
#include "SampleClip.h"
#include "SampleBuffer.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "MidiController.h"
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
QJsonObject arguments() { return {{"track", 0}, {"clip", 0}, {"position", 192}, {"length", 96}, {"offset", -12}, {"name", "Edited"}, {"muted", true}}; }
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
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-clip-edit-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
namespace
{
Clip* typedClip(int type)
{
	const auto kind = type == 0 ? Track::Type::Instrument : type == 1 ? Track::Type::Sample : type == 2 ? Track::Type::Automation : Track::Type::Pattern;
	auto clip = Track::create(kind, Engine::getSong())->createClip(0);
	if (type == 0) { populate(static_cast<MidiClip*>(clip)); }
	if (type == 1) { std::vector<SampleFrame> frames(48000); for (std::size_t index = 0; index < frames.size(); ++index) { frames[index][0] = (index % 97) / 1000.f; frames[index][1] = -frames[index][0]; } static_cast<SampleClip*>(clip)->setSampleBuffer(std::make_shared<SampleBuffer>(std::move(frames), 48000)); }
	return clip;
}
void types() { QTest::addColumn<int>("type"); QTest::newRow("midi") << 0; QTest::newRow("sample") << 1; QTest::newRow("automation") << 2; QTest::newRow("pattern") << 3; }
Clip::EditOptions options() { Clip::EditOptions result; result.position = 192; result.length = 96; result.offset = -12; result.name = QString("Edited"); result.muted = true; return result; }
}
class StudioClipEditTest : public QObject
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

	void nativeFieldsKeepTheOriginalClipAndNativeContent_data() { types(); }
	void nativeFieldsKeepTheOriginalClipAndNativeContent()
	{
		QFETCH(int, type); auto clip = typedClip(type); const auto id = clip->id(); NoteVector notes; if (type == 0) { notes = static_cast<MidiClip*>(clip)->notes(); } const auto buffer = type == 1 ? static_cast<SampleClip*>(clip)->sample().buffer() : std::shared_ptr<const SampleBuffer>{}; bool changed = false; QString error; QVERIFY2(clip->edit(options(), changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(clip->id(), id); QCOMPARE(clip->startPosition().getTicks(), 192); QCOMPARE(clip->length().getTicks(), 96); QCOMPARE(clip->startTimeOffset().getTicks(), -12); QVERIFY(!clip->getAutoResize()); QCOMPARE(clip->name(), QString("Edited")); QCOMPARE(clip->mutedModel()->storedValue(), 1.f); if (type == 0) { QCOMPARE(static_cast<MidiClip*>(clip)->notes(), notes); QVERIFY(linked(notes[0], notes[1])); } if (type == 1) { QCOMPARE(static_cast<SampleClip*>(clip)->sample().buffer(), buffer); }
	}
	void sampleOffsetRetainsNativeFrameSemanticsAcrossTempoChange()
	{
		const auto tempo = Engine::getSong()->getTempo(); const auto restore = qScopeGuard([&] { Engine::getSong()->setTempo(tempo); }); Engine::getSong()->setTempo(120); auto clip = static_cast<SampleClip*>(typedClip(1)); QVERIFY(clip->setSampleWindow(100, 47000)); const auto buffer = clip->sample().buffer(); Clip::EditOptions edit; edit.length = 48; edit.offset = -12; bool changed = false; QString error; QVERIFY(clip->edit(edit, changed, error)); const auto playback = clip->playbackSample(0); QVERIFY(playback.has_value()); QCOMPARE(playback->startFrame(), 6100); QCOMPARE(playback->endFrame(), 30100); Engine::getSong()->setTempo(240); QCOMPARE(clip->startTimeOffset().getTicks(), -24); QCOMPARE(clip->length().getTicks(), 48); QCOMPARE(clip->sample().buffer(), buffer);
	}
	void emptyAndEqualEditsAvoidNativeHistory_data()
	{ QTest::addColumn<bool>("equal"); QTest::newRow("empty") << false; QTest::newRow("equal") << true; }
	void emptyAndEqualEditsAvoidNativeHistory()
	{
		QFETCH(bool, equal); auto clip = typedClip(0); clip->setAutoResize(false); Clip::EditOptions edit; if (equal) { edit.position = clip->startPosition().getTicks(); edit.length = clip->length().getTicks(); edit.offset = clip->startTimeOffset().getTicks(); edit.autoResize = false; edit.name = clip->name(); edit.muted = false; } Engine::projectJournal()->setJournalling(true); int notifications = 0; const auto connection = QObject::connect(clip, &Clip::dataChanged, this, [&] { ++notifications; }); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error; QVERIFY(clip->edit(edit, changed, error)); QVERIFY(!changed); QCOMPARE(notifications, 0); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void zeroLengthNativePatternClipStillAllowsMetadataEditing()
	{
		auto clip = typedClip(3); clip->changeLength(0); Clip::EditOptions edit; edit.name = QString("Empty pattern"); bool changed = false; QString error; QVERIFY2(clip->edit(edit, changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(clip->length().getTicks(), 0); QCOMPARE(clip->name(), QString("Empty pattern"));
	}
	void explicitAutomaticRefreshUsesNativeLengthAndOffsetRules()
	{
		auto clip = static_cast<MidiClip*>(typedClip(0)); clip->setAutoResize(false); clip->changeLength(96); clip->setStartTimeOffset(-12); Clip::EditOptions edit; edit.autoResize = true; bool changed = false; QString error; QVERIFY(clip->edit(edit, changed, error)); QVERIFY(changed); QCOMPARE(clip->length().getTicks(), TimePos::ticksPerBar()); QCOMPARE(clip->startTimeOffset().getTicks(), 0); QVERIFY(clip->getAutoResize()); QVERIFY(clip->edit(edit, changed, error)); QVERIFY(changed);
	}
	void oneNativeUndoRestoresCompleteFieldsAndLinkedControls_data() { types(); }
	void oneNativeUndoRestoresCompleteFieldsAndLinkedControls()
	{
		QFETCH(int, type); auto clip = typedClip(type); const auto size = clip->length(); const auto name = clip->name(); QVERIFY(clip->mutedModel()->linkToModel(clip->soloModel())); auto journal = Engine::projectJournal(); journal->setJournalling(true); bool changed = false; QString error; QVERIFY2(clip->edit(options(), changed, error), qPrintable(error)); QVERIFY(journal->isJournalling()); QVERIFY(journal->canUndo()); QCOMPARE(clip->soloModel()->storedValue(), 1.f); journal->undo(); QCOMPARE(clip->startPosition().getTicks(), 0); QCOMPARE(clip->length(), size); QCOMPARE(clip->startTimeOffset().getTicks(), 0); QCOMPARE(clip->name(), name); QVERIFY(clip->getAutoResize()); QCOMPARE(clip->mutedModel()->storedValue(), 0.f); QCOMPARE(clip->soloModel()->storedValue(), 0.f); QVERIFY(clip->mutedModel()->isLinkedToModel(clip->soloModel())); QVERIFY(!journal->canUndo()); journal->redo(); QCOMPARE(clip->startPosition().getTicks(), 192); QCOMPARE(clip->length().getTicks(), 96); QCOMPARE(clip->startTimeOffset().getTicks(), -12); QCOMPARE(clip->name(), QString("Edited")); QVERIFY(!clip->getAutoResize()); QCOMPARE(clip->mutedModel()->storedValue(), 1.f);
	}
	void nativeBoundsPatternOwnershipAndHistoryRefuseBeforeWrites_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("negative-position") << 0; QTest::newRow("zero-length") << 1; QTest::newRow("large-offset") << 2; QTest::newRow("endpoint") << 3; QTest::newRow("minimum-offset") << 4; QTest::newRow("refresh-with-length") << 5; QTest::newRow("refresh-with-offset") << 6; QTest::newRow("zero-refresh-meter") << 7; QTest::newRow("pattern-cell-position") << 8; QTest::newRow("history-outside") << 9; }
	void nativeBoundsPatternOwnershipAndHistoryRefuseBeforeWrites()
	{
		QFETCH(int, failure); Clip* clip; if (failure == 8) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(static_cast<MidiClip*>(clip)); } else { clip = typedClip(0); } const int meter = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(meter); }); auto edit = options(); if (failure == 0) { edit.position = -1; } if (failure == 1) { edit.length = 0; } if (failure == 2) { edit.offset = std::numeric_limits<int>::max(); } if (failure == 3) { edit.position = MaxSongLength - 1; } if (failure == 4) { edit.offset = std::numeric_limits<int>::min(); } if (failure == 5) { edit.offset.reset(); edit.autoResize = true; } if (failure == 6) { edit.length.reset(); edit.autoResize = true; } if (failure == 7) { edit.length.reset(); edit.offset.reset(); edit.autoResize = true; TimePos::setTicksPerBar(0); } if (failure == 9) { auto outside = typedClip(1); QVERIFY(clip->mutedModel()->linkToModel(outside->soloModel())); Engine::projectJournal()->setJournalling(true); } const auto position = clip->startPosition(); const auto size = clip->length(); const auto name = clip->name(); bool changed = true; QString error; QVERIFY(!clip->edit(edit, changed, error)); QVERIFY(!changed); QVERIFY(!error.isEmpty()); QCOMPARE(clip->startPosition(), position); QCOMPARE(clip->length(), size); QCOMPARE(clip->name(), name); QCOMPARE(clip->mutedModel()->storedValue(), 0.f); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void recordingOwnershipAndCooperativeChangesRefuseAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("midi-recording") << 0; QTest::newRow("sample-armed") << 1; QTest::newRow("automation-recording") << 2; QTest::newRow("foreign-thread") << 3; QTest::newRow("cancel") << 4; QTest::newRow("clip-retired") << 5; QTest::newRow("position-changed") << 6; QTest::newRow("refresh-meter-changed") << 7; QTest::newRow("offset-clock-changed") << 8; }
	void recordingOwnershipAndCooperativeChangesRefuseAdmission()
	{
		QFETCH(int, failure); auto clip = typedClip(failure == 1 ? 1 : failure == 2 ? 2 : 0); const QPointer<Clip> weak(clip); const int meter = TimePos::ticksPerBar(); const auto tempo = Engine::getSong()->getTempo(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(meter); Engine::getSong()->setTempo(tempo); }); auto edit = options(); if (failure == 7) { edit.length.reset(); edit.offset.reset(); edit.autoResize = true; } bool changed = true, success = true, invoked = false, timer = false; QString error; int capture; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; }); if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = clip->edit(edit, changed, error); clip->getTrack()->endNoteRecording(&capture); } else if (failure == 1) { static_cast<SampleClip*>(clip)->setRecord(true); success = clip->edit(edit, changed, error); static_cast<SampleClip*>(clip)->setRecord(false); } else if (failure == 2) { static_cast<AutomationClip*>(clip)->setRecording(true); success = clip->edit(edit, changed, error); static_cast<AutomationClip*>(clip)->setRecording(false); } else if (failure == 3) { std::thread worker([&] { success = clip->edit(edit, changed, error); }); worker.join(); } else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 5) { delete clip; } if (failure == 6) { clip->movePosition(24); } if (failure == 7) { TimePos::setTicksPerBar(meter + 12); } if (failure == 8) { Engine::getSong()->setTempo(tempo + 1); } } return failure == 4; }, false); success = clip->edit(edit, changed, error); } QVERIFY(!success); QVERIFY(!changed); QVERIFY(!timer); QVERIFY(!error.isEmpty()); if (weak) { QCOMPARE(weak->startPosition().getTicks(), failure == 6 ? 24 : 0); }
	}
	void malformedStructuredEditsRefuseMutation_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("missing-track") << 0; QTest::newRow("missing-clip") << 1; QTest::newRow("fractional-position") << 2; QTest::newRow("offset-bound") << 3; QTest::newRow("manual-refresh") << 4; QTest::newRow("endpoint") << 5; QTest::newRow("empty-name") << 6; QTest::newRow("wrong-muted") << 7; QTest::newRow("unknown-field") << 8; }
	void malformedStructuredEditsRefuseMutation()
	{
		QFETCH(int, failure); auto clip = typedClip(0); auto args = arguments(); if (failure == 0) { args.remove("track"); } if (failure == 1) { args.remove("clip"); } if (failure == 2) { args.insert("position", 192.5); } if (failure == 3) { args.insert("offset", std::numeric_limits<int>::max()); } if (failure == 4) { args.insert("autoResize", true); } if (failure == 5) { args.insert("position", MaxSongLength - 1); } if (failure == 6) { args.insert("name", ""); } if (failure == 7) { args.insert("muted", 1); } if (failure == 8) { args.insert("unknown", true); } const auto size = clip->length(); const auto name = clip->name(); QVERIFY(!StudioCapabilities::execute("clip.edit", args).value("ok").toBool()); QCOMPARE(clip->startPosition().getTicks(), 0); QCOMPARE(clip->length(), size); QCOMPARE(clip->name(), name);
	}
	void receiptsWorkInSongAndPatternStore_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void receiptsWorkInSongAndPatternStore()
	{
		QFETCH(bool, pattern); Clip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = (new InstrumentTrack(Engine::patternStore()))->getClip(0); populate(static_cast<MidiClip*>(clip)); } else { clip = typedClip(0); } auto args = arguments(); if (pattern) { args.insert("container", "pattern"); args.remove("position"); } const auto response = StudioCapabilities::execute("clip.edit", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QCOMPARE(result.value("position").toInt(), pattern ? 0 : 192); QCOMPARE(result.value("length").toInt(), 96); QCOMPARE(result.value("offset").toInt(), -12); QVERIFY(!result.value("autoResize").toBool()); QVERIFY(result.value("changed").toBool()); QCOMPARE(clip->name(), QString("Edited"));
	}
	void runtimeHistoryReplayAndReopeningKeepNativeClipProperties()
	{
		QTemporaryDir directory; QString error; const auto command = request("edit", "clip.edit", arguments()); QJsonObject receipt; { StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); QCOMPARE(runtime.process(command), receipt); QCOMPARE(midi()->startPosition().getTicks(), 192); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->startPosition().getTicks(), 0); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->startTimeOffset().getTicks(), -12); } StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); QCOMPARE(reopened.process(command), receipt); QCOMPARE(midi()->name(), QString("Edited")); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void runtimeRecoversReturnedVirtualSetterAndNotificationFailures_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("data-throw") << 0; QTest::newRow("position-retirement") << 1; QTest::newRow("length-tamper") << 2; QTest::newRow("data-mute-tamper") << 3; }
	void runtimeRecoversReturnedVirtualSetterAndNotificationFailures()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); const auto name = midi()->name(); const auto size = midi()->length(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; QMetaObject::Connection connection; if (failure == 1) { connection = QObject::connect(clip, &Clip::positionChanged, this, [&] { if (done) { return; } done = true; delete clip; }); } else if (failure == 2) { connection = QObject::connect(clip, &Clip::lengthChanged, this, [&] { if (done) { return; } done = true; clip->changeLength(97); }); } else { connection = QObject::connect(clip, &Clip::dataChanged, this, [&] { if (done) { return; } if (failure == 3 && !clip->isMuted()) { return; } done = true; if (failure == 0) { throw std::runtime_error("fixture clip notification"); } clip->setMuted(false); }); } const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("reject", "clip.edit", arguments()); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->startPosition().getTicks(), 0); QCOMPARE(midi()->length(), size); QCOMPARE(midi()->name(), name); QCOMPARE(midi()->mutedModel()->storedValue(), 0.f); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QCOMPARE(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}
	void returnedFailureRestoresJournalFlagAndKeepsOneNativeUndo()
	{
		auto clip = typedClip(0); const auto name = clip->name(); auto journal = Engine::projectJournal(); journal->setJournalling(true); bool done = false; const auto connection = QObject::connect(clip, &Clip::dataChanged, this, [&] { if (done) { return; } done = true; throw std::runtime_error("fixture direct clip edit"); }); bool changed = true; QString error; QVERIFY(!clip->edit(options(), changed, error)); QObject::disconnect(connection); QVERIFY(!changed); QVERIFY(done); QVERIFY(journal->isJournalling()); QVERIFY(journal->canUndo()); journal->undo(); QCOMPARE(clip->startPosition().getTicks(), 0); QCOMPARE(clip->name(), name); QVERIFY(!journal->canUndo());
	}
	void scriptedAgentsCreateThenEditFutureClips_data() { protocols(); }
	void scriptedAgentsCreateThenEditFutureClips()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { auto args = arguments(); args.insert("clip", 1); return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Arrange a new phrase"}, {"steps", QJsonArray{"Create a clip", "Edit its native span"}}}), call("create", "clip.add", {{"track", 0}, {"position", 384}}, true), call("edit", "clip.edit", args, catalog), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native clip arranged"}, {"evidence", QJsonArray{"create", "edit"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Create a clip then move and trim it."}})).value("ok").toBool()); for (int index = 0; index < 8; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "edit").value("ok").toBool()); QCOMPARE(midi(0, 1)->length().getTicks(), 96); QCOMPARE(midi(0, 1)->startPosition().getTicks(), 192); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void scriptedAgentsSkipReplacedClipOwners_data() { protocols(); }
	void scriptedAgentsSkipReplacedClipOwners()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("first", "clip.remove", {{"track", 0}, {"clip", 0}}, true), call("stale", "clip.edit", arguments(), catalog), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old clip"}, {"evidence", QJsonArray{"first"}}})}); } return decision(protocol, {call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed retired clip"}, {"evidence", QJsonArray{"first"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Edit the clip after it changes."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QVERIFY(!record(directory, "stale").value("ok").toBool()); QVERIFY(record(directory, "stale").value("error").toString().contains("changed a queued native selection")); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void schemaAndDirectCatalogRoutesKeepGeometryBounds()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "clip.edit") { schema = value.toObject(); } } const auto fields = schema.value("inputSchema").toObject().value("properties").toObject(); QCOMPARE(fields.value("position").toObject().value("minimum").toInt(), 0); QCOMPARE(fields.value("length").toObject().value("minimum").toInt(), 1); QCOMPARE(fields.value("offset").toObject().value("minimum").toInt(), -MaxSongLength); for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "clip_edit"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } QJsonObject args{{"op", "clip.edit"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("clip.edit")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioClipEditTest)
#include "StudioClipEditTest.moc"
